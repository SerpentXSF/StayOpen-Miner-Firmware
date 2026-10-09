#ifndef POOL_FIELDS_H
#define POOL_FIELDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "global_state.h"
#include "pool_scheduler.h"

/*
 * Every per-pool setting, and how each one is spelled in NVS and in the API.
 *
 * One table, because the same twelve settings were written out by hand in
 * three places -- the start-up load in system.c, and the GET and POST
 * handlers in http_server.c -- and quad pool would have turned that into
 * thirty-nine near-identical blocks. Adding a pool is now a change to
 * POOL_MAX rather than forty more string literals.
 *
 * It also closes the gap that shape invites: a field that is saved but never
 * reported, or loaded but never saved. poolBTLS was exactly that -- accepted
 * and stored but missing from the GET, so the pool page read it back as off
 * and pressing Save turned pool B's TLS off for anyone who opened the tab.
 * A field in this table is wired into all three paths or none of them.
 *
 * Keys are built as "pool" + the pool's letter + the suffix: lowercase for
 * NVS ("poolburl"), uppercase for the API ("poolBUrl"). That is precisely how
 * pool B's keys were already spelled -- POOL_B is 1 and 'a' + 1 is 'b' -- so
 * configuration written by earlier firmware is read back unchanged.
 */

typedef enum {
    POOL_FIELD_STR,
    POOL_FIELD_U16
} pool_field_kind_t;

typedef struct {
    pool_field_kind_t kind;
    const char *nvs;      /* suffix after "pool<letter>" */
    const char *json;     /* suffix after "pool<Letter>" */
    size_t      offset;   /* of the member within PoolConfig */
    const char *def_str;
    uint16_t    def_u16;

    /*
     * Whether a GET reports it. Passwords and certificates are stored and
     * accepted but never handed back, so the page cannot leak one and cannot
     * overwrite a stored secret with a blank on save.
     */
    bool publish;
} PoolField;

#define POOL_FIELD_COUNT 13
extern const PoolField POOL_FIELDS[POOL_FIELD_COUNT];

/*
 * Build a settings key: "pool" + the pool's letter + `suffix`.
 * `upper` picks the API spelling ("poolBUrl") over the NVS one ("poolburl").
 *
 * NVS caps a key at 15 characters. The longest here is "poolbfbport" at
 * eleven, so there is room, but a longer suffix added later would be
 * truncated silently -- hence the assert in pool_fields.c.
 */
void pool_key(char *out, size_t n, pool_id_t id, const char *suffix, bool upper);

#endif /* POOL_FIELDS_H */
