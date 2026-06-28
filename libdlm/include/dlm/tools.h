/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — auto-managed external tools (yt-dlp, ffmpeg/ffprobe).
 *
 * dlm shells out to yt-dlp for stream extraction/download and to ffmpeg for
 * muxing. Rather than require the user to install these, the library can fetch
 * them into a per-user tools directory and keep yt-dlp up to date. A binary that
 * already exists (managed or on PATH) is never re-downloaded.
 */
#ifndef DLM_TOOLS_H
#define DLM_TOOLS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve a usable path/command for a tool ("yt-dlp", "ffmpeg", "ffprobe").
 * Resolution order: a managed binary in the tools dir, then a binary on PATH
 * (returned as the bare name so the spawner's PATH search finds it), else the
 * managed path (which a subsequent ensure may populate). Never returns NULL; the
 * returned string is owned by the library — do not free. */
const char *dlm_tool_path(const char *name);

/* Directory holding managed ffmpeg/ffprobe, suitable for yt-dlp's
 * --ffmpeg-location. Returns NULL when ffmpeg is not managed (yt-dlp should then
 * find ffmpeg on PATH itself). */
const char *dlm_tool_ffmpeg_dir(void);

/* Ensure required tools are present, downloading any that are missing. With
 * allow_network==0 this only resolves existing tools and never hits the network.
 * Honors the auto-tools switch. Non-fatal: missing tools degrade to PATH/today's
 * behavior rather than failing. Returns 0. */
int dlm_tools_ensure_ready(int allow_network);

/* If force, or if it has been >= 7 days since the last check, query the latest
 * yt-dlp release and re-download when newer. No-op when auto-tools is disabled.
 * Returns 0. */
int dlm_tools_check_updates(int force);

/* Auto-download/update switch. Disabled by DLM_NO_AUTO_TOOLS=1 or a persisted
 * flag; enabled by default. */
int dlm_tools_auto_enabled(void);
void dlm_tools_set_auto_enabled(int on);

/* Build a JSON object describing tool status (versions, managed flags, auto).
 * Stores a malloc'd string in *out (caller frees). Returns 0 on success. */
int dlm_tools_status_json(char **out);

#ifdef __cplusplus
}
#endif

#endif /* DLM_TOOLS_H */
