#ifndef STRATUM_RECV_CTX_H
#define STRATUM_RECV_CTX_H

#include <stddef.h>
#include "esp_transport.h"

// Reentrant JSON-RPC line receive using a caller-owned buffer (bufp/sizep start as
// {NULL, 0}). Lets a second concurrent Stratum connection (dual-mining Pool B) receive
// lines without sharing the global accumulator in the upstream stratum_api.c.
//
// Deliberately lives in the dual_pool component (not stratum_api.c) so the dual-pool
// changes stay off the high-churn upstream stratum file — see MAINTENANCE.md.
//
// Returns a malloc'd line (caller frees) or NULL on error (buffer freed + reset to NULL).
char *STRATUM_V1_receive_jsonrpc_line_ctx(esp_transport_handle_t transport, char **bufp, size_t *sizep);

typedef enum {
    STRATUM_RX_LINE = 0,  // a complete line came out; the caller frees it
    STRATUM_RX_IDLE,      // nothing complete yet, the connection is fine
    STRATUM_RX_ERROR      // the connection is gone; the buffer has been freed
} stratum_rx_t;

/*
 * The same receive, but it gives up after `timeout_ms` instead of waiting for
 * a whole line.
 *
 * This is what lets one task hold several Stratum sessions at once. The
 * blocking form spins inside esp_transport_read until a newline arrives, so a
 * task sitting in it for pool B cannot look at pool C -- fine while there were
 * only ever two pools and pool B had a task to itself, useless with four.
 *
 * Telling "nothing yet" apart from "the connection died" is the point of the
 * return code. The blocking form has only NULL for both, and a caller that
 * treats a quiet pool as a dead one reconnects every few seconds forever.
 *
 * Partial input stays in the caller's accumulator between calls, so a line
 * split across several visits is assembled across them.
 */
stratum_rx_t STRATUM_V1_poll_jsonrpc_line_ctx(esp_transport_handle_t transport,
                                              char **bufp, size_t *sizep,
                                              int timeout_ms, char **line_out);

#endif // STRATUM_RECV_CTX_H
