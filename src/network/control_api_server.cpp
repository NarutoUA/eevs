#include "network/control_api_server.h"
#include "core/crypto_utils.h"
#include "core/logger.h"
#include "core/voice_engine.h"

#include <uwebsockets/App.h>

#include <charconv>
#include <cstring>
#include <format>
#include <future>
#include <glaze/glaze.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace eevs {

namespace {
    std::string_view TrimWhitespace(std::string_view sv) noexcept {
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
            sv.remove_prefix(1);
        }
        while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
            sv.remove_suffix(1);
        }
        return sv;
    }

    char ToLowerAscii(char c) noexcept {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    char ToUpperAscii(char c) noexcept {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }

    std::string ToUpperString(std::string_view sv) {
        std::string out(sv);
        for (char& c : out) {
            c = ToUpperAscii(c);
        }
        return out;
    }

    bool StartsWithCaseInsensitive(std::string_view str, std::string_view prefix) noexcept {
        if (str.size() < prefix.size()) {
            return false;
        }
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (ToLowerAscii(str[i]) != ToLowerAscii(prefix[i])) {
                return false;
            }
        }
        return true;
    }

    std::string_view ExtractBearerOrKeyToken(uWS::HttpRequest* req, std::string_view jsonBody = {}) noexcept {
        std::string_view authHeader = TrimWhitespace(req->getHeader("authorization"));
        if (!authHeader.empty()) {
            if (StartsWithCaseInsensitive(authHeader, "bearer ")) {
                return TrimWhitespace(authHeader.substr(7));
            }
            return authHeader;
        }

        std::string_view keyHeader = TrimWhitespace(req->getHeader("x-eevs-key"));
        if (!keyHeader.empty()) {
            return keyHeader;
        }

        std::string_view queryToken = TrimWhitespace(req->getQuery("token"));
        if (!queryToken.empty()) {
            return queryToken;
        }

        // Also allow "_token" field in JSON body if headers/query are stripped by proxy/runtime
        if (!jsonBody.empty()) {
            size_t pos = jsonBody.find("\"_token\"");
            if (pos != std::string_view::npos) {
                pos = jsonBody.find(':', pos + 8);
                if (pos != std::string_view::npos) {
                    size_t q1 = jsonBody.find('"', pos + 1);
                    size_t q2 = (q1 != std::string_view::npos) ? jsonBody.find('"', q1 + 1) : std::string_view::npos;
                    if (q1 != std::string_view::npos && q2 != std::string_view::npos) {
                        return TrimWhitespace(jsonBody.substr(q1 + 1, q2 - q1 - 1));
                    }
                }
            }
        }
        return {};
    }

    const char* FormatHttpStatusLine(uint16_t statusCode) noexcept {
        switch (statusCode) {
            case 200: return "200 OK";
            case 400: return "400 Bad Request";
            case 401: return "401 Unauthorized";
            case 404: return "404 Not Found";
            default:  return "500 Internal Server Error";
        }
    }

    std::vector<uint8_t> DecodeHexBytes(std::string_view hex) {
        std::vector<uint8_t> out;
        if (hex.size() % 2 != 0) {
            return out;
        }
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2) {
            uint8_t byteVal = 0;
            auto [ptr, ec] = std::from_chars(hex.data() + i, hex.data() + i + 2, byteVal, 16);
            if (ec != std::errc{}) {
                return {};
            }
            out.push_back(byteVal);
        }
        return out;
    }

    RoutingMode ParseRoutingModeString(std::string_view modeStr) noexcept {
        if (modeStr == "3D" || modeStr == "3d" || modeStr == "spatial" || modeStr == "1") {
            return RoutingMode::Spatial3D;
        }
        if (modeStr == "2D" || modeStr == "2d" || modeStr == "nonspatial" || modeStr == "radio" || modeStr == "2") {
            return RoutingMode::NonSpatial;
        }
        if (modeStr == "both" || modeStr == "Both" || modeStr == "BOTH" || modeStr == "3") {
            return RoutingMode::Both;
        }
        if (modeStr == "none" || modeStr == "0") {
            return RoutingMode::None;
        }
        return RoutingMode::NonSpatial;
    }

    BatchRouteOp ParseBatchOpString(std::string_view opStr) noexcept {
        if (opStr == "remove" || opStr == "delete" || opStr == "clear") {
            return BatchRouteOp::Remove;
        }
        if (opStr == "block") {
            return BatchRouteOp::Block;
        }
        if (opStr == "merge") {
            return BatchRouteOp::Merge;
        }
        if (opStr == "clear_speaker" || opStr == "clearSpeaker") {
            return BatchRouteOp::ClearSpeaker;
        }
        return BatchRouteOp::Add;
    }

    // Glaze-reflected JSON DTOs for Control API requests and responses
    constexpr glz::opts kRelaxedJsonOpts{.error_on_unknown_keys = false};
} // namespace

