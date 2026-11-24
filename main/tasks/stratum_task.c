#include "esp_log.h"
// #include "addr_from_stdin.h"
#include "lt0051.h"
#include "connect.h"
#include "system.h"
#include "global_state.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include <lwip/tcpip.h>
#include "lwip/inet.h"
#include "nvs_config.h"
#include "stratum_task.h"
#include "work_queue.h"
#include "pool_scheduler.h"
#include "esp_wifi.h"
#include <esp_sntp.h>
#include <time.h>
#include "lvgl_porting.h"
#include "network.h"
#include <sys/time.h>
#include "esp_timer.h"
#include <stdbool.h>
#include "utils.h"
#include "main.h"
#include "esp_transport.h"
#include "esp_attr.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "lwip/sockets.h"
#include <strings.h>
#include <unistd.h>

/* The payout address the shipped config templates carry, so an
 * unconfigured miner cannot quietly mine to somebody else's wallet.
 * Matched as a substring because the templates append a worker suffix. */
#define STRATUM_PLACEHOLDER_USER "REPLACE-WITH-YOUR"

#define PORT CONFIG_STRATUM_PORT
#define STRATUM_URL CONFIG_STRATUM_URL

#define FALLBACK_PORT CONFIG_FALLBACK_STRATUM_PORT
#define FALLBACK_STRATUM_URL CONFIG_FALLBACK_STRATUM_URL

#define STRATUM_PW CONFIG_STRATUM_PW
#define FALLBACK_STRATUM_PW CONFIG_FALLBACK_STRATUM_PW
#define STRATUM_DIFFICULTY CONFIG_STRATUM_DIFFICULTY

#define MAX_RETRY_ATTEMPTS 3
#define MAX_CRITICAL_RETRY_ATTEMPTS 5
#define MAX_EXTRANONCE_2_LEN 32

#define BUFFER_SIZE 1024

typedef struct
{
    uint32_t stratum_difficulty;
    bool isconnected;
    char lastResolvedIp[INET_ADDRSTRLEN];
} StratumTaskModule;

static const char * TAG = "stratum_task";

static StratumApiV1Message stratum_api_v1_message = {.error_str = NULL, .mining_notification = NULL};
StratumTaskModule STATUM_MODULE = {.stratum_difficulty = 8192, .isconnected = false, .lastResolvedIp = {0},};


struct timeval tcp_snd_timeout = {
    .tv_sec = 5,
    .tv_usec = 0
};

struct timeval tcp_rcv_timeout = {
    .tv_sec = 60 * 10,
    .tv_usec = 0
};

bool is_wifi_connected() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return true;
    } else {
        return false;
    }
}

void cleanQueue(GlobalState * GLOBAL_STATE) {
    ESP_LOGD(TAG, "Clean Jobs: clearing stratum queue");
    GLOBAL_STATE->abandon_work = 1;
    if(GLOBAL_STATE->stratum_queue.count > 0)
        queue_clear(&GLOBAL_STATE->stratum_queue);

    for(uint32_t chain_num = 0; chain_num < MAX_CHAIN_NUM; chain_num++){
        if(GLOBAL_STATE->chain_pluged[chain_num]){
            pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);

            /*
             * This is pool A's clean_jobs. It used to clear the whole ASIC
             * queue and invalidate all 128 slots -- including pool B's work,
             * which pool A has no business abandoning. Every nonce the ASIC
             * then returned for a pool B job was discarded as an unknown job,
             * on every new block. That is lost shares, and one of them could
             * be a block.
             */
            if (GLOBAL_STATE->ASIC_jobs_queue[chain_num].count > 0){
                ESP_LOGD(TAG, "Clean Jobs: clearing chain %"PRIu32" asic job queue", chain_num);
                if (GLOBAL_STATE->dual_enable) {
                    ASIC_jobs_queue_clear_pool(&GLOBAL_STATE->ASIC_jobs_queue[chain_num], POOL_A);
                } else {
                    ASIC_jobs_queue_clear(&GLOBAL_STATE->ASIC_jobs_queue[chain_num]);
                }
            }

            for (int i = 0; i < 128; i++) {
                if(NULL == GLOBAL_STATE->valid_jobs[chain_num])
                    continue;
                if (GLOBAL_STATE->dual_enable &&
                    GLOBAL_STATE->job_pool[chain_num][i] != POOL_A) {
                    continue;   /* another pool's slot; not pool A's to invalidate */
                }
                GLOBAL_STATE->valid_jobs[chain_num][i] = 0;
            }

            pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);
        }
    }
}

void stratum_reset_uid(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Resetting stratum uid");
    GLOBAL_STATE->send_uid = 1;
}

bool stratum_connected_status(void)
{
    return STATUM_MODULE.isconnected;
}

char * stratum_get_pool_ip(void)
{
    return STATUM_MODULE.lastResolvedIp;
}

/*
 * Resolve into the caller's own buffer, for anyone who must not touch
 * STATUM_MODULE.lastResolvedIp. That buffer is shared, and stratum_task reads
 * it between its own resolve and its connect, so a second writer can send it
 * to the wrong pool with the wrong credentials. getaddrinfo rather than
 * gethostbyname because lwip's gethostbyname returns a pointer into static
 * storage, which races the same way.
 */
bool stratum_resolve_into(const char * host, char * out, size_t out_len)
{
    if (host == NULL || host[0] == '\0' || out == NULL || out_len == 0) {
        return false;
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo * res = NULL;

    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        return false;
    }

    struct sockaddr_in * sa = (struct sockaddr_in *) res->ai_addr;
    const char * written = inet_ntop(AF_INET, &sa->sin_addr, out, out_len);
    freeaddrinfo(res);

    return written != NULL;
}

bool stratum_last_resolved_hostname(char *host)
{
    struct hostent *primary_dns_addr = gethostbyname(host);
    if (primary_dns_addr == NULL) {
        ESP_LOGW(TAG, "Heartbeat. Failed DNS check for: %s!", host);
        return false;
    }

    inet_ntop(AF_INET, (void *)primary_dns_addr->h_addr_list[0], STATUM_MODULE.lastResolvedIp, INET_ADDRSTRLEN);

    return true;
}

void stratum_close_connection(GlobalState * GLOBAL_STATE)
{
    /*
     * Take the handle and null the field under the lock, then let go before
     * closing it.
     *
     * Releasing first is safe, and the reasoning is the whole point of the
     * lock: a submit can only touch the handle while holding this mutex, so
     * once we hold it no submit is inside one, and once the field is NULL no
     * later submit can obtain it. Holding on across the close instead would
     * be safe too, but esp_transport_close on a TLS session can block for
     * seconds under SO_SNDTIMEO, and ASIC_result_task -- one per chain, at a
     * much higher priority -- would wait behind it, processing no nonces for
     * any pool. (stratum_poolb_task's pool_close does hold it across the
     * destroy. It could take the same treatment; that is its own change.)
     *
     * Only stratum_task may call this. asic_result_task used to, from the
     * share-submit path, which freed the handle while this task could be
     * blocked in esp_transport_read() on it; it now sets write_failed and
     * lets this task do it.
     */
    pthread_mutex_lock(&GLOBAL_STATE->transport_lock);
    esp_transport_handle_t t = GLOBAL_STATE->transport;
    GLOBAL_STATE->transport = NULL;
    GLOBAL_STATE->write_failed = false;
    GLOBAL_STATE->drop_requested = false;
    pthread_mutex_unlock(&GLOBAL_STATE->transport_lock);

    if (t == NULL) {
        ESP_LOGE(TAG, "Transport already NULL, not shutting down again..");
        return;
    }

    ESP_LOGE(TAG, "Shutting down transport and restarting...");
    esp_transport_close(t);
    esp_transport_destroy(t);

    /*
     * The receive accumulator keeps everything after the last newline for the
     * next call, so it has to be dropped with the session. Every error return
     * from STRATUM_V1_receive_jsonrpc_line frees it, which is why the paths
     * that end a session *through* a failed read have always been clean -- and
     * why ending one any other way was not. Both such paths are in this file:
     * the write_failed break below, and the CLIENT_RECONNECT break, which has
     * had this defect since it was written. Without this, a pool that sent two
     * lines in one segment would have the second parsed into the *next*
     * session -- marking it connected on evidence from a dead socket, and
     * building work against the previous session's extranonce.
     */
    cleanup_stratum_buffer();
    cleanQueue(GLOBAL_STATE);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}

