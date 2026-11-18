#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "audio/audio_pipeline.h"
#include "audio/clip_manager.h"
#include "audio/opus_codec_pool.h"
#include "audio/packet_synthesizer.h"
#include "audio_io.h"
#include "core/crypto_utils.h"
#include "core/routing_matrix.h"
#include "core/server_config.h"
#include "core/session_registry.h"
#include "core/voice_engine.h"
#include "eevs/plugin.h"
#include "eevs/plugin_scope.h"
#include "network/enet.h"
#include "network/protocol.h"

#include <glaze/glaze.hpp>
#include <lz4.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

using namespace eevs;

namespace {

std::array<uint8_t, 32> MakeToken(uint8_t seedByte) {
    std::array<uint8_t, 32> tok{};
    for (size_t i = 0; i < tok.size(); ++i) {
        tok[i] = static_cast<uint8_t>(seedByte + i * 7u + 1u);
    }
    return tok;
}

std::vector<float> MakeSineWave(size_t sampleCount, uint32_t sampleRate, float freqHz = 440.0f, float amplitude = 0.5f) {
    std::vector<float> pcm(sampleCount);
    const float step = 2.0f * std::numbers::pi_v<float> * freqHz / static_cast<float>(sampleRate);
    for (size_t i = 0; i < sampleCount; ++i) {
        pcm[i] = amplitude * std::sin(step * static_cast<float>(i));
    }
    return pcm;
}

std::vector<uint8_t> BuildClientFastPathPacket(
    uint16_t peerId,
    const std::array<uint8_t, 32>& token,
    NetId senderNetId,
    uint32_t sequenceNumber,
    std::span<const uint8_t> opusFrame,
    size_t paddedPayloadSize = 1274
) {
    std::vector<uint8_t> pkt(26 + paddedPayloadSize, 0);
    std::memcpy(pkt.data() + 0, &peerId, sizeof(uint16_t));
    std::memcpy(pkt.data() + 2, token.data(), 16);
    pkt[18] = protocol::kVoiceClientAudioPacketId;
    std::memcpy(pkt.data() + 19, &senderNetId, sizeof(uint16_t));
    std::memcpy(pkt.data() + 21, &sequenceNumber, sizeof(uint32_t));
    pkt[25] = static_cast<uint8_t>(RoutingMode::Spatial3D);
    const size_t copyBytes = std::min(opusFrame.size(), paddedPayloadSize);
    std::memcpy(pkt.data() + 26, opusFrame.data(), copyBytes);
    return pkt;
}

} // namespace

// ============================================================================
// 1. CryptoUtils & ServerConfig Unit Tests
// ============================================================================

TEST_CASE("CryptoUtils: JOAAT, SHA-256, ConstantTimeEquals, and IsAllZeros32") {
    const std::string_view key = "cfxk_test_license_key_12345";
    const uint32_t h1 = crypto::ComputeJoaat(key);
    const uint32_t h2 = crypto::ComputeJoaat(reinterpret_cast<const uint8_t*>(key.data()), key.size());
    CHECK(h1 == h2);
    CHECK(h1 != 0u);

    const auto shaA = crypto::ComputeSha256(key);
    const auto shaB = crypto::ComputeSha256("cfxk_different_key");
    CHECK(shaA != shaB);
    CHECK_FALSE(crypto::IsAllZeros32(shaA.data()));

    std::array<uint8_t, 32> zeroBuf{};
    CHECK(crypto::IsAllZeros32(zeroBuf.data()));

    const std::string hex = crypto::ToHexString(shaA.data(), shaA.size());
    CHECK(hex.size() == 64);
    CHECK(crypto::ConstantTimeEquals(hex, hex));
    CHECK_FALSE(crypto::ConstantTimeEquals(hex, "0000"));
}