namespace detail {

struct ErrorResponseDto {
    bool ok = false;
    std::string error;
};

struct SimpleOkResponseDto {
    bool ok = true;
};

struct PipelineTelemetryDto {
    uint64_t ingressPackets = 0;
    uint64_t egressPackets = 0;
    size_t activeSpeakers = 0;
    uint64_t decodedFrames = 0;
    double opusDecodeAvgUs = 0.0;
    double opusDecodeMaxUs = 0.0;
    uint64_t dspFrames = 0;
    double dspProcessAvgUs = 0.0;
    double dspProcessMaxUs = 0.0;
    uint64_t encodedFrames = 0;
    double opusEncodeAvgUs = 0.0;
    double opusEncodeMaxUs = 0.0;
    double pipelineAvgUs = 0.0;
    double pipelineMaxUs = 0.0;
};

struct HealthResponseDto {
    bool ok = true;
    bool gatewayConnected = false;
    bool gatewayAuthenticated = false;
    bool voiceServerRunning = false;
    size_t sessions = 0;
    size_t plugins = 0;
    PipelineTelemetryDto telemetry{};
};

struct SessionItemDto {
    ClientId clientId = 0;
    NetId netId = 0;
    bool authenticated = false;
    bool talking = false;
};

struct SessionsResponseDto {
    std::vector<SessionItemDto> sessions;
};

struct RouteMutationReqDto {
    std::optional<uint16_t> fromNetId;
    std::optional<uint16_t> toNetId;
    std::string mode = "2D";
    std::string op;
    std::string action;
};

struct RouteBatchReqDto {
    std::vector<RouteMutationReqDto> routes;
};

struct RouteBatchResponseDto {
    bool ok = true;
    size_t total = 0;
    size_t applied = 0;
};

struct RouteSyncTargetDto {
    std::optional<uint16_t> toNetId;
    std::optional<uint16_t> netId;
    std::string mode = "2D";
};

struct RouteSyncSpeakerDto {
    std::optional<uint16_t> fromNetId;
    std::optional<uint16_t> speakerNetId;
    std::vector<RouteSyncTargetDto> targets;
};

struct RouteSyncReqDto {
    std::vector<RouteSyncSpeakerDto> speakers;
};

struct RouteSyncResponseDto {
    bool ok = true;
    size_t updatedSpeakers = 0;
};

struct AudioInjectReqDto {
    std::optional<uint16_t> senderNetId;
    std::string opusHex;
    std::string mode = "2D";
    std::vector<uint16_t> listenerNetIds;
};

struct AudioBurstReqDto {
    std::optional<uint16_t> senderNetId;
    std::vector<std::string> framesHex;
    std::string mode = "2D";
    std::vector<uint16_t> listenerNetIds;
};

struct ClipItemDto {
    std::string clipId;
    uint32_t durationMs = 0;
    size_t frames = 0;
    size_t bytes = 0;
    bool pinned = false;
};

struct ClipListResponseDto {
    size_t transientBytes = 0;
    size_t maxTransientBytes = 0;
    size_t totalBytes = 0;
    std::vector<ClipItemDto> clips;
};

struct ClipPlayReqDto {
    std::string clipId;
    std::optional<uint16_t> senderNetId;
    std::string mode = "2D";
    std::vector<uint16_t> listenerNetIds;
};

struct ClipStopReqDto {
    std::optional<uint64_t> playbackId;
    std::optional<uint16_t> senderNetId;
    std::string clipId;
};

struct ClipLoadReqDto {
    std::string file;
    std::string clipId;
    std::string format;
    bool pinned = true;
};

struct WsCommandEnvelopeDto {
    std::string id;
    std::string method = "POST";
    std::string path;
    glz::raw_json body;
};

} // namespace detail

using namespace detail;

struct ControlApiServer::UwsContext {
    struct WsClientData {
        bool authenticated = true;
    };

    struct PendingAsyncResponse {
        ControlApiServer* server = nullptr;
        uWS::HttpResponse<false>* res = nullptr;
        uWS::WebSocket<false, true, WsClientData>* ws = nullptr;
        std::string reqId;
        std::string method;
        std::string path;
        std::shared_ptr<std::atomic<bool>> aborted;
        bool isWebSocket = false;
    };

    uWS::Loop* loop = nullptr;
    uWS::App* app = nullptr;
    us_listen_socket_t* listenSocket = nullptr;
    std::promise<bool> startPromise;
};

ControlApiServer::ControlApiServer(VoiceEngine& engine)
    : m_engine(engine) {
}

ControlApiServer::~ControlApiServer() {
    Stop();
}

void EEVS_CALL ControlApiServer::OnWildcardHostEvent(
    void* userData,
    const char* topic,
    NetId subjectNetId,
    const char* jsonPayload
) {
    auto* self = static_cast<ControlApiServer*>(userData);
    if (!self || !self->IsRunning()) {
        return;
    }
    self->BroadcastWebSocketEvent(
        topic ? topic : "event",
        subjectNetId,
        jsonPayload ? jsonPayload : "{}"
    );
}

