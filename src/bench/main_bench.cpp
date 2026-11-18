/**
 * @file main_bench.cpp
 * @brief Synthetic load and latency benchmark for EEVS (`eevs_bench`).
 */

#include "core/crypto_utils.h"
#include "core/server_config.h"
#include "eevs/types.h"
#include "network/enet.h"
#include "network/protocol.h"

#include <CLI/CLI.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <glaze/glaze.hpp>
#include <hdr/hdr_histogram.h>
#include <lz4.h>
#include <opus/opus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace eevs::bench {

namespace {

std::atomic<bool> g_stopRequested{false};

void SignalHandler(int /*sig*/) noexcept {
    g_stopRequested.store(true, std::memory_order_relaxed);
}

uint64_t NowSteadyNs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

enum class BenchStage : int {
    FastPath      = 1,
    HighFanout    = 2,
    DspProcessing = 3,
    RouteChurn    = 4
};

const char* StageName(BenchStage stage) noexcept {
    switch (stage) {
        case BenchStage::FastPath:      return "1: Fast-Path Relay";
        case BenchStage::HighFanout:    return "2: High Fan-Out";
        case BenchStage::DspProcessing: return "3: DSP Processing";
        case BenchStage::RouteChurn:    return "4: Route Churn";
        default:                        return "Custom";
    }
}

struct BenchOptions {
    std::string configPath = "eevs.cfg";
    std::string host = "127.0.0.1";
    uint16_t gatewayPort = 30122;
    uint16_t voicePort = 30123;
    uint16_t controlPort = 30124;
    std::string licenseKey = "changeme_cfxk_your_fivem_license_key";
    std::string controlKey;

    uint16_t peerCount = 2000;
    uint16_t activeSpeakers = 200;
    uint16_t clusterSize = 16;
    uint16_t dspSpeakers = 0;
    uint32_t durationSec = 0; // 0 = interactive until quit, >0 = timed run
    int initialStage = 1;
    bool headless = false;
    bool hostMockGateway = true;
    std::string jsonReportPath;
};

struct VirtualPeer {
    uint16_t clientId = 0;
    uint16_t netId = 0;
    std::array<uint8_t, 32> token{};
    uint32_t tokenJoaat = 0;
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    std::atomic<uint16_t> assignedPeerId{0xFFFFu};
    std::atomic<bool> authenticated{false};
    uint32_t sequenceNumber = 1;
    std::vector<uint16_t> currentSyncTargets;
};

struct TelemetrySnapshot {
    uint64_t ingressPps = 0;
    uint64_t egressPps = 0;
    double ingressMbps = 0.0;
    double egressMbps = 0.0;
    uint64_t totalIngressPackets = 0;
    uint64_t totalEgressPackets = 0;
    uint64_t syncSnapshotsSent = 0;
    uint32_t connectedPeers = 0;
    uint32_t authenticatedPeers = 0;
    bool gatewayConnected = false;
    bool gatewayAuthenticated = false;
    int64_t p50Us = 0;
    int64_t p90Us = 0;
    int64_t p99Us = 0;
    int64_t p999Us = 0;
    int64_t maxUs = 0;
    double meanUs = 0.0;
};

struct JsonBenchmarkReport {
    std::string stage;
    uint16_t configuredPeers = 0;
    uint32_t authenticatedPeers = 0;
    uint16_t activeSpeakers = 0;
    uint16_t clusterSize = 0;
    uint16_t dspSpeakers = 0;
    uint64_t peakIngressPps = 0;
    uint64_t peakEgressPps = 0;
    double peakIngressMbps = 0.0;
    double peakEgressMbps = 0.0;
    uint64_t totalIngressPackets = 0;
    uint64_t totalEgressPackets = 0;
    int64_t latencyP50Us = 0;
    int64_t latencyP90Us = 0;
    int64_t latencyP99Us = 0;
    int64_t latencyP999Us = 0;
    int64_t latencyMaxUs = 0;
};

// Simple lock-free timestamp lookup table indexed by (senderNetId, seq & 0xFF)
class SendTimestampTracker {
public:
    static constexpr size_t kMaxNetIds = 4096;
    static constexpr size_t kSeqRingMask = 0xFFu;
    static constexpr size_t kSeqRingSize = 256;

    SendTimestampTracker()
        : m_timestamps(kMaxNetIds * kSeqRingSize) {
        for (auto& a : m_timestamps) {
            a.store(0, std::memory_order_relaxed);
        }
    }

    void RecordSend(uint16_t netId, uint32_t seq, uint64_t nowNs) noexcept {
        if (netId >= kMaxNetIds) {
            return;
        }
        const size_t idx = (static_cast<size_t>(netId) << 8u) | (seq & kSeqRingMask);
        m_timestamps[idx].store(nowNs, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t LookupSendNs(uint16_t netId, uint32_t seq) const noexcept {
        if (netId >= kMaxNetIds) {
            return 0;
        }
        const size_t idx = (static_cast<size_t>(netId) << 8u) | (seq & kSeqRingMask);
        return m_timestamps[idx].load(std::memory_order_relaxed);
    }

private:
    std::vector<std::atomic<uint64_t>> m_timestamps;
};

// Minimal HTTP POST helper to configure DSP effects on `eevs.exe` Control API (:30124)
bool SendControlHttpPost(
    const std::string& host,
    uint16_t port,
    const std::string& path,
    const std::string& bearerToken,
    const std::string& jsonBody
) {
#if defined(_WIN32)
    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return false;
    }
    DWORD timeoutMs = 400;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return false;
    }
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#if defined(_WIN32)
        ::closesocket(sock);
#else
        ::close(sock);
#endif
        return false;
    }

    std::string req = std::format(
        "POST {} HTTP/1.1\r\n"
        "Host: {}:{}\r\n"
        "Authorization: Bearer {}\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: {}\r\n"
        "Connection: close\r\n\r\n{}",
        path,
        host,
        port,
        bearerToken,
        jsonBody.size(),
        jsonBody
    );

    ::send(sock, req.data(), static_cast<int>(req.size()), 0);
    char respBuf[256]{};
    ::recv(sock, respBuf, sizeof(respBuf) - 1, 0);

#if defined(_WIN32)
    ::closesocket(sock);
#else
    ::close(sock);
#endif
    return std::string_view(respBuf).find("200") != std::string_view::npos;
}

class SwarmController {
public:
    explicit SwarmController(BenchOptions opts)
        : m_opts(std::move(opts))
        , m_peers(m_opts.peerCount) {
        hdr_init(1, 10'000'000, 3, &m_intervalHistogram);
        hdr_init(1, 10'000'000, 3, &m_cumulativeHistogram);

        m_activeSpeakers.store(m_opts.activeSpeakers, std::memory_order_relaxed);
        m_clusterSize.store(m_opts.clusterSize, std::memory_order_relaxed);
        m_dspSpeakers.store(m_opts.dspSpeakers, std::memory_order_relaxed);
        m_currentStage.store(static_cast<BenchStage>(std::clamp(m_opts.initialStage, 1, 4)), std::memory_order_relaxed);

        InitializePeerIdentities();
        GenerateSampleOpusFrames();
    }

    ~SwarmController() {
        Stop();
        if (m_intervalHistogram) {
            hdr_close(m_intervalHistogram);
        }
        if (m_cumulativeHistogram) {
            hdr_close(m_cumulativeHistogram);
        }
    }

