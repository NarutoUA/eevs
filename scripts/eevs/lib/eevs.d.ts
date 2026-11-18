/**
 * TypeScript / JavaScript definitions for `exports.eevs` in FiveM Server resources.
 */

export type EevsRoutingMode = '2D' | '3D' | 'BOTH' | 'NONE';
export type EevsRouteAction = 'force' | 'merge' | 'block';
export type EevsCallback<T = any> = (status: number, data: T) => void;

export interface EevsBatchRouteItem {
    op?: 'add' | 'merge' | 'block' | 'remove' | 'clear_speaker';
    fromNetId: number;
    toNetId?: number;
    mode?: EevsRoutingMode;
}

export interface EevsSpeakerSyncEntry {
    fromNetId: number;
    targets: Array<{ toNetId: number; mode?: EevsRoutingMode }>;
}

export interface EevsExports {
    Request<T = any>(method: string, path: string, payload?: Record<string, any>, cb?: EevsCallback<T>): T;

    GetHealth(cb?: EevsCallback): any;
    GetSessions(cb?: EevsCallback): any;

    IsPlayerTalking(netId: number): boolean;
    IsPlayerAuthenticated(netId: number): boolean;

    AddRoute(fromNetId: number, toNetId: number, mode?: EevsRoutingMode, action?: EevsRouteAction | EevsCallback, cb?: EevsCallback): any;
    RemoveRoute(fromNetId: number, toNetId: number, cb?: EevsCallback): any;
    BlockRoute(fromNetId: number, toNetId: number, cb?: EevsCallback): any;
    ClearRoutes(fromNetId?: number | EevsCallback, cb?: EevsCallback): any;
    ApplyBatchRoutes(routes: EevsBatchRouteItem[], cb?: EevsCallback): any;
    SyncSpeakerRoutes(speakers: EevsSpeakerSyncEntry[], cb?: EevsCallback): any;

    InjectOpusFrame(senderNetId: number, opusHex: string, mode?: EevsRoutingMode, listenerNetIds?: number[], cb?: EevsCallback): any;
    QueueOpusBurst(senderNetId: number, framesHex: string[], mode?: EevsRoutingMode, listenerNetIds?: number[], cb?: EevsCallback): any;

    ListClips(cb?: EevsCallback): any;
    PlayClip(clipId: string, senderNetId: number, mode?: EevsRoutingMode, listenerNetIds?: number[] | EevsCallback, cb?: EevsCallback): any;
    StopClip(playbackIdOrOpts: number | { playbackId?: number; senderNetId?: number }, cb?: EevsCallback): any;
    DeleteClip(clipId: string, cb?: EevsCallback): any;
}