void ControlApiServer::BroadcastWebSocketEvent(
    std::string_view topic,
    NetId subjectNetId,
    std::string_view jsonPayload
) {
    if (!m_running.load(std::memory_order_relaxed) || !m_uws || !m_uws->loop) {
        return;
    }

    std::string frame = std::format(
        R"({{"type":"event","topic":"{}","netId":{},"data":{}}})",
        topic,
        subjectNetId,
        jsonPayload.empty() ? "{}" : jsonPayload
    );

    UwsContext* ctx = m_uws.get();
    ctx->loop->defer([ctx, msg = std::move(frame)]() {
        if (ctx->app) {
            ctx->app->publish("events", msg, uWS::OpCode::TEXT, false);
        }
    });
}

bool ControlApiServer::Start(std::string_view bindHost, uint16_t bindPort) {
    Stop();

    m_uws = std::make_unique<UwsContext>();
    std::future<bool> startedFuture = m_uws->startPromise.get_future();

    std::string hostStr(bindHost.empty() ? "127.0.0.1" : bindHost);
    m_ioThread = std::jthread(&ControlApiServer::RunEventLoopThread, this, hostStr, bindPort);

    if (!startedFuture.get()) {
        m_ioThread = std::jthread{};
        m_uws.reset();
        return false;
    }

    m_bindPort = bindPort;
    m_running.store(true, std::memory_order_relaxed);

    if (!m_subscribedToHostEvents) {
        m_engine.GetPluginManager().Subscribe("*", &ControlApiServer::OnWildcardHostEvent, this);
        m_subscribedToHostEvents = true;
    }

    EEVS_LOG_INFO("[ControlAPI] uWebSockets listening on http://{}:{} (WebSocket: ws://{}:{}/ws)",
                  hostStr, bindPort, hostStr, bindPort);
    return true;
}

void ControlApiServer::Stop() {
    if (m_subscribedToHostEvents) {
        m_engine.GetPluginManager().Unsubscribe("*", &ControlApiServer::OnWildcardHostEvent, this);
        m_subscribedToHostEvents = false;
    }

    m_running.store(false, std::memory_order_relaxed);

    if (m_uws && m_uws->loop) {
        UwsContext* ctx = m_uws.get();
        ctx->loop->defer([ctx]() {
            if (ctx->listenSocket) {
                us_listen_socket_close(0, ctx->listenSocket);
                ctx->listenSocket = nullptr;
            }
            if (ctx->app) {
                ctx->app->close();
            }
        });
    }

    m_ioThread = std::jthread{};
    m_uws.reset();
}

