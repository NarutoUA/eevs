# EEVS FiveM Server SDK (`scripts/eevs`)

The `eevs` resource is the core FiveM server-side SDK for EEVS. It maintains a persistent WebSocket connection (`bridge.js`) to the EEVS Control API (`/ws`) with automatic HTTP fallback (`PerformHttpRequest`), caches real-time Voice Activity Detection (VAD) and session state in memory, and provides cross-language exports (Lua, JS/TS, C#) plus drop-in FiveM Voice Channel & Proximity native wrappers.

---

## Installation

1. Copy or symlink `scripts/eevs` into your FiveM server's `resources/` directory (e.g. `resources/[voice]/eevs`).
2. Add `ensure eevs` to `server.cfg`.
3. To use the `EEVS` helper table and global `Eevs*` native wrappers in another Lua resource, include `@eevs/lib/eevs.lua` in your resource's `fxmanifest.lua`:

```lua
fx_version 'cerulean'
game 'gta5'

server_scripts {
    '@eevs/lib/eevs.lua',
    'server.lua'
}
```

---

## Dual-Mode Execution (Callback or Coroutine Await)

Every asynchronous `EEVS.*` method (and `exports.eevs:*` export) supports two calling conventions:
1. **Callback mode**: Pass `function(statusCode, data)` as the last argument.
2. **Coroutine await mode**: Omit the callback to yield via `Citizen.Await` and receive `data, statusCode` directly.

```lua
-- 1. Coroutine / Await style:
local res, status = EEVS.AddRoute(speakerSrc, adminSrc, '2D', 'force')

-- 2. Callback style:
EEVS.AddRoute(speakerSrc, adminSrc, '2D', 'force', function(status, res)
    print('Route added:', status, res.ok)
end)
```

---

## API Reference

### Synchronous State Queries (`O(1)`)
- `EEVS.IsPlayerTalking(netId) -> boolean`
- `EEVS.IsPlayerAuthenticated(netId) -> boolean`

### Asymmetric Routing & Custom Matrix
- `EEVS.AddRoute(fromNetId, toNetId, mode, action [, cb])`
- `EEVS.RemoveRoute(fromNetId, toNetId [, cb])`
- `EEVS.BlockRoute(fromNetId, toNetId [, cb])`
- `EEVS.ClearRoutes([fromNetId] [, cb])`
- `EEVS.ApplyBatchRoutes(routes [, cb])`
- `EEVS.SyncSpeakerRoutes(speakers [, cb])`

### Audio Injection & Clip Playback
- `EEVS.ListClips([cb])`
- `EEVS.PlayClip(clipId, senderNetId, mode [, listenerNetIds] [, cb])`
- `EEVS.StopClip(playbackIdOrFilterTable [, cb])`
- `EEVS.DeleteClip(clipId [, cb])`
- `EEVS.ClearClips([cb])`
- `EEVS.InjectOpusFrame(senderNetId, opusHex, mode [, listenerNetIds] [, cb])`
- `EEVS.QueueOpusBurst(senderNetId, framesHex, mode [, listenerNetIds] [, cb])`

### FiveM Enhanced Voice Channel & Proximity Wrappers
Drop-in wrappers around FiveM Enhanced's built-in voice natives with automatic support for **`mode == 2` (Custom Streaming)**, **one-way channel wiretapping**, and **channel-wide clip broadcasts**:
- `EevsCreateVoiceChannel(mode, maxDistance [, options]) -> channelId`
- `EevsDeleteVoiceChannel(channelId) -> boolean`
- `EevsAddPlayerToVoiceChannel(channelId, playerSrc) -> boolean`
- `EevsRemovePlayerFromVoiceChannel(channelId, playerSrc) -> boolean`
- `EevsSetPlayerVoiceChannelMuted(channelId, playerSrc, muted) -> boolean`
- `EevsSetPlayerVoiceChannelDeaf(channelId, playerSrc, deaf) -> boolean`
- `EevsDoesPlayerExistInVoiceChannel(channelId, playerSrc) -> boolean`
- `EevsIsPlayerVoiceChannelMuted(channelId, playerSrc) -> boolean`
- `EevsIsPlayerVoiceChannelDeaf(channelId, playerSrc) -> boolean`
- `EevsWiretapVoiceChannel(channelId, listenerSrc [, mode]) -> boolean`
- `EevsUnwiretapVoiceChannel(channelId, listenerSrc) -> boolean`
- `EevsPlayClipInVoiceChannel(channelId, clipId [, senderNetId] [, modeOverride] [, cb])`
- `EevsSetPlayerVoiceProximityRange(playerSrc, proximityRange) -> boolean`
- `EevsGetPlayerVoiceProximityRange(playerSrc) -> number, boolean`

---

## Real-Time Server Events

```lua
EEVS.OnTalkState(function(netId, isTalking, clientId)
    print(('Player %d talking: %s'):format(netId, tostring(isTalking)))
end)

EEVS.OnClientAuthenticated(function(netId, clientId)
    print(('Player %d authenticated on UDP voice'):format(netId))
end)

EEVS.OnEvent('stt.completed', function(netId, data)
    print(('STT transcript from %d: %s'):format(netId, data.text or ''))
end)
```