/*
 * While pool A is on its fallback, probe the configured primary and ask
 * stratum_task to go back to it once the primary is genuinely serving work.
 *
 * None of this ran before. The xTaskCreate was commented out in the vendor
 * import (878900f), so the only route back to the primary was the toggle in
 * stratum_task, which flips is_using_fallback after MAX_RETRY_ATTEMPTS
 * failures on whichever pool is currently selected. A healthy fallback never
 * fails, so it never toggled: observed 2026-11-23 with both boards parked on
 * ckpool for hours while the configured primary answered the whole time. Only
 * a restart cleared it, is_using_fallback being false at init and never
 * persisted.
 *
 * What counts as "serving work" is the whole difficulty, and two separate
 * things had to be got right.
 *
 * A mining.subscribe *reply* contains the literal "mining.notify" -- it is one
 * of the subscription tuples in result[0], the element
 * _parse_stratum_subscribe_result_message() skips past to reach the extranonce
 * in result[1] -- so searching the buffer for that string matches a pool that
 * has sent no work at all. This waits for a line whose *method* is
 * mining.notify, accumulated across reads because a notify carrying a full
 * merkle branch list runs to kilobytes.
 *
 * And a pool can serve work to the world while refusing *us*. ckpool-family
 * pools send set_difficulty and notify on subscribe, before authorize is
 * answered, so a notify alone says nothing about whether our worker is
 * accepted. Promoting to a pool that rejects the worker is worse than staying
 * on the fallback: stratum_task would connect, be refused, and -- because the
 * STRATUM_RESULT_SETUP branch resets retry_attempts before it looks at
 * response_success -- never count enough failures to fall back. A board that
 * was mining on the fallback would mine nothing instead. So the authorize
 * reply must come back with result:true as well.
 *
 * Being strict costs little and buys a lot: the failover path resets the share
 * counters, so a promotion that does not stick costs a measurement window.
 *
 * It does not write is_using_fallback, and it re-reads it at the moment of
 * promotion rather than trusting the read from the top of the cycle. See
 * GlobalState::promote_requested, and N1 in the history of this file: a
 * seventeen-second-old read of that flag was enough to tear down a healthy
 * primary session.
 *
 * Resident, with its stack in PSRAM. Created once at stratum_task start, so it
 * is never created at the moment of a failover, when a BC04's internal heap is
 * at its worst -- that board has measured 7,680 bytes as its largest free
 * internal block one minute after boot and 91 bytes free in steady state. In
 * PSRAM the stack costs no internal RAM at all, only the TCB, and a task that
 * never exits has no lifecycle left to get wrong.
 *
 * One rule for anyone editing it: nothing in this task may touch flash. A
 * task whose stack is in PSRAM trips assert(esp_task_stack_is_sane_cache_
 * disabled()) if it runs a flash operation, so no NVS write, no OTA, and no
 * restart helper that persists a reason.
 */
#define PRIMARY_PROBE_PERIOD_MS      60000
#define PRIMARY_PROBE_QUIET_MS       12000
/* Matches STRATUM_MAX_LINE in stratum_api.c, which is what the live session
 * accepts. A smaller buffer here means a pool whose notify is larger than it
 * can never be promoted to, which is the failure this whole file exists to
 * remove. PSRAM .bss, so the size costs nothing internal. */
#define PRIMARY_PROBE_RX             (16 * 1024)
#define PRIMARY_PROBE_TX             512
/* A reply line; a notify is never this short, which is what keeps cJSON off
 * the kilobyte lines. */
#define PRIMARY_PROBE_REPLY_MAX      512
/*
 * PSRAM .bss, so this number is not traded against internal RAM and there is
 * no reason to be tight with it. 8192 was the usual figure for an esp-tls
 * task and the plain-TCP path measured about 2,350 of it -- but the deep path
 * is an mbedtls handshake with esp_crt_bundle logging at INFO from inside the
 * x509 verify callback, and no task in this tree has been measured doing TLS
 * on a PSRAM stack. An overflow there would write over neighbouring PSRAM
 * heap before the canary check noticed, so the margin is the point.
 */
#define PRIMARY_PROBE_STACK          16384
#define PRIMARY_PROBE_HOLDOFF_MIN_MS 60000
#define PRIMARY_PROBE_HOLDOFF_MAX_MS 1800000
/* A promotion still standing after this long is treated as having stuck. */
#define PRIMARY_PROBE_FLAP_WINDOW_US ((int64_t) 600 * 1000 * 1000)

#define PROBE_CONFIGURE_ID 1
#define PROBE_SUBSCRIBE_ID 2
#define PROBE_AUTHORIZE_ID 3

static TaskHandle_t primary_probe_handle = NULL;

/*
 * Allocated statically, both halves.
 *
 * xTaskCreateWithCaps puts the stack where it is asked but the TCB always
 * goes through pvPortMalloc, which is internal-only: 348 bytes on this build,
 * taken at app_main ahead of every ASIC, HTTP and system task. On a BC04 that
 * is a pool whose floor has been measured in the tens of bytes, and this
 * project has already watched a new task's internal appetite stop
 * asic_result_0 being created, leaving the miner hashing nothing.
 *
 * Static allocation does not give those 348 bytes back -- .bss comes out of
 * the same RAM -- but it stops them being a runtime allocation that competes
 * with later tasks and can fail at exactly the wrong moment. The cost becomes
 * a link-time constant: visible in the map, identical on every boot, and
 * incapable of failing halfway. StaticTask_t stays in internal .bss because
 * the scheduler touches it; only the stack goes to PSRAM.
 *
 * Stack depth is in bytes on this port: StackType_t is uint8_t on Xtensa.
 */
EXT_RAM_BSS_ATTR __attribute__((aligned(16)))
static StackType_t probe_stack[PRIMARY_PROBE_STACK];
static StaticTask_t probe_tcb;

/*
 * Out of internal RAM for the reason above, and safe there: none of these is
 * touched from an ISR or during a flash operation.
 */
EXT_RAM_BSS_ATTR static char probe_rx[PRIMARY_PROBE_RX];
EXT_RAM_BSS_ATTR static char probe_tx[PRIMARY_PROBE_TX];
EXT_RAM_BSS_ATTR static char probe_url[128];
EXT_RAM_BSS_ATTR static char probe_user[192];
EXT_RAM_BSS_ATTR static char probe_pass[64];

/*
 * Loud enough to be read on a shipped board. A probe that can never succeed
 * is indistinguishable from the original bug unless it says so, and the
 * default log level is INFO, so the per-cycle detail cannot be ESP_LOGD alone.
 */
static uint32_t probe_fail_streak = 0;

static void probe_note_failure(const char * stage)
{
    probe_fail_streak++;
    if (probe_fail_streak == 1 || (probe_fail_streak % 10) == 0) {
        ESP_LOGW(TAG, "primary probe: %s (%u in a row; pool A is not being "
                      "returned to its primary)", stage,
                 (unsigned) probe_fail_streak);
    } else {
        ESP_LOGD(TAG, "primary probe: %s", stage);
    }
}

/* A mining.notify *method* line, not the subscribe reply that merely
 * advertises the method's name. */
