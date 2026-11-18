using System;
using CitizenFX.Core;

namespace Eevs.Server
{
    /// <summary>
    /// Thin C# wrapper around the `eevs` FXServer resource exports and events.
    /// Drop this file into any FiveM C# server project.
    /// </summary>
    public sealed class EevsClient
    {
        private readonly dynamic _exp;

        public EevsClient(ExportDictionary exports)
        {
            _exp = exports["eevs"];
        }

        public bool IsPlayerTalking(int netId) => (bool)_exp.IsPlayerTalking(netId);

        public bool IsPlayerAuthenticated(int netId) => (bool)_exp.IsPlayerAuthenticated(netId);

        public void AddRoute(int fromNetId, int toNetId, string mode = "2D", string action = "force", Action<int, dynamic> cb = null)
            => _exp.AddRoute(fromNetId, toNetId, mode, action, cb);

        public void RemoveRoute(int fromNetId, int toNetId, Action<int, dynamic> cb = null)
            => _exp.RemoveRoute(fromNetId, toNetId, cb);

        public void BlockRoute(int fromNetId, int toNetId, Action<int, dynamic> cb = null)
            => _exp.BlockRoute(fromNetId, toNetId, cb);

        public void ClearRoutes(int? fromNetId = null, Action<int, dynamic> cb = null)
            => _exp.ClearRoutes(fromNetId, cb);

        public void PlayClip(string clipId, int senderNetId, string mode = "3D", int[] listenerNetIds = null, Action<int, dynamic> cb = null)
            => _exp.PlayClip(clipId, senderNetId, mode, listenerNetIds, cb);

        public void StopClip(long playbackId, Action<int, dynamic> cb = null)
            => _exp.StopClip(playbackId, cb);

        public void DeleteClip(string clipId, Action<int, dynamic> cb = null)
            => _exp.DeleteClip(clipId, cb);

        public void Request(string method, string path, object payload = null, Action<int, dynamic> cb = null)
            => _exp.Request(method, path, payload, cb);
    }
}
