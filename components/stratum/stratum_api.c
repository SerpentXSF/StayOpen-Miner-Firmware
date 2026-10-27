/******************************************************************************
 *  *
 * References:
 *  1. Stratum Protocol - [link](https://reference.cash/mining/stratum-protocol)
 *****************************************************************************/

#include "stratum_api.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "esp_crt_bundle.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>

#define BUFFER_SIZE (2300)
#define TRANSPORT_RECV_TIMEOUT_MS 60000

/* How long pool A may be silent before the session is given up on.
 *
 * This is the ten minutes that stratum_task.c already asks for with
 * SO_RCVTIMEO and that global_state.h already describes. It had no effect:
 * esp_transport_read's own poll expires after TRANSPORT_RECV_TIMEOUT_MS and
 * returns 0, which this file read as a close. Counting those timeouts here is
 * what makes the documented limit the one that actually applies. */
#define STRATUM_IDLE_LIMIT_MS (10 * 60 * 1000)

/*
 * Longest line this file will accumulate from a pool.
 *
 * Without a cap, a pool that sends bytes and never sends a newline grows the
 * accumulator forever: the first few kilobytes come out of the internal heap
 * that already bottoms out at a few hundred bytes free on every boot, and when
 * realloc finally fails realloc_json_buffer() calls esp_restart(). So "never
 * send \n" was a remote reboot, repeatable on every reconnect, needing no
 * credentials on a plaintext stratum+tcp session.
 *
 * A real mining.notify with the full merkle branch list runs to roughly 3 KB.
 * 16 KB leaves several times that and still bounds the damage.
 */
#define STRATUM_MAX_LINE (16 * 1024)

/*
 * Longest job id accepted from a pool.
 *
 * mining.submit is built with the pool's own job id in it, into a fixed
 * BUFFER_SIZE buffer on the caller's stack. Real pools use 8 to 64 characters.
 * The snprintf bounds below are what actually prevent the overflow; this cap
 * refuses the job outright so the miner does not accept work it will never be
 * able to submit, and says so in the log rather than failing later at the
 * share.
 */
#define STRATUM_MAX_JOB_ID 128

/* Stratum itself nests about four deep: the notify params array holding the
 * merkle branch array. Sixteen leaves room for anything a real pool sends and
 * stops a line that would recurse cJSON into the task's stack canary. */
#define STRATUM_MAX_JSON_DEPTH 16
static const char * TAG = "stratum_api";

static char * json_rpc_buffer = NULL;
static size_t json_rpc_buffer_size = 0;

static void debug_stratum_tx(const char *);
int _parse_stratum_subscribe_result_message(const char * result_json_str, char ** extranonce, int * extranonce2_len);

esp_transport_handle_t STRATUM_V1_transport_init(tls_mode tls, char *cert)
{
    esp_transport_handle_t transport;

    if (tls == DISABLED)
    {
        ESP_LOGI(TAG, "TLS disabled, using TCP transport");
        transport = esp_transport_tcp_init();
    }
    else
    {
        ESP_LOGI(TAG, "Using TLS transport");
        transport = esp_transport_ssl_init();
        if (transport == NULL) {
            ESP_LOGE(TAG, "Failed to initialize SSL transport");
            return NULL;
        }
        switch (tls) {
            case BUNDLED_CRT:
                ESP_LOGI(TAG, "Using default cert bundle");
                esp_transport_ssl_crt_bundle_attach(transport, esp_crt_bundle_attach);
                break;
            case CUSTOM_CRT:
                ESP_LOGI(TAG, "Using custom cert");
                if (cert == NULL) {
                    ESP_LOGE(TAG, "Error: no TLS certificate");
                    esp_transport_destroy(transport);
                    return NULL;
                }
                esp_transport_ssl_set_cert_data(transport, cert, strlen(cert));
                break;
            default:
                ESP_LOGE(TAG, "Invalid TLS mode: %d", tls);
                esp_transport_destroy(transport);
                return NULL;
        }
    }
    return transport;
}

void STRATUM_V1_initialize_buffer()
{
    if(json_rpc_buffer == NULL)
    {
        json_rpc_buffer = malloc(BUFFER_SIZE);
        json_rpc_buffer_size = BUFFER_SIZE;
        if (json_rpc_buffer == NULL) {
            printf("Error: Failed to allocate memory for buffer\n");
            exit(1);
        }
        memset(json_rpc_buffer, 0, BUFFER_SIZE);
    }
}

