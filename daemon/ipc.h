/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dlmd — JSON-lines request handling.
 *
 * The wire protocol is one JSON object per line over an AF_UNIX stream socket.
 *
 * Download list / queue:
 *   {"cmd":"add","url":...,"out":...,"connections":N,"delegate":bool}
 *   {"cmd":"list"}                          downloads + packages + settings
 *   {"cmd":"pause","id":N} {"cmd":"resume","id":N} {"cmd":"cancel","id":N}
 *   {"cmd":"rm","id":N[,"package":true]}    remove a link, or a whole package
 *   {"cmd":"priority","id":N,"level":-3..3[,"package":true]}
 *   {"cmd":"enable"|"disable","id":N[,"package":true]}
 *   {"cmd":"autostart","id":N,"on":bool[,"package":true]}   per-link auto switch
 *   {"cmd":"force","id":N[,"package":true]}  manual start now (ignore limits)
 *   {"cmd":"move","id":N,"dir":"up|down|top|bottom"[,"package":true]}
 *   {"cmd":"clear_finished"}                 drop all completed links
 *
 * Linkgrabber (staging):
 *   {"cmd":"grab","name":...,"folder":...,"links":[{"url","out","name",
 *                                  "size","connections","delegate"}...]}
 *   {"cmd":"confirm"[,"id":N,"package":bool,"start":bool]}  move to downloads
 *   {"cmd":"lg_remove"[,"id":N,"package":bool]} / {"cmd":"lg_clear"}
 *
 * Packages / settings:
 *   {"cmd":"pkg","id":N,"name":..,"folder":..,"comment":..,"priority":..,
 *                "collapsed":bool}
 *   {"cmd":"set","max_active":N,"max_speed":B,"autostart":bool}
 *   {"cmd":"subscribe"} {"cmd":"ping"} {"cmd":"shutdown"}
 *
 * Responses: {"ok":true,...} / {"ok":false,"error":"..."}
 * Events (to subscribers): {"event":"progress","packages":[...],"downloads":[...]}
 */
#ifndef DLM_IPC_H
#define DLM_IPC_H

#include "queue.h"

/* Handle one request line. Returns a malloc'd JSON response string (no trailing
 * newline) the caller must free. Sets *want_subscribe / *want_shutdown when the
 * corresponding command is received. */
char *dlm_ipc_handle(dlm_queue *q, const char *line, int *want_subscribe,
                     int *want_shutdown);

/* Build a {"event":"progress","downloads":[...]} line (malloc'd, no newline). */
char *dlm_ipc_progress_event(dlm_queue *q);

#endif /* DLM_IPC_H */