static bool probe_line_is_notify(const char * line)
{
    return strstr(line, "mining.notify") != NULL
        && strstr(line, "\"method\"") != NULL
        && strstr(line, "\"result\"") == NULL;
}

/* The authorize reply, accepted. Parsed rather than substring-matched, because
 * "result":true has to belong to our id and not to some other reply. */
static bool probe_line_is_authorized(const char * line)
{
    if (strlen(line) > PRIMARY_PROBE_REPLY_MAX) {
        return false;
    }
    if (strstr(line, "\"result\"") == NULL) {
        return false;
    }
    /*
     * Before cJSON recurses, and for the same reason the live parser does it
     * in STRATUM_V1_parse: a frame per nesting level, 8 KB of stack under
     * this task, and no limit of cJSON's own worth anything at that depth.
     * The 512-byte cap above is not a depth limit -- 512 open brackets is
     * around 500 levels, roughly four times what fits.
     */
    if (STRATUM_V1_json_too_deep(line)) {
        ESP_LOGW(TAG, "primary probe: ignoring a reply from the primary nested "
                      "deeper than %d levels", STRATUM_MAX_JSON_DEPTH);
        return false;
    }

    cJSON * root = cJSON_Parse(line);
    if (root == NULL) {
        return false;
    }

    bool ok = false;
    cJSON * id = cJSON_GetObjectItem(root, "id");
    cJSON * result = cJSON_GetObjectItem(root, "result");
    cJSON * error = cJSON_GetObjectItem(root, "error");
    /*
     * A non-null error is a refusal whatever result says, which is the order
     * the live parser tests in. Measured against the node this runs with: a
     * refused worker answers {"result":false,"error":null}, so a null error
     * proves nothing on its own and result is the field carrying the answer
     * -- but a pool that fills in both must not read as accepted.
     */
    bool no_error = (error == NULL) || cJSON_IsNull(error);
    if (cJSON_IsNumber(id) && id->valueint == PROBE_AUTHORIZE_ID
            && cJSON_IsTrue(result) && no_error) {
        ok = true;
    }

    cJSON_Delete(root);
    return ok;
}

/*
 * Read until the pool has both accepted our worker and sent real work, or the
 * deadline passes. Either half alone is not enough: see the comment above.
 */
static bool probe_wait_for_work(esp_transport_handle_t hb, bool * saw_notify,
                                bool * saw_auth)
{
    size_t filled = 0;
    bool overlong = false;
    int64_t deadline = esp_timer_get_time() + ((int64_t) PRIMARY_PROBE_QUIET_MS * 1000);

    *saw_notify = false;
    *saw_auth = false;
    probe_rx[0] = '\0';

    while (esp_timer_get_time() < deadline) {
        int n = esp_transport_read(hb, probe_rx + filled,
                                   (int) (PRIMARY_PROBE_RX - 1 - filled), 2000);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            /* A read timeout, which esp_transport.h defines as 0 rather than
             * an error. The pool may simply not have a job yet, so wait out
             * the deadline instead of giving up on the first gap. */
            continue;
        }

        filled += (size_t) n;
        probe_rx[filled] = '\0';

        char * start = probe_rx;
        char * nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            if (!overlong) {
                if (probe_line_is_notify(start)) {
                    *saw_notify = true;
                } else if (probe_line_is_authorized(start)) {
                    *saw_auth = true;
                }
                if (*saw_notify && *saw_auth) {
                    return true;
                }
            }
            overlong = false;
            start = nl + 1;
        }

        size_t rest = strlen(start);
        if (rest > 0 && start != probe_rx) {
            memmove(probe_rx, start, rest + 1);
        }
        filled = rest;

        if (filled >= PRIMARY_PROBE_RX - 1) {
            /* One line longer than a buffer that already matches the live
             * session's limit. Drop it rather than test a fragment. */
            filled = 0;
            probe_rx[0] = '\0';
            overlong = true;
        }
    }

    return false;
}

/* Copy a setting, and say so if it did not fit: a truncated host or worker
 * means no promotion, and silence would make that look like a dead primary. */
static void probe_copy_setting(char * dst, size_t len, const char * src,
                               const char * what)
{
    int n = snprintf(dst, len, "%s", src ? src : "");
    if (n < 0 || (size_t) n >= len) {
        ESP_LOGW(TAG, "primary probe: the configured %s is longer than %u bytes "
                      "and was truncated; the primary will not be probed "
                      "correctly", what, (unsigned) (len - 1));
    }
}

