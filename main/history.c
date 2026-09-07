#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "device.h"
#include "history.h"

static const char * TAG = "history";

static history_sample_t * ring = NULL;
static uint32_t held = 0;      /* how many of the ring are populated */
static uint32_t next = 0;      /* where the following sample goes */
static int64_t  last_us = 0;

static bool ensure_ring(void)
{
    if (ring != NULL) {
        return true;
    }

    /*
     * PSRAM by preference. 23 KB of internal RAM is not fatal on this part,
     * but it is 23 KB that the TLS buffers and the web server want more than
     * a chart does, and every board this firmware runs on has PSRAM.
     */
    ring = heap_caps_calloc(HISTORY_CAPACITY, sizeof(history_sample_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ring == NULL) {
        ring = calloc(HISTORY_CAPACITY, sizeof(history_sample_t));
        if (ring == NULL) {
            ESP_LOGW(TAG, "no room for %d samples; history is off",
                     HISTORY_CAPACITY);
            return false;
        }
        ESP_LOGW(TAG, "history is in internal RAM (%u bytes)",
                 (unsigned)(HISTORY_CAPACITY * sizeof(history_sample_t)));
    }

    return true;
}

static uint16_t clamp_u16(float value)
{
    if (!(value > 0.0f)) {   /* also catches NaN */
        return 0;
    }
    if (value > 65535.0f) {
        return 65535;
    }
    return (uint16_t)(value + 0.5f);
}

static int8_t clamp_i8(int value)
{
    if (value < -128) {
        return -128;
    }
    if (value > 127) {
        return 127;
    }
    return (int8_t)value;
}

void history_tick(GlobalState * GLOBAL_STATE)
{
    if (GLOBAL_STATE == NULL) {
        return;
    }

    int64_t now_us = esp_timer_get_time();

    if (last_us != 0 &&
        (now_us - last_us) < ((int64_t)HISTORY_INTERVAL_SECONDS * 1000000)) {
        return;
    }

    if (!ensure_ring()) {
        return;
    }

    last_us = now_us;

    history_sample_t s = {0};
    s.hashrate_gh = clamp_u16((float)GLOBAL_STATE->SYSTEM_MODULE.current_hashrate);
    s.power_dw    = clamp_u16(GLOBAL_STATE->HEALTH_MODULE.power * 10.0f);
    s.board_c     = clamp_i8(GLOBAL_STATE->HEALTH_MODULE.board_temperature[0]);
    s.vr_c        = clamp_i8(read_power_temp());
    s.valid       = 1;

    ring[next] = s;
    next = (next + 1) % HISTORY_CAPACITY;
    if (held < HISTORY_CAPACITY) {
        held++;
    }
}

uint32_t history_span_seconds(void)
{
    return held * HISTORY_INTERVAL_SECONDS;
}

uint32_t history_age_seconds(void)
{
    if (last_us == 0) {
        return 0;
    }
    return (uint32_t)((esp_timer_get_time() - last_us) / 1000000);
}

uint32_t history_query(uint32_t window_seconds, uint32_t max_points,
                       history_sample_t * out, uint32_t * out_interval_seconds)
{
    if (out == NULL || max_points == 0 || ring == NULL || held == 0) {
        if (out_interval_seconds != NULL) {
            *out_interval_seconds = HISTORY_INTERVAL_SECONDS;
        }
        return 0;
    }

    uint32_t wanted = window_seconds / HISTORY_INTERVAL_SECONDS;
    if (wanted == 0) {
        wanted = 1;
    }
    if (wanted > held) {
        wanted = held;
    }

    /*
     * One returned point per `bucket` stored samples. A day at half-minute
     * resolution is 2880 records, which is more than a chart a few hundred
     * pixels wide can show and more than is worth sending to a browser over
     * this device's wifi, so the far end asks for a point count and gets
     * averages rather than a decimated sample that happens to miss the spike.
     */
    uint32_t bucket = (wanted + max_points - 1) / max_points;
    if (bucket == 0) {
        bucket = 1;
    }
    uint32_t points = (wanted + bucket - 1) / bucket;

    if (out_interval_seconds != NULL) {
        *out_interval_seconds = bucket * HISTORY_INTERVAL_SECONDS;
    }

    /* Oldest of the wanted samples, walking back from the newest written. */
    uint32_t start = (next + HISTORY_CAPACITY - wanted) % HISTORY_CAPACITY;

    for (uint32_t p = 0; p < points; p++) {
        uint32_t taken = 0;
        uint32_t hash_sum = 0, power_sum = 0;
        int32_t board_sum = 0, vr_sum = 0;

        for (uint32_t k = 0; k < bucket; k++) {
            uint32_t index = p * bucket + k;
            if (index >= wanted) {
                break;
            }
            const history_sample_t * s = &ring[(start + index) % HISTORY_CAPACITY];
            if (!s->valid) {
                continue;
            }
            hash_sum  += s->hashrate_gh;
            power_sum += s->power_dw;
            board_sum += s->board_c;
            vr_sum    += s->vr_c;
            taken++;
        }

        history_sample_t avg = {0};
        if (taken > 0) {
            avg.hashrate_gh = (uint16_t)(hash_sum / taken);
            avg.power_dw    = (uint16_t)(power_sum / taken);
            avg.board_c     = clamp_i8(board_sum / (int32_t)taken);
            avg.vr_c        = clamp_i8(vr_sum / (int32_t)taken);
            avg.valid       = 1;
        }
        out[p] = avg;
    }

    return points;
}