TEST_CASE("ServerConfig: Parses key-value pairs, comments, and strips sv_licenseKey from plugin properties") {
    const std::filesystem::path tmpCfg = std::filesystem::temp_directory_path() / "eevs_test_server.cfg";
    {
        std::ofstream out(tmpCfg, std::ios::trunc);
        out << "# Comment line\n"
            << "sv_licenseKey \"cfxk_secret_license_999\" // trailing comment\n"
            << "eevs_control_key \"ctrl_secret_123\"\n"
            << "voice_external_host \"0.0.0.0:30193\" \"127.0.0.1:30192\"\n"
            << "eevs_max_peers 2048\n"
            << "voice_inBitrate 64000\n"
            << "eevs_clip_cache_mb 32\n"
            << "eevs_audiocpp_host \"10.0.0.5\"\n";
    }

    ServerConfig cfg{};
    std::string err;
    REQUIRE(ServerConfig::LoadFromFile(tmpCfg, cfg, &err));
    CHECK(err.empty());
    CHECK(cfg.licenseKey == "cfxk_secret_license_999");
    CHECK(cfg.controlKey == "ctrl_secret_123");
    CHECK(cfg.voiceBindHost == "0.0.0.0");
    CHECK(cfg.voiceBindPort == 30193);
    CHECK(cfg.gatewayHost == "127.0.0.1");
    CHECK(cfg.gatewayPort == 30192);
    CHECK(cfg.maxPeers == 2048);
    CHECK(cfg.opusBitrate == 64000);
    CHECK(cfg.clipCacheMaxMb == 32);
    CHECK(cfg.properties["eevs_audiocpp_host"] == "10.0.0.5");
    CHECK_FALSE(cfg.properties.contains("sv_licenseKey"));

    std::error_code ec;
    std::filesystem::remove(tmpCfg, ec);
}

// ============================================================================
// 2. SessionRegistry Unit Tests
// ============================================================================

TEST_CASE("SessionRegistry: Registration, in-place ped NetID update, and JOAAT multiset collisions") {
    struct TestSessionListener final : ISessionListener {
        int registeredCount = 0;
        int unregisteredCount = 0;
        void OnClientRegistered(ClientId, NetId) noexcept override { ++registeredCount; }
        void OnClientUnregistered(ClientId, NetId) noexcept override { ++unregisteredCount; }
    };

    SessionRegistry registry;
    TestSessionListener listener;
    registry.RegisterSessionListener(&listener);

    const auto tok1 = MakeToken(0x10);
    const uint32_t joaat1 = crypto::ComputeJoaat(tok1.data(), tok1.size());

    registry.RegisterPlayerToken(1, 101, tok1);
    CHECK(listener.registeredCount == 1);
    CHECK(registry.GetRegisteredClientCount() == 1);
    CHECK(registry.IsTokenJoaatAllowed(joaat1));
    REQUIRE(registry.ResolveClientIdByNetId(101).has_value());
    CHECK(*registry.ResolveClientIdByNetId(101) == 1);

    // Respawn / ped NetID change with identical token updates in-place without unregistering
    registry.RegisterPlayerToken(1, 202, tok1);
    CHECK(listener.registeredCount == 1);
    CHECK(listener.unregisteredCount == 0);
    CHECK_FALSE(registry.ResolveClientIdByNetId(101).has_value());
    REQUIRE(registry.ResolveClientIdByNetId(202).has_value());
    CHECK(*registry.ResolveClientIdByNetId(202) == 1);

    // Second client with same JOAAT hash: unregistering client 1 keeps JOAAT allowed for client 2
    registry.RegisterPlayerToken(2, 303, tok1);
    CHECK(registry.GetRegisteredClientCount() == 2);
    registry.UnregisterPlayer(1);
    CHECK(listener.unregisteredCount == 1);
    CHECK(registry.IsTokenJoaatAllowed(joaat1));

    registry.UnregisterPlayer(2);
    CHECK_FALSE(registry.IsTokenJoaatAllowed(joaat1));
    CHECK(registry.GetRegisteredClientCount() == 0);
    registry.UnregisterSessionListener(&listener);
}

// ============================================================================
// 3. RoutingMatrix Unit Tests
// ============================================================================

