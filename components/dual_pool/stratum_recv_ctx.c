#include "stratum_recv_ctx.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Reentrant receive for dual mining (moved out of stratum_api.c). Operates on a
// caller-owned buffer instead of the global json_rpc_buffer, so a second concurrent
// Stratum connection (Pool B) never corrupts the primary connection's accumulator.

static const char *TAG = "stratum_recv_ctx";
#define RX_BUFFER_SIZE 1024
#define RX_TIMEOUT_MS  5000

static void realloc_ctx(char **bufp, size_t *sizep, size_t len)
{
    size_t old = strlen(*bufp);
    size_t neu = old + len + 1;
    if (neu < *sizep) {
        return;
    }
    neu = neu + (RX_BUFFER_SIZE - (neu % RX_BUFFER_SIZE));
    void *nb = realloc(*bufp, neu);
    if (nb == NULL) {
        ESP_LOGE(TAG, "Restarting System because of ERROR: realloc failed in ctx receive");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
    }
    *bufp = nb;
    memset(*bufp + old, 0, neu - old);
    *sizep = neu;
}

/*
 * Cut the first complete line out of the accumulator, if there is one.
 *
 * Returns a malloc'd line or NULL, and leaves whatever followed the newline at
 * the front of the buffer for the next call.
 */
static char *take_line(char **bufp)
{
    char *newline_pos = strchr(*bufp, '\n');
    if (newline_pos == NULL) {
        return NULL;
    }

    size_t buflen = strlen(*bufp);
    size_t line_len = newline_pos - *bufp;
    char *line = strndup(*bufp, line_len);

    size_t remaining_len = buflen - line_len - 1;
    if (remaining_len > 0) {
        memmove(*bufp, newline_pos + 1, remaining_len);
        (*bufp)[remaining_len] = '\0';
    } else {
        (*bufp)[0] = '\0';
    }
    return line;
}

static bool ensure_buffer(char **bufp, size_t *sizep)
{
    if (*bufp != NULL) {
        return true;
    }
    *bufp = malloc(RX_BUFFER_SIZE);
    if (*bufp == NULL) {
        ESP_LOGE(TAG, "Failed to allocate ctx receive buffer");
        return false;
    }
    *sizep = RX_BUFFER_SIZE;
    memset(*bufp, 0, RX_BUFFER_SIZE);
    return true;
}

static void drop_buffer(char **bufp, size_t *sizep)
{
    free(*bufp);
    *bufp = NULL;
    *sizep = 0;
}

stratum_rx_t STRATUM_V1_poll_jsonrpc_line_ctx(esp_transport_handle_t transport,
                                              char **bufp, size_t *sizep,
                                              int timeout_ms, char **line_out)
{
    *line_out = NULL;

    if (!ensure_buffer(bufp, sizep)) {
        return STRATUM_RX_ERROR;
    }

    /*
     * Anything left over from last time first. Several lines can arrive in one
     * read, and a caller that only ever drained one per visit would fall
     * further behind the busier a pool got.
     */
    *line_out = take_line(bufp);
    if (*line_out != NULL) {
        return STRATUM_RX_LINE;
    }

    char recv_buffer[RX_BUFFER_SIZE];
    memset(recv_buffer, 0, RX_BUFFER_SIZE);

    int nbytes = esp_transport_read(transport, recv_buffer, RX_BUFFER_SIZE - 1, timeout_ms);
    if (nbytes < 0) {
        ESP_LOGE(TAG, "ctx transport read failed (code: %d)", nbytes);
        drop_buffer(bufp, sizep);
        return STRATUM_RX_ERROR;
    }
    if (nbytes == 0) {
        /* Nothing within the timeout. A quiet pool is not a dead one. */
        return STRATUM_RX_IDLE;
    }

    realloc_ctx(bufp, sizep, nbytes);
    strncat(*bufp, recv_buffer, nbytes);

    *line_out = take_line(bufp);
    return (*line_out != NULL) ? STRATUM_RX_LINE : STRATUM_RX_IDLE;
}

char *STRATUM_V1_receive_jsonrpc_line_ctx(esp_transport_handle_t transport, char **bufp, size_t *sizep)
{
    /*
     * The original blocking form, now a loop over the polling one. Kept
     * because a caller holding a single session has no reason to poll, and
     * because it is the behaviour the pool A path was written against.
     */
    for (;;) {
        char *line = NULL;
        stratum_rx_t rc = STRATUM_V1_poll_jsonrpc_line_ctx(transport, bufp, sizep,
                                                           RX_TIMEOUT_MS, &line);
        if (rc == STRATUM_RX_LINE) {
            return line;
        }
        if (rc == STRATUM_RX_ERROR) {
            return NULL;
        }
        /* IDLE: keep waiting, exactly as the old loop did. */
    }
}