void ControlApiServer::RunEventLoopThread(std::string bindHost, uint16_t bindPort) {
    uWS::Loop* loop = uWS::Loop::get();
    m_uws->loop = loop;

    uWS::App app;
    m_uws->app = &app;

    // 1. WebSocket endpoint (`/ws`) for real-time bidirectional control & live event streaming
    uWS::App::WebSocketBehavior<UwsContext::WsClientData> wsBehavior{};
    wsBehavior.maxPayloadLength = 256 * 1024;
    wsBehavior.idleTimeout = 120;

    wsBehavior.upgrade = [this](auto* res, auto* req, auto* context) {
        if (m_requireAuth && !VerifyToken(ExtractBearerOrKeyToken(req))) {
            res->writeStatus("401 Unauthorized");
            res->writeHeader("Content-Type", "application/json");
            res->end(R"({"ok":false,"error":"Unauthorized: missing or invalid Bearer token"})");
            return;
        }

        res->template upgrade<UwsContext::WsClientData>(
            UwsContext::WsClientData{.authenticated = true},
            req->getHeader("sec-websocket-key"),
            req->getHeader("sec-websocket-protocol"),
            req->getHeader("sec-websocket-extensions"),
            context
        );
    };

    wsBehavior.open = [](auto* ws) {
        ws->subscribe("events");
    };

    wsBehavior.message = [this](auto* ws, std::string_view message, uWS::OpCode /*opCode*/) {
        WsCommandEnvelopeDto env{};
        if (glz::read<kRelaxedJsonOpts>(env, message) || env.path.empty()) {
            ws->send(R"({"ok":false,"error":"Expected JSON command with 'path'"})", uWS::OpCode::TEXT);
            return;
        }

        auto abortedFlag = std::make_shared<std::atomic<bool>>(false);
        auto* pending = new UwsContext::PendingAsyncResponse{
            .server = this,
            .res = nullptr,
            .ws = ws,
            .reqId = std::move(env.id),
            .method = ToUpperString(env.method),
            .path = env.path,
            .aborted = abortedFlag,
            .isWebSocket = true
        };

        std::string_view rawBody = env.body.str;

        m_engine.GetPluginManager().EnqueueControlRequest(
            pending->method.c_str(),
            pending->path.c_str(),
            rawBody.data(),
            rawBody.size(),
            [](void* userData, uint16_t statusCode, const char* jsonResponse, size_t responseLen) noexcept {
                auto* p = static_cast<UwsContext::PendingAsyncResponse*>(userData);
                ControlApiServer* srv = p->server;
                std::string respCopy(jsonResponse ? jsonResponse : "{}", responseLen);
                if (statusCode >= 400) {
                    EEVS_LOG_WARN("[ControlAPI] WS {} {} returned status {}: {}",
                                  p->method, p->path, statusCode, respCopy);
                }

                if (srv && srv->m_uws && srv->m_uws->loop) {
                    srv->m_uws->loop->defer([p, statusCode, body = std::move(respCopy)]() {
                        if (!p->aborted->load(std::memory_order_relaxed) && p->ws) {
                            std::string frame = p->reqId.empty()
                                ? std::format(R"({{"type":"response","status":{},"data":{}}})", statusCode, body)
                                : std::format(R"({{"type":"response","id":"{}","status":{},"data":{}}})", p->reqId, statusCode, body);
                            p->ws->send(frame, uWS::OpCode::TEXT);
                        }
                        delete p;
                    });
                } else {
                    delete p;
                }
            },
            pending
        );
    };

    app.ws<UwsContext::WsClientData>("/ws", std::move(wsBehavior));

    // 2. Catch-all HTTP/1.1 handler for built-in and plugin endpoints
    app.any("/*", [this](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
        std::string headerOrQueryToken(ExtractBearerOrKeyToken(req));
        std::string methodUpper = ToUpperString(req->getMethod());
        std::string pathStr(req->getUrl());

        auto abortedFlag = std::make_shared<std::atomic<bool>>(false);
        res->onAborted([abortedFlag]() {
            abortedFlag->store(true, std::memory_order_relaxed);
        });

        auto bodyAcc = std::make_shared<std::string>();
        res->onData([this, res, abortedFlag, bodyAcc,
                     token = std::move(headerOrQueryToken),
                     method = std::move(methodUpper),
                     path = std::move(pathStr)](
                        std::string_view chunk, bool isLast) mutable {
            if (!chunk.empty()) {
                bodyAcc->append(chunk.data(), chunk.size());
            }
            if (!isLast) {
                return;
            }

            std::string_view candidateToken = token;
            if (candidateToken.empty() && !bodyAcc->empty()) {
                size_t pos = bodyAcc->find("\"_token\"");
                if (pos != std::string::npos) {
                    pos = bodyAcc->find(':', pos + 8);
                    if (pos != std::string::npos) {
                        size_t q1 = bodyAcc->find('"', pos + 1);
                        size_t q2 = (q1 != std::string::npos) ? bodyAcc->find('"', q1 + 1) : std::string::npos;
                        if (q1 != std::string::npos && q2 != std::string::npos) {
                            candidateToken = TrimWhitespace(std::string_view(*bodyAcc).substr(q1 + 1, q2 - q1 - 1));
                        }
                    }
                }
            }

            if (m_requireAuth && !VerifyToken(candidateToken)) {
                res->cork([res]() {
                    res->writeStatus("401 Unauthorized");
                    res->writeHeader("Content-Type", "application/json");
                    res->end(R"({"ok":false,"error":"Unauthorized: missing or invalid Bearer token"})");
                });
                return;
            }

            auto* pending = new UwsContext::PendingAsyncResponse{
                .server = this,
                .res = res,
                .ws = nullptr,
                .reqId = {},
                .method = method,
                .path = path,
                .aborted = abortedFlag,
                .isWebSocket = false
            };

            m_engine.GetPluginManager().EnqueueControlRequest(
                method.c_str(),
                path.c_str(),
                bodyAcc->data(),
                bodyAcc->size(),
                [](void* userData, uint16_t statusCode, const char* jsonResponse, size_t responseLen) noexcept {
                    auto* p = static_cast<UwsContext::PendingAsyncResponse*>(userData);
                    ControlApiServer* srv = p->server;
                    std::string respCopy(jsonResponse ? jsonResponse : "{}", responseLen);
                    if (statusCode >= 400) {
                        EEVS_LOG_WARN("[ControlAPI] HTTP {} {} returned status {}: {}",
                                      p->method, p->path, statusCode, respCopy);
                    }

                    if (srv && srv->m_uws && srv->m_uws->loop) {
                        srv->m_uws->loop->defer([p, statusCode, body = std::move(respCopy)]() {
                            if (!p->aborted->load(std::memory_order_relaxed) && p->res) {
                                p->res->cork([res = p->res, statusCode, &body]() {
                                    res->writeStatus(FormatHttpStatusLine(statusCode));
                                    res->writeHeader("Content-Type", "application/json");
                                    res->end(body);
                                });
                            }
                            delete p;
                        });
                    } else {
                        delete p;
                    }
                },
                pending
            );
        });
    });

    app.listen(bindHost, static_cast<int>(bindPort), [this](us_listen_socket_t* sock) {
        m_uws->listenSocket = sock;
        m_uws->startPromise.set_value(sock != nullptr);
    });

    if (m_uws->listenSocket) {
        loop->run();
    }

    m_uws->app = nullptr;
    m_uws->loop = nullptr;
}