TEST_CASE("RoutingMatrix: Sync bitfields, Force/Merge/Block overrides, synthetic routes, and IRouteProvider") {
    SessionRegistry sessions;
    sessions.RegisterPlayerToken(1, 1, MakeToken(1));
    sessions.RegisterPlayerToken(2, 2, MakeToken(2));
    sessions.RegisterPlayerToken(3, 3, MakeToken(3));
    sessions.RegisterPlayerToken(4, 4, MakeToken(4));

    RoutingMatrix matrix(&sessions);

    // 1. FXServer VoiceStreamingSync bitfield (0x2000 = Spatial3D, 0x4000 = NonSpatial)
    const uint16_t addSpatialTo2 = static_cast<uint16_t>(2u | 0x2000u);
    const uint16_t addBothTo3 = static_cast<uint16_t>(3u | 0x6000u);
    const std::array<uint16_t, 2> added = {addSpatialTo2, addBothTo3};
    matrix.ApplySyncDeltaBatch(1, added, {});

    auto routes = matrix.GetResolvedRoutes(1);
    REQUIRE(routes.size() == 2);
    CHECK(routes[0].targetClientId == 2);
    CHECK(routes[0].mode == RoutingMode::Spatial3D);
    CHECK(routes[1].targetClientId == 3);
    CHECK(routes[1].mode == RoutingMode::Both);

    // Removing only Spatial3D (0x2000) from client 3 leaves NonSpatial (0x4000) active
    const std::array<uint16_t, 1> removeSpatialFrom3 = {static_cast<uint16_t>(3u | 0x2000u)};
    matrix.ApplySyncDeltaBatch(1, {}, removeSpatialFrom3);
    routes = matrix.GetResolvedRoutes(1);
    REQUIRE(routes.size() == 2);
    CHECK(routes[1].targetClientId == 3);
    CHECK(routes[1].mode == RoutingMode::NonSpatial);

    // 2. Custom Overrides: MergeMode on client 2 (Spatial3D | NonSpatial -> Both), Block on client 3, ForceMode on client 4
    matrix.SetCustomRoute(1, 2, RoutingMode::NonSpatial, RouteOverrideAction::MergeMode);
    matrix.BlockRoute(1, 3);
    matrix.SetCustomRoute(1, 4, RoutingMode::NonSpatial, RouteOverrideAction::ForceMode);

    routes = matrix.GetResolvedRoutes(1);
    REQUIRE(routes.size() == 2);
    CHECK(routes[0].targetClientId == 2);
    CHECK(routes[0].mode == RoutingMode::Both);
    CHECK(routes[1].targetClientId == 4);
    CHECK(routes[1].mode == RoutingMode::NonSpatial);

    // 3. Synthetic NPC NetID routes & automatic listener cleanup on ClearClient
    matrix.SetSyntheticNetRoute(9000, 2, RoutingMode::Spatial3D);
    matrix.SetSyntheticNetRoute(9000, 4, RoutingMode::NonSpatial);
    CHECK(matrix.GetSyntheticNetRoutes(9000).size() == 2);

    matrix.ClearClient(4);
    CHECK(matrix.GetSyntheticNetRoutes(9000).size() == 1);
    CHECK(matrix.GetSyntheticNetRoutes(9000)[0].targetClientId == 2);

    // 4. IRouteProvider plugin hook
    struct AppendClient3Provider final : IRouteProvider {
        void ResolveSpeakerRoutes(
            ClientId speakerClientId,
            NetId /*speakerNetId*/,
            ResolvedRoute* routeBuf,
            size_t& inOutCount,
            size_t maxCapacity
        ) noexcept override {
            if (speakerClientId == 1 && inOutCount < maxCapacity) {
                routeBuf[inOutCount++] = ResolvedRoute{3, RoutingMode::Spatial3D};
            }
        }
    } provider;

    matrix.RegisterRouteProvider(&provider);
    routes = matrix.GetResolvedRoutes(1);
    CHECK(routes.size() == 2);
    matrix.UnregisterRouteProvider(&provider);
}

// ============================================================================
// 4. OpusCodecPool, ClipManager, & Disk Container Round-Trip Tests
// ============================================================================