    bool Start() {
        if (m_opts.hostMockGateway) {
            ENetAddress gwAddr{};
            enet_address_set_host(&gwAddr, m_opts.host.c_str());
            gwAddr.port = m_opts.gatewayPort;

            m_gatewayHost = enet_host_create(&gwAddr, 8, protocol::kChannelCount, 0, 0);
            if (!m_gatewayHost) {
                std::cerr << "[eevs_bench] Failed to bind Mock FXServer Gateway on "
                          << m_opts.host << ":" << m_opts.gatewayPort
                          << " (is FXServer already bound to that port? Use --no-mock-gateway if so).\n";
                return false;
            }
            m_gatewayHost->mtu = protocol::kDefaultMtu;
        }

        m_running.store(true, std::memory_order_release);

        if (m_gatewayHost) {
            m_gatewayThread = std::jthread([this](std::stop_token st) { GatewayLoop(st); });
        }

        // Partition virtual peers across worker threads for ENet service + egress UDP reading
        const unsigned int hwThreads = std::max(4u, std::min(16u, std::thread::hardware_concurrency()));
        m_swarmThreads.reserve(hwThreads);
        for (unsigned int t = 0; t < hwThreads; ++t) {
            m_swarmThreads.emplace_back([this, t, hwThreads](std::stop_token st) {
                SwarmReceiveAndConnectLoop(st, t, hwThreads);
            });
        }

        m_senderThread = std::jthread([this](std::stop_token st) { AudioSenderLoop(st); });
        return true;
    }