void cleanup_stratum_buffer()
{
    if(json_rpc_buffer)
    {
        free(json_rpc_buffer);
        json_rpc_buffer = NULL;
    }
}

static void realloc_json_buffer(size_t len)
{
    size_t old, new;

    old = strlen(json_rpc_buffer);
    new = old + len + 1;

    if (new < json_rpc_buffer_size) {
        return;
    }

    new = new + (BUFFER_SIZE - (new % BUFFER_SIZE));
    void * new_sockbuf = realloc(json_rpc_buffer, new);

    if (new_sockbuf == NULL) {
        fprintf(stderr, "Error: realloc failed in recalloc_sock()\n");
        ESP_LOGI(TAG, "Restarting System because of ERROR: realloc failed in recalloc_sock");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    json_rpc_buffer = new_sockbuf;
    memset(json_rpc_buffer + old, 0, new - old);
    json_rpc_buffer_size = new;
}

char * STRATUM_V1_receive_jsonrpc_line(esp_transport_handle_t transport)
{
    if (json_rpc_buffer == NULL) {
        STRATUM_V1_initialize_buffer();
        if (json_rpc_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to initialize JSON-RPC buffer");
            return NULL;
        }
    }
    char *line, *tok = NULL;
    char recv_buffer[BUFFER_SIZE];
    int nbytes;
    size_t buflen = 0;
    int idle_ms = 0;

    if (!strstr(json_rpc_buffer, "\n")) {
        do {
            memset(recv_buffer, 0, BUFFER_SIZE);
            nbytes = esp_transport_read(transport, recv_buffer, BUFFER_SIZE - 1, TRANSPORT_RECV_TIMEOUT_MS);
            if (nbytes < 0) {
                ESP_LOGI(TAG, "Error: transport read failed (code: %d)", nbytes);
                if (json_rpc_buffer) {
                    free(json_rpc_buffer);
                    json_rpc_buffer = NULL;
                }
                return NULL;
            }
            else if (nbytes == 0)
            {
                /* A timeout, not a close. esp_transport_read returns
                 * ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT (0) when its poll
                 * expires and ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN (-1)
                 * when the peer really goes away, so a FIN is handled by the
                 * branch above. Treating 0 as a close tore the session down
                 * after 60 s of quiet, discarding every in-flight job, against
                 * any pool that does not notify at least once a minute. */
                idle_ms += TRANSPORT_RECV_TIMEOUT_MS;
                if (idle_ms >= STRATUM_IDLE_LIMIT_MS) {
                    ESP_LOGW(TAG, "no data from the pool in %d minutes; "
                                  "giving up on this session",
                             STRATUM_IDLE_LIMIT_MS / 60000);
                    if (json_rpc_buffer) {
                        free(json_rpc_buffer);
                        json_rpc_buffer = NULL;
                    }
                    return NULL;
                }
                ESP_LOGD(TAG, "pool quiet for %d s, still connected",
                         idle_ms / 1000);
                continue;
            }

            idle_ms = 0;

            /*
             * Capped before growing. Without this, a pool that sends bytes and
             * never sends a newline grows the accumulator until realloc fails,
             * at which point realloc_json_buffer() restarts the board -- a
             * remote reboot, repeatable on every reconnect, from any pool or
             * anyone on a plaintext path. See STRATUM_MAX_LINE.
             */
            if (strlen(json_rpc_buffer) + (size_t) nbytes + 1 > STRATUM_MAX_LINE) {
                ESP_LOGW(TAG, "pool sent %u bytes with no newline; dropping "
                              "the connection rather than growing past %d",
                         (unsigned) (strlen(json_rpc_buffer) + (size_t) nbytes),
                         STRATUM_MAX_LINE);
                free(json_rpc_buffer);
                json_rpc_buffer = NULL;
                return NULL;
            }

            realloc_json_buffer(nbytes);
            strncat(json_rpc_buffer, recv_buffer, nbytes);
        } while (!strstr(json_rpc_buffer, "\n"));
    }
    buflen = strlen(json_rpc_buffer);
    tok = strtok(json_rpc_buffer, "\n");
    /*
     * strtok() returns NULL when the buffer holds nothing but delimiters, so
     * a pool sending a bare newline landed in strdup(NULL). The loop above
     * exits as soon as it sees a newline, which a blank line satisfies, so
     * getting here with nothing to tokenise needs no misbehaviour from the
     * pool beyond an empty line.
     *
     * The reentrant copy used for pool B does not have this -- it splits on
     * the newline it found and checks the result -- which is why only the
     * primary pool could take the miner down this way.
     *
     * The caller already treats NULL as a dropped connection and reconnects,
     * which is the right answer to a pool talking nonsense.
     */
    if (tok == NULL) {
        ESP_LOGW(TAG, "pool sent a line with no content; dropping the "
                      "connection rather than parsing nothing");
        strcpy(json_rpc_buffer, "");
        return NULL;
    }
    line = strdup(tok);
    int len = strlen(line);
    if (buflen > len + 1)
        memmove(json_rpc_buffer, json_rpc_buffer + len + 1, buflen - len + 1);
    else
        strcpy(json_rpc_buffer, "");
    return line;
}

/*
 * One array element as a string, or NULL if it is not there or is not one.
 *
 * Everything this file parses arrives from a pool, and cJSON_GetArrayItem()
 * answers NULL for an index that does not exist. Reaching straight through it
 * for ->valuestring is how a short params array became a crash.
 */
/* Deepest [ or { nesting in a line, ignoring brackets inside string
 * literals so a job id full of them cannot trip the limit. Returns early once
 * the limit is passed; there is no reason to measure how much worse it gets. */
static int json_nesting_exceeds(const char * text, int limit)
{
    int depth = 0;
    int in_string = 0;

    for (const char * p = text; *p != '\0'; p++) {
        if (in_string) {
            if (*p == '\\' && p[1] != '\0') {
                p++;                      /* skip the escaped character */
            } else if (*p == '"') {
                in_string = 0;
            }
            continue;
        }
        if (*p == '"') {
            in_string = 1;
        } else if (*p == '[' || *p == '{') {
            if (++depth > limit) {
                return depth;
            }
        } else if (*p == ']' || *p == '}') {
            if (depth > 0) {
                depth--;
            }
        }
    }
    return 0;
}

static const char * str_at(cJSON * array, int index)
{
    cJSON * item = cJSON_IsArray(array) ? cJSON_GetArrayItem(array, index) : NULL;
    return (cJSON_IsString(item) && item->valuestring != NULL) ? item->valuestring : NULL;
}

void STRATUM_V1_parse(StratumApiV1Message * message, const char * stratum_json)
{
    /* Before cJSON recurses. See json_nesting_exceeds(): the parser takes a
     * stack frame per level and these tasks have 8 KB, so a deeply nested
     * line from the pool reaches the stack canary and panics the miner. */
    if (json_nesting_exceeds(stratum_json, STRATUM_MAX_JSON_DEPTH)) {
        ESP_LOGW(TAG, "ignoring a stratum line nested deeper than %d levels",
                 STRATUM_MAX_JSON_DEPTH);
        message->method = STRATUM_UNKNOWN;
        return;
    }

    cJSON * json = cJSON_Parse(stratum_json);

    cJSON * id_json = cJSON_GetObjectItem(json, "id");
    int64_t parsed_id = -1;
    if (id_json != NULL && cJSON_IsNumber(id_json)) {
        parsed_id = id_json->valueint;
    }
    message->message_id = parsed_id;
    message->response_success = false;

    cJSON * method_json = cJSON_GetObjectItem(json, "method");
    stratum_method result = STRATUM_UNKNOWN;

    //if there is a method, then use that to decide what to do
    if (method_json != NULL && cJSON_IsString(method_json)) {
        if (strcmp("mining.notify", method_json->valuestring) == 0) {
            result = MINING_NOTIFY;
        } else if (strcmp("mining.set_difficulty", method_json->valuestring) == 0) {
            result = MINING_SET_DIFFICULTY;
        } else if (strcmp("mining.set_version_mask", method_json->valuestring) == 0) {
            result = MINING_SET_VERSION_MASK;
        } else if (strcmp("client.reconnect", method_json->valuestring) == 0) {
            result = CLIENT_RECONNECT;
        } else if (strcmp("mining.set_extranonce", method_json->valuestring) == 0) {
            /*update the extranonce 1, provided by nicehash*/
            result = MINING_SET_EXTRANONCE;
        }else {
            ESP_LOGI(TAG, "unhandled method in stratum message: %s", stratum_json);
        }

    //if there is no method, then it is a result
    } else {
        // parse results
        cJSON * result_json = cJSON_GetObjectItem(json, "result");
        cJSON * error_json = cJSON_GetObjectItem(json, "error");
        cJSON * reject_reason_json = cJSON_GetObjectItem(json, "reject-reason");

        #if 0
        ESP_LOGI(TAG, "recv %s", stratum_json);
        if(NULL != result_json){
            ESP_LOGI(TAG, "result %s", cJSON_PrintUnformatted(result_json));
        }
        if(NULL != error_json){
            ESP_LOGI(TAG, "error %s", cJSON_PrintUnformatted(error_json));
        }
        if(NULL != reject_reason_json){
            ESP_LOGI(TAG, "reject_reason %s", cJSON_PrintUnformatted(reject_reason_json));
        }
        #endif

        // if the result is null, then it's a fail
        if (result_json == NULL) {
            message->response_success = false;
            message->error_str = strdup("unknown");
            
        // if it's an error, then it's a fail
        } else if (error_json != NULL && !cJSON_IsNull(error_json) && parsed_id != STRATUM_ID_SUBSCRIBE) {
            if (parsed_id < 5) {
                result = STRATUM_RESULT_SETUP;
            } else {
                result = STRATUM_RESULT;
            }

            if (cJSON_IsArray(error_json)) {
                int len = cJSON_GetArraySize(error_json);
                if (len >= 2) {
                    cJSON * error_msg = cJSON_GetArrayItem(error_json, 1);
                    if (cJSON_IsString(error_msg)) {
                        message->error_str = strdup(cJSON_GetStringValue(error_msg));
                    }
                }
                message->response_success = false;
            }
            else{
                message->error_str = cJSON_PrintUnformatted(error_json);
            }
            //ESP_LOGW(TAG, "recv %s", stratum_json); 
        // if the result is a boolean, then parse it
        } else if (cJSON_IsBool(result_json)) {
            if (parsed_id < 5) {
                result = STRATUM_RESULT_SETUP;
            } else {
                result = STRATUM_RESULT;
            }

            if (cJSON_IsTrue(result_json)) {
                message->response_success = true;
            } else {
                message->response_success = false;
                /* One allocation, not two. This used to strdup("unknown") and
                 * then overwrite the pointer with the pool's reason without
                 * freeing it, leaking 8 bytes of internal DRAM per rejected
                 * share against any pool that sends reject-reason. */
                message->error_str =
                    cJSON_IsString(reject_reason_json)
                        ? strdup(cJSON_GetStringValue(reject_reason_json))
                        : strdup("unknown");
                ESP_LOGW(TAG, "recv %s", stratum_json);          
            }
        
        //if the id is STRATUM_ID_SUBSCRIBE parse it
        } else if (parsed_id == STRATUM_ID_SUBSCRIBE) {
            /*
             * Everything is validated before anything is assigned, and a bad
             * result resets the method instead of jumping past the assignment.
             *
             * The two `goto done` here landed AFTER `message->method = result`
             * further down, so the message kept the PREVIOUS line's method.
             * Both callers reuse one message struct for every line, so pool A
             * would act on a stale MINING_NOTIFY and re-enqueue a
             * mining_notification the work queue had already freed -- a
             * use-after-free, then a double free when create_jobs released
             * what it thought was the old one. The secondary pools NULL their
             * notification pointer after enqueuing, so they got a NULL
             * dereference instead. Either way the board goes down.
             *
             * This needed no malice: a pool refusing mining.subscribe with the
             * standard {"id":2,"result":null,"error":[20,...]} reaches this
             * branch with a cJSON null, GetArrayItem answers NULL, and the old
             * code jumped. A banned address was enough to do it.
             *
             * str_at() also covers the separate crash of reading
             * ->valuestring off a non-string: {"id":2,"result":[1,2,3]} was
             * strlen(NULL).
             */
            cJSON * extranonce2_len_json = cJSON_GetArrayItem(result_json, 2);
            const char * p_extranonce = str_at(result_json, 1);

            if (!cJSON_IsNumber(extranonce2_len_json) || p_extranonce == NULL) {
                ESP_LOGE(TAG, "ignoring a malformed mining.subscribe result: "
                              "extranonce missing or not a string, or its "
                              "length is not a number");
                message->response_success = false;
                message->method = STRATUM_UNKNOWN;
                cJSON_Delete(json);
                return;
            }

            result = STRATUM_RESULT_SUBSCRIBE;
            message->extranonce_2_len = extranonce2_len_json->valueint;
            /* NULLed by STRATUM_V1_free_message, so this is either NULL or
             * ours; freed rather than overwritten, which leaked a subscribe's
             * worth on every reconnect. */
            free(message->extranonce_str);
            message->extranonce_str = strdup(p_extranonce);
            message->response_success = true;

            //print the extranonce_str
            ESP_LOGI(TAG, "extranonce_str: %s", message->extranonce_str);
            ESP_LOGI(TAG, "extranonce_2_len: %d", message->extranonce_2_len);

        //if the id is STRATUM_ID_CONFIGURE parse it
        } else if (parsed_id == STRATUM_ID_CONFIGURE) {
            /* cJSON_IsString, not just non-NULL: a result of
             * {"version-rolling.mask": true} has a NULL valuestring, and
             * strtoul(NULL) panics. */
            cJSON * mask = cJSON_GetObjectItem(result_json, "version-rolling.mask");
            if (cJSON_IsString(mask) && mask->valuestring != NULL) {
                result = STRATUM_RESULT_VERSION_MASK;
                message->version_mask = strtoul(mask->valuestring, NULL, 16);
                ESP_LOGI(TAG, "Set version mask: %08lx", message->version_mask);
            } else {
                ESP_LOGI(TAG, "error setting version mask: %s", stratum_json);
            }

        } else {
            ESP_LOGI(TAG, "unhandled result in stratum message: %s", stratum_json);
        }
    }

    message->method = result;

    if (message->method == MINING_NOTIFY) {

        /*
         * Everything here comes off the wire from a pool, and none of it was
         * checked. cJSON_GetArrayItem() returns NULL for a short or missing
         * params array and every one of these dereferenced it straight away,
         * so a malformed mining.notify took the miner down. Worse, a params
         * array with more than MAX_MERKLE_BRANCHES entries called abort() --
         * a remote party deciding to reboot somebody's miner.
         *
         * A pool that sends nonsense gets its job ignored. The method is reset
         * to STRATUM_UNKNOWN, which both callers already skip, so a bad job is
         * dropped rather than mined or crashed on.
         */
        cJSON * params = cJSON_GetObjectItem(json, "params");
        const char * p_job   = str_at(params, 0);
        const char * p_prev  = str_at(params, 1);
        const char * p_cb1   = str_at(params, 2);
        const char * p_cb2   = str_at(params, 3);
        cJSON * merkle_branch = cJSON_IsArray(params) ? cJSON_GetArrayItem(params, 4) : NULL;

        /* 5, 6 and 7 are checked here, with the rest, and not where they are
         * used further down. They used to be read as
         * cJSON_GetArrayItem(params, 5)->valuestring with no check at all: a
         * notify carrying five params returns NULL from GetArrayItem, and a
         * non-string item has a NULL valuestring, so either one panics in
         * strtoul. Validating them after new_work is malloc'd and the four
         * strings are strdup'd would mean leaking all five on the way out, so
         * they belong in this guard. */
        const char * p_ver   = str_at(params, 5);
        const char * p_tgt   = str_at(params, 6);
        const char * p_time  = str_at(params, 7);

        if (NULL == p_job || NULL == p_prev || NULL == p_cb1 || NULL == p_cb2 ||
            NULL == p_ver || NULL == p_tgt || NULL == p_time ||
            !cJSON_IsArray(merkle_branch)) {
            ESP_LOGW(TAG, "ignoring a malformed mining.notify: params missing "
                          "or not strings");
            message->method = STRATUM_UNKNOWN;
            cJSON_Delete(json);
            return;
        }

        /*
         * The guard above checks types and said nothing about length. The job
         * id is copied into every mining.submit built for this job, so an
         * over-long one is a job whose shares can never be sent -- refused
         * here, where it can be said plainly, rather than at each share.
         */
        if (strlen(p_job) > STRATUM_MAX_JOB_ID) {
            ESP_LOGW(TAG, "ignoring a mining.notify: job id is %u characters "
                          "and the limit is %d",
                     (unsigned) strlen(p_job), STRATUM_MAX_JOB_ID);
            message->method = STRATUM_UNKNOWN;
            cJSON_Delete(json);
            return;
        }

        int n_branches = cJSON_GetArraySize(merkle_branch);
        if (n_branches < 0 || n_branches > MAX_MERKLE_BRANCHES) {
            ESP_LOGW(TAG, "ignoring a mining.notify with %d merkle branches; "
                          "the limit is %d", n_branches, MAX_MERKLE_BRANCHES);
            message->method = STRATUM_UNKNOWN;
            cJSON_Delete(json);
            return;
        }

        for (int i = 0; i < n_branches; i++) {
            if (NULL == str_at(merkle_branch, i)) {
                ESP_LOGW(TAG, "ignoring a mining.notify: merkle branch %d is "
                              "not a string", i);
                message->method = STRATUM_UNKNOWN;
                cJSON_Delete(json);
                return;
            }
        }

        mining_notify * new_work = malloc(sizeof(mining_notify));
        if (NULL == new_work) {
            message->method = STRATUM_UNKNOWN;
            cJSON_Delete(json);
            return;
        }
        new_work->job_id = strdup(p_job);
        new_work->prev_block_hash = strdup(p_prev);
        new_work->coinbase_1 = strdup(p_cb1);
        new_work->coinbase_2 = strdup(p_cb2);
        new_work->n_merkle_branches = n_branches;
        new_work->merkle_branches = malloc(HASH_SIZE * (n_branches ? n_branches : 1));
        for (int i = 0; i < n_branches; i++) {
            hex2bin(str_at(merkle_branch, i),
                    new_work->merkle_branches + HASH_SIZE * i, HASH_SIZE);
        }

        new_work->version = strtoul(p_ver, NULL, 16);
        new_work->target = strtoul(p_tgt, NULL, 16);
        new_work->ntime = strtoul(p_time, NULL, 16);

        message->mining_notification = new_work;

        // params can be varible length
        int paramsLength = cJSON_GetArraySize(params);
        int value = cJSON_IsTrue(cJSON_GetArrayItem(params, paramsLength - 1));
        message->should_abandon_work = value;
    } else if (message->method == MINING_SET_DIFFICULTY) {
        cJSON * params = cJSON_GetObjectItem(json, "params");
        /* An empty params array, or a non-number in it, used to be a NULL
         * dereference here -- the same defect the notify path was hardened
         * against, in a method that is just as easy to send. */
        cJSON * diff_json = cJSON_IsArray(params)
                          ? cJSON_GetArrayItem(params, 0) : NULL;
        if (!cJSON_IsNumber(diff_json)) {
            ESP_LOGW(TAG, "ignoring a malformed mining.set_difficulty: "
                          "params[0] missing or not a number");
            message->method = STRATUM_UNKNOWN;
            cJSON_Delete(json);
            return;
        }

        message->new_difficulty = diff_json->valueint;
    } else if (message->method == MINING_SET_VERSION_MASK) {
        cJSON * params = cJSON_GetObjectItem(json, "params");
        const char * p_mask = str_at(params, 0);
        if (NULL == p_mask) {
            ESP_LOGW(TAG, "ignoring a malformed mining.set_version_mask: "
                          "params[0] missing or not a string");
            message->method = STRATUM_UNKNOWN;
            cJSON_Delete(json);
            return;
        }
        message->version_mask = strtoul(p_mask, NULL, 16);

    } else if (message->method == MINING_SET_EXTRANONCE) {
        /*
         * Validated the same way as the subscribe result above. Three
         * separate faults lived in this block:
         *
         *  - params [1, 2] gave a non-string item whose ->valuestring is
         *    NULL, read straight into strlen(). A panic on both pool paths.
         *  - params with one element, or a params that is not an array at
         *    all, left the method set to MINING_SET_EXTRANONCE with
         *    extranonce_str still NULL. Pool A's handler calls strdup() on
         *    it, so that is a panic too; the secondary path happened to
         *    check.
         *  - the two cJSON_PrintUnformatted() results in the old error paths
         *    were never freed, so repeating a malformed message leaked
         *    internal RAM a few dozen bytes at a time -- out of the pool that
         *    already runs down to a few hundred bytes on every boot.
         */
        cJSON * params = cJSON_GetObjectItem(json, "params");
        cJSON * extranonce2_len_json = cJSON_IsArray(params)
                                           ? cJSON_GetArrayItem(params, 1)
                                           : NULL;
        const char * p_extranonce = str_at(params, 0);

        if (!cJSON_IsNumber(extranonce2_len_json) || p_extranonce == NULL) {
            ESP_LOGW(TAG, "ignoring a malformed mining.set_extranonce: "
                          "extranonce missing or not a string, or its length "
                          "is not a number");
            message->response_success = false;
            message->method = STRATUM_UNKNOWN;
            cJSON_Delete(json);
            return;
        }

        message->extranonce_2_len = extranonce2_len_json->valueint;
        free(message->extranonce_str);
        message->extranonce_str = strdup(p_extranonce);
        message->response_success = true;
        //print the extranonce_str
        ESP_LOGI(TAG, "extranonce_str: %s", message->extranonce_str);
        ESP_LOGI(TAG, "extranonce_2_len: %d", message->extranonce_2_len);
    }

    /* The `done:` label that used to be here is gone with the last `goto`
     * that reached it. Every one of them skipped the method assignment above,
     * which is what made a malformed line act on the previous line's method;
     * each now resets the method and returns on the spot instead. */
    cJSON_Delete(json);
}

void STRATUM_V1_free_message(StratumApiV1Message * message)
{
    if (message == NULL) {
        return;
    }
    free(message->error_str);
    message->error_str = NULL;
    free(message->extranonce_str);
    message->extranonce_str = NULL;
}

void STRATUM_V1_free_mining_notify(mining_notify * params)
{
    free(params->job_id);
    free(params->prev_block_hash);
    free(params->coinbase_1);
    free(params->coinbase_2);
    free(params->merkle_branches);
    free(params);
}

int _parse_stratum_subscribe_result_message(const char * result_json_str, char ** extranonce, int * extranonce2_len)
{
    cJSON * root = cJSON_Parse(result_json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Unable to parse %s", result_json_str);
        return -1;
    }
    cJSON * result = cJSON_GetObjectItem(root, "result");
    if (result == NULL) {
        ESP_LOGE(TAG, "Unable to parse subscribe result %s", result_json_str);
        return -1;
    }

    cJSON * extranonce2_len_json = cJSON_GetArrayItem(result, 2);
    if (extranonce2_len_json == NULL) {
        ESP_LOGE(TAG, "Unable to parse extranonce2_len: %s", result->valuestring);
        return -1;
    }
    *extranonce2_len = extranonce2_len_json->valueint;

    cJSON * extranonce_json = cJSON_GetArrayItem(result, 1);
    if (extranonce_json == NULL) {
        ESP_LOGE(TAG, "Unable parse extranonce: %s", result->valuestring);
        return -1;
    }
    *extranonce = malloc(strlen(extranonce_json->valuestring) + 1);
    strcpy(*extranonce, extranonce_json->valuestring);

    cJSON_Delete(root);

    return 0;
}

int STRATUM_V1_subscribe(esp_transport_handle_t transport, int send_uid, char * model)
{
    // Subscribe
    char subscribe_msg[BUFFER_SIZE];
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *version = app_desc->version;	
    sprintf(subscribe_msg, "{\"id\": %d, \"method\": \"mining.subscribe\", \"params\": [\"volc/%s/%s\"]}\n", send_uid, model, version);
    debug_stratum_tx(subscribe_msg);

    return esp_transport_write(transport, subscribe_msg, strlen(subscribe_msg), 0);
}

int STRATUM_V1_suggest_difficulty(esp_transport_handle_t transport, int send_uid, uint32_t difficulty)
{
    char difficulty_msg[BUFFER_SIZE];
    sprintf(difficulty_msg, "{\"id\": %d, \"method\": \"mining.suggest_difficulty\", \"params\": [%ld]}\n", send_uid, difficulty);
    debug_stratum_tx(difficulty_msg);

    return esp_transport_write(transport, difficulty_msg, strlen(difficulty_msg), 0);
}

int STRATUM_V1_extranonce_subscribe(esp_transport_handle_t transport, int send_uid)
{
    char extranonce_msg[BUFFER_SIZE];
    sprintf(extranonce_msg, "{\"id\": %d, \"method\": \"mining.extranonce.subscribe\", \"params\": []}\n", send_uid);
    debug_stratum_tx(extranonce_msg);

    return esp_transport_write(transport, extranonce_msg, strlen(extranonce_msg), 0);
}

int STRATUM_V1_authenticate(esp_transport_handle_t transport, int send_uid, const char * username, const char * pass)
{
    char authorize_msg[BUFFER_SIZE];
    /* Owner-supplied rather than pool-supplied, so this is a long worker name
     * corrupting the stack rather than a remote party doing it; bounded all
     * the same, since the cost is one check. */
    int n = snprintf(authorize_msg, sizeof authorize_msg,
            "{\"id\": %d, \"method\": \"mining.authorize\", \"params\": [\"%s\", \"%s\"]}\n", send_uid, username,
            pass);
    if (n < 0 || n >= (int) sizeof authorize_msg) {
        ESP_LOGE(TAG, "worker name and password need %d bytes and the buffer "
                      "is %d; not authorising", n, (int) sizeof authorize_msg);
        return -1;
    }
    debug_stratum_tx(authorize_msg);

    return esp_transport_write(transport, authorize_msg, strlen(authorize_msg), 0);
}

int STRATUM_V1_authorize(esp_transport_handle_t transport, int send_uid, const char * username, const char * pass)
{
    char authorize_msg[BUFFER_SIZE];
    /* Bounded for the same reason as STRATUM_V1_authenticate above. */
    int n = snprintf(authorize_msg, sizeof authorize_msg,
            "{\"id\": %d, \"method\": \"mining.authorize\", \"params\": [\"%s\", \"%s\"]}\n", send_uid, username,
            pass);
    if (n < 0 || n >= (int) sizeof authorize_msg) {
        ESP_LOGE(TAG, "worker name and password need %d bytes and the buffer "
                      "is %d; not authorising", n, (int) sizeof authorize_msg);
        return -1;
    }
    debug_stratum_tx(authorize_msg);

    return esp_transport_write(transport, authorize_msg, strlen(authorize_msg), 0);
}

/// @param transport esp_transport handle to write to
/// @param username The client's user name.
/// @param jobid The job ID for the work being submitted.
/// @param ntime The hex-encoded time value use in the block header.
/// @param extranonce_2 The hex-encoded value of extra nonce 2.
/// @param nonce The hex-encoded nonce value to use in the block header.
int STRATUM_V1_submit_share(esp_transport_handle_t transport, int send_uid, const char * username, const char * jobid,
                            const char * extranonce_2, const uint32_t ntime,
                            const uint32_t nonce, const uint32_t version)
{
    char submit_msg[BUFFER_SIZE];
    /*
     * snprintf, and the truncation checked, because `jobid` and
     * `extranonce_2` come from the pool and `username` from the owner's
     * settings. This was sprintf into a BUFFER_SIZE array on the stack of a
     * task with 6144 bytes of it, so a mining.notify carrying a 3000-character
     * job id wrote the pool's own bytes upward past the array and over this
     * frame's return address on the first share found against that job. The
     * stack canary sits at the low end of the stack and never sees a write
     * going the other way. No credentials needed: stratum+tcp is plaintext, so
     * the pool or anyone on the path between could do it.
     */
    int n = snprintf(submit_msg, sizeof submit_msg,
            "{\"id\": %d, \"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%08lx\", \"%08lx\", \"%08lx\"]}\n",
            send_uid, username, jobid, extranonce_2, ntime, nonce, version);
    if (n < 0 || n >= (int) sizeof submit_msg) {
        ESP_LOGW(TAG, "not submitting a share: the message needs %d bytes and "
                      "the buffer is %d -- check the job id and worker name",
                 n, (int) sizeof submit_msg);
        return -1;
    }
    debug_stratum_tx(submit_msg);

    return esp_transport_write(transport, submit_msg, strlen(submit_msg), 0);
}

int STRATUM_V1_submit_ltc_share(esp_transport_handle_t transport, int send_uid, const char * username, const char * jobid,
                            const char * extranonce_2, const uint32_t ntime,
                            const uint32_t nonce)
{
    char submit_msg[BUFFER_SIZE];
    /* Bounded for the same reason as the Bitcoin variant above. */
    int n = snprintf(submit_msg, sizeof submit_msg,
            "{\"id\": %d, \"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%08lx\", \"%08lx\"]}\n",
            send_uid, username, jobid, extranonce_2, ntime, nonce);
    if (n < 0 || n >= (int) sizeof submit_msg) {
        ESP_LOGW(TAG, "not submitting a share: the message needs %d bytes and "
                      "the buffer is %d -- check the job id and worker name",
                 n, (int) sizeof submit_msg);
        return -1;
    }
    debug_stratum_tx(submit_msg);

    return esp_transport_write(transport, submit_msg, strlen(submit_msg), 0);
}

int STRATUM_V1_configure_version_rolling(esp_transport_handle_t transport, int send_uid, uint32_t * version_mask)
{
    char configure_msg[BUFFER_SIZE * 2];
    sprintf(configure_msg,
            "{\"id\": %d, \"method\": \"mining.configure\", \"params\": [[\"version-rolling\"], {\"version-rolling.mask\": "
            "\"ffffffff\"}]}\n",
            send_uid);
    debug_stratum_tx(configure_msg);

    return esp_transport_write(transport, configure_msg, strlen(configure_msg), 0);
}

static void debug_stratum_tx(const char * msg)
{
    //remove the trailing newline
    char * newline = strchr(msg, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }
    ESP_LOGD(TAG, "tx: %s", msg);

    //put it back!
    if (newline != NULL) {
        *newline = '\n';
    }
}