TEST_CASE("OpusCodecPool & ClipManager: Encode/Decode, LRU eviction, pinning, and .wav/.eevs round-trip") {
    OpusCodecPool codecs;

    // 1. 10ms 48kHz Opus encode & decode round-trip
    const std::vector<float> sine10ms = MakeSineWave(OpusCodecPool::kFrameSamples10ms, OpusCodecPool::kSampleRate, 440.0f);
    std::vector<uint8_t> opusBuf(OpusCodecPool::kMaxOpusPacketBytes);
    const uint32_t encKey = OpusCodecPool::MakeEncoderKey(1, RoutingMode::Spatial3D);

    const int encodedBytes = codecs.EncodeFloat(encKey, sine10ms, opusBuf);
    REQUIRE(encodedBytes > 0);

    std::vector<float> decodedPcm(OpusCodecPool::kMaxFrameSamples, 0.0f);
    const int decodedSamples = codecs.DecodeFloat(
        1,
        std::span<const uint8_t>(opusBuf.data(), static_cast<size_t>(encodedBytes)),
        decodedPcm
    );
    CHECK(decodedSamples == static_cast<int>(OpusCodecPool::kFrameSamples10ms));

    // 2. ClipManager StorePcmClip (with 24kHz -> 48kHz resampling) and LRU eviction
    ClipManager clips(codecs);
    const std::vector<float> sine200ms24k = MakeSineWave(24000 / 5, 24000, 440.0f); // 200ms @ 24kHz = 20 frames
    uint32_t durMs = 0;
    const std::string idPinned = clips.StorePcmClip("pinned_clip", sine200ms24k, 24000, &durMs, true);
    CHECK(idPinned == "pinned_clip");
    CHECK(durMs == 200);
    CHECK(clips.HasClip("pinned_clip"));

    // Store two transient clips and set budget tight enough that inserting a third evicts the oldest transient clip
    clips.StorePcmClip("trans_1", sine200ms24k, 24000, nullptr, false);
    clips.StorePcmClip("trans_2", sine200ms24k, 24000, nullptr, false);
    const size_t twoClipBytes = clips.GetCurrentTransientBytes();
    REQUIRE(twoClipBytes > 0);

    // Touch trans_1 so trans_2 becomes the least-recently-used transient clip
    REQUIRE(clips.FindClip("trans_1") != nullptr);
    clips.SetMaxTransientBytes(twoClipBytes);

    clips.StorePcmClip("trans_3", sine200ms24k, 24000, nullptr, false);
    CHECK(clips.HasClip("pinned_clip")); // Pinned clip is never evicted
    CHECK(clips.HasClip("trans_1"));     // Recently accessed clip is kept
    CHECK_FALSE(clips.HasClip("trans_2")); // Oldest transient clip was evicted
    CHECK(clips.HasClip("trans_3"));

    // 3. .eevs and .wav file container round-trip
    const AudioClip* clipPtr = clips.FindClip("pinned_clip");
    REQUIRE(clipPtr != nullptr);

    const std::filesystem::path tmpEevs = std::filesystem::temp_directory_path() / "eevs_test_clip.eevs";
    const std::filesystem::path tmpWav = std::filesystem::temp_directory_path() / "eevs_test_clip.wav";

    REQUIRE(plugins::common::WriteEevsClipFile(tmpEevs, clipPtr->opusFrames));
    std::vector<std::vector<uint8_t>> loadedFrames;
    uint32_t loadedDurMs = 0;
    REQUIRE(plugins::common::ReadEevsClipFile(tmpEevs, loadedFrames, &loadedDurMs));
    CHECK(loadedDurMs == 200);
    CHECK(loadedFrames == clipPtr->opusFrames);

    REQUIRE(plugins::common::WriteMonoFloatToWavFile(tmpWav, sine200ms24k, 24000));
    std::vector<float> loadedWavPcm;
    uint32_t loadedWavRate = 0;
    REQUIRE(plugins::common::ReadWavFileToMonoFloat(tmpWav, loadedWavPcm, loadedWavRate));
    CHECK(loadedWavRate == 24000);
    CHECK(loadedWavPcm.size() == sine200ms24k.size());

    std::error_code ec;
    std::filesystem::remove(tmpEevs, ec);
    std::filesystem::remove(tmpWav, ec);
}

// ============================================================================
// 5. AudioPipeline, SIMD Zero-Strip, Filters, Taps, DSP, & VAD Unit Tests
// ============================================================================

