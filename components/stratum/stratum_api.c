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
                ESP_LOGW(TAG, "Connection closed by peer (transport read returned 0)");
                if (json_rpc_buffer) {
                    free(json_rpc_buffer);
                    json_rpc_buffer = NULL;
                }
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
static const char * str_at(cJSON * array, int index)
{
    cJSON * item = cJSON_IsArray(array) ? cJSON_GetArrayItem(array, index) : NULL;
    return (cJSON_IsString(item) && item->valuestring != NULL) ? item->valuestring : NULL;
}

void STRATUM_V1_parse(StratumApiV1Message * message, const char * stratum_json)
{
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
                message->error_str = strdup("unknown");
                if (cJSON_IsString(reject_reason_json)) {
                    message->error_str = strdup(cJSON_GetStringValue(reject_reason_json));
                }
                ESP_LOGW(TAG, "recv %s", stratum_json);          
            }
        
        //if the id is STRATUM_ID_SUBSCRIBE parse it
        } else if (parsed_id == STRATUM_ID_SUBSCRIBE) {
            result = STRATUM_RESULT_SUBSCRIBE;

            cJSON * extranonce2_len_json = cJSON_GetArrayItem(result_json, 2);
            if (extranonce2_len_json == NULL) {
                ESP_LOGE(TAG, "Unable to parse extranonce2_len: %s", result_json->valuestring);
                message->response_success = false;
                goto done;
            }
            message->extranonce_2_len = extranonce2_len_json->valueint;

            cJSON * extranonce_json = cJSON_GetArrayItem(result_json, 1);
            if (extranonce_json == NULL) {
                ESP_LOGE(TAG, "Unable parse extranonce: %s", result_json->valuestring);
                message->response_success = false;
                goto done;
            }
            message->extranonce_str = malloc(strlen(extranonce_json->valuestring) + 1);
            strcpy(message->extranonce_str, extranonce_json->valuestring);
            message->response_success = true;

            //print the extranonce_str
            ESP_LOGI(TAG, "extranonce_str: %s", message->extranonce_str);
            ESP_LOGI(TAG, "extranonce_2_len: %d", message->extranonce_2_len);

        //if the id is STRATUM_ID_CONFIGURE parse it
        } else if (parsed_id == STRATUM_ID_CONFIGURE) {
            cJSON * mask = cJSON_GetObjectItem(result_json, "version-rolling.mask");
            if (mask != NULL) {
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

        if (NULL == p_job || NULL == p_prev || NULL == p_cb1 || NULL == p_cb2 ||
            !cJSON_IsArray(merkle_branch)) {
            ESP_LOGW(TAG, "ignoring a malformed mining.notify: params missing "
                          "or not strings");
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

        new_work->version = strtoul(cJSON_GetArrayItem(params, 5)->valuestring, NULL, 16);
        new_work->target = strtoul(cJSON_GetArrayItem(params, 6)->valuestring, NULL, 16);
        new_work->ntime = strtoul(cJSON_GetArrayItem(params, 7)->valuestring, NULL, 16);

        message->mining_notification = new_work;

        // params can be varible length
        int paramsLength = cJSON_GetArraySize(params);
        int value = cJSON_IsTrue(cJSON_GetArrayItem(params, paramsLength - 1));
        message->should_abandon_work = value;
    } else if (message->method == MINING_SET_DIFFICULTY) {
        cJSON * params = cJSON_GetObjectItem(json, "params");
        uint32_t difficulty = cJSON_GetArrayItem(params, 0)->valueint;

        message->new_difficulty = difficulty;
    } else if (message->method == MINING_SET_VERSION_MASK) {
        cJSON * params = cJSON_GetObjectItem(json, "params");
        uint32_t version_mask = strtoul(cJSON_GetArrayItem(params, 0)->valuestring, NULL, 16);
        message->version_mask = version_mask;

    } else if (message->method == MINING_SET_EXTRANONCE) {
        cJSON * params = cJSON_GetObjectItem(json, "params");

        if(cJSON_IsArray(params)){
            cJSON * extranonce2_len_json = cJSON_GetArrayItem(params, 1);
            if (extranonce2_len_json == NULL) {
                ESP_LOGE(TAG, "Unable to parse extranonce2_len: %s", cJSON_PrintUnformatted(params));
                message->response_success = false;
                goto done;
            }
            message->extranonce_2_len = extranonce2_len_json->valueint;
            cJSON * extranonce_json = cJSON_GetArrayItem(params, 0);
            if (extranonce_json == NULL) {
                ESP_LOGE(TAG, "Unable parse extranonce: %s", cJSON_PrintUnformatted(params));
                message->response_success = false;
                goto done;
            }

            if(NULL != message->extranonce_str){
                free(message->extranonce_str);
            }
            message->extranonce_str = malloc(strlen(extranonce_json->valuestring) + 1);
            strcpy(message->extranonce_str, extranonce_json->valuestring);
            message->response_success = true;
            //print the extranonce_str
            ESP_LOGI(TAG, "extranonce_str: %s", message->extranonce_str);
            ESP_LOGI(TAG, "extranonce_2_len: %d", message->extranonce_2_len);
        }
    }

    done:
    cJSON_Delete(json);
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
    sprintf(authorize_msg, "{\"id\": %d, \"method\": \"mining.authorize\", \"params\": [\"%s\", \"%s\"]}\n", send_uid, username,
            pass);
    debug_stratum_tx(authorize_msg);

    return esp_transport_write(transport, authorize_msg, strlen(authorize_msg), 0);
}

int STRATUM_V1_authorize(esp_transport_handle_t transport, int send_uid, const char * username, const char * pass)
{
    char authorize_msg[BUFFER_SIZE];
    sprintf(authorize_msg, "{\"id\": %d, \"method\": \"mining.authorize\", \"params\": [\"%s\", \"%s\"]}\n", send_uid, username,
            pass);
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
    sprintf(submit_msg,
            "{\"id\": %d, \"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%08lx\", \"%08lx\", \"%08lx\"]}\n",
            send_uid, username, jobid, extranonce_2, ntime, nonce, version);
    debug_stratum_tx(submit_msg);

    return esp_transport_write(transport, submit_msg, strlen(submit_msg), 0);
}

int STRATUM_V1_submit_ltc_share(esp_transport_handle_t transport, int send_uid, const char * username, const char * jobid,
                            const char * extranonce_2, const uint32_t ntime,
                            const uint32_t nonce)
{
    char submit_msg[BUFFER_SIZE];
    sprintf(submit_msg,
            "{\"id\": %d, \"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%08lx\", \"%08lx\"]}\n",
            send_uid, username, jobid, extranonce_2, ntime, nonce);
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