    void Stop() {
        if (!m_running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        if (m_senderThread.joinable()) {
            m_senderThread.request_stop();
            m_senderThread.join();
        }
        for (auto& t : m_swarmThreads) {
            if (t.joinable()) {
                t.request_stop();
            }
        }
        for (auto& t : m_swarmThreads) {
            if (t.joinable()) {
                t.join();
            }
        }
        m_swarmThreads.clear();

        if (m_gatewayThread.joinable()) {
            m_gatewayThread.request_stop();
            m_gatewayThread.join();
        }

        for (VirtualPeer& vp : m_peers) {
            if (vp.peer && vp.host) {
                enet_peer_disconnect_now(vp.peer, 0);
                vp.peer = nullptr;
            }
            if (vp.host) {
                enet_host_destroy(vp.host);
                vp.host = nullptr;
            }
        }

        if (m_gatewayHost) {
            enet_host_destroy(m_gatewayHost);
            m_gatewayHost = nullptr;
        }
    }

    void ApplyStagePreset(BenchStage stage) {
        m_currentStage.store(stage, std::memory_order_relaxed);
        switch (stage) {
            case BenchStage::FastPath:
                m_activeSpeakers.store(std::min<uint16_t>(200, m_opts.peerCount), std::memory_order_relaxed);
                m_clusterSize.store(std::min<uint16_t>(16, m_opts.peerCount), std::memory_order_relaxed);
                m_dspSpeakers.store(0, std::memory_order_relaxed);
                m_churnEnabled.store(false, std::memory_order_relaxed);
                break;
            case BenchStage::HighFanout:
                m_activeSpeakers.store(std::min<uint16_t>(50, m_opts.peerCount), std::memory_order_relaxed);
                m_clusterSize.store(std::min<uint16_t>(51, m_opts.peerCount), std::memory_order_relaxed);
                m_dspSpeakers.store(0, std::memory_order_relaxed);
                m_churnEnabled.store(false, std::memory_order_relaxed);
                break;
            case BenchStage::DspProcessing:
                m_activeSpeakers.store(std::min<uint16_t>(100, m_opts.peerCount), std::memory_order_relaxed);
                m_clusterSize.store(std::min<uint16_t>(21, m_opts.peerCount), std::memory_order_relaxed);
                m_dspSpeakers.store(std::min<uint16_t>(100, m_opts.peerCount), std::memory_order_relaxed);
                m_churnEnabled.store(false, std::memory_order_relaxed);
                break;
            case BenchStage::RouteChurn:
                m_activeSpeakers.store(std::min<uint16_t>(150, m_opts.peerCount), std::memory_order_relaxed);
                m_clusterSize.store(std::min<uint16_t>(11, m_opts.peerCount), std::memory_order_relaxed);
                m_dspSpeakers.store(0, std::memory_order_relaxed);
                m_churnEnabled.store(true, std::memory_order_relaxed);
                break;
        }
        m_resyncRequested.store(true, std::memory_order_release);
        m_dspSyncRequested.store(true, std::memory_order_release);
    }

    TelemetrySnapshot SampleMetrics(double elapsedDtSec) {
        TelemetrySnapshot snap{};
        const uint64_t inTotal = m_totalIngressPkts.load(std::memory_order_relaxed);
        const uint64_t outTotal = m_totalEgressPkts.load(std::memory_order_relaxed);
        const uint64_t inDelta = inTotal - m_prevIngressPkts;
        const uint64_t outDelta = outTotal - m_prevEgressPkts;
        m_prevIngressPkts = inTotal;
        m_prevEgressPkts = outTotal;

        if (elapsedDtSec > 0.05) {
            snap.ingressPps = static_cast<uint64_t>(static_cast<double>(inDelta) / elapsedDtSec);
            snap.egressPps = static_cast<uint64_t>(static_cast<double>(outDelta) / elapsedDtSec);
            // 1300 bytes ingress payload + 28B IP/UDP header = 1328B * 8 = 10,624 bits
            snap.ingressMbps = (static_cast<double>(snap.ingressPps) * 1300.0 * 8.0) / 1'000'000.0;
            // 1282 bytes egress payload * 8 = 10,256 bits
            snap.egressMbps = (static_cast<double>(snap.egressPps) * 1282.0 * 8.0) / 1'000'000.0;
        }

        snap.totalIngressPackets = inTotal;
        snap.totalEgressPackets = outTotal;
        snap.syncSnapshotsSent = m_syncSnapshotsSent.load(std::memory_order_relaxed);
        snap.gatewayConnected = m_gatewayConnected.load(std::memory_order_relaxed);
        snap.gatewayAuthenticated = m_gatewayAuthenticated.load(std::memory_order_relaxed);

        uint32_t authCount = 0;
        uint32_t connCount = 0;
        for (const VirtualPeer& vp : m_peers) {
            if (vp.authenticated.load(std::memory_order_relaxed)) {
                ++authCount;
                ++connCount;
            } else if (vp.peer != nullptr) {
                ++connCount;
            }
        }
        snap.connectedPeers = connCount;
        snap.authenticatedPeers = authCount;

        {
            std::scoped_lock lock(m_histMutex);
            if (m_intervalHistogram && m_intervalHistogram->total_count > 0) {
                snap.p50Us = hdr_value_at_percentile(m_intervalHistogram, 50.0);
                snap.p90Us = hdr_value_at_percentile(m_intervalHistogram, 90.0);
                snap.p99Us = hdr_value_at_percentile(m_intervalHistogram, 99.0);
                snap.p999Us = hdr_value_at_percentile(m_intervalHistogram, 99.9);
                snap.maxUs = hdr_max(m_intervalHistogram);
                snap.meanUs = hdr_mean(m_intervalHistogram);
                hdr_reset(m_intervalHistogram);
            } else if (m_cumulativeHistogram && m_cumulativeHistogram->total_count > 0) {
                snap.p50Us = hdr_value_at_percentile(m_cumulativeHistogram, 50.0);
                snap.p90Us = hdr_value_at_percentile(m_cumulativeHistogram, 90.0);
                snap.p99Us = hdr_value_at_percentile(m_cumulativeHistogram, 99.0);
                snap.p999Us = hdr_value_at_percentile(m_cumulativeHistogram, 99.9);
                snap.maxUs = hdr_max(m_cumulativeHistogram);
                snap.meanUs = hdr_mean(m_cumulativeHistogram);
            }
        }

        m_peakIngressPps = std::max(m_peakIngressPps, snap.ingressPps);
        m_peakEgressPps = std::max(m_peakEgressPps, snap.egressPps);
        m_peakIngressMbps = std::max(m_peakIngressMbps, snap.ingressMbps);
        m_peakEgressMbps = std::max(m_peakEgressMbps, snap.egressMbps);

        return snap;
    }

    JsonBenchmarkReport BuildFinalReport() const {
        JsonBenchmarkReport rep{};
        rep.stage = StageName(m_currentStage.load(std::memory_order_relaxed));
        rep.configuredPeers = m_opts.peerCount;
        uint32_t authCount = 0;
        for (const VirtualPeer& vp : m_peers) {
            if (vp.authenticated.load(std::memory_order_relaxed)) {
                ++authCount;
            }
        }
        rep.authenticatedPeers = authCount;
        rep.activeSpeakers = m_activeSpeakers.load(std::memory_order_relaxed);
        rep.clusterSize = m_clusterSize.load(std::memory_order_relaxed);
        rep.dspSpeakers = m_dspSpeakers.load(std::memory_order_relaxed);
        rep.peakIngressPps = m_peakIngressPps;
        rep.peakEgressPps = m_peakEgressPps;
        rep.peakIngressMbps = m_peakIngressMbps;
        rep.peakEgressMbps = m_peakEgressMbps;
        rep.totalIngressPackets = m_totalIngressPkts.load(std::memory_order_relaxed);
        rep.totalEgressPackets = m_totalEgressPkts.load(std::memory_order_relaxed);

        std::scoped_lock lock(m_histMutex);
        if (m_cumulativeHistogram && m_cumulativeHistogram->total_count > 0) {
            rep.latencyP50Us = hdr_value_at_percentile(m_cumulativeHistogram, 50.0);
            rep.latencyP90Us = hdr_value_at_percentile(m_cumulativeHistogram, 90.0);
            rep.latencyP99Us = hdr_value_at_percentile(m_cumulativeHistogram, 99.0);
            rep.latencyP999Us = hdr_value_at_percentile(m_cumulativeHistogram, 99.9);
            rep.latencyMaxUs = hdr_max(m_cumulativeHistogram);
        }
        return rep;
    }

    [[nodiscard]] BenchStage GetCurrentStage() const noexcept {
        return m_currentStage.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint16_t GetActiveSpeakers() const noexcept {
        return m_activeSpeakers.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint16_t GetClusterSize() const noexcept {
        return m_clusterSize.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint16_t GetDspSpeakers() const noexcept {
        return m_dspSpeakers.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint16_t GetPeerCount() const noexcept {
        return m_opts.peerCount;
    }

    void AdjustSpeakers(int delta) {
        int next = static_cast<int>(m_activeSpeakers.load(std::memory_order_relaxed)) + delta;
        m_activeSpeakers.store(
            static_cast<uint16_t>(std::clamp(next, 1, static_cast<int>(m_opts.peerCount))),
            std::memory_order_relaxed
        );
    }

    void AdjustClusterSize(int delta) {
        int next = static_cast<int>(m_clusterSize.load(std::memory_order_relaxed)) + delta;
        m_clusterSize.store(
            static_cast<uint16_t>(std::clamp(next, 2, static_cast<int>(std::min<uint16_t>(250, m_opts.peerCount)))),
            std::memory_order_relaxed
        );
        m_resyncRequested.store(true, std::memory_order_release);
    }

private:
    void InitializePeerIdentities() {
        for (uint16_t i = 0; i < m_opts.peerCount; ++i) {
            VirtualPeer& vp = m_peers[i];
            vp.clientId = static_cast<uint16_t>(i + 1);
            vp.netId = static_cast<uint16_t>(i + 1);

            std::string seed = std::format("eevs_bench_peer_token_{}", vp.clientId);
            vp.token = crypto::ComputeSha256(seed);
            vp.tokenJoaat = crypto::ComputeJoaat(vp.token.data(), vp.token.size());
        }
    }

    void GenerateSampleOpusFrames() {
        int opusErr = OPUS_OK;
        OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &opusErr);
        if (opusErr != OPUS_OK || !enc) {
            // Fallback valid CELT 10ms frame starting with 0x70
            std::vector<uint8_t> fallback = {0x70, 0x07, 0xC9, 0x41, 0x22, 0x88, 0x10, 0x55};
            m_sampleOpusFrames.push_back(std::move(fallback));
            return;
        }

        opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

        constexpr size_t kFrameSamples = 480; // 10ms @ 48kHz
        std::array<float, kFrameSamples> pcm{};
        std::array<uint8_t, 256> encoded{};

        // Encode 50 frames (500ms loop) of a voice-like harmonic signal
        for (int f = 0; f < 50; ++f) {
            for (size_t s = 0; s < kFrameSamples; ++s) {
                const double t = static_cast<double>(f * kFrameSamples + s) / 48000.0;
                const double env = 0.6 + 0.3 * std::sin(2.0 * std::numbers::pi * 4.0 * t);
                const double sig = 0.5 * std::sin(2.0 * std::numbers::pi * 180.0 * t)
                                 + 0.3 * std::sin(2.0 * std::numbers::pi * 360.0 * t)
                                 + 0.2 * std::sin(2.0 * std::numbers::pi * 720.0 * t);
                pcm[s] = static_cast<float>(env * sig * 0.4);
            }
            const int bytes = opus_encode_float(enc, pcm.data(), static_cast<int>(kFrameSamples), encoded.data(), static_cast<opus_int32>(encoded.size()));
            if (bytes > 0) {
                m_sampleOpusFrames.emplace_back(encoded.data(), encoded.data() + bytes);
            }
        }
        opus_encoder_destroy(enc);
    }

    // Builds and sends LZ4-compressed `VoiceStreamingSync` packets in batches of 250 clients
    void SendVoiceStreamingSyncSnapshot(ENetPeer* gatewayClientPeer, bool includeTokens, uint32_t clusterShift = 0) {
        if (!gatewayClientPeer || !m_gatewayHost) {
            return;
        }

        const uint16_t totalPeers = m_opts.peerCount;
        const uint16_t clusterSz = std::max<uint16_t>(2, m_clusterSize.load(std::memory_order_relaxed));
        constexpr uint16_t kBatchClients = 15;

        std::vector<uint8_t> rawBuf;
        rawBuf.reserve(128 * 1024);
        std::vector<uint8_t> compressedPacket;

        for (uint16_t batchStart = 0; batchStart < totalPeers; batchStart += kBatchClients) {
            rawBuf.clear();
            const uint16_t batchEnd = std::min<uint16_t>(totalPeers, static_cast<uint16_t>(batchStart + kBatchClients));

            // Compute cluster neighbors (clusterIdx * clusterSz .. (clusterIdx + 1) * clusterSz - 1)
            for (uint16_t idx = batchStart; idx < batchEnd; ++idx) {
                VirtualPeer& vp = m_peers[idx];
                const uint16_t rawClientId = includeTokens
                    ? static_cast<uint16_t>(vp.clientId | 0x8000u)
                    : vp.clientId;

                auto appendU16 = [&rawBuf](uint16_t v) {
                    const uint8_t b[2] = {
                        static_cast<uint8_t>(v & 0xFFu),
                        static_cast<uint8_t>((v >> 8u) & 0xFFu)
                    };
                    rawBuf.insert(rawBuf.end(), b, b + 2);
                };

                appendU16(rawClientId);
                if (includeTokens) {
                    rawBuf.insert(rawBuf.end(), vp.token.begin(), vp.token.end());
                    appendU16(vp.netId);
                    vp.currentSyncTargets.clear();
                }

                const uint16_t clusterBase = static_cast<uint16_t>((idx / clusterSz) * clusterSz);
                const uint16_t clusterLimit = std::min<uint16_t>(totalPeers, static_cast<uint16_t>(clusterBase + clusterSz));

                std::vector<uint16_t> nextTargets;
                nextTargets.reserve(clusterSz);
                for (uint16_t c = clusterBase; c < clusterLimit; ++c) {
                    const uint16_t targetIdx = static_cast<uint16_t>((c + clusterShift) % totalPeers);
                    if (targetIdx != idx) {
                        const uint16_t targetClientId = m_peers[targetIdx].clientId;
                        // Bit 13 (0x2000) = RoutingMode::Spatial3D (1 << 13)
                        nextTargets.push_back(static_cast<uint16_t>((targetClientId & 0x1FFFu) | (1u << 13u)));
                    }
                }

                // Compute added & removed deltas relative to vp.currentSyncTargets
                std::vector<uint16_t> addedEntries;
                std::vector<uint16_t> removedEntries;
                addedEntries.reserve(nextTargets.size());
                removedEntries.reserve(vp.currentSyncTargets.size());

                for (uint16_t entry : nextTargets) {
                    if (std::ranges::find(vp.currentSyncTargets, entry) == vp.currentSyncTargets.end()) {
                        addedEntries.push_back(entry);
                    }
                }
                for (uint16_t oldEntry : vp.currentSyncTargets) {
                    if (std::ranges::find(nextTargets, oldEntry) == nextTargets.end()) {
                        removedEntries.push_back(oldEntry);
                    }
                }

                vp.currentSyncTargets = std::move(nextTargets);

                appendU16(static_cast<uint16_t>(addedEntries.size()));
                for (uint16_t entry : addedEntries) {
                    appendU16(entry);
                }

                appendU16(static_cast<uint16_t>(removedEntries.size()));
                for (uint16_t entry : removedEntries) {
                    appendU16(entry);
                }
            }

            const int maxDst = LZ4_compressBound(static_cast<int>(rawBuf.size()));
            compressedPacket.resize(1 + static_cast<size_t>(maxDst));
            compressedPacket[0] = protocol::kVoiceStreamingSyncPacketId;

            const int compBytes = LZ4_compress_default(
                reinterpret_cast<const char*>(rawBuf.data()),
                reinterpret_cast<char*>(compressedPacket.data() + 1),
                static_cast<int>(rawBuf.size()),
                maxDst
            );

            if (compBytes > 0) {
                ENetPacket* pkt = enet_packet_create(
                    compressedPacket.data(),
                    1 + static_cast<size_t>(compBytes),
                    ENET_PACKET_FLAG_RELIABLE
                );
                if (pkt) {
                    enet_peer_send(gatewayClientPeer, 0, pkt);
                    m_syncSnapshotsSent.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        enet_host_flush(m_gatewayHost);
    }

    void GatewayLoop(std::stop_token st) {
        ENetPeer* activeGatewayPeer = nullptr;
        uint32_t churnTick = 0;
        auto lastChurnTime = std::chrono::steady_clock::now();

        while (!st.stop_requested() && m_running.load(std::memory_order_relaxed)) {
            ENetEvent ev{};
            while (enet_host_service(m_gatewayHost, &ev, 2) > 0) {
                switch (ev.type) {
                    case ENET_EVENT_TYPE_CONNECT: {
                        activeGatewayPeer = ev.peer;
                        m_gatewayConnected.store(true, std::memory_order_relaxed);
                        break;
                    }
                    case ENET_EVENT_TYPE_RECEIVE: {
                        if (ev.packet && ev.packet->dataLength >= sizeof(protocol::VoiceAuthPacket)) {
                            const uint8_t* d = ev.packet->data;
                            if (d[0] == protocol::kControlPacketMarker) {
                                uint16_t pktId = 0;
                                std::memcpy(&pktId, d + 1, sizeof(uint16_t));
                                if (pktId == protocol::kVoiceAuthPacketId) {
                                    protocol::VoiceHandshakeDonePacket ack{};
                                    ack.marker = protocol::kControlPacketMarker;
                                    ack.packetId = protocol::kVoiceHandshakeDonePacketId;
                                    ENetPacket* ackPkt = enet_packet_create(&ack, sizeof(ack), ENET_PACKET_FLAG_RELIABLE);
                                    enet_peer_send(ev.peer, 0, ackPkt);
                                    enet_host_flush(m_gatewayHost);

                                    m_gatewayAuthenticated.store(true, std::memory_order_release);
                                    SendVoiceStreamingSyncSnapshot(ev.peer, true, 0);
                                }
                            }
                        }
                        if (ev.packet) {
                            enet_packet_destroy(ev.packet);
                        }
                        break;
                    }
                    case ENET_EVENT_TYPE_DISCONNECT:
                    case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: {
                        if (ev.peer == activeGatewayPeer) {
                            activeGatewayPeer = nullptr;
                            m_gatewayConnected.store(false, std::memory_order_relaxed);
                            m_gatewayAuthenticated.store(false, std::memory_order_relaxed);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }

            if (activeGatewayPeer && m_gatewayAuthenticated.load(std::memory_order_relaxed)) {
                if (m_resyncRequested.exchange(false, std::memory_order_acq_rel)) {
                    SendVoiceStreamingSyncSnapshot(activeGatewayPeer, true, churnTick);
                }

                if (m_churnEnabled.load(std::memory_order_relaxed)) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastChurnTime >= std::chrono::milliseconds(100)) {
                        lastChurnTime = now;
                        ++churnTick;
                        SendVoiceStreamingSyncSnapshot(activeGatewayPeer, false, churnTick * 3);
                    }
                }
            }
        }
    }

    void SwarmReceiveAndConnectLoop(std::stop_token st, unsigned int threadIdx, unsigned int totalThreads) {
        ENetAddress voiceServerAddr{};
        enet_address_set_host(&voiceServerAddr, m_opts.host.c_str());
        voiceServerAddr.port = m_opts.voicePort;

        size_t nextConnectSlot = threadIdx;
        auto lastConnectBatch = std::chrono::steady_clock::now();
        auto lastEnetServiceTime = lastConnectBatch;

        while (!st.stop_requested() && m_running.load(std::memory_order_relaxed)) {
            const bool canConnect = !m_opts.hostMockGateway || m_gatewayAuthenticated.load(std::memory_order_acquire);

            // Stagger connection creation (up to 16 peers per thread every 15ms) so we don't overflow SYN/UDP queues
            const auto now = std::chrono::steady_clock::now();
            if (canConnect && nextConnectSlot < m_peers.size() &&
                (now - lastConnectBatch) >= std::chrono::milliseconds(15)) {
                lastConnectBatch = now;
                for (int b = 0; b < 16 && nextConnectSlot < m_peers.size(); ++b, nextConnectSlot += totalThreads) {
                    VirtualPeer& vp = m_peers[nextConnectSlot];
                    if (!vp.host) {
                        vp.host = enet_host_create(nullptr, 1, protocol::kChannelCount, 0, 0);
                        if (vp.host) {
                            vp.host->mtu = protocol::kDefaultMtu;
                            vp.host->userData = this;
                            vp.host->intercept = reinterpret_cast<ENetInterceptCallback>(&SwarmController::ClientFastPathReceiveIntercept);
                            enet_socket_set_option(vp.host->socket, ENET_SOCKOPT_RCVBUF, 512 * 1024);
                            enet_socket_set_option(vp.host->socket, ENET_SOCKOPT_SNDBUF, 512 * 1024);
                            vp.peer = enet_host_connect(vp.host, &voiceServerAddr, protocol::kChannelCount, vp.tokenJoaat);
                        }
                    }
                }
            }

            bool didWork = false;
            const auto pollNow = std::chrono::steady_clock::now();
            const bool serviceFullEnet = (pollNow - lastEnetServiceTime) >= std::chrono::milliseconds(15);
            if (serviceFullEnet) {
                lastEnetServiceTime = pollNow;
            }

            std::array<uint8_t, protocol::kDefaultMtu> rxBuf{};
            ENetAddress fromAddr{};

            for (size_t i = threadIdx; i < m_peers.size(); i += totalThreads) {
                VirtualPeer& vp = m_peers[i];
                if (!vp.host) {
                    continue;
                }

                if (vp.authenticated.load(std::memory_order_relaxed)) {
                    if (!serviceFullEnet) {
                        char peekByte = 0;
                        const int peekLen = ::recv(vp.host->socket, &peekByte, 1, MSG_PEEK);
                        if (peekLen > 0 && static_cast<uint8_t>(peekByte) == protocol::kVoiceServerAudioPacketId) {
                            for (int pktIdx = 0; pktIdx < 32; ++pktIdx) {
                                ENetBuffer buf{};
                                buf.data = rxBuf.data();
                                buf.dataLength = rxBuf.size();
                                const int recvLen = enet_socket_receive(vp.host->socket, &fromAddr, &buf, 1);
                                if (recvLen <= 0) {
                                    break;
                                }
                                didWork = true;
                                if (static_cast<uint8_t>(rxBuf[0]) == protocol::kVoiceServerAudioPacketId) {
                                    OnFastPathServerVoiceReceived(rxBuf.data(), static_cast<size_t>(recvLen));
                                }
                                // Check if next packet in queue is also a voice packet
                                const int nextPeek = ::recv(vp.host->socket, &peekByte, 1, MSG_PEEK);
                                if (nextPeek <= 0 || static_cast<uint8_t>(peekByte) != protocol::kVoiceServerAudioPacketId) {
                                    break;
                                }
                            }
                        }
                        continue;
                    }
                }

                ENetEvent ev{};
                while (enet_host_service(vp.host, &ev, 0) > 0) {
                    didWork = true;
                    switch (ev.type) {
                        case ENET_EVENT_TYPE_CONNECT: {
                            // Record our peer slot ID on the EEVS server (`outgoingPeerID`)
                            vp.assignedPeerId.store(ev.peer->outgoingPeerID, std::memory_order_relaxed);

                            protocol::VoiceAuthPacket auth{};
                            auth.marker = protocol::kControlPacketMarker;
                            auth.packetId = protocol::kVoiceAuthPacketId;
                            auth.clientIdOrZero = vp.clientId;
                            std::memcpy(auth.secret, vp.token.data(), 32);

                            ENetPacket* pkt = enet_packet_create(&auth, sizeof(auth), ENET_PACKET_FLAG_RELIABLE);
                            enet_peer_send(ev.peer, 0, pkt);
                            enet_host_flush(vp.host);
                            break;
                        }
                        case ENET_EVENT_TYPE_RECEIVE: {
                            if (ev.packet && ev.packet->dataLength >= sizeof(protocol::VoiceHandshakeDonePacket)) {
                                const uint8_t* d = ev.packet->data;
                                if (d[0] == protocol::kControlPacketMarker) {
                                    uint16_t cid = 0;
                                    std::memcpy(&cid, d + 1, sizeof(uint16_t));
                                    if (cid == protocol::kVoiceHandshakeDonePacketId) {
                                        vp.authenticated.store(true, std::memory_order_release);
                                    }
                                }
                            }
                            if (ev.packet) {
                                enet_packet_destroy(ev.packet);
                            }
                            break;
                        }
                        case ENET_EVENT_TYPE_DISCONNECT:
                        case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: {
                            vp.authenticated.store(false, std::memory_order_relaxed);
                            vp.assignedPeerId.store(0xFFFFu, std::memory_order_relaxed);
                            vp.peer = enet_host_connect(vp.host, &voiceServerAddr, protocol::kChannelCount, vp.tokenJoaat);
                            break;
                        }
                        default:
                            break;
                    }
                }
            }

            if (!didWork) {
                std::this_thread::yield();
            }
        }
    }

    void OnFastPathServerVoiceReceived(const uint8_t* data, size_t length) {
        if (length < sizeof(protocol::ServerVoicePacketHeader) ||
            data[0] != protocol::kVoiceServerAudioPacketId) {
            return;
        }

        uint16_t senderNetId = 0;
        uint32_t seqNum = 0;
        std::memcpy(&senderNetId, data + 1, sizeof(uint16_t));
        std::memcpy(&seqNum, data + 3, sizeof(uint32_t));

        const uint64_t nowNs = NowSteadyNs();
        const uint64_t sentNs = m_timestamps.LookupSendNs(senderNetId, seqNum);
        if (sentNs > 0 && nowNs >= sentNs) {
            const int64_t rttUs = std::max<int64_t>(1, static_cast<int64_t>((nowNs - sentNs) / 1000u));
            RecordLatencySampleUs(rttUs);
        }

        m_totalEgressPkts.fetch_add(1, std::memory_order_relaxed);
    }

    static int ENET_CALLBACK ClientFastPathReceiveIntercept(ENetHost* host, ENetEvent* /*event*/) {
        auto* self = reinterpret_cast<SwarmController*>(host->userData);
        if (!self || host->receivedDataLength < sizeof(protocol::ServerVoicePacketHeader)) {
            return 0;
        }

        const uint8_t* data = host->receivedData;
        if (data[0] != protocol::kVoiceServerAudioPacketId) {
            return 0; // Let ENet handle normal control/handshake packets
        }

        self->OnFastPathServerVoiceReceived(data, host->receivedDataLength);
        return 1; // Fast-path consumed
    }

    void RecordLatencySampleUs(int64_t latencyUs) {
        // Sample 1 in 8 packets under extreme fan-out to keep lock contention near zero
        const uint64_t n = m_latencySampleCounter.fetch_add(1, std::memory_order_relaxed);
        if ((n & 0x7u) != 0) {
            return;
        }
        std::unique_lock lock(m_histMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            hdr_record_value(m_intervalHistogram, latencyUs);
            hdr_record_value(m_cumulativeHistogram, latencyUs);
        }
    }

    void SyncDspEffectsWithControlApi() {
        const uint16_t dspTarget = m_dspSpeakers.load(std::memory_order_relaxed);
        const std::string bearer = !m_opts.controlKey.empty() ? m_opts.controlKey : m_opts.licenseKey;

        for (uint16_t i = 0; i < std::min<uint16_t>(120, m_opts.peerCount); ++i) {
            const uint16_t netId = m_peers[i].netId;
            if (i < dspTarget) {
                const char* preset = (i % 2 == 0) ? "radio" : "mask";
                std::string body = std::format(R"({{"netId":{},"preset":"{}"}})", netId, preset);
                SendControlHttpPost(m_opts.host, m_opts.controlPort, "/effects/player", bearer, body);
            } else if (m_lastConfiguredDspSpeakers > 0 && i < m_lastConfiguredDspSpeakers) {
                std::string body = std::format(R"({{"netId":{},"preset":"none"}})", netId);
                SendControlHttpPost(m_opts.host, m_opts.controlPort, "/effects/player", bearer, body);
            }
        }
        m_lastConfiguredDspSpeakers = dspTarget;
    }

    void AudioSenderLoop(std::stop_token st) {
        ENetAddress voiceServerAddr{};
        enet_address_set_host(&voiceServerAddr, m_opts.host.c_str());
        voiceServerAddr.port = m_opts.voicePort;

        std::array<uint8_t, protocol::kDefaultMtu> packetBuf{};
        size_t frameCursor = 0;

        // Instead of blasting all 200 speakers at the exact same microsecond every 10ms,
        // pace them across 10 sub-slices of 1ms each (just like real unsynchronized game clients).
        constexpr uint32_t kSubTicksPer10ms = 10;
        uint32_t subTick = 0;
        auto nextSubTick = std::chrono::steady_clock::now();

        while (!st.stop_requested() && m_running.load(std::memory_order_relaxed)) {
            if (m_dspSyncRequested.exchange(false, std::memory_order_acq_rel)) {
                SyncDspEffectsWithControlApi();
            }

            nextSubTick += std::chrono::milliseconds(1);
            const uint16_t speakers = std::min(m_activeSpeakers.load(std::memory_order_relaxed), m_opts.peerCount);
            const auto& opusFrame = m_sampleOpusFrames[frameCursor % m_sampleOpusFrames.size()];
            const uint32_t slice = subTick % kSubTicksPer10ms;
            if (++subTick % kSubTicksPer10ms == 0) {
                ++frameCursor;
            }

            for (uint16_t i = static_cast<uint16_t>(slice); i < speakers; i = static_cast<uint16_t>(i + kSubTicksPer10ms)) {
                VirtualPeer& vp = m_peers[i];
                if (!vp.authenticated.load(std::memory_order_relaxed) || !vp.host) {
                    continue;
                }

                const uint16_t peerSlotId = vp.assignedPeerId.load(std::memory_order_relaxed);
                if (peerSlotId == 0xFFFFu) {
                    continue;
                }

                const uint32_t seq = vp.sequenceNumber++;

                packetBuf.fill(0);
                std::memcpy(packetBuf.data(), &peerSlotId, sizeof(uint16_t));
                std::memcpy(packetBuf.data() + 2, vp.token.data(), 16);
                packetBuf[18] = protocol::kVoiceClientAudioPacketId;
                std::memcpy(packetBuf.data() + 19, &vp.netId, sizeof(uint16_t));
                std::memcpy(packetBuf.data() + 21, &seq, sizeof(uint32_t));
                packetBuf[25] = static_cast<uint8_t>(RoutingMode::Spatial3D);
                std::memcpy(packetBuf.data() + 26, opusFrame.data(), std::min<size_t>(opusFrame.size(), 1274));

                m_timestamps.RecordSend(vp.netId, seq, NowSteadyNs());

                ENetBuffer buf{};
                buf.data = packetBuf.data();
                buf.dataLength = packetBuf.size();
                if (enet_socket_send(vp.host->socket, &voiceServerAddr, &buf, 1) == static_cast<int>(packetBuf.size())) {
                    m_totalIngressPkts.fetch_add(1, std::memory_order_relaxed);
                }
            }

            std::this_thread::sleep_until(nextSubTick);
            const auto now = std::chrono::steady_clock::now();
            if (now - nextSubTick > std::chrono::milliseconds(20)) {
                nextSubTick = now;
            }
        }
    }

    BenchOptions m_opts;
    std::vector<VirtualPeer> m_peers;
    std::vector<std::vector<uint8_t>> m_sampleOpusFrames;
    SendTimestampTracker m_timestamps;

    ENetHost* m_gatewayHost = nullptr;
    std::jthread m_gatewayThread;
    std::vector<std::jthread> m_swarmThreads;
    std::jthread m_senderThread;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_gatewayConnected{false};
    std::atomic<bool> m_gatewayAuthenticated{false};
    std::atomic<bool> m_resyncRequested{false};
    std::atomic<bool> m_dspSyncRequested{false};
    std::atomic<bool> m_churnEnabled{false};

    std::atomic<BenchStage> m_currentStage{BenchStage::FastPath};
    std::atomic<uint16_t> m_activeSpeakers{200};
    std::atomic<uint16_t> m_clusterSize{16};
    std::atomic<uint16_t> m_dspSpeakers{0};
    uint16_t m_lastConfiguredDspSpeakers = 0;

    std::atomic<uint64_t> m_totalIngressPkts{0};
    std::atomic<uint64_t> m_totalEgressPkts{0};
    std::atomic<uint64_t> m_syncSnapshotsSent{0};
    std::atomic<uint64_t> m_latencySampleCounter{0};

    uint64_t m_prevIngressPkts = 0;
    uint64_t m_prevEgressPkts = 0;
    uint64_t m_peakIngressPps = 0;
    uint64_t m_peakEgressPps = 0;
    double m_peakIngressMbps = 0.0;
    double m_peakEgressMbps = 0.0;

    mutable std::mutex m_histMutex;
    hdr_histogram* m_intervalHistogram = nullptr;
    hdr_histogram* m_cumulativeHistogram = nullptr;
};

} // namespace
} // namespace eevs::bench

template <>
struct glz::meta<eevs::bench::JsonBenchmarkReport> {
    using T = eevs::bench::JsonBenchmarkReport;
    static constexpr auto value = object(
        "stage", &T::stage,
        "configuredPeers", &T::configuredPeers,
        "authenticatedPeers", &T::authenticatedPeers,
        "activeSpeakers", &T::activeSpeakers,
        "clusterSize", &T::clusterSize,
        "dspSpeakers", &T::dspSpeakers,
        "peakIngressPps", &T::peakIngressPps,
        "peakEgressPps", &T::peakEgressPps,
        "peakIngressMbps", &T::peakIngressMbps,
        "peakEgressMbps", &T::peakEgressMbps,
        "totalIngressPackets", &T::totalIngressPackets,
        "totalEgressPackets", &T::totalEgressPackets,
        "latencyP50Us", &T::latencyP50Us,
        "latencyP90Us", &T::latencyP90Us,
        "latencyP99Us", &T::latencyP99Us,
        "latencyP999Us", &T::latencyP999Us,
        "latencyMaxUs", &T::latencyMaxUs
    );
};

int main(int argc, char* argv[]) {
    using namespace eevs::bench;

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    BenchOptions opts{};
    CLI::App app{"EEVS Benchmark (eevs_bench)"};

    app.add_option("-c,--config", opts.configPath, "Path to eevs.cfg to auto-load ports & keys");
    app.add_option("--host", opts.host, "Target EEVS host IP (default: 127.0.0.1)");
    app.add_option("--gateway-port", opts.gatewayPort, "Mock FXServer Gateway port (default: 30122)");
    app.add_option("--voice-port", opts.voicePort, "Target EEVS UDP voice port (default: 30123)");
    app.add_option("--control-port", opts.controlPort, "Target EEVS HTTP Control API port (default: 30124)");
    app.add_option("--license-key", opts.licenseKey, "FXServer license key (for gateway SHA-256 auth)");
    app.add_option("-p,--peers", opts.peerCount, "Number of virtual game clients to spawn (1..4000)")
        ->check(CLI::Range(1, 4000));
    app.add_option("-s,--speakers", opts.activeSpeakers, "Concurrent active speakers sending 100 pkt/s Opus audio")
        ->check(CLI::Range(1, 4000));
    app.add_option("-k,--cluster-size", opts.clusterSize, "Players per proximity cluster (listeners = clusterSize - 1)")
        ->check(CLI::Range(2, 500));
    app.add_option("--dsp-speakers", opts.dspSpeakers, "Number of speakers with server-side DSP presets enabled")
        ->check(CLI::Range(0, 1000));
    app.add_option("-d,--duration", opts.durationSec, "Run duration in seconds (0 = interactive until quit)");
    app.add_option("--stage", opts.initialStage, "Initial benchmark stage preset (1=FastPath, 2=FanOut, 3=DSP, 4=Churn)")
        ->check(CLI::Range(1, 4));
    app.add_flag("--headless", opts.headless, "Run in headless console mode without FTXUI interactive TUI");
    app.add_flag("!--no-mock-gateway", opts.hostMockGateway, "Disable built-in Mock FXServer Gateway on port 30122");
    app.add_option("--json-out", opts.jsonReportPath, "Write final JSON benchmark report to file path");

    CLI11_PARSE(app, argc, argv);

    // Load defaults from config file if present and not overridden on CLI
    std::error_code ec;
    if (!opts.configPath.empty() && std::filesystem::exists(opts.configPath, ec)) {
        eevs::ServerConfig cfg{};
        if (eevs::ServerConfig::LoadFromFile(opts.configPath, cfg)) {
            if (app.count("--gateway-port") == 0) opts.gatewayPort = cfg.gatewayPort;
            if (app.count("--voice-port") == 0) opts.voicePort = cfg.voiceBindPort;
            if (app.count("--control-port") == 0 && cfg.controlBindPort != 0) opts.controlPort = cfg.controlBindPort;
            if (app.count("--license-key") == 0 && !cfg.licenseKey.empty()) opts.licenseKey = cfg.licenseKey;
            opts.controlKey = cfg.controlKey;
        }
    }

#if defined(_WIN32)
    timeBeginPeriod(1);
#endif
    if (enet_initialize() != 0) {
        std::cerr << "[eevs_bench] Failed to initialize ENet.\n";
        return 1;
    }

    SwarmController swarm(opts);
    if (app.count("--stage") > 0 && app.count("--speakers") == 0 && app.count("--cluster-size") == 0) {
        swarm.ApplyStagePreset(static_cast<BenchStage>(opts.initialStage));
    }

    if (!swarm.Start()) {
        enet_deinitialize();
        return 1;
    }

    const auto benchStart = std::chrono::steady_clock::now();
    auto lastSampleTime = benchStart;

    if (opts.headless) {
        std::cout << "================================================================================\n"
                  << " EEVS SYNTHETIC SWARM BENCHMARK (Headless Mode)\n"
                  << " Target Voice UDP: " << opts.host << ":" << opts.voicePort
                  << " | Mock Gateway: " << (opts.hostMockGateway ? std::format("{}:{}", opts.host, opts.gatewayPort) : "disabled") << "\n"
                  << " Virtual Peers   : " << opts.peerCount
                  << " | Speakers: " << swarm.GetActiveSpeakers()
                  << " | Cluster Size: " << swarm.GetClusterSize() << "\n"
                  << "================================================================================\n";

        while (!g_stopRequested.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - lastSampleTime).count();
            lastSampleTime = now;

            const TelemetrySnapshot snap = swarm.SampleMetrics(dt);
            const auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(now - benchStart).count();

            std::cout << std::format(
                "[{:02d}:{:02d}] Peers: {:4d}/{:4d} | In: {:6d} pkt/s ({:6.1f} Mbps) | Out: {:7d} pkt/s ({:7.1f} Mbps) | "
                "RTT p50: {:4d} us | p99: {:5d} us | p99.9: {:5d} us\n",
                elapsedSec / 60,
                elapsedSec % 60,
                snap.authenticatedPeers,
                opts.peerCount,
                snap.ingressPps,
                snap.ingressMbps,
                snap.egressPps,
                snap.egressMbps,
                snap.p50Us,
                snap.p99Us,
                snap.p999Us
            );

            if (opts.durationSec > 0 && elapsedSec >= static_cast<long long>(opts.durationSec)) {
                break;
            }
        }
    } else {
        using namespace ftxui;
        auto screen = ScreenInteractive::Fullscreen();

        TelemetrySnapshot latestSnap{};
        std::vector<int> ingressHistory(60, 0);
        std::vector<int> egressHistory(60, 0);
        long long elapsedSeconds = 0;

        auto renderer = Renderer([&] {
            const float peerRatio = opts.peerCount > 0
                ? static_cast<float>(latestSnap.authenticatedPeers) / static_cast<float>(opts.peerCount)
                : 0.0f;
            const double fanoutRatio = latestSnap.ingressPps > 0
                ? static_cast<double>(latestSnap.egressPps) / static_cast<double>(latestSnap.ingressPps)
                : 0.0;

            auto gwBadge = latestSnap.gatewayAuthenticated
                ? text(" AUTHENTICATED ") | bgcolor(Color::Green) | color(Color::Black) | bold
                : text(" WAITING FOR EEVS.EXE ") | bgcolor(Color::Yellow) | color(Color::Black) | bold;

            auto headerBox = vbox({
                hbox({
                    text(" EEVS BENCHMARK ") | bold | color(Color::Cyan),
                    filler(),
                    text(std::format("Target: {}:{} ", opts.host, opts.voicePort)) | color(Color::GrayLight),
                    gwBadge,
                    text(std::format(" {:02d}:{:02d} ", elapsedSeconds / 60, elapsedSeconds % 60)) | bold
                }),
                separator(),
                hbox({
                    text(" Profile: ") | color(Color::GrayLight),
                    text(StageName(swarm.GetCurrentStage())) | bold | color(Color::Yellow),
                    filler(),
                    text(std::format("Speakers: {}  |  Cluster: {} ({} listeners/speaker)  |  DSP Speakers: {} ",
                                     swarm.GetActiveSpeakers(),
                                     swarm.GetClusterSize(),
                                     std::max(0, static_cast<int>(swarm.GetClusterSize()) - 1),
                                     swarm.GetDspSpeakers()))
                })
            }) | border;

            auto swarmGaugeBox = vbox({
                hbox({
                    text(" Virtual Client Swarm Handshakes: "),
                    text(std::format("{} / {} Authenticated Peers", latestSnap.authenticatedPeers, opts.peerCount)) | bold | color(Color::GreenLight),
                    filler(),
                    text(std::format("LZ4 Sync Snapshots: {}", latestSnap.syncSnapshotsSent)) | color(Color::GrayLight)
                }),
                gauge(peerRatio) | color(Color::Green)
            }) | border;

            auto throughputBox = vbox({
                text(" LIVE VOICE PACKET THROUGHPUT") | bold | color(Color::Cyan),
                separator(),
                hbox({
                    vbox({
                        text(" INGRESS (ClientVoicePacket)") | color(Color::GrayLight),
                        text(std::format(" {:>8} pkt/s", latestSnap.ingressPps)) | bold | color(Color::GreenLight),
                        text(std::format(" {:>8.1f} Mbps", latestSnap.ingressMbps)) | color(Color::GrayLight),
                        text(std::format(" Total: {} pkts", latestSnap.totalIngressPackets)) | dim
                    }) | flex,
                    separator(),
                    vbox({
                        text(" EGRESS FAN-OUT (ServerVoicePacket)") | color(Color::GrayLight),
                        text(std::format(" {:>8} pkt/s", latestSnap.egressPps)) | bold | color(Color::CyanLight),
                        text(std::format(" {:>8.1f} Mbps", latestSnap.egressMbps)) | color(Color::GrayLight),
                        text(std::format(" Fan-Out: {:.1f}x  |  Total: {}", fanoutRatio, latestSnap.totalEgressPackets)) | dim
                    }) | flex
                }),
                separator(),
                text(" Egress Throughput Sparkline (kPkt/s)") | dim,
                graph([&](int width, int height) {
                    std::vector<int> out(width, 0);
                    const int maxVal = std::max(100, *std::ranges::max_element(egressHistory));
                    for (int x = 0; x < width; ++x) {
                        const size_t idx = (egressHistory.size() * static_cast<size_t>(x)) / static_cast<size_t>(std::max(1, width));
                        out[x] = (egressHistory[idx] * height) / maxVal;
                    }
                    return out;
                }) | color(Color::CyanLight) | size(HEIGHT, EQUAL, 6)
            }) | border | flex;

            auto latencyBar = [](std::string_view label, int64_t valUs, Color c) {
                const float norm = std::clamp(static_cast<float>(valUs) / 2000.0f, 0.01f, 1.0f);
                return hbox({
                    text(std::format(" {:<10}: {:>6} us ", label, valUs)) | size(WIDTH, EQUAL, 24),
                    gauge(norm) | color(c) | flex
                });
            };

            auto latencyBox = vbox({
                text(" HDR FORWARDING LATENCY (UDP RTT)") | bold | color(Color::MagentaLight),
                separator(),
                latencyBar("p50 (Med)", latestSnap.p50Us, Color::Green),
                latencyBar("p90", latestSnap.p90Us, Color::GreenLight),
                latencyBar("p99", latestSnap.p99Us, Color::Yellow),
                latencyBar("p99.9", latestSnap.p999Us, Color::Orange1),
                latencyBar("Max", latestSnap.maxUs, Color::RedLight),
                separator(),
                text(std::format(" Mean Latency: {:.1f} us", latestSnap.meanUs)) | color(Color::GrayLight)
            }) | border | flex;

            auto footerBox = hbox({
                text(" [1] ") | bold | color(Color::Yellow), text("Fast-Path  "),
                text(" [2] ") | bold | color(Color::Yellow), text("High Fan-Out  "),
                text(" [3] ") | bold | color(Color::Yellow), text("DSP Processing  "),
                text(" [4] ") | bold | color(Color::Yellow), text("Route Churn  "),
                text(" [+/-] ") | bold | color(Color::Cyan), text("Speakers  "),
                text(" [ [/] ] ") | bold | color(Color::Cyan), text("Cluster  "),
                filler(),
                text(" [Q] Quit ") | bold | color(Color::RedLight)
            }) | border;

            return vbox({
                headerBox,
                swarmGaugeBox,
                hbox({
                    throughputBox,
                    latencyBox
                }) | flex,
                footerBox
            });
        });

        auto component = CatchEvent(renderer, [&](Event event) {
            if (event == Event::Character('q') || event == Event::Character('Q') || event == Event::Escape) {
                screen.Exit();
                return true;
            }
            if (event == Event::Character('1')) {
                swarm.ApplyStagePreset(BenchStage::FastPath);
                return true;
            }
            if (event == Event::Character('2')) {
                swarm.ApplyStagePreset(BenchStage::HighFanout);
                return true;
            }
            if (event == Event::Character('3')) {
                swarm.ApplyStagePreset(BenchStage::DspProcessing);
                return true;
            }
            if (event == Event::Character('4')) {
                swarm.ApplyStagePreset(BenchStage::RouteChurn);
                return true;
            }
            if (event == Event::Character('+') || event == Event::Character('=')) {
                swarm.AdjustSpeakers(25);
                return true;
            }
            if (event == Event::Character('-')) {
                swarm.AdjustSpeakers(-25);
                return true;
            }
            if (event == Event::Character(']')) {
                swarm.AdjustClusterSize(5);
                return true;
            }
            if (event == Event::Character('[')) {
                swarm.AdjustClusterSize(-5);
                return true;
            }
            return false;
        });

        Loop loop(&screen, component);
        while (!loop.HasQuitted() && !g_stopRequested.load(std::memory_order_relaxed)) {
            loop.RunOnce();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - lastSampleTime).count();
            if (dt >= 0.25) {
                lastSampleTime = now;
                latestSnap = swarm.SampleMetrics(dt);
                elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - benchStart).count();

                ingressHistory.erase(ingressHistory.begin());
                ingressHistory.push_back(static_cast<int>(latestSnap.ingressPps / 1000));
                egressHistory.erase(egressHistory.begin());
                egressHistory.push_back(static_cast<int>(latestSnap.egressPps / 1000));

                screen.PostEvent(Event::Custom);

                if (opts.durationSec > 0 && elapsedSeconds >= static_cast<long long>(opts.durationSec)) {
                    screen.Exit();
                    break;
                }
            }
        }
    }

    const JsonBenchmarkReport finalReport = swarm.BuildFinalReport();
    swarm.Stop();
    enet_deinitialize();
#if defined(_WIN32)
    timeEndPeriod(1);
#endif

    if (!opts.jsonReportPath.empty()) {
        std::string jsonStr;
        if (!glz::write_json(finalReport, jsonStr)) {
            std::ofstream out(opts.jsonReportPath, std::ios::trunc);
            out << jsonStr << "\n";
            std::cout << "[eevs_bench] Wrote JSON benchmark report to '" << opts.jsonReportPath << "'\n";
        }
    }

    std::cout << std::format(
        "\n=== EEVS Benchmark Summary ({}) ===\n"
        "  Authenticated Peers : {} / {}\n"
        "  Peak Ingress        : {} pkt/s ({:.1f} Mbps)\n"
        "  Peak Egress Fan-Out : {} pkt/s ({:.1f} Mbps)\n"
        "  Total Packets       : {} in / {} out\n"
        "  Latency (us)        : p50={} us | p90={} us | p99={} us | p99.9={} us | max={} us\n",
        finalReport.stage,
        finalReport.authenticatedPeers,
        finalReport.configuredPeers,
        finalReport.peakIngressPps,
        finalReport.peakIngressMbps,
        finalReport.peakEgressPps,
        finalReport.peakEgressMbps,
        finalReport.totalIngressPackets,
        finalReport.totalEgressPackets,
        finalReport.latencyP50Us,
        finalReport.latencyP90Us,
        finalReport.latencyP99Us,
        finalReport.latencyP999Us,
        finalReport.latencyMaxUs
    );

    return 0;
}