TEST_CASE("AudioPipeline: AVX2 zero-padding strip, packet filters, taps, DSP stages, and VAD hangover") {
    SessionRegistry sessions;
    const auto tok1 = MakeToken(0x42);
    sessions.RegisterPlayerToken(1, 10, tok1);
    // Mark session 1 as authenticated so AudioPipeline processes its packets
    VoicePlayerSession* s1 = sessions.FindByClientId(1);
    REQUIRE(s1 != nullptr);
    s1->authenticated = true;

    RoutingMatrix routing(&sessions);
    VoiceUdpServer udpServer(sessions);
    OpusCodecPool codecs;
    PacketSynthesizer synth(sessions, routing, udpServer, codecs);
    AudioPipeline pipeline(sessions, routing, udpServer, synth, codecs);

    // Encode a valid 10ms Opus frame
    const std::vector<float> pcmIn = MakeSineWave(OpusCodecPool::kFrameSamples10ms, OpusCodecPool::kSampleRate, 300.0f);
    std::vector<uint8_t> opusRaw(OpusCodecPool::kMaxOpusPacketBytes);
    const int opusLen = codecs.EncodeFloat(OpusCodecPool::MakeEncoderKey(99, RoutingMode::Spatial3D), pcmIn, opusRaw);
    REQUIRE(opusLen > 0);
    const std::span<const uint8_t> opusSpan(opusRaw.data(), static_cast<size_t>(opusLen));

    struct TestTap final : IAudioTap {
        bool wantPcm = false;
        TapStage stage = TapStage::PreDsp;
        size_t opusCount = 0;
        size_t lastOpusSize = 0;
        size_t pcmCount = 0;
        float lastFirstSample = 0.0f;
        int talkStarts = 0;
        int talkStops = 0;

        [[nodiscard]] bool WantsDecodedPcm(ClientId) const noexcept override { return wantPcm; }
        [[nodiscard]] TapStage GetPcmTapStage() const noexcept override { return stage; }
        void OnOpusFrame(const SpeakerContext&, OpusFrameView opus) noexcept override {
            ++opusCount;
            lastOpusSize = opus.size;
        }
        void OnPcmFrame(const SpeakerContext&, PcmFrameView pcm) noexcept override {
            ++pcmCount;
            if (!pcm.Empty()) {
                lastFirstSample = pcm.AsSpan()[0];
            }
        }
        void OnTalkStateChanged(ClientId, NetId, bool isTalking) noexcept override {
            if (isTalking) ++talkStarts;
            else ++talkStops;
        }
    } tap;

    pipeline.RegisterAudioTap(&tap);

    // 1. Test AVX2 trailing-zero stripping across multiple padded buffer lengths (including 32-byte boundaries)
    // Ensure the last byte of our test Opus frame is non-zero
    if (opusRaw[static_cast<size_t>(opusLen) - 1] == 0x00) {
        opusRaw[static_cast<size_t>(opusLen) - 1] = 0x01;
    }
    const std::array<size_t, 6> padSizes = {
        static_cast<size_t>(opusLen),
        static_cast<size_t>(opusLen) + 1u,
        64u,
        65u,
        128u,
        1274u
    };
    for (size_t padSize : padSizes) {
        if (padSize < static_cast<size_t>(opusLen)) continue;
        auto pkt = BuildClientFastPathPacket(0, tok1, 10, 1, opusSpan, padSize);
        pipeline.ProcessIngressPacket(1, pkt.data(), pkt.size(), true);
        CHECK(tap.lastOpusSize == static_cast<size_t>(opusLen));
    }
    CHECK(pipeline.IsSpeakerTalking(1));
    CHECK(tap.talkStarts == 1);

    // 2. Test PacketFilterVerdict::DropSilent
    struct ConfigurableFilter final : IVoicePacketFilter {
        PacketFilterVerdict verdict = PacketFilterVerdict::Allow;
        [[nodiscard]] PacketFilterVerdict InspectIngressPacket(const VoiceIngressPacketView&) noexcept override {
            return verdict;
        }
    } filter;

    pipeline.RegisterPacketFilter(&filter);
    filter.verdict = PacketFilterVerdict::DropSilent;
    const size_t prevOpusCount = tap.opusCount;
    auto pkt = BuildClientFastPathPacket(0, tok1, 10, 2, opusSpan);
    pipeline.ProcessIngressPacket(1, pkt.data(), pkt.size(), true);
    CHECK(tap.opusCount == prevOpusCount); // Dropped before reaching tap

    // 3. Test PerSpeakerIngress DSP + PostIngressDsp Tap
    filter.verdict = PacketFilterVerdict::Allow;
    tap.wantPcm = true;
    tap.stage = TapStage::PostIngressDsp;

    struct ZeroingDsp final : IDspProcessor {
        bool active = true;
        [[nodiscard]] DspStage GetStage() const noexcept override { return DspStage::PerSpeakerIngress; }
        [[nodiscard]] bool ShouldProcess(ClientId, NetId, RoutingMode) const noexcept override { return active; }
        void ProcessPcm(const SpeakerContext&, RoutingMode, PcmFrameBuffer& pcm) noexcept override {
            std::ranges::fill(pcm.AsSpan(), 0.25f);
        }
    } dsp;

    // Add a dummy route so ingress DSP is evaluated
    routing.SetCustomRoute(1, 1, RoutingMode::Spatial3D);
    pipeline.RegisterDspProcessor(&dsp);

    pkt = BuildClientFastPathPacket(0, tok1, 10, 3, opusSpan);
    pipeline.ProcessIngressPacket(1, pkt.data(), pkt.size(), true);
    CHECK(tap.pcmCount == 1);
    CHECK(tap.lastFirstSample == doctest::Approx(0.25f));

    // 4. Test VAD Hangover expiration
    pipeline.SetVadHangoverMs(5);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    pipeline.TickVad();
    CHECK_FALSE(pipeline.IsSpeakerTalking(1));
    CHECK(tap.talkStops == 1);

    pipeline.UnregisterDspProcessor(&dsp);
    pipeline.UnregisterPacketFilter(&filter);
    pipeline.UnregisterAudioTap(&tap);
}

