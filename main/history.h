#ifndef _HISTORY_H
#define _HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#include "global_state.h"

/*
 * Since-boot history of the four figures the dashboard plots.
 *
 * The chart used to be drawn entirely from arrays the browser accumulated in
 * sessionStorage, which meant it showed how long that tab had been open
 * rather than what the miner had been doing: close the tab and it started
 * empty, and a miner that ran all week unattended had nothing to show.
 *
 * One sample every HISTORY_INTERVAL_SECONDS, kept for a day, is 2880 records
 * of eight bytes -- 23 KB, taken from PSRAM where there is any. Nothing is
 * written to flash: the record is deliberately lost on reboot, which costs
 * nothing anyone was relying on and avoids putting a periodic write on a
 * device whose flash also holds the firmware.
 */

#define HISTORY_INTERVAL_SECONDS 30
#define HISTORY_RETENTION_SECONDS (24 * 60 * 60)
#define HISTORY_CAPACITY (HISTORY_RETENTION_SECONDS / HISTORY_INTERVAL_SECONDS)

/* Eight bytes, and every field sized to what the hardware can actually
 * report: 65535 GH/s is forty times a BC04, and 6553.5 W is far past any
 * supply these boards accept. */
typedef struct {
    uint16_t hashrate_gh;   /* GH/s */
    uint16_t power_dw;      /* deciwatts */
    int8_t   board_c;
    int8_t   vr_c;
    uint8_t  valid;
    uint8_t  reserved;
} history_sample_t;

/* Call as often as convenient; it samples on its own schedule and is cheap
 * to call in between. Allocates on first use. */
void history_tick(GlobalState * GLOBAL_STATE);

/* Seconds actually held, which is min(uptime, retention). */
uint32_t history_span_seconds(void);

/* How long ago the newest stored sample was taken. Lets the caller place the
 * series against its own clock instead of trusting the miner's. */
uint32_t history_age_seconds(void);

/*
 * Average the last `window_seconds` into at most `max_points` buckets,
 * oldest first. Returns the number written, and reports through
 * `out_interval_seconds` how many seconds each returned point covers.
 */
uint32_t history_query(uint32_t window_seconds, uint32_t max_points,
                       history_sample_t * out, uint32_t * out_interval_seconds);

#endif