void ControlApiServer::ConfigureAuth(std::string_view licenseKey, std::string_view explicitControlKey) {
    std::string_view chosen = !explicitControlKey.empty() ? explicitControlKey : licenseKey;
    if (chosen.empty()) {
        m_requireAuth = false;
        m_primarySecret.clear();
        m_primarySecretSha256Hex.clear();
        return;
    }

    m_primarySecret = std::string(chosen);
    const auto sha256Bytes = crypto::ComputeSha256(chosen);
    m_primarySecretSha256Hex = crypto::ToHexString(sha256Bytes.data(), sha256Bytes.size());
    m_requireAuth = true;
}

bool ControlApiServer::VerifyToken(std::string_view candidateToken) const noexcept {
    if (!m_requireAuth) {
        return true;
    }
    candidateToken = TrimWhitespace(candidateToken);
    if (candidateToken.empty()) {
        return false;
    }
    return crypto::ConstantTimeEquals(candidateToken, m_primarySecret) ||
           crypto::ConstantTimeEquals(candidateToken, m_primarySecretSha256Hex);
}

bool ControlApiServer::RegisterEndpoint(
    std::string_view path,
    IHostContext::ControlEndpointHandler handler,
    void* userData
) {
    if (path.empty() || !handler) {
        return false;
    }
    m_customEndpoints[std::string(path)] = CustomEndpoint{
        .syncHandler = handler,
        .asyncHandler = nullptr,
        .userData = userData
    };
    return true;
}

bool ControlApiServer::RegisterAsyncEndpoint(
    std::string_view path,
    IHostContext::AsyncControlEndpointHandler handler,
    void* userData
) {
    if (path.empty() || !handler) {
        return false;
    }
    m_customEndpoints[std::string(path)] = CustomEndpoint{
        .syncHandler = nullptr,
        .asyncHandler = handler,
        .userData = userData
    };
    return true;
}

void ControlApiServer::UnregisterEndpoint(std::string_view path) {
    m_customEndpoints.erase(std::string(path));
}

void ControlApiServer::Poll() {
    // uWebSockets runs on its own dedicated event loop thread and marshals
    // control requests via PluginManager::EnqueueControlRequest + Loop::defer.
}