TEST_CASE("PacketSynthesizer: Monotonic sequence tracking and multi-burst playback") {
    SessionRegistry sessions;
    RoutingMatrix routing(&sessions);
    VoiceUdpServer udpServer(sessions);
    OpusCodecPool codecs;
    PacketSynthesizer synth(sessions, routing, udpServer, codecs);

    synth.ObserveSpeakerSequence(42, 500);
    CHECK(synth.NextSequenceNumber(42) == 501);
    CHECK(synth.NextSequenceNumber(42) == 502);

    synth.ClearSequence(42);
    CHECK(synth.NextSequenceNumber(42) == 0);

    ClipManager clips(codecs);
    const std::vector<float> pcm50ms = MakeSineWave(OpusCodecPool::kFrameSamples10ms * 5, OpusCodecPool::kSampleRate, 440.0f);
    clips.StorePcmClip("burst_a", pcm50ms, OpusCodecPool::kSampleRate);
    clips.StorePcmClip("burst_b", pcm50ms, OpusCodecPool::kSampleRate);

    const AudioClip* clipA = clips.FindClip("burst_a");
    const AudioClip* clipB = clips.FindClip("burst_b");
    REQUIRE(clipA != nullptr);
    REQUIRE(clipB != nullptr);

    const uint64_t idA = synth.PlayClip(*clipA, 100, RoutingMode::Spatial3D);
    const uint64_t idB = synth.PlayClip(*clipB, 100, RoutingMode::NonSpatial);
    CHECK(idA > 0);
    CHECK(idB > 0);
    CHECK(synth.IsPlaybackActive(idA));
    CHECK(synth.IsPlaybackActive(idB));

    CHECK(synth.StopPlayback(idA));
    CHECK_FALSE(synth.IsPlaybackActive(idA));
    CHECK(synth.StopAllPlaybacksForSender(100) == 1);
    CHECK_FALSE(synth.IsPlaybackActive(idB));
}

// ============================================================================
// 6. VoiceEngine, ControlApiServer, & ScopedPluginRegistrations Integration Test
// ============================================================================

