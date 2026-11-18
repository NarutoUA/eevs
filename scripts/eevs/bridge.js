'use strict';

const fs = require('fs');
const path = require('path');
const http = require('http');
const crypto = require('crypto');

let cachedToken = null;
let wsSocket = null;
let wsConnected = false;
let reconnectTimer = null;
let nextReqId = 1;
const pendingWsRequests = new Map();

function stripQuotes(s) {
    if (!s) return '';
    s = String(s).trim();
    if (s.length >= 2 && ((s.startsWith('"') && s.endsWith('"')) || (s.startsWith("'") && s.endsWith("'")))) {
        return s.slice(1, -1);
    }
    return s;
}

function resolveAuthToken() {
    if (cachedToken) return cachedToken;

    const explicitKey = stripQuotes(GetConvar('eevs_control_key', ''));
    if (explicitKey && explicitKey !== 'changeme') {
        cachedToken = explicitKey;
        return cachedToken;
    }

    const licenseConvar = stripQuotes(GetConvar('sv_licenseKey', ''));
    if (licenseConvar && licenseConvar !== 'changeme') {
        cachedToken = licenseConvar;
        return cachedToken;
    }

    const candidates = [
        path.resolve(process.cwd(), 'server.cfg'),
        path.resolve(process.cwd(), '..', 'server.cfg'),
        path.resolve(process.cwd(), 'eevs.cfg')
    ];

    for (const cfgPath of candidates) {
        try {
            if (!fs.existsSync(cfgPath)) continue;
            const lines = fs.readFileSync(cfgPath, 'utf8').split(/\r?\n/);
            let foundControl = null;
            let foundLicense = null;

            for (const rawLine of lines) {
                const clean = rawLine.replace(/#.*$/, '').replace(/\/\/.*$/, '');
                const ctrlMatch = clean.match(/eevs_control_key\s+([^\s]+)/);
                if (ctrlMatch) {
                    const val = stripQuotes(ctrlMatch[1]);
                    if (val && val !== 'changeme') foundControl = val;
                }
                const licMatch = clean.match(/sv_licenseKey\s+([^\s]+)/);
                if (licMatch) {
                    const val = stripQuotes(licMatch[1]);
                    if (val && val !== 'changeme') foundLicense = val;
                }
            }

            if (foundControl) {
                cachedToken = foundControl;
                return cachedToken;
            }
            if (foundLicense) {
                cachedToken = foundLicense;
                return cachedToken;
            }
        } catch (_err) {
            // Ignore unreadable candidate paths
        }
    }

    return '';
}

function getControlUrl() {
    const ctrlHost = stripQuotes(GetConvar('eevs_control_host', ''));
    if (ctrlHost) {
        return /^https?:\/\//i.test(ctrlHost) ? ctrlHost : `http://${ctrlHost}`;
    }
    return stripQuotes(GetConvar('eevs_control_url', 'http://127.0.0.1:30124'));
}

function encodeWsTextFrame(str) {
    const payload = Buffer.from(str, 'utf8');
    const len = payload.length;
    const maskKey = crypto.randomBytes(4);

    let headerLen = 2;
    if (len >= 126 && len <= 65535) headerLen = 4;
    else if (len > 65535) headerLen = 10;

    const frame = Buffer.allocUnsafe(headerLen + 4 + len);
    frame[0] = 0x81; // FIN + Opcode 1 (TEXT)

    let offset = 2;
    if (len < 126) {
        frame[1] = 0x80 | len;
    } else if (len <= 65535) {
        frame[1] = 0x80 | 126;
        frame.writeUInt16BE(len, 2);
        offset = 4;
    } else {
        frame[1] = 0x80 | 127;
        frame.writeBigUInt64BE(BigInt(len), 2);
        offset = 10;
    }

    maskKey.copy(frame, offset);
    offset += 4;

    for (let i = 0; i < len; i++) {
        frame[offset + i] = payload[i] ^ maskKey[i & 3];
    }
    return frame;
}

function handleIncomingWsMessage(jsonText) {
    let msg;
    try {
        msg = JSON.parse(jsonText);
    } catch (_e) {
        return;
    }

    if (!msg || typeof msg !== 'object') return;

    if (msg.type === 'response' && msg.id) {
        const entry = pendingWsRequests.get(String(msg.id));
        if (entry) {
            clearTimeout(entry.timer);
            pendingWsRequests.delete(String(msg.id));
            entry.cb(msg.status || 200, msg.data || { ok: false, error: 'Empty WS response' });
        }
        return;
    }

    if (msg.type === 'event' && msg.topic) {
        emit('eevs:internalEvent', String(msg.topic), Number(msg.netId || 0), msg.data || {});
    }
}

function connectWebSocket() {
    if (wsSocket) return;

    let parsedUrl;
    try {
        parsedUrl = new URL(getControlUrl());
    } catch (_e) {
        parsedUrl = new URL('http://127.0.0.1:30124');
    }

    const token = resolveAuthToken();
    const wsKey = crypto.randomBytes(16).toString('base64');
    const req = http.request({
        hostname: parsedUrl.hostname || '127.0.0.1',
        port: parsedUrl.port || 30124,
        path: '/ws?key=' + encodeURIComponent(token),
        method: 'GET',
        headers: {
            'Connection': 'Upgrade',
            'Upgrade': 'websocket',
            'Sec-WebSocket-Version': '13',
            'Sec-WebSocket-Key': wsKey,
            'Authorization': 'Bearer ' + token,
            'X-EEVS-Key': token
        }
    });

    req.on('upgrade', (_res, socket, head) => {
        wsSocket = socket;
        wsConnected = true;
        socket.setNoDelay(true);

        let rxBuffer = head && head.length > 0 ? Buffer.from(head) : Buffer.alloc(0);

        const processFrames = () => {
            while (rxBuffer.length >= 2) {
                const b0 = rxBuffer[0];
                const b1 = rxBuffer[1];
                const opcode = b0 & 0x0f;
                const masked = (b1 & 0x80) !== 0;
                let payloadLen = b1 & 0x7f;
                let headerSize = 2;

                if (payloadLen === 126) {
                    if (rxBuffer.length < 4) return;
                    payloadLen = rxBuffer.readUInt16BE(2);
                    headerSize = 4;
                } else if (payloadLen === 127) {
                    if (rxBuffer.length < 10) return;
                    payloadLen = Number(rxBuffer.readBigUInt64BE(2));
                    headerSize = 10;
                }

                if (masked) headerSize += 4;
                if (rxBuffer.length < headerSize + payloadLen) return;

                let payload = rxBuffer.subarray(headerSize, headerSize + payloadLen);
                if (masked) {
                    const mask = rxBuffer.subarray(headerSize - 4, headerSize);
                    const unmasked = Buffer.allocUnsafe(payloadLen);
                    for (let i = 0; i < payloadLen; i++) {
                        unmasked[i] = payload[i] ^ mask[i & 3];
                    }
                    payload = unmasked;
                }

                rxBuffer = rxBuffer.subarray(headerSize + payloadLen);

                if (opcode === 0x01) {
                    handleIncomingWsMessage(payload.toString('utf8'));
                } else if (opcode === 0x08) {
                    socket.end();
                    return;
                } else if (opcode === 0x09) {
                    // Reply with Pong (0x8A)
                    const pong = Buffer.from([0x8a, 0x80, 0, 0, 0, 0]);
                    socket.write(pong);
                }
            }
        };

        if (rxBuffer.length > 0) processFrames();

        socket.on('data', (chunk) => {
            rxBuffer = rxBuffer.length === 0 ? chunk : Buffer.concat([rxBuffer, chunk]);
            processFrames();
        });

        const onDisconnect = () => {
            if (wsSocket === socket) {
                wsSocket = null;
                wsConnected = false;
                for (const [id, entry] of pendingWsRequests.entries()) {
                    clearTimeout(entry.timer);
                    pendingWsRequests.delete(id);
                    entry.cb(503, { ok: false, error: 'EEVS WebSocket disconnected' });
                }
                emit('eevs:internalConnectionState', false);
                scheduleReconnect();
            }
        };

        socket.on('close', onDisconnect);
        socket.on('error', onDisconnect);

        emit('eevs:internalConnectionState', true);
    });

    req.on('response', (res) => {
        res.resume();
        scheduleReconnect();
    });

    req.on('error', () => {
        scheduleReconnect();
    });

    req.end();
}

function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        connectWebSocket();
    }, 2500);
}

on('eevs:internalSendWs', (method, reqPath, payloadObj, cb) => {
    if (!wsConnected || !wsSocket) {
        cb(0, null);
        return;
    }

    const id = String(nextReqId++);
    const timer = setTimeout(() => {
        if (pendingWsRequests.has(id)) {
            pendingWsRequests.delete(id);
            cb(504, { ok: false, error: 'EEVS WebSocket request timed out' });
        }
    }, 60000);

    pendingWsRequests.set(id, { cb, timer });

    const envelope = JSON.stringify({
        id,
        method: String(method || 'POST').toUpperCase(),
        path: String(reqPath || '/health'),
        body: payloadObj || {}
    });

    try {
        wsSocket.write(encodeWsTextFrame(envelope));
    } catch (_e) {
        clearTimeout(timer);
        pendingWsRequests.delete(id);
        cb(0, null);
    }
});

setImmediate(connectWebSocket);