void stratum_primary_heartbeat(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    struct timeval tcp_timeout = {
        .tv_sec = 5,
        .tv_usec = 0
    };

    /* Back-off state. A primary that passes the probe but fails the real
     * session would otherwise flap at the probe period, and each failover
     * wipes the share counters. */
    int64_t last_promote_us = 0;
    int64_t holdoff_until_us = 0;
    uint32_t holdoff_ms = PRIMARY_PROBE_HOLDOFF_MIN_MS;
    uint32_t fail_streak_acted = 0;
    bool logged_stack = false;
    bool logged_same_endpoint = false;

    /*
     * Make lwIP's per-thread semaphore now, while the board is booting and
     * internal RAM is at its best. LWIP_NETCONN_SEM_PER_THREAD is 1 on this
     * port, so the first lwIP call from any task allocates a binary semaphore
     * out of internal RAM -- and if that allocation fails the caller does not
     * get an error back, it hits LWIP_ASSERT in tcpip_send_msg_wait_sem() and
     * the board panics.
     *
     * Left alone, this task would pay that on its first cycle after the first
     * failover of each boot: the one moment the comment above this function
     * says the design keeps allocations away from, because that is when a
     * BC04's internal heap is at its worst. Every other task pays it at boot.
     * This one now does too.
     */
    int warm = socket(AF_INET, SOCK_STREAM, 0);
    if (warm >= 0) {
        close(warm);
    } else {
        ESP_LOGW(TAG, "primary probe: could not pre-arm lwIP; the first probe "
                      "after a failover will have to allocate instead");
    }

    ESP_LOGI(TAG, "primary probe: resident, will watch the primary whenever "
                  "pool A is on its fallback");

    while (1) {
        vTaskDelay(PRIMARY_PROBE_PERIOD_MS / portTICK_PERIOD_MS);

        int64_t now = esp_timer_get_time();

        if (last_promote_us != 0) {
            if (module->is_using_fallback
                    && now - last_promote_us < PRIMARY_PROBE_FLAP_WINDOW_US) {
                /* Promoted, and back on the fallback already: it did not
                 * stick. Back off before trying the same thing again. */
                holdoff_ms *= 2;
                if (holdoff_ms > PRIMARY_PROBE_HOLDOFF_MAX_MS) {
                    holdoff_ms = PRIMARY_PROBE_HOLDOFF_MAX_MS;
                }
                holdoff_until_us = now + ((int64_t) holdoff_ms * 1000);
                last_promote_us = 0;
                ESP_LOGW(TAG, "primary probe: the last switch back did not hold; "
                              "not probing again for %u s",
                         (unsigned) (holdoff_ms / 1000));
            } else if (now - last_promote_us >= PRIMARY_PROBE_FLAP_WINDOW_US) {
                /* It held. Forget the back-off. */
                holdoff_ms = PRIMARY_PROBE_HOLDOFF_MIN_MS;
                last_promote_us = 0;
            }
        }

        if (!module->is_using_fallback || GLOBAL_STATE->promote_requested) {
            /* Each spell on the fallback is its own episode. A streak carried
             * over from the last one would swallow the first warning of the
             * next, and would hold its first probe off by a ladder earned
             * against a different outage. */
            probe_fail_streak = 0;
            fail_streak_acted = 0;
            continue;
        }

        /*
         * Back off on repeated failures, not only on promotions that did not
         * hold. A primary that answers but never qualifies -- the ordinary
         * case being a pool that refuses this worker -- would otherwise be
         * connected to, subscribed to and authorized against once a minute
         * for as long as that lasts. That is 1,440 times a day, and to the
         * pool it looks like a client stuck in a retry loop, which is the
         * sort of thing that earns a rate limit or a ban on the address.
         *
         * Derived from the streak rather than set at each failure, so that a
         * failure path added later cannot forget to back off.
         */
        if (probe_fail_streak != fail_streak_acted) {
            fail_streak_acted = probe_fail_streak;
            uint32_t fail_ms = PRIMARY_PROBE_HOLDOFF_MIN_MS;
            for (uint32_t i = 1; i < probe_fail_streak; i++) {
                if (fail_ms >= PRIMARY_PROBE_HOLDOFF_MAX_MS) {
                    break;
                }
                fail_ms *= 2;
            }
            if (fail_ms > PRIMARY_PROBE_HOLDOFF_MAX_MS) {
                fail_ms = PRIMARY_PROBE_HOLDOFF_MAX_MS;
            }
            int64_t fail_until = now + ((int64_t) fail_ms * 1000);
            if (fail_until > holdoff_until_us) {
                holdoff_until_us = fail_until;
            }
        }

        if (now < holdoff_until_us) {
            continue;
        }
        if (!is_wifi_connected() && !network_is_connected()) {
            continue;
        }

        /*
         * Copy before use. A settings PATCH does free() then strdup() on
         * pool_url and pool_user while this runs, and these are held across a
         * resolve, a connect and up to twelve seconds of reads. This narrows
         * that to the copy itself; closing it needs a settings mutex shared
         * with the PATCH handler, which stratum_task needs too.
         */
        if (module->pool_url == NULL || module->pool_url[0] == '\0') {
            continue;
        }
        probe_copy_setting(probe_url, sizeof(probe_url), module->pool_url, "pool URL");
        probe_copy_setting(probe_user, sizeof(probe_user), module->pool_user, "worker name");
        probe_copy_setting(probe_pass, sizeof(probe_pass), module->pool_pass, "pool password");
        uint16_t port = module->pool_port;
        tls_mode tls = module->pool_tls;
        char * cert = module->pool_cert;

        /*
         * Nothing to switch back to when both URLs are the same endpoint --
         * which is what the project ships: config.cvs.example and
         * config.bc04.cvs.example both set stratumurl and fbstratumurl to
         * solo.ckpool.org:3333. On such a board the "primary" is serving by
         * definition the moment the fallback is, so every failover would be
         * followed by this task tearing down a working session to reconnect
         * to the very same pool, throwing away its in-flight work and
         * resetting its difficulty for nothing.
         */
        if (module->fallback_pool_url != NULL
                && strcasecmp(probe_url, module->fallback_pool_url) == 0
                && port == module->fallback_pool_port) {
            if (!logged_same_endpoint) {
                logged_same_endpoint = true;
                ESP_LOGI(TAG, "primary probe: pool A's primary and fallback are "
                              "the same endpoint (%s:%d), so there is nothing to "
                              "switch back to; staying quiet", probe_url, port);
            }
            continue;
        }

        /* Our own buffer, never STATUM_MODULE.lastResolvedIp, which
         * stratum_task reads between its own resolve and its connect. */
        char probe_ip[INET_ADDRSTRLEN] = {0};
        if (!stratum_resolve_into(probe_url, probe_ip, sizeof(probe_ip))) {
            probe_note_failure("cannot resolve the primary");
            continue;
        }

        esp_transport_handle_t hb = STRATUM_V1_transport_init(tls, cert);
        if (hb == NULL) {
            probe_note_failure("could not create a transport");
            continue;
        }
        if (tls != DISABLED) {
            esp_transport_ssl_set_common_name(hb, probe_url);
        }

        if (esp_transport_connect(hb, probe_ip, port, 5000) != ESP_OK) {
            probe_note_failure("the primary is not accepting connections");
            esp_transport_close(hb);
            esp_transport_destroy(hb);
            continue;
        }

        int hb_sock = esp_transport_get_socket(hb);
        if (hb_sock >= 0) {
            if (setsockopt(hb_sock, SOL_SOCKET, SO_RCVTIMEO, &tcp_timeout, sizeof(tcp_timeout)) != 0) {
                ESP_LOGE(TAG, "primary probe: fail to setsockopt SO_RCVTIMEO");
            }
        }

        /*
         * Written here rather than through STRATUM_V1_subscribe and
         * STRATUM_V1_authenticate, which each put a char[2300] on the
         * caller's stack. The user agent matches what the live session sends,
         * so a pool that treats clients differently by agent treats the probe
         * the same way.
         */
        /*
         * mining.configure first, exactly as the live session does. Sending
         * less than the live session sends is the one asymmetry that matters
         * in the dangerous direction: a pool that drops the connection on
         * configure would pass the probe and then fail every real session,
         * and pool A would be promoted straight into a loop. The reply is not
         * inspected, because the live session does not gate on it either --
         * what is being matched here is what goes out, not what comes back.
         */
        int n = snprintf(probe_tx, sizeof(probe_tx),
                         "{\"id\": %d, \"method\": \"mining.configure\", \"params\": "
                         "[[\"version-rolling\"], {\"version-rolling.mask\": \"ffffffff\"}]}\n",
                         PROBE_CONFIGURE_ID);
        if (n > 0 && n < (int) sizeof(probe_tx)) {
            esp_transport_write(hb, probe_tx, n, 0);
        }

        n = snprintf(probe_tx, sizeof(probe_tx),
                         "{\"id\": %d, \"method\": \"mining.subscribe\", \"params\": [\"volc/%s/%s\"]}\n",
                         PROBE_SUBSCRIBE_ID, GLOBAL_STATE->asic_model_str,
                         esp_app_get_description()->version);
        if (n > 0 && n < (int) sizeof(probe_tx)) {
            esp_transport_write(hb, probe_tx, n, 0);
        }

        n = snprintf(probe_tx, sizeof(probe_tx),
                     "{\"id\": %d, \"method\": \"mining.authorize\", \"params\": [\"%s\", \"%s\"]}\n",
                     PROBE_AUTHORIZE_ID, probe_user, probe_pass);
        if (n > 0 && n < (int) sizeof(probe_tx)) {
            esp_transport_write(hb, probe_tx, n, 0);
        }

        bool saw_notify = false;
        bool saw_auth = false;
        bool serving = probe_wait_for_work(hb, &saw_notify, &saw_auth);

        esp_transport_close(hb);
        esp_transport_destroy(hb);

        if (!logged_stack) {
            logged_stack = true;
            ESP_LOGI(TAG, "primary probe: %u of its %d stack bytes were never used "
                          "on the first full cycle",
                     (unsigned) (uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
                     PRIMARY_PROBE_STACK);
        }

        if (!serving) {
            /* Which half was missing, because they mean different things: no
             * work is a pool not ready, no authorize is a pool refusing this
             * worker, and the second will not fix itself. */
            if (saw_notify && !saw_auth) {
                probe_note_failure("the primary is serving work but did not accept "
                                   "this worker");
            } else if (saw_auth && !saw_notify) {
                probe_note_failure("the primary accepted this worker but served no work");
            } else {
                probe_note_failure("the primary answered but neither accepted this "
                                   "worker nor served work");
            }
            continue;
        }

        /*
         * Ask, do not act -- and only while the fallback is still the live
         * session. is_using_fallback was last read up to seventeen seconds
         * ago, at the top of this cycle, and stratum_task may have toggled
         * back to the primary by itself since. Promoting then would shut down
         * a healthy primary session and throw away its in-flight work. The
         * test and the shutdown go inside the lock that guards the handle so
         * nothing can come between them; stratum_task also ignores and clears
         * a promotion that reaches it when it is already on the primary, which
         * is what actually makes this safe, the flag being written without
         * this lock.
         */
        pthread_mutex_lock(&GLOBAL_STATE->transport_lock);
        bool still_on_fallback = module->is_using_fallback;
        if (still_on_fallback) {
            GLOBAL_STATE->promote_requested = true;
            if (GLOBAL_STATE->transport != NULL) {
                int sock = esp_transport_get_socket(GLOBAL_STATE->transport);
                if (sock >= 0) {
                    shutdown(sock, SHUT_RD);
                }
            }
        }
        pthread_mutex_unlock(&GLOBAL_STATE->transport_lock);

        probe_fail_streak = 0;

        if (!still_on_fallback) {
            ESP_LOGI(TAG, "primary probe: %s:%d is serving work, but pool A had "
                          "already returned to it", probe_url, port);
            continue;
        }

        ESP_LOGI(TAG, "primary probe: %s:%d is serving work and accepted this "
                      "worker, asking for a switch back", probe_url, port);
        last_promote_us = esp_timer_get_time();
    }
}


void decode_mining_notification(GlobalState * GLOBAL_STATE, const mining_notify *mining_notification)
{
    double network_difficulty = networkDifficulty(mining_notification->target);
    suffixString(network_difficulty, GLOBAL_STATE->network_diff_string, DIFF_STRING_SIZE, 0);    

    int coinbase_1_len = strlen(mining_notification->coinbase_1) / 2;
    int coinbase_2_len = strlen(mining_notification->coinbase_2) / 2;
    
    int coinbase_1_offset = 41; // Skip version (4), inputcount (1), prevhash (32), vout (4)
    if (coinbase_1_len < coinbase_1_offset) return;

    uint8_t scriptsig_len;
    hex2bin(mining_notification->coinbase_1 + (coinbase_1_offset * 2), &scriptsig_len, 1);
    coinbase_1_offset++;

    if (coinbase_1_len < coinbase_1_offset) return;
    
    uint8_t block_height_len;
    hex2bin(mining_notification->coinbase_1 + (coinbase_1_offset * 2), &block_height_len, 1);
    coinbase_1_offset++;

    if (coinbase_1_len < coinbase_1_offset || block_height_len == 0 || block_height_len > 4) return;

    uint32_t block_height = 0;
    hex2bin(mining_notification->coinbase_1 + (coinbase_1_offset * 2), (uint8_t *)&block_height, block_height_len);
    coinbase_1_offset += block_height_len;

    if (block_height != GLOBAL_STATE->block_height) {
        ESP_LOGI(TAG, "Block height %d", block_height);
        GLOBAL_STATE->block_height = block_height;
    }

    size_t scriptsig_length = scriptsig_len - 1 - block_height_len - (strlen(GLOBAL_STATE->extranonce_str) / 2) - GLOBAL_STATE->extranonce_2_len;
    if (scriptsig_length <= 0) return;
    
    char * scriptsig = malloc(scriptsig_length + 1);

    int coinbase_1_tag_len = coinbase_1_len - coinbase_1_offset;
    hex2bin(mining_notification->coinbase_1 + (coinbase_1_offset * 2), (uint8_t *) scriptsig, coinbase_1_tag_len);

    int coinbase_2_tag_len = scriptsig_length - coinbase_1_tag_len;

    if (coinbase_2_len < coinbase_2_tag_len) return;
    
    if (coinbase_2_tag_len > 0) {
        hex2bin(mining_notification->coinbase_2, (uint8_t *) scriptsig + coinbase_1_tag_len, coinbase_2_tag_len);
    }

    for (int i = 0; i < scriptsig_length; i++) {
        if (!isprint((unsigned char)scriptsig[i])) {
            scriptsig[i] = '.';
        }
    }

    scriptsig[scriptsig_length] = '\0';

    if (GLOBAL_STATE->scriptsig == NULL || strcmp(scriptsig, GLOBAL_STATE->scriptsig) != 0) {
        ESP_LOGI(TAG, "Scriptsig: %s", scriptsig);

        char * previous_miner_tag = GLOBAL_STATE->scriptsig;
        GLOBAL_STATE->scriptsig = scriptsig;
        free(previous_miner_tag);
    } else {
        free(scriptsig);
    }
}

void stratum_task(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    char * stratum_url = GLOBAL_STATE->SYSTEM_MODULE.pool_url;
    uint16_t port = GLOBAL_STATE->SYSTEM_MODULE.pool_port;
    bool extranonce_subscribe = GLOBAL_STATE->SYSTEM_MODULE.pool_extranonce_subscribe;
    uint16_t difficulty = GLOBAL_STATE->SYSTEM_MODULE.pool_difficulty;

    STRATUM_V1_initialize_buffer();
    int retry_attempts = 0;
    int retry_critical_attempts = 0;

    /*
     * Created here, once, with its stack in PSRAM: see the comment above the
     * probe for why it is resident rather than started at the failover. A
     * failure is not fatal -- it only means this board cannot climb back off
     * its fallback without a restart, which is what it did before -- so it is
     * logged loudly and mining carries on.
     */
    primary_probe_handle = xTaskCreateStatic(
        stratum_primary_heartbeat, "primary probe", PRIMARY_PROBE_STACK,
        pvParameters, 1, probe_stack, &probe_tcb);
    if (primary_probe_handle == NULL) {
        /* Cannot happen for want of memory now that both buffers are static;
         * it would mean one of them was NULL. Kept because the consequence is
         * worth naming either way: a failover goes back to being one-way. */
        ESP_LOGE(TAG, "primary probe: could not be created; a failover will be "
                      "one-way until the fallback itself fails or the board restarts");
    }

    //ESP_LOGI(TAG, "Opening connection to pool: %s:%d", stratum_url, port);
    while (1) {
        /*
         * Consume a promotion the probe asked for, here at the top, before
         * any of the three reads of is_using_fallback that one connection
         * attempt makes. Doing it here is what keeps those three agreeing;
         * see GlobalState::promote_requested.
         */
        if (GLOBAL_STATE->promote_requested) {
            GLOBAL_STATE->promote_requested = false;
            if (GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback) {
                GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback = false;
                retry_attempts = 0;
                /* The same reset the failover does, for the same reason: these
                 * counters describe one pool's session, and carrying the
                 * fallback's over is how a switch back would show up as a
                 * burst of rejects that never happened. */
                for (int i = 0; i < GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats_count; i++) {
                    GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats[i].count = 0;
                    GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats[i].message[0] = '\0';
                }
                GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats_count = 0;
                GLOBAL_STATE->SYSTEM_MODULE.shares_accepted = 0;
                GLOBAL_STATE->SYSTEM_MODULE.shares_rejected = 0;
                GLOBAL_STATE->SYSTEM_MODULE.work_received = 0;
                GLOBAL_STATE->SYSTEM_MODULE.best_session_nonce_diff = 0;
                ESP_LOGI(TAG, "pool A: the primary is serving work again, switching back to it");
            } else {
                /* Already back on the primary by another route. Clearing it
                 * and doing nothing else is the point: acting on a stale
                 * promotion would reset retry_attempts for a pool that may be
                 * failing right now. */
                ESP_LOGD(TAG, "pool A: a switch back was asked for but it is "
                              "already on the primary");
            }
        }

        if (!is_wifi_connected()) {
            //SYSTEM_notify_error_info(GLOBAL_STATE, WIFI_CONNETION_ERROR, NULL);
            ESP_LOGI(TAG, "WiFi disconnected, attempting to reconnect...");
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            if(!network_is_connected())
			{
                ESP_LOGI(TAG, "ETH disconnected, attempting to reconnect...");
                vTaskDelay(5000 / portTICK_PERIOD_MS);
				continue;
			}
        }

        if((strlen(GLOBAL_STATE->SYSTEM_MODULE.pool_url) < 5 ) && (strlen(GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url) < 5))
        {
            ESP_LOGI(TAG, "error pool : %s , %s ",GLOBAL_STATE->SYSTEM_MODULE.pool_url, GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url);
            vTaskDelay(10000 / portTICK_PERIOD_MS);
            continue;
        }

        if (retry_attempts >= MAX_RETRY_ATTEMPTS)
        {
            if (GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url == NULL || GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url[0] == '\0') {
                SYSTEM_notify_error_info(GLOBAL_STATE, NETWORK_ERROR, NULL);
                ESP_LOGI(TAG, "Unable to switch to fallback. No url configured. (retries: %d)...", retry_attempts);
                GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback = false;
                retry_attempts = 0;
                continue;
            }

            GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback = !GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback;
            // Reset share stats at failover
            for (int i = 0; i < GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats_count; i++) {
                GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats[i].count = 0;
                GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats[i].message[0] = '\0';
            }
            GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats_count = 0;
            GLOBAL_STATE->SYSTEM_MODULE.shares_accepted = 0;
            GLOBAL_STATE->SYSTEM_MODULE.shares_rejected = 0;
            GLOBAL_STATE->SYSTEM_MODULE.work_received = 0;
            GLOBAL_STATE->SYSTEM_MODULE.best_session_nonce_diff = 0;
            
            ESP_LOGI(TAG, "Switching target due to too many failures (retries: %d)...", retry_attempts);
            retry_attempts = 0;

            /*
             * The probe is deliberately NOT woken here. It polls on its own
             * period, and waking it at the failover made the first probe run
             * seconds after the primary had just failed three times -- which,
             * against a primary that answers a probe but fails a real session,
             * flapped faster than the probe period and left the board barely
             * mining on either pool.
             *
             * A promotion must also not survive the toggle that undoes it.
             */
            GLOBAL_STATE->promote_requested = false;
        }

        stratum_url = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url : GLOBAL_STATE->SYSTEM_MODULE.pool_url;
        port = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_port : GLOBAL_STATE->SYSTEM_MODULE.pool_port;
        extranonce_subscribe = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_extranonce_subscribe : GLOBAL_STATE->SYSTEM_MODULE.pool_extranonce_subscribe;
        difficulty = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_difficulty : GLOBAL_STATE->SYSTEM_MODULE.pool_difficulty;

        ESP_LOGI(TAG, "Extranonce subscribe status: %s", extranonce_subscribe ? "ENABLED" : "DISABLED");

        if(retry_attempts == 0)
        {
            ESP_LOGI(TAG, "Opening connection to pool: %s:%d", stratum_url, port);
        }
        
        refresh_network_from_system(stratum_url, port);

        if (!stratum_last_resolved_hostname(stratum_url)) {
            ESP_LOGW(TAG, "hostname resolution failed for %s", stratum_url);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            retry_attempts++;
            continue;
        }

        tls_mode tls = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? \
            GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_tls : GLOBAL_STATE->SYSTEM_MODULE.pool_tls;
        char * cert = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? \
            GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_cert : GLOBAL_STATE->SYSTEM_MODULE.pool_cert;

        const char *protocol_prefix = (tls != DISABLED) ? "stratum+ssl" : "stratum+tcp";
        ESP_LOGI(TAG, "Connecting to: %s://%s:%d (%s)", protocol_prefix, stratum_url, port, STATUM_MODULE.lastResolvedIp);

        /*
         * Local until it works. The handle used to be published into
         * GLOBAL_STATE->transport the moment it was created, so
         * asic_result_task could find a transport that had not connected,
         * subscribed or authorized yet and write a share into it. That write
         * fails, and now that a failed write asks for a teardown, it would
         * tear down the session that was seconds from being usable.
         */
        esp_transport_handle_t transport = STRATUM_V1_transport_init(tls, cert);
        if (transport == NULL) {
            ESP_LOGE(TAG, "Unable to create transport");
            if (++retry_critical_attempts > MAX_CRITICAL_RETRY_ATTEMPTS) {
                ESP_LOGE(TAG, "Max retry attempts reached, restarting...");
                restart_with_reason("Max transport creation retry attempts reached");
            }
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }
        retry_critical_attempts = 0;

        if (tls != DISABLED) {
            esp_transport_ssl_set_common_name(transport, stratum_url);
        }

        ESP_LOGI(TAG, "Transport created, connecting to %s:%d", STATUM_MODULE.lastResolvedIp, port);
        esp_err_t err = esp_transport_connect(transport, STATUM_MODULE.lastResolvedIp, port, 5000);
        if (err != ESP_OK)
        {
            retry_attempts++;
            ESP_LOGE(TAG, "Transport unable to connect to %s:%d (errno %d)", stratum_url, port, err);
            SYSTEM_notify_error_info(GLOBAL_STATE, NETWORK_ERROR, NULL);
            // close the transport
            esp_transport_close(transport);
            esp_transport_destroy(transport);
            // instead of restarting, retry this every 5 seconds
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        int sock = esp_transport_get_socket(transport);
        if (sock >= 0) {
            if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tcp_snd_timeout, sizeof(tcp_snd_timeout)) != 0) {
                ESP_LOGE(TAG, "Fail to setsockopt SO_SNDTIMEO");
            }

            if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO , &tcp_rcv_timeout, sizeof(tcp_rcv_timeout)) != 0) {
                ESP_LOGE(TAG, "Fail to setsockopt SO_RCVTIMEO ");
            }
        }

        stratum_reset_uid(GLOBAL_STATE);
        cleanQueue(GLOBAL_STATE);

        ///// Start Stratum Action
        // mining.configure - ID: 1
        int configure_message_id = GLOBAL_STATE->send_uid++;
        STRATUM_V1_configure_version_rolling(transport, configure_message_id, &GLOBAL_STATE->version_mask);

        // mining.subscribe - ID: 2
        int subscribe_message_id = GLOBAL_STATE->send_uid++;
        STRATUM_V1_subscribe(transport, subscribe_message_id, GLOBAL_STATE->asic_model_str);

        char * username = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? \
            GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user : GLOBAL_STATE->SYSTEM_MODULE.pool_user;
        char * password = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? \
            GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_pass : GLOBAL_STATE->SYSTEM_MODULE.pool_pass;

        /*
         * The shipped config templates carry a placeholder payout address, so
         * that a miner flashed and left unconfigured cannot quietly mine to
         * somebody else's wallet. That is deliberate and worth keeping.
         *
         * What is not worth keeping is what the owner sees when they hit it.
         * The pool refuses the authorize and closes the connection, and the
         * only clue is "setup message rejected: 3,unknown" -- which says
         * nothing about the address. Observed on a BC04 that had been flashed
         * from the web flasher and never configured: a sixty-second connect,
         * reject, disconnect, retry loop with no indication why.
         */
        if (NULL != username && NULL != strstr(username, STRATUM_PLACEHOLDER_USER)) {
            ESP_LOGE(TAG, "=====================================================");
            ESP_LOGE(TAG, "The payout address is still the placeholder:");
            ESP_LOGE(TAG, "  %s", username);
            ESP_LOGE(TAG, "The pool will refuse this and close the connection.");
            ESP_LOGE(TAG, "Set your own address in Settings, or as stratumuser");
            ESP_LOGE(TAG, "in config.cvs. Nothing will mine until you do.");
            ESP_LOGE(TAG, "=====================================================");
        }

        int extranonce_message_id = -1;   /* set if we send one, below */
        int authorize_message_id = GLOBAL_STATE->send_uid++;
        // mining.authorize - ID: 3
        STRATUM_V1_authorize(transport, authorize_message_id, username, password);

        // mining.suggest_difficulty - ID: 4
        // STRATUM_V1_suggest_difficulty(GLOBAL_STATE->sock, GLOBAL_STATE->send_uid++, STRATUM_DIFFICULTY);

        /*
         * Publish it now: connected, subscribed, authorized. write_failed is
         * cleared in the same breath so a failure recorded against the
         * session that just ended cannot take down the one replacing it.
         */
        pthread_mutex_lock(&GLOBAL_STATE->transport_lock);
        GLOBAL_STATE->write_failed = false;
        GLOBAL_STATE->drop_requested = false;
        GLOBAL_STATE->transport = transport;
        pthread_mutex_unlock(&GLOBAL_STATE->transport_lock);

        // Everything is set up, lets make sure we don't abandon work unnecessarily.
        GLOBAL_STATE->abandon_work = 0;

        while (1) {
            /*
             * Checked before the read, not after: the share-submit path sets
             * this and then shuts the socket down, so the usual way in is the
             * !line branch below. This catches the other order -- the flag
             * set while a line was already in the buffer -- so a session the
             * submit path has given up on cannot carry on for another ten
             * minutes of reads.
             */
            bool promoting = GLOBAL_STATE->promote_requested
                    && GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback;

            if (GLOBAL_STATE->write_failed || GLOBAL_STATE->drop_requested
                    || promoting) {
                /* Which one fired decides only what gets logged. Both end the
                 * session by the same path on purpose: a drop asked for over
                 * the API is only worth having if it exercises the teardown a
                 * real failure takes. Same reasoning as the secondaries. */
                if (promoting) {
                    ESP_LOGW(TAG, "pool A: ending the fallback session to go back to the primary");
                } else if (GLOBAL_STATE->drop_requested) {
                    ESP_LOGW(TAG, "pool A: dropping the session, asked over the API");
                } else {
                    ESP_LOGW(TAG, "pool A: a share could not be written, dropping the session");
                }
                /* A promotion is not a failed connection. Counting it would
                 * let a flaky fallback reach MAX_RETRY_ATTEMPTS on the
                 * promotion itself and toggle straight back, wiping the share
                 * counters once a minute. */
                if (!promoting) {
                    retry_attempts++;
                }
                stratum_close_connection(GLOBAL_STATE);
                break;
            }

            char * line = STRATUM_V1_receive_jsonrpc_line(GLOBAL_STATE->transport);
            if (!line) {
                bool woken_to_promote = GLOBAL_STATE->promote_requested
                        && GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback;

                /* Not an error when we asked for it: this would otherwise set
                 * is_network_error and light the error LED for a deliberate
                 * switch back to the primary. */
                if (!woken_to_promote) {
                    SYSTEM_notify_error_info(GLOBAL_STATE, NETWORK_ERROR, NULL);
                }
                /* Which of the two it was, because the log is the only record
                 * anyone has afterwards and they mean different things: a
                 * failed write is the peer gone, a quiet socket is this end
                 * giving up on a pool that said nothing. */
                /* Each flag read once, into a local above, because a share
                 * can fail or a drop be asked for in the window between the
                 * read returning and these tests, and the log is the only
                 * record of which it was. */
                if (woken_to_promote) {
                    ESP_LOGW(TAG, "pool A: receive woken to go back to the primary");
                } else if (GLOBAL_STATE->drop_requested) {
                    ESP_LOGW(TAG, "pool A: receive woken by an API drop, dropping the session");
                } else if (GLOBAL_STATE->write_failed) {
                    ESP_LOGW(TAG, "pool A: receive woken by a failed share write, dropping the session");
                } else {
                    ESP_LOGE(TAG, "Failed to receive JSON-RPC line, reconnecting...");
                }
                if (!woken_to_promote) {
                    retry_attempts++;
                }
                stratum_close_connection(GLOBAL_STATE);
                break;
            }

            STATUM_MODULE.isconnected = true;

            ESP_LOGD(TAG, "rx: %s", line);
            STRATUM_V1_parse(&stratum_api_v1_message, line);
            free(line);

            #ifdef STATISTIC_SYSTEM_FEATURE
            SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;
            uint8_t pool_id = 0;
            if (module->is_using_fallback) {
                pool_id = 1;
            }
            #endif

            if (stratum_api_v1_message.method == MINING_NOTIFY) {
                GLOBAL_STATE->SYSTEM_MODULE.work_received++;
                SYSTEM_notify_new_ntime(GLOBAL_STATE, stratum_api_v1_message.mining_notification->ntime);
                if (stratum_api_v1_message.should_abandon_work &&
                    (GLOBAL_STATE->stratum_queue.count > 0 || GLOBAL_STATE->ASIC_jobs_queue[0].count > 0
                        || GLOBAL_STATE->ASIC_jobs_queue[1].count > 0)) {
                    cleanQueue(GLOBAL_STATE);
                }
                /* Non-blocking, for the reason stratum_poolb_task.c gives at
                 * the same spot: cleanQueue() can empty this queue between the
                 * test and the dequeue, and queue_dequeue() then waits forever
                 * on a queue only this task fills -- pool A dead with
                 * isconnected still true, so poolAConnected reports 1 and no
                 * reconnect is ever attempted. The secondary side was fixed;
                 * this one was missed. A NULL just means someone else already
                 * made room. */
                if (GLOBAL_STATE->stratum_queue.count == QUEUE_SIZE) {
                    mining_notify * next_notify_json_str =
                        (mining_notify *) queue_dequeue_timeout(&GLOBAL_STATE->stratum_queue, 0);
                    if (next_notify_json_str != NULL) {
                        STRATUM_V1_free_mining_notify(next_notify_json_str);
                    }
                }
                stratum_api_v1_message.mining_notification->difficulty = STATUM_MODULE.stratum_difficulty;
                SYSTEM_notify_get_work(GLOBAL_STATE);

                #ifdef STATISTIC_SYSTEM_FEATURE
                statistic_notice_getwork(&(GLOBAL_STATE->STATISTIC_MODULE), pool_id);
                #endif

                queue_enqueue(&GLOBAL_STATE->stratum_queue, stratum_api_v1_message.mining_notification);
                /*
                 * The queue owns it now, so stop holding a pointer to it.
                 * stratum_poolb_task.c has always done this; this path did
                 * not, and the message struct is static and reused for every
                 * line. Any later line that left `method` reading MINING_NOTIFY
                 * without setting a fresh notification would write through
                 * this dangling pointer and enqueue it a second time -- a
                 * use-after-free and then a double free when create_jobs
                 * released what it took for the previous job.
                 *
                 * The parser no longer leaves a stale method behind, so this
                 * is the second line of defence rather than the fix. It is
                 * two lines and it turns that whole class of mistake into a
                 * NULL dereference, which fails loudly instead of corrupting
                 * the heap.
                 */
                stratum_api_v1_message.mining_notification = NULL;
                //decode_mining_notification(GLOBAL_STATE, stratum_api_v1_message.mining_notification);
            } else if (stratum_api_v1_message.method == MINING_SET_DIFFICULTY) {
                /*
                 * Always take the value; only announce it when it moves.
                 *
                 * This used to do both inside the comparison, and the two
                 * variables do not start life agreeing: STATUM_MODULE begins
                 * at 8192 while GLOBAL_STATE->stratum_difficulty begins at
                 * zero. A pool whose first mining.set_difficulty is exactly
                 * 8192 -- an ordinary default -- therefore matched, the
                 * assignment was skipped, and create_jobs_task went on
                 * building every job at difficulty zero. Every nonce the chip
                 * found then cleared that bar and was submitted, and the pool
                 * rejected nearly all of them.
                 */
                bool diff_changed =
                    (stratum_api_v1_message.new_difficulty != STATUM_MODULE.stratum_difficulty) ||
                    (GLOBAL_STATE->stratum_difficulty != stratum_api_v1_message.new_difficulty);

                GLOBAL_STATE->stratum_difficulty = stratum_api_v1_message.new_difficulty;
                STATUM_MODULE.stratum_difficulty = stratum_api_v1_message.new_difficulty;

                if (diff_changed) {
                    ESP_LOGI(TAG, "Set stratum difficulty: %ld", STATUM_MODULE.stratum_difficulty);
                    #ifdef STATISTIC_SYSTEM_FEATURE
                    statistic_pool_set_diff(&(GLOBAL_STATE->STATISTIC_MODULE), pool_id, STATUM_MODULE.stratum_difficulty);
                    #endif
                }
                GLOBAL_STATE->new_set_mining_difficulty_msg = true;
            } else if (stratum_api_v1_message.method == MINING_SET_VERSION_MASK ||
                    stratum_api_v1_message.method == STRATUM_RESULT_VERSION_MASK) {
                // 1fffe000
                ESP_LOGD(TAG, "Set version mask: %08lx", stratum_api_v1_message.version_mask);
                GLOBAL_STATE->version_mask = stratum_api_v1_message.version_mask;
                GLOBAL_STATE->new_stratum_version_rolling_msg = true;
            } else if (stratum_api_v1_message.method == STRATUM_RESULT_SUBSCRIBE) {
                ESP_LOGI(TAG, "Initial Extranonce received - extranonce_str: %s, extranonce_2_len: %u", 
                    stratum_api_v1_message.extranonce_str, stratum_api_v1_message.extranonce_2_len);
                if(xSemaphoreTake(GLOBAL_STATE->global_parameter_mutex, pdMS_TO_TICKS(2000))){
                    if(NULL != GLOBAL_STATE->extranonce_str){
                        free(GLOBAL_STATE->extranonce_str);
                    }
                    GLOBAL_STATE->extranonce_str = strdup(stratum_api_v1_message.extranonce_str);
                    GLOBAL_STATE->extranonce_2_len = stratum_api_v1_message.extranonce_2_len;
                    xSemaphoreGive(GLOBAL_STATE->global_parameter_mutex);
                }else{
                    ESP_LOGE(TAG, "Failed to get the global_parameter_mutex.");
                }
            } else if(stratum_api_v1_message.method == MINING_SET_EXTRANONCE){
                ESP_LOGI(TAG, "Dynamic Extranonce updated - extranonce_str: %s, extranonce_2_len: %u",
                    stratum_api_v1_message.extranonce_str, stratum_api_v1_message.extranonce_2_len);
                if(xSemaphoreTake(GLOBAL_STATE->global_parameter_mutex, pdMS_TO_TICKS(2000))){
                    if(NULL != GLOBAL_STATE->extranonce_str){
                        free(GLOBAL_STATE->extranonce_str);
                    }
                    GLOBAL_STATE->extranonce_str = strdup(stratum_api_v1_message.extranonce_str);
                    GLOBAL_STATE->extranonce_2_len = stratum_api_v1_message.extranonce_2_len;
                    xSemaphoreGive(GLOBAL_STATE->global_parameter_mutex);
                }else{
                    ESP_LOGE(TAG, "Failed to get the global_parameter_mutex.");
                }
            }else if (stratum_api_v1_message.method == CLIENT_RECONNECT) {
                ESP_LOGE(TAG, "Pool requested client reconnect...");
                stratum_close_connection(GLOBAL_STATE);
                /* This break leaves the loop, so it misses the free at the
                 * bottom. Once per reconnect is not much, but it is the one
                 * path out of here that is not the loop condition. */
                STRATUM_V1_free_message(&stratum_api_v1_message);
                break;
            } else if (stratum_api_v1_message.method == STRATUM_RESULT) {
                if (stratum_api_v1_message.response_success) {
                    ESP_LOGD(TAG, "message result accepted");
                    SYSTEM_notify_accepted_share(GLOBAL_STATE);
                    #ifdef STATISTIC_SYSTEM_FEATURE
                    statistic_notice_share_accept(&(GLOBAL_STATE->STATISTIC_MODULE), pool_id);
                    #endif
                } else {
                    if(NULL != stratum_api_v1_message.error_str){
                        ESP_LOGW(TAG, "message result rejected: %lld,%s", stratum_api_v1_message.message_id, stratum_api_v1_message.error_str);
                        SYSTEM_notify_rejected_share(GLOBAL_STATE, stratum_api_v1_message.error_str);
                    } else {
                        ESP_LOGW(TAG, "message result rejected: %lld",stratum_api_v1_message.message_id);
                        SYSTEM_notify_rejected_share(GLOBAL_STATE, NULL);
                    }
                    #ifdef STATISTIC_SYSTEM_FEATURE
                    statistic_notice_share_reject(&(GLOBAL_STATE->STATISTIC_MODULE), pool_id);
                    #endif
                }
            } else if (stratum_api_v1_message.method == STRATUM_RESULT_SETUP) {
                // Reset retry attempts after successfully receiving data.
                retry_attempts = 0;
                if (stratum_api_v1_message.response_success) {
                    ESP_LOGI(TAG, "setup message accepted (id=%lld)", stratum_api_v1_message.message_id);
                    if (stratum_api_v1_message.message_id == authorize_message_id) {
                        // authorize 成功后发送 extranonce.subscribe（仅一次）
                        if (extranonce_subscribe) {
                            extranonce_message_id = GLOBAL_STATE->send_uid++;
                            /*
                             * Under the lock. This is the only other write to
                             * the published handle, and without the lock it
                             * can land in the middle of a share submit from
                             * ASIC_result_task: on plain TCP lwIP fails one of
                             * the two writes, and if the loser is the submit
                             * then write_failed is set and a perfectly healthy
                             * session is torn down -- a failed write only
                             * means "the peer is gone" while this task is the
                             * sole writer. On TLS the two interleave inside
                             * one mbedtls context and corrupt a record.
                             */
                            pthread_mutex_lock(&GLOBAL_STATE->transport_lock);
                            if (GLOBAL_STATE->transport != NULL) {
                                STRATUM_V1_extranonce_subscribe(GLOBAL_STATE->transport, extranonce_message_id);
                            }
                            pthread_mutex_unlock(&GLOBAL_STATE->transport_lock);
                        }
                    }
                } else {
                    /* Name the method. A bare id tells the owner nothing, and
                     * "3" is the one they most need to understand. */
                    const char *which = "unknown message";
                    if (stratum_api_v1_message.message_id == authorize_message_id) {
                        which = "mining.authorize -- the pool refused this "
                                "worker/payout address";
                    } else if (stratum_api_v1_message.message_id == subscribe_message_id) {
                        which = "mining.subscribe";
                    } else if (stratum_api_v1_message.message_id == configure_message_id) {
                        which = "mining.configure (version rolling)";
                    } else if (stratum_api_v1_message.message_id == extranonce_message_id) {
                        which = "mining.extranonce.subscribe";
                    }

                    /* The parser substitutes "unknown" when the pool sends
                     * error:null, which is exactly the case here. Printing it
                     * verbatim reads as though the pool said something. */
                    if(NULL != stratum_api_v1_message.error_str &&
                       0 != strcmp(stratum_api_v1_message.error_str, "unknown")) {
                        ESP_LOGE(TAG, "pool rejected %s (id %lld): %s",
                                 which, stratum_api_v1_message.message_id,
                                 stratum_api_v1_message.error_str);
                    } else {
                        ESP_LOGE(TAG, "pool rejected %s (id %lld), no reason given",
                                 which, stratum_api_v1_message.message_id);
                    }
                }
            }

            /*
             * The strings this parse allocated. stratum_api_v1_message is
             * reused for every line pool A reads, so each rejection reason
             * and each extranonce was overwritten and lost -- a steady drain
             * whenever a pool is refusing shares, on a board with about a
             * kilobyte of internal RAM to spare.
             *
             * Safe to free here: both extranonce handlers above strdup what
             * they keep rather than taking the pointer, and the mining
             * notification -- which the queue does take -- is left alone.
             */
            STRATUM_V1_free_message(&stratum_api_v1_message);
        }
        STATUM_MODULE.isconnected = false;
    }
    vTaskDelete(NULL);
}