TEST_CASE("VoiceEngine & Plugin SDK: Static plugin lifecycle, member-function endpoints, and telemetry") {
    struct TestSdkPlugin final : IPlugin {
        IHostContext* host = nullptr;
        ScopedPluginRegistrations scope;
        int pingCalls = 0;

        [[nodiscard]] const char* GetName() const noexcept override { return "test_sdk_plugin"; }
        [[nodiscard]] const char* GetVersion() const noexcept override { return "1.0.0"; }

        bool OnLoad(IHostContext* h) noexcept override {
            host = h;
            scope.Bind(h);
            return scope.AddEndpoint<&TestSdkPlugin::HandlePing>("/test/ping", this);
        }

        void OnUnload() noexcept override {
            scope.Reset();
            host = nullptr;
        }

        bool HandlePing(
            std::string_view /*method*/,
            std::string_view /*path*/,
            std::string_view /*body*/,
            char* responseBuf,
            size_t responseCap,
            uint16_t& outStatusCode
        ) {
            ++pingCalls;
            outStatusCode = 200;
            constexpr std::string_view kResp = R"({"ok":true,"pong":true})";
            const size_t n = std::min(kResp.size(), responseCap - 1);
            std::memcpy(responseBuf, kResp.data(), n);
            responseBuf[n] = '\0';
            return true;
        }
    };

    VoiceEngine engine;
    std::unique_ptr<IPlugin, PluginDeleter> plugin(new TestSdkPlugin());
    REQUIRE(engine.GetPluginManager().LoadStaticPlugin(std::move(plugin)));
    CHECK(engine.GetPluginManager().GetLoadedPluginCount() == 1);

    // Dispatch synchronous requests to built-in /health and plugin-registered /test/ping
    uint16_t status = 0;
    std::string body;
    REQUIRE(engine.GetControlApi().DispatchRequest("GET", "/health", "", status, body));
    CHECK(status == 200);
    CHECK(body.find("\"plugins\":1") != std::string::npos);

    status = 0;
    body.clear();
    REQUIRE(engine.GetControlApi().DispatchRequest("GET", "/test/ping", "", status, body));
    CHECK(status == 200);
    CHECK(body == R"({"ok":true,"pong":true})");

    // Unload plugins and verify ScopedPluginRegistrations automatically unregistered /test/ping
    engine.GetPluginManager().UnloadAll();
    CHECK(engine.GetPluginManager().GetLoadedPluginCount() == 0);

    status = 0;
    body.clear();
    REQUIRE(engine.GetControlApi().DispatchRequest("GET", "/test/ping", "", status, body));
    CHECK(status == 404);
}

