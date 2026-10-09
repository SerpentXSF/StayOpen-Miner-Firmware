#include "pool_fields.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

/*
 * The order here is the order the pool page shows them in, which is also the
 * order a reader of the API sees. Defaults match what pool B has always
 * loaded, so a miner upgrading onto this build comes up configured exactly as
 * it was.
 *
 * `pass` and `fb_pass` default to "x" because that is what a Stratum pool
 * expects when it ignores the password, and an empty string is refused by
 * some of them.
 */
const PoolField POOL_FIELDS[POOL_FIELD_COUNT] = {
    { POOL_FIELD_STR, "url",    "Url",    offsetof(PoolConfig, url),     "",  0,    true  },
    { POOL_FIELD_U16, "port",   "Port",   offsetof(PoolConfig, port),    NULL, 3333, true  },
    { POOL_FIELD_STR, "user",   "User",   offsetof(PoolConfig, user),    "",  0,    true  },
    { POOL_FIELD_STR, "pass",   "Pass",   offsetof(PoolConfig, pass),    "x", 0,    false },
    { POOL_FIELD_U16, "tls",    "TLS",    offsetof(PoolConfig, tls),     NULL, 0,    true  },
    { POOL_FIELD_STR, "cert",   "Cert",   offsetof(PoolConfig, cert),    "",  0,    false },
    { POOL_FIELD_U16, "xnsub",  "ExtranonceSubscribe",
                                          offsetof(PoolConfig, extranonce_subscribe), NULL, 0, true },
    { POOL_FIELD_STR, "fburl",  "FbUrl",  offsetof(PoolConfig, fb_url),  "",  0,    true  },
    { POOL_FIELD_U16, "fbport", "FbPort", offsetof(PoolConfig, fb_port), NULL, 3333, true  },
    { POOL_FIELD_STR, "fbuser", "FbUser", offsetof(PoolConfig, fb_user), "",  0,    true  },
    { POOL_FIELD_STR, "fbpass", "FbPass", offsetof(PoolConfig, fb_pass), "x", 0,    false },
    { POOL_FIELD_U16, "fbtls",  "FbTLS",  offsetof(PoolConfig, fb_tls),  NULL, 0,    true  },

    /*
     * Share of the slices this pool wants, relative to the others. Zero means
     * "no preference": build_pool_weights() then splits whatever pool A does
     * not take evenly between the configured pools, which is what a user who
     * fills in three hosts and sets nothing else expects to happen.
     */
    { POOL_FIELD_U16, "wt",     "Weight", offsetof(PoolConfig, weight),  NULL, 0,    true  },
};

void pool_key(char *out, size_t n, pool_id_t id, const char *suffix, bool upper)
{
    char letter = (char) ((upper ? 'A' : 'a') + (int) id);
    int written = snprintf(out, n, "pool%c%s", letter, suffix);

    /*
     * NVS silently truncates past 15 characters, and two pools whose keys
     * truncate to the same string would share one stored value. Nothing in
     * the table comes close today -- "poolbfbport" is eleven -- but a longer
     * suffix added later would do it quietly, so say so here instead.
     */
    if (written < 0 || (size_t) written >= n) {
        ESP_LOGE("pool_fields", "settings key for pool %d suffix '%s' does not "
                                "fit in %u bytes", (int) id, suffix, (unsigned) n);
    }
}