bool ControlApiServer::DispatchRequest(
    std::string_view method,
    std::string_view path,
    std::string_view body,
    uint16_t& outStatus,
    std::string& outJsonBody,
    IHostContext::AsyncControlRequestHandle requestHandle
) {
    // 1. Check plugin-registered endpoints first (both synchronous and deferred async)
    auto customIt = m_customEndpoints.find(std::string(path));
    if (customIt != m_customEndpoints.end()) {
        std::string methodStr(method);
        std::string pathStr(path);
        std::string bodyStr(body);

        if (customIt->second.asyncHandler && requestHandle != 0) {
            if (customIt->second.asyncHandler(
                    customIt->second.userData,
                    requestHandle,
                    methodStr.c_str(),
                    pathStr.c_str(),
                    bodyStr.c_str(),
                    bodyStr.size()
                )) {
                return false; // Deferred; completed asynchronously via CompleteAsyncRequest(requestHandle, ...)
            }
        }

        if (customIt->second.syncHandler) {
            char respBuf[4096]{};
            uint16_t status = 200;
            if (customIt->second.syncHandler(
                    customIt->second.userData,
                    methodStr.c_str(),
                    pathStr.c_str(),
                    bodyStr.c_str(),
                    bodyStr.size(),
                    respBuf,
                    sizeof(respBuf),
                    status
                )) {
                outStatus = status;
                outJsonBody = respBuf[0] != '\0' ? std::string(respBuf) : R"({"ok":true})";
                return true;
            }
        }
    }

    // 2. Built-in Host Control Endpoints
    if (method == "GET" && (path == "/health" || path == "/metrics")) {
        outStatus = 200;
        const auto snap = m_engine.GetAudioPipeline().GetTelemetrySnapshot();
        HealthResponseDto dto{
            .ok = true,
            .gatewayConnected = m_engine.IsConnected(),
            .gatewayAuthenticated = m_engine.IsAuthenticated(),
            .voiceServerRunning = m_engine.IsVoiceServerRunning(),
            .sessions = m_engine.GetSessions().GetRegisteredClientCount(),
            .plugins = m_engine.GetPluginManager().GetLoadedPluginCount(),
            .telemetry = PipelineTelemetryDto{
                .ingressPackets = snap.ingressPackets,
                .egressPackets = snap.egressPackets,
                .activeSpeakers = snap.activeSpeakers,
                .decodedFrames = snap.decodedFrames,
                .opusDecodeAvgUs = snap.opusDecodeAvgUs,
                .opusDecodeMaxUs = snap.opusDecodeMaxUs,
                .dspFrames = snap.dspFrames,
                .dspProcessAvgUs = snap.dspProcessAvgUs,
                .dspProcessMaxUs = snap.dspProcessMaxUs,
                .encodedFrames = snap.encodedFrames,
                .opusEncodeAvgUs = snap.opusEncodeAvgUs,
                .opusEncodeMaxUs = snap.opusEncodeMaxUs,
                .pipelineAvgUs = snap.pipelineAvgUs,
                .pipelineMaxUs = snap.pipelineMaxUs
            }
        };
        (void)glz::write_json(dto, outJsonBody);
        return true;
    }

    if (method == "POST" && path == "/metrics/reset") {
        outStatus = 200;
        m_engine.GetAudioPipeline().ResetTelemetry();
        (void)glz::write_json(SimpleOkResponseDto{}, outJsonBody);
        return true;
    }

    if (method == "GET" && path == "/sessions") {
        outStatus = 200;
        SessionsResponseDto dto{};
        auto activeIds = m_engine.GetSessions().GetActiveClientIds();
        dto.sessions.reserve(activeIds.size());
        for (ClientId clientId : activeIds) {
            if (const VoicePlayerSession* session = m_engine.GetSessions().FindByClientId(clientId)) {
                dto.sessions.push_back(SessionItemDto{
                    .clientId = clientId,
                    .netId = session->netId,
                    .authenticated = session->authenticated,
                    .talking = m_engine.GetAudioPipeline().IsSpeakerTalking(clientId)
                });
            }
        }
        (void)glz::write_json(dto, outJsonBody);
        return true;
    }

    if (method == "POST" && path == "/route/add") {
        RouteMutationReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, body) || !req.fromNetId || !req.toNetId) {
            outStatus = 400;
            (void)glz::write_json(ErrorResponseDto{false, "Missing or invalid fromNetId/toNetId"}, outJsonBody);
            return true;
        }

        RoutingMode mode = ParseRoutingModeString(req.mode);
        RouteOverrideAction action = RouteOverrideAction::ForceMode;
        if (req.action == "merge") {
            action = RouteOverrideAction::MergeMode;
        } else if (req.action == "block") {
            action = RouteOverrideAction::Block;
        }

        bool applied = m_engine.SetCustomRouteByNetId(*req.fromNetId, *req.toNetId, mode, action);
        if (!applied) {
            outStatus = 404;
            (void)glz::write_json(ErrorResponseDto{false, "Speaker or listener NetID is not registered in EEVS"}, outJsonBody);
            return true;
        }
        outStatus = 200;
        (void)glz::write_json(SimpleOkResponseDto{true}, outJsonBody);
        return true;
    }

    if (method == "POST" && path == "/route/remove") {
        RouteMutationReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, body) || !req.fromNetId || !req.toNetId) {
            outStatus = 400;
            (void)glz::write_json(ErrorResponseDto{false, "Missing or invalid fromNetId/toNetId"}, outJsonBody);
            return true;
        }

        bool cleared = m_engine.ClearCustomRouteByNetId(*req.fromNetId, *req.toNetId);
        if (!cleared) {
            outStatus = 404;
            (void)glz::write_json(ErrorResponseDto{false, "Listener NetID is not registered in EEVS"}, outJsonBody);
            return true;
        }
        outStatus = 200;
        (void)glz::write_json(SimpleOkResponseDto{true}, outJsonBody);
        return true;
    }

    if (method == "POST" && path == "/route/block") {
        RouteMutationReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, body) || !req.fromNetId || !req.toNetId) {
            outStatus = 400;
            (void)glz::write_json(ErrorResponseDto{false, "Missing or invalid fromNetId/toNetId"}, outJsonBody);
            return true;
        }

        bool applied = m_engine.SetCustomRouteByNetId(
            *req.fromNetId,
            *req.toNetId,
            RoutingMode::None,
            RouteOverrideAction::Block
        );
        if (!applied) {
            outStatus = 404;
            (void)glz::write_json(ErrorResponseDto{false, "Speaker or listener NetID is not registered in EEVS"}, outJsonBody);
            return true;
        }
        outStatus = 200;
        (void)glz::write_json(SimpleOkResponseDto{true}, outJsonBody);
        return true;
    }

    if (method == "POST" && path == "/route/clear") {
        RouteMutationReqDto req{};
        if (!body.empty()) {
            (void)glz::read<kRelaxedJsonOpts>(req, body);
        }

        if (req.fromNetId) {
            m_engine.ClearSpeakerCustomRoutesByNetId(*req.fromNetId);
            outStatus = 200;
            outJsonBody = R"({"ok":true,"scope":"speaker"})";
            return true;
        }

        m_engine.ClearAllCustomRoutes();
        outStatus = 200;
        outJsonBody = R"({"ok":true,"scope":"all"})";
        return true;
    }

    if (method == "POST" && path == "/route/batch") {
        RouteBatchReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, body)) {
            // Also support top-level JSON array `[{...}, {...}]`
            if (glz::read<kRelaxedJsonOpts>(req.routes, body)) {
                outStatus = 400;
                (void)glz::write_json(ErrorResponseDto{false, "Expected 'routes' array in JSON body"}, outJsonBody);
                return true;
            }
        }

        std::vector<BatchRouteCommand> commands;
        commands.reserve(req.routes.size());

        for (const RouteMutationReqDto& item : req.routes) {
            if (!item.fromNetId) {
                continue;
            }

            std::string_view opStr = !item.op.empty() ? std::string_view(item.op) : std::string_view(item.action);
            BatchRouteOp op = ParseBatchOpString(opStr);

            NetId toNetIdVal = 0;
            if (op != BatchRouteOp::ClearSpeaker) {
                if (!item.toNetId) {
                    continue;
                }
                toNetIdVal = *item.toNetId;
            }

            commands.push_back(BatchRouteCommand{
                .fromNetId = *item.fromNetId,
                .toNetId = toNetIdVal,
                .mode = ParseRoutingModeString(item.mode),
                .op = op
            });
        }

        const size_t applied = m_engine.ApplyBatchRoutes(commands);
        outStatus = 200;
        (void)glz::write_json(RouteBatchResponseDto{true, commands.size(), applied}, outJsonBody);
        return true;
    }

    if (method == "POST" && path == "/route/sync") {
        RouteSyncReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, body)) {
            if (glz::read<kRelaxedJsonOpts>(req.speakers, body)) {
                outStatus = 400;
                (void)glz::write_json(ErrorResponseDto{false, "Expected 'speakers' array in JSON body"}, outJsonBody);
                return true;
            }
        }

        size_t updatedSpeakers = 0;
        for (const RouteSyncSpeakerDto& spk : req.speakers) {
            auto fromNetId = spk.fromNetId ? spk.fromNetId : spk.speakerNetId;
            if (!fromNetId) {
                continue;
            }

            std::vector<NetId> listenerNetIds;
            std::vector<RoutingMode> modes;
            listenerNetIds.reserve(spk.targets.size());
            modes.reserve(spk.targets.size());

            for (const RouteSyncTargetDto& tgt : spk.targets) {
                auto toNetId = tgt.toNetId ? tgt.toNetId : tgt.netId;
                if (!toNetId) {
                    continue;
                }
                listenerNetIds.push_back(*toNetId);
                modes.push_back(ParseRoutingModeString(tgt.mode));
            }

            if (m_engine.SetSpeakerCustomTargetsByNetId(*fromNetId, listenerNetIds, modes)) {
                ++updatedSpeakers;
            }
        }

        outStatus = 200;
        (void)glz::write_json(RouteSyncResponseDto{true, updatedSpeakers}, outJsonBody);
        return true;
    }

    if (method == "POST" && path == "/audio/inject") {
        AudioInjectReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, body) || !req.senderNetId || req.opusHex.empty()) {
            outStatus = 400;
            (void)glz::write_json(ErrorResponseDto{false, "Missing senderNetId or opusHex"}, outJsonBody);
            return true;
        }

        std::vector<uint8_t> opusBytes = DecodeHexBytes(req.opusHex);
        if (opusBytes.empty()) {
            outStatus = 400;
            (void)glz::write_json(ErrorResponseDto{false, "Invalid opusHex payload"}, outJsonBody);
            return true;
        }

        std::vector<ClientId> explicitListeners;
        for (NetId listenerNetId : req.listenerNetIds) {
            if (auto cid = m_engine.GetSessions().ResolveClientIdByNetId(listenerNetId)) {
                explicitListeners.push_back(*cid);
            }
        }

        RoutingMode mode = ParseRoutingModeString(req.mode);
        size_t sent = m_engine.InjectOpusFrame(*req.senderNetId, mode, opusBytes, explicitListeners);
        outStatus = 200;
        outJsonBody = std::format(R"({{"ok":true,"sentPeers":{}}})", sent);
        return true;
    }

    if (method == "POST" && path == "/audio/burst") {
        AudioBurstReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, body) || !req.senderNetId || req.framesHex.empty()) {
            outStatus = 400;
            (void)glz::write_json(ErrorResponseDto{false, "Missing senderNetId or framesHex array"}, outJsonBody);
            return true;
        }

        std::vector<std::vector<uint8_t>> opusFrames;
        opusFrames.reserve(req.framesHex.size());
        for (const std::string& hexStr : req.framesHex) {
            std::vector<uint8_t> decoded = DecodeHexBytes(hexStr);
            if (!decoded.empty()) {
                opusFrames.push_back(std::move(decoded));
            }
        }

        if (opusFrames.empty()) {
            outStatus = 400;
            (void)glz::write_json(ErrorResponseDto{false, "No valid Opus frames in framesHex"}, outJsonBody);
            return true;
        }

        std::vector<ClientId> explicitListeners;
        for (NetId listenerNetId : req.listenerNetIds) {
            if (auto cid = m_engine.GetSessions().ResolveClientIdByNetId(listenerNetId)) {
                explicitListeners.push_back(*cid);
            }
        }

        const size_t queuedCount = opusFrames.size();
        RoutingMode mode = ParseRoutingModeString(req.mode);
        const uint64_t playbackId = m_engine.GetSynthesizer().QueueOpusBurst(
            *req.senderNetId,
            mode,
            std::move(opusFrames),
            std::move(explicitListeners)
        );

        outStatus = 200;
        outJsonBody = std::format(
            R"({{"ok":true,"playbackId":{},"queuedFrames":{}}})",
            playbackId,
            queuedCount
        );
        return true;
    }

    if (method == "GET" && path == "/clip/list") {
        outStatus = 200;
        ClipListResponseDto dto{
            .transientBytes = m_engine.GetClipManager().GetCurrentTransientBytes(),
            .maxTransientBytes = m_engine.GetClipManager().GetMaxTransientBytes(),
            .totalBytes = m_engine.GetClipManager().GetTotalClipBytes(),
            .clips = {}
        };
        for (const auto& [clipId, clip] : m_engine.GetClipManager().GetAllClips()) {
            dto.clips.push_back(ClipItemDto{
                .clipId = clipId,
                .durationMs = clip.durationMs,
                .frames = clip.opusFrames.size(),
                .bytes = clip.byteSize,
                .pinned = clip.pinned
            });
        }
        (void)glz::write_json(dto, outJsonBody);
        return true;
    }

    if (method == "POST" && path == "/clip/play") {
        ClipPlayReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, body) || req.clipId.empty() || !req.senderNetId) {
            outStatus = 400;
            (void)glz::write_json(ErrorResponseDto{false, "Missing clipId or senderNetId"}, outJsonBody);
            return true;
        }

        const AudioClip* clip = m_engine.GetClipManager().FindClip(req.clipId);
        if (!clip) {
            outStatus = 404;
            (void)glz::write_json(ErrorResponseDto{false, "Clip not found"}, outJsonBody);
            return true;
        }

        std::vector<ClientId> explicitListeners;
        for (NetId listenerNetId : req.listenerNetIds) {
            if (auto cid = m_engine.GetSessions().ResolveClientIdByNetId(listenerNetId)) {
                explicitListeners.push_back(*cid);
            }
        }

        RoutingMode mode = ParseRoutingModeString(req.mode);
        const uint64_t playbackId = m_engine.GetSynthesizer().PlayClip(
            *clip,
            *req.senderNetId,
            mode,
            std::move(explicitListeners)
        );

        outStatus = playbackId != 0 ? 200 : 500;
        outJsonBody = std::format(
            R"({{"ok":{},"playbackId":{},"durationMs":{}}})",
            playbackId != 0 ? "true" : "false",
            playbackId,
            clip->durationMs
        );
        return true;
    }

    if (method == "POST" && path == "/clip/stop") {
        ClipStopReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, body)) {
            outStatus = 400;
            (void)glz::write_json(ErrorResponseDto{false, "Invalid JSON body"}, outJsonBody);
            return true;
        }

        if (req.playbackId) {
            bool stopped = m_engine.GetSynthesizer().StopPlayback(*req.playbackId);
            outStatus = 200;
            outJsonBody = std::format(R"({{"ok":true,"stopped":{}}})", stopped ? "true" : "false");
            return true;
        }
        if (req.senderNetId) {
            size_t count = m_engine.GetSynthesizer().StopAllPlaybacksForSender(*req.senderNetId);
            outStatus = 200;
            outJsonBody = std::format(R"({{"ok":true,"stoppedCount":{}}})", count);
            return true;
        }
        outStatus = 400;
        (void)glz::write_json(ErrorResponseDto{false, "Missing playbackId or senderNetId"}, outJsonBody);
        return true;
    }

    if (method == "POST" && path == "/clip/delete") {
        ClipStopReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, body) || req.clipId.empty()) {
            outStatus = 400;
            (void)glz::write_json(ErrorResponseDto{false, "Missing clipId"}, outJsonBody);
            return true;
        }
        bool deleted = m_engine.GetClipManager().DeleteClip(req.clipId);
        outStatus = deleted ? 200 : 404;
        (void)glz::write_json(SimpleOkResponseDto{deleted}, outJsonBody);
        return true;
    }

    if (method == "POST" && path == "/clip/clear") {
        const size_t removedCount = m_engine.GetClipManager().GetAllClips().size();
        m_engine.GetClipManager().ClearAllClips();
        outStatus = 200;
        outJsonBody = std::format(R"({{"ok":true,"cleared":{}}})", removedCount);
        return true;
    }

    outStatus = 404;
    (void)glz::write_json(ErrorResponseDto{false, "Unknown endpoint"}, outJsonBody);
    return true;
}

} // namespace eevs