TEST_CASE("Integration: End-to-end voice handshake, custom routing, and fast-path audio relay") {
    constexpr uint16_t kTestVoicePort = 30223;
    const auto tokSpeaker = MakeToken(0xA1);
    const auto tokListener = MakeToken(0xB2);

    VoiceEngine engine;
    REQUIRE(engine.StartVoiceServer("127.0.0.1", kTestVoicePort, 64));

    // Register speaker (ClientId 1, NetId 11) and listener (ClientId 2, NetId 22) and route 1 -> 2
    engine.GetSessions().RegisterPlayerToken(1, 11, tokSpeaker);
    engine.GetSessions().RegisterPlayerToken(2, 22, tokListener);
    engine.SetCustomRoute(1, 2, RoutingMode::Spatial3D);

    auto createAndAuthClient = [&](uint16_t clientId, const std::array<uint8_t, 32>& token, ENetPeer*& outPeer, uint16_t& outAssignedPeerId) -> ENetHost* {
        ENetHost* host = enet_host_create(nullptr, 1, protocol::kChannelCount, 0, 0);
        REQUIRE(host != nullptr);
        host->mtu = protocol::kDefaultMtu;

        ENetAddress addr{};
        enet_address_set_host(&addr, "127.0.0.1");
        addr.port = kTestVoicePort;

        const uint32_t joaat = crypto::ComputeJoaat(token.data(), token.size());
        outPeer = enet_host_connect(host, &addr, protocol::kChannelCount, joaat);
        REQUIRE(outPeer != nullptr);

        bool connected = false;
        bool handshakeDone = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

        while (std::chrono::steady_clock::now() < deadline && !handshakeDone) {
            engine.Poll(1);

            ENetEvent ev{};
            while (enet_host_service(host, &ev, 0) > 0) {
                if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                    connected = true;
                    outAssignedPeerId = ev.peer->outgoingPeerID;

                    protocol::VoiceAuthPacket auth{};
                    auth.marker = protocol::kControlPacketMarker;
                    auth.packetId = protocol::kVoiceAuthPacketId;
                    auth.clientIdOrZero = clientId;
                    std::memcpy(auth.secret, token.data(), 32);

                    ENetPacket* pkt = enet_packet_create(&auth, sizeof(auth), ENET_PACKET_FLAG_RELIABLE);
                    enet_peer_send(outPeer, 0, pkt);
                    enet_host_flush(host);
                } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    if (ev.packet->dataLength >= sizeof(protocol::VoiceHandshakeDonePacket) &&
                        ev.packet->data[0] == protocol::kControlPacketMarker) {
                        uint16_t ctrlId = 0;
                        std::memcpy(&ctrlId, ev.packet->data + 1, sizeof(uint16_t));
                        if (ctrlId == protocol::kVoiceHandshakeDonePacketId) {
                            handshakeDone = true;
                        }
                    }
                    enet_packet_destroy(ev.packet);
                }
            }
        }

        CHECK(connected);
        CHECK(handshakeDone);
        return host;
    };

    ENetPeer* speakerPeer = nullptr;
    ENetPeer* listenerPeer = nullptr;
    uint16_t speakerPeerId = 0xFFFFu;
    uint16_t listenerPeerId = 0xFFFFu;

    ENetHost* speakerHost = createAndAuthClient(1, tokSpeaker, speakerPeer, speakerPeerId);
    ENetHost* listenerHost = createAndAuthClient(2, tokListener, listenerPeer, listenerPeerId);

    // Encode a 10ms Opus frame and transmit it via raw fast-path UDP from speakerHost to EEVS
    const std::vector<float> pcm = MakeSineWave(OpusCodecPool::kFrameSamples10ms, OpusCodecPool::kSampleRate, 440.0f);
    std::vector<uint8_t> opusBuf(OpusCodecPool::kMaxOpusPacketBytes);
    const int opusBytes = engine.GetCodecPool().EncodeFloat(
        OpusCodecPool::MakeEncoderKey(1, RoutingMode::Spatial3D),
        pcm,
        opusBuf
    );
    REQUIRE(opusBytes > 0);
    if (opusBuf[static_cast<size_t>(opusBytes) - 1] == 0x00) {
        opusBuf[static_cast<size_t>(opusBytes) - 1] = 0x01;
    }

    const auto fastPathPkt = BuildClientFastPathPacket(
        speakerPeerId,
        tokSpeaker,
        11,
        77,
        std::span<const uint8_t>(opusBuf.data(), static_cast<size_t>(opusBytes))
    );

    ENetBuffer sendBuf{};
    sendBuf.data = const_cast<uint8_t*>(fastPathPkt.data());
    sendBuf.dataLength = fastPathPkt.size();
    CHECK(enet_socket_send(speakerHost->socket, &speakerPeer->address, &sendBuf, 1) == static_cast<int>(fastPathPkt.size()));

    // Poll EEVS and verify listenerHost receives the forwarded ServerVoicePacket
    bool receivedForwardedPacket = false;
    protocol::ServerVoicePacketHeader rxHdr{};
    std::vector<uint8_t> rxBuf(1500, 0);

    const auto rxDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < rxDeadline && !receivedForwardedPacket) {
        engine.Poll(1);

        ENetAddress rxAddr{};
        ENetBuffer recvBuf{};
        recvBuf.data = rxBuf.data();
        recvBuf.dataLength = rxBuf.size();
        const int rxLen = enet_socket_receive(listenerHost->socket, &rxAddr, &recvBuf, 1);
        if (rxLen >= static_cast<int>(sizeof(protocol::ServerVoicePacketHeader)) &&
            rxBuf[0] == protocol::kVoiceServerAudioPacketId) {
            std::memcpy(&rxHdr, rxBuf.data(), sizeof(rxHdr));
            receivedForwardedPacket = true;
        }
    }

    REQUIRE(receivedForwardedPacket);
    CHECK(rxHdr.packetId == protocol::kVoiceServerAudioPacketId);
    CHECK(rxHdr.senderNetId == 11);
    CHECK(rxHdr.sequenceNumber == 77);
    CHECK(rxHdr.routingMode == static_cast<uint8_t>(RoutingMode::Spatial3D));
    CHECK(std::memcmp(rxBuf.data() + sizeof(rxHdr), opusBuf.data(), static_cast<size_t>(opusBytes)) == 0);

    enet_host_destroy(speakerHost);
    enet_host_destroy(listenerHost);
    engine.Shutdown();
}
