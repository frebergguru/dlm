/* dlm — IPC protocol version.
 *
 * Bump this whenever the daemon's wire protocol or download semantics change in
 * a way that an older running daemon would mishandle. Clients compare the
 * running daemon's reported protocol against this value on connect and restart
 * the daemon automatically on a mismatch, so a rebuilt binary never ends up
 * talking to a stale daemon.
 *
 *   1 - initial queue/IPC
 *   2 - added the `delegate` flag (yt-dlp streams/series via the daemon)
 *   3 - JDownloader-style queue: linkgrabber, packages, priorities,
 *       enable/disable, per-link & global autostart, force, move, clear_finished
 */
#ifndef DLM_PROTO_H
#define DLM_PROTO_H

#define DLM_PROTO_VERSION 3

#endif /* DLM_PROTO_H */
