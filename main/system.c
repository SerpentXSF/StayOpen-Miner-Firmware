#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"

#include "system.h"
#include "pool_scheduler.h"
#include "pool_fields.h"
#include "dual_clamp.h"
#include "connect.h"
#include "history.h"
#include "nvs_config.h"
#include "nvs_device.h"
#include "eeprom.h"
#include "gpio_input_output.h"
#include "asic.h"
#include "lvgl_porting.h"
#include "device.h"
#include "esp_mac.h"
#include "main.h"
#include "vcore.h"

/* The BM1370-based BC boards, which share bring-up, watchdog and boot-mode
 * handling. Kept in one place so adding a model does not mean editing every
 * model test in this file. */
static inline bool is_bc_family(DeviceModel model)
{
    return model == DEVICE_BC01 || model == DEVICE_BC02 ||
           model == DEVICE_BC04 || model == DEVICE_BC06 ||
           model == DEVICE_BC08;
}


static const char * TAG = "SystemModule";

static void _suffix_string(uint64_t, char *, size_t, int);

static char sn_str[24] = {0};
static double found_nonce_time_stamps = 0;
static int miner_started = 0;
static int no_first_nonce_count = 0;

//local function prototypes
//static esp_err_t ensure_overheat_mode_config();

static void _check_for_best_diff(GlobalState * GLOBAL_STATE, double diff, uint8_t job_id);
static void _suffix_string(uint64_t val, char * buf, size_t bufsiz, int sigdigits);

void SYSTEM_init_system(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->duration_start = 0;
    module->historical_hashrate_rolling_index = 0;
    module->historical_hashrate_init = 0;
    module->current_hashrate = 0;
    module->screen_page = 0;
    module->shares_accepted = 0;
    module->shares_rejected = 0;
    module->best_nonce_diff = nvs_config_get_u64(NVS_CONFIG_BEST_DIFF, 0);
    module->best_session_nonce_diff = 0;
    module->start_time = esp_timer_get_time();
    module->is_network_error = false;
    module->lastClockSync = 0;
    module->FOUND_BLOCK = false;
    module->BLOCK_NUM = nvs_config_get_u16(NVS_CONFIG_BLOCK_NUM, 0);
    
    // set the pool url
    module->pool_url = nvs_config_get_string(NVS_CONFIG_STRATUM_URL, CONFIG_STRATUM_URL);
    module->fallback_pool_url = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_URL, CONFIG_FALLBACK_STRATUM_URL);

    // set the pool port
    module->pool_port = nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT, CONFIG_STRATUM_PORT);
    module->fallback_pool_port = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, CONFIG_FALLBACK_STRATUM_PORT);

    // set the pool user
    module->pool_user = nvs_config_get_string(NVS_CONFIG_STRATUM_USER, CONFIG_STRATUM_USER);
    module->fallback_pool_user = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_USER, CONFIG_FALLBACK_STRATUM_USER);

    // set the pool password
    module->pool_pass = nvs_config_get_string(NVS_CONFIG_STRATUM_PASS, CONFIG_STRATUM_PW);
    module->fallback_pool_pass = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_PASS, CONFIG_FALLBACK_STRATUM_PW);

    // set the pool difficulty
    module->pool_difficulty = nvs_config_get_u16(NVS_CONFIG_STRATUM_DIFFICULTY, CONFIG_STRATUM_DIFFICULTY);
    module->fallback_pool_difficulty = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_DIFFICULTY, CONFIG_FALLBACK_STRATUM_DIFFICULTY);

    // set the pool extranonce subscribe
    module->pool_extranonce_subscribe = nvs_config_get_u16(NVS_CONFIG_STRATUM_EXTRANONCE_SUBSCRIBE, STRATUM_EXTRANONCE_SUBSCRIBE);
    module->fallback_pool_extranonce_subscribe = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_EXTRANONCE_SUBSCRIBE, FALLBACK_STRATUM_EXTRANONCE_SUBSCRIBE);

    // set the pool TLS mode
    module->pool_tls = nvs_config_get_u16(NVS_CONFIG_STRATUM_TLS, STRATUM_TLS);
    module->fallback_pool_tls = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_TLS, FALLBACK_STRATUM_TLS);

    // set the pool TLS custom certificate
    module->pool_cert = nvs_config_get_string(NVS_CONFIG_STRATUM_CERT, STRATUM_CERT);
    module->fallback_pool_cert = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_CERT, FALLBACK_STRATUM_CERT);

    /* ---- the pools that are not pool A ----
     * Every one of them is loaded; a pool with no URL is simply never
     * connected. Off unless dual mining is explicitly enabled and pool B has
     * a URL, so an upgrade from a single-pool build behaves exactly as
     * before -- see dual_enable below. */
    for (pool_id_t id = POOL_B; id < POOL_MAX; id++) {
        PoolConfig *pool = &module->pool[id];
        char key[32];

        for (size_t f = 0; f < POOL_FIELD_COUNT; f++) {
            const PoolField *field = &POOL_FIELDS[f];
            pool_key(key, sizeof(key), id, field->nvs, false);

            if (field->kind == POOL_FIELD_STR) {
                *(char **) ((char *) pool + field->offset) =
                    nvs_config_get_string(key, field->def_str);
            } else {
                *(uint16_t *) ((char *) pool + field->offset) =
                    nvs_config_get_u16(key, field->def_u16);
            }
        }

        pool->is_using_failover = false;
        pool->shares_accepted = 0;
        pool->shares_rejected = 0;
        pool->connected = false;
    }

    /* Slice scheduling lives on GlobalState, next to the other per-session
     * mining state. Dual mining needs a pool B URL as well as the flag: a
     * miner told to dual-mine nowhere should just mine pool A. */
    GLOBAL_STATE->dual_ratio_a     = dual_clamp_ratio(nvs_config_get_u16(NVS_CONFIG_DUAL_RATIO, 50));
    GLOBAL_STATE->dual_interval_ms = dual_clamp_interval(nvs_config_get_u16(NVS_CONFIG_DUAL_SLICE, 2000));
    GLOBAL_STATE->dual_enable      = nvs_config_get_u16(NVS_CONFIG_DUAL_ENABLE, 0)
                                     && module->pool[POOL_B].url != NULL
                                     && module->pool[POOL_B].url[0] != '\0';
    if (GLOBAL_STATE->dual_enable) {
        ESP_LOGI(TAG, "Dual mining enabled: pool B %s:%u, %u%% to pool A, %u ms slices",
                 module->pool[POOL_B].url, module->pool[POOL_B].port,
                 GLOBAL_STATE->dual_ratio_a, GLOBAL_STATE->dual_interval_ms);
    }

    // set fallback to false.
    module->is_using_fallback = false;

    // Initialize overheat_mode
    module->overheat_mode = 0;//nvs_config_get_u16(NVS_CONFIG_OVERHEAT_MODE, 0);
    //ESP_LOGI(TAG, "Initial overheat_mode value: %d", module->overheat_mode);

    // Initialize boot_mode
    module->boot_mode = nvs_config_get_u16(NVS_CONFIG_BOOT_MODE, 0);
    ESP_LOGI(TAG, "boot_mode value: %d", module->boot_mode);

    //Initialize power_fault fault mode
    module->power_fault = 0;

    // set the best diff string
    _suffix_string(module->best_nonce_diff, module->best_diff_string, DIFF_STRING_SIZE, 0);
    _suffix_string(module->best_session_nonce_diff, module->best_session_diff_string, DIFF_STRING_SIZE, 0);

    // set the ssid string to blank
    memset(module->ssid, 0, sizeof(module->ssid));

    // set the wifi_status to blank
    memset(module->wifi_status, 0, 20);
    module->system_error = NULL;
    module->system_error_code = SYSTEM_ERROR_MAX_NUM;

    module->sn[0] = nvs_config_get_string(NVS_CONFIG_SN, "AL");//"ALDC02ABC0001"
    if(strlen(module->sn[0]) < 4)
    {
        uint8_t mac [6] = {0};
        esp_read_mac(mac,ESP_MAC_WIFI_STA);
        snprintf(sn_str,sizeof(sn_str)-1,"%s%s%02X%02X%02X%02X%02X%02X", module->sn[0],GLOBAL_STATE->device_model_str,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        if(module->sn[0])
            free(module->sn[0]);
        module->sn[0] = sn_str;
        nvs_config_set_string(NVS_CONFIG_SN, sn_str);
    }
    else if(strlen(module->sn[0]) < 8)
    {
        uint8_t mac [6] = {0};
        esp_read_mac(mac,ESP_MAC_WIFI_STA);
        snprintf(sn_str,sizeof(sn_str)-1,"%s%02X%02X%02X%02X%02X%02X", module->sn[0],
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        if(module->sn[0])
            free(module->sn[0]);
        module->sn[0] = sn_str;
        nvs_config_set_string(NVS_CONFIG_SN, sn_str);
    }
    else
    {
        strncpy(sn_str, module->sn[0], sizeof(sn_str)-1); 
        sn_str[sizeof(sn_str)-1] = '\0';
    }
    ESP_LOGI(TAG, "SN %s", sn_str);

	SYSTEM_get_config_by_boot_mode(GLOBAL_STATE);
}

char * SYSTEM_get_sn(void)
{
    return sn_str;
}

esp_err_t SYSTEM_update_freq_voltage(GlobalState * GLOBAL_STATE)
{
    esp_err_t ret = ESP_OK;

#ifdef CONFIG_BC04_INDIVIDUAL_FREQ
    if (GLOBAL_STATE->device_model == DEVICE_BC04) {
        uint16_t default_freq = nvs_config_get_u16(NVS_CONFIG_ASIC_FREQ, CONFIG_ASIC_FREQUENCY);
        GLOBAL_STATE->bc04_chip_freqs[0] = nvs_config_get_u16("asicfreq_c0", default_freq);
        GLOBAL_STATE->bc04_chip_freqs[1] = nvs_config_get_u16("asicfreq_c1", default_freq);
        GLOBAL_STATE->bc04_chip_freqs[2] = nvs_config_get_u16("asicfreq_c2", default_freq);
        GLOBAL_STATE->bc04_chip_freqs[3] = nvs_config_get_u16("asicfreq_c3", default_freq);
        GLOBAL_STATE->asic_freqency = GLOBAL_STATE->bc04_chip_freqs[0];
    } else {
        GLOBAL_STATE->asic_freqency = nvs_config_get_u16(NVS_CONFIG_ASIC_FREQ, CONFIG_ASIC_FREQUENCY);
    }
#else
    GLOBAL_STATE->asic_freqency = nvs_config_get_u16(NVS_CONFIG_ASIC_FREQ, CONFIG_ASIC_FREQUENCY);
#endif
    ESP_LOGI(TAG, "asic frequency %.0f", (float)GLOBAL_STATE->asic_freqency);
    
    uint16_t uint_voltage = nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE, GLOBAL_STATE->asic_vol_default);
    GLOBAL_STATE->HEALTH_MODULE.voltage = (float)uint_voltage;
    ESP_LOGI(TAG, "asic voltage %.0f", GLOBAL_STATE->HEALTH_MODULE.voltage);
    return ret;
}

esp_err_t SYSTEM_get_config_by_boot_mode(GlobalState * GLOBAL_STATE){
    esp_err_t ret = ESP_OK;
    uint16_t boot_mode = GLOBAL_STATE->SYSTEM_MODULE.boot_mode;

    if(NORMAL_MODE == boot_mode){
        ESP_LOGI(TAG, "Normal Mode....");
        GLOBAL_STATE->asic_freqency = nvs_config_get_u16(NVS_CONFIG_ASIC_NORMAL_FREQ, CONFIG_ASIC_FREQUENCY);
        GLOBAL_STATE->HEALTH_MODULE.voltage = (float)nvs_config_get_u16(NVS_CONFIG_ASIC_NORMAL_VOLTAGE, GLOBAL_STATE->asic_vol_default);
        ESP_LOGI(TAG, "asic frequency %.0f", (float)GLOBAL_STATE->asic_freqency);
        ESP_LOGI(TAG, "asic voltage %.0f", GLOBAL_STATE->HEALTH_MODULE.voltage);
    }
    else if(OVER_FREQ_MODE == boot_mode){
        ESP_LOGI(TAG, "Over freq Mode....");
        GLOBAL_STATE->asic_freqency = nvs_config_get_u16(NVS_CONFIG_ASIC_OVER_FREQ, CONFIG_ASIC_FREQUENCY);
        GLOBAL_STATE->HEALTH_MODULE.voltage = (float)nvs_config_get_u16(NVS_CONFIG_ASIC_OVER_VOLTAGE, GLOBAL_STATE->asic_vol_default);
        ESP_LOGI(TAG, "asic frequency %.0f", (float)GLOBAL_STATE->asic_freqency);
        ESP_LOGI(TAG, "asic voltage %.0f", GLOBAL_STATE->HEALTH_MODULE.voltage);
    }
    else if(USER_CUSTOMIZED_MODE == boot_mode){
        ESP_LOGI(TAG, "User Custmized Mode...");
        ret = SYSTEM_update_freq_voltage(GLOBAL_STATE);
    }
    else
    {
        ESP_LOGI(TAG, "Mode %d",boot_mode);
        ret = SYSTEM_update_freq_voltage(GLOBAL_STATE);
    }

    if(is_bc_family(GLOBAL_STATE->device_model))
    {
#ifdef CONFIG_BC04_INDIVIDUAL_FREQ
        if (GLOBAL_STATE->device_model == DEVICE_BC04 && boot_mode == USER_CUSTOMIZED_MODE) {
            uint16_t default_fallback = nvs_config_get_u16(NVS_CONFIG_ASIC_NORMAL_FREQ, CONFIG_ASIC_FREQUENCY);
            if (default_fallback < 300 || default_fallback > 820) {
                default_fallback = CONFIG_ASIC_FREQUENCY;
            }
            for (int i = 0; i < 4; i++) {
                if (GLOBAL_STATE->bc04_chip_freqs[i] < 300 || GLOBAL_STATE->bc04_chip_freqs[i] > 820) {
                    ESP_LOGW(TAG, "Chip %d freqency %d unsupported, set default %d", i, GLOBAL_STATE->bc04_chip_freqs[i], default_fallback);
                    GLOBAL_STATE->bc04_chip_freqs[i] = default_fallback;
                }
            }
            GLOBAL_STATE->asic_freqency = GLOBAL_STATE->bc04_chip_freqs[0];
        } else {
            uint16_t max_freq = 820;
            if(GLOBAL_STATE->asic_freqency < 300 || GLOBAL_STATE->asic_freqency > max_freq)
            {
                uint16_t asic_freqency = nvs_config_get_u16(NVS_CONFIG_ASIC_NORMAL_FREQ, CONFIG_ASIC_FREQUENCY);
                if(asic_freqency < 300 || asic_freqency > max_freq)
                {
                    asic_freqency = CONFIG_ASIC_FREQUENCY;
                }
                ESP_LOGW(TAG, "freqency %f unsupport, set default %d",(float)GLOBAL_STATE->asic_freqency,asic_freqency);
                GLOBAL_STATE->asic_freqency = asic_freqency;
            }
        }
#else
        uint16_t max_freq = 820;
        if(GLOBAL_STATE->asic_freqency < 300 || GLOBAL_STATE->asic_freqency > max_freq)
        {
            uint16_t asic_freqency = nvs_config_get_u16(NVS_CONFIG_ASIC_NORMAL_FREQ, CONFIG_ASIC_FREQUENCY);
            if(asic_freqency < 300 || asic_freqency > max_freq)
            {
                asic_freqency = CONFIG_ASIC_FREQUENCY;
            }
            ESP_LOGW(TAG, "freqency %f unsupport, set default %d",(float)GLOBAL_STATE->asic_freqency,asic_freqency);
            GLOBAL_STATE->asic_freqency = asic_freqency;
        }
#endif
    }
    else if(GLOBAL_STATE->asic_freqency < 700 || GLOBAL_STATE->asic_freqency > 2600)
    {
    	uint16_t asic_freqency = nvs_config_get_u16(NVS_CONFIG_ASIC_NORMAL_FREQ, CONFIG_ASIC_FREQUENCY);
        if(asic_freqency < 700 || asic_freqency > 2600)
        {
            asic_freqency = CONFIG_ASIC_FREQUENCY;
        }
        ESP_LOGW(TAG, "freqency %f unsupport, set default %d",(float)GLOBAL_STATE->asic_freqency,asic_freqency);
        GLOBAL_STATE->asic_freqency = asic_freqency;
    }

    if((GLOBAL_STATE->HEALTH_MODULE.voltage < GLOBAL_STATE->asic_vol_min) || 
    	(GLOBAL_STATE->HEALTH_MODULE.voltage > GLOBAL_STATE->asic_vol_max))
    {
    	uint16_t voltage = nvs_config_get_u16(NVS_CONFIG_ASIC_NORMAL_VOLTAGE, GLOBAL_STATE->asic_vol_default);
        if((voltage < GLOBAL_STATE->asic_vol_min) || (voltage > GLOBAL_STATE->asic_vol_max))
        {
            voltage = GLOBAL_STATE->asic_vol_default;
        }
        ESP_LOGW(TAG, "voltage %.0f unsupport, set default %d",GLOBAL_STATE->HEALTH_MODULE.voltage, voltage);
		GLOBAL_STATE->HEALTH_MODULE.voltage = voltage;
    }
    /*
     * Whatever the stored voltage is, it does not get to raise the model's
     * ceiling. This lifted asic_vol_default to match asicnormalvol with only
     * a min..max check behind it, so on a BC04 -- capped at 4.80 V by the
     * vendor, but with TPS546_VOUT_MAX at 5.20 -- one NVS key was enough to
     * bring the board up over the cap.
     */
    uint16_t vol_ceiling = device_core_voltage_ceiling(GLOBAL_STATE);
    if(GLOBAL_STATE->HEALTH_MODULE.voltage > (float)vol_ceiling)
    {
        ESP_LOGW(TAG, "stored voltage %.0f is over this board's ceiling of "
                      "%" PRIu16 "; using the ceiling",
                 GLOBAL_STATE->HEALTH_MODULE.voltage, vol_ceiling);
        GLOBAL_STATE->HEALTH_MODULE.voltage = (float)vol_ceiling;
    }
    if(GLOBAL_STATE->HEALTH_MODULE.voltage > GLOBAL_STATE->asic_vol_default)
    {
        GLOBAL_STATE->asic_vol_default = GLOBAL_STATE->HEALTH_MODULE.voltage;
    }
    ESP_LOGI(TAG, "vol def  %d",GLOBAL_STATE->asic_vol_default);
    return ret;
}

void SYSTEM_notify_get_work(GlobalState * GLOBAL_STATE)
{
    SYSTEM_notify_status_change(GLOBAL_STATE, SYSTEM_NORMAL_MINING);
}

void SYSTEM_notify_accepted_share(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->shares_accepted++;
    SYSTEM_notify_status_change(GLOBAL_STATE, SYSTEM_NORMAL_MINING);
}

static int compare_rejected_reason_stats(const void *a, const void *b) {
    const RejectedReasonStat *ea = a;
    const RejectedReasonStat *eb = b;
    return (eb->count > ea->count) - (ea->count > eb->count);
}

void SYSTEM_notify_rejected_share(GlobalState * GLOBAL_STATE, char * error_msg)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->shares_rejected++;

    if(error_msg)
    {
        for (int i = 0; i < module->rejected_reason_stats_count; i++) {
            if (strncmp(module->rejected_reason_stats[i].message, error_msg, sizeof(module->rejected_reason_stats[i].message) - 1) == 0) {
                module->rejected_reason_stats[i].count++;
                return;
            }
        }

        /*
         * sizeof the array, not its element count, bounded this for a
         * 10-entry array of 68-byte structs: the limit read 680. The
         * eleventh distinct reject string wrote past the end and the
         * first field after it is rejected_reason_stats_count itself,
         * so the count took the first four bytes of the message --
         * around half a billion -- and the qsort below then ran over
         * that many elements. Pools that put a job id in the reject
         * reason make every stale share a new string, so this was a
         * matter of time rather than of bad luck.
         */
        if (module->rejected_reason_stats_count <
            (sizeof(module->rejected_reason_stats) / sizeof(module->rejected_reason_stats[0]))) {
            strncpy(module->rejected_reason_stats[module->rejected_reason_stats_count].message, 
                    error_msg, 
                    sizeof(module->rejected_reason_stats[module->rejected_reason_stats_count].message) - 1);
            module->rejected_reason_stats[module->rejected_reason_stats_count].message[sizeof(module->rejected_reason_stats[module->rejected_reason_stats_count].message) - 1] = '\0'; // Ensure null termination
            module->rejected_reason_stats[module->rejected_reason_stats_count].count = 1;
            module->rejected_reason_stats_count++;
        }

        if (module->rejected_reason_stats_count > 1) {
            qsort(module->rejected_reason_stats, module->rejected_reason_stats_count, 
                sizeof(module->rejected_reason_stats[0]), compare_rejected_reason_stats);
        }  
    }
}

void SYSTEM_notify_mining_started(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->duration_start = esp_timer_get_time();
    module->historical_hashrate_rolling_index = 0;
    module->historical_hashrate[0] = 0;
    module->historical_hashrate_time_stamps[0] = 0;
    miner_started = 0;
}

void SYSTEM_notify_new_ntime(GlobalState * GLOBAL_STATE, uint32_t ntime)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    // Hourly clock sync
    if (module->lastClockSync + (60 * 60) > ntime) {
        return;
    }
    ESP_LOGD(TAG, "Syncing clock");
    module->lastClockSync = ntime;
    struct timeval tv;
    tv.tv_sec = ntime;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
}

void SYSTEM_notify_found_nonce(
    GlobalState * GLOBAL_STATE, double found_diff, 
    uint8_t job_id, uint8_t chain_id, uint8_t chip_id, uint8_t core_id)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    // Calculate the time difference in seconds with sub-second precision
    // hashrate = (nonce_difficulty * 2^32) / time_to_find

    #ifdef HW_STATISTIC_FEATURE
    module->recveived_nonce += 1;
    /* counted per core so an error count means something next to it */
    if (core_id < CORE_STATS_CORES) {
        module->core_nonces[core_id]++;
    }
    #if 0
    if(module->recveived_nonce == 100)
    {
        module->FOUND_BLOCK = true;
        module->BLOCK_NUM ++;
    }
    #endif
    #endif

    miner_started = 1;

    found_nonce_time_stamps = esp_timer_get_time();

    #if 1
    double time_durationps = found_nonce_time_stamps - module->duration_start;

    module->historical_hashrate[module->historical_hashrate_rolling_index] += GLOBAL_STATE->asic_difficulty;
    module->historical_hashrate_time_stamps[module->historical_hashrate_rolling_index] = time_durationps;

    double sum_hashrate = module->historical_hashrate[module->historical_hashrate_rolling_index] ;
    double sun_time = module->historical_hashrate_time_stamps[module->historical_hashrate_rolling_index];

    if(time_durationps >= 10000000)
    {
        module->historical_hashrate_rolling_index = (module->historical_hashrate_rolling_index + 1) % HISTORY_LENGTH;
        module->historical_hashrate[module->historical_hashrate_rolling_index] = 0;
        module->historical_hashrate_time_stamps[module->historical_hashrate_rolling_index] = 0;
        module->duration_start = found_nonce_time_stamps;
        if (module->historical_hashrate_init < HISTORY_LENGTH) {
            module->historical_hashrate_init++;
        }
        //ESP_LOGI(TAG, "hash %.1f, time %.1f, init %d, num %lld", sum_hashrate, sun_time/1000000,
        //    module->historical_hashrate_init,module->recveived_nonce);
    }

    for (int i = 0; i < module->historical_hashrate_init; i++) {
        sum_hashrate += module->historical_hashrate[i];
        sun_time += module->historical_hashrate_time_stamps[i];
    }
    
    module->current_hashrate = (sum_hashrate * 4294967296) / (sun_time * 1000);

    #else

    module->historical_hashrate[module->historical_hashrate_rolling_index] = GLOBAL_STATE->asic_difficulty;
    module->historical_hashrate_time_stamps[module->historical_hashrate_rolling_index] = esp_timer_get_time();
    module->historical_hashrate_rolling_index = (module->historical_hashrate_rolling_index + 1) % HISTORY_LENGTH;

    // ESP_LOGI(TAG, "nonce_diff %.1f, ttf %.1f, res %.1f", nonce_diff, duration,
    // historical_hashrate[historical_hashrate_rolling_index]);

    if (module->historical_hashrate_init < HISTORY_LENGTH) {
        module->historical_hashrate_init++;
    } else {
        module->duration_start =
            module->historical_hashrate_time_stamps[(module->historical_hashrate_rolling_index + 1) % HISTORY_LENGTH];
    }
    double sum = 0;
    for (int i = 0; i < module->historical_hashrate_init; i++) {
        sum += module->historical_hashrate[i];
    }

    double duration = (double) (esp_timer_get_time() - module->duration_start) / 1000000;
#ifdef BTC
    double rolling_rate = (sum * 4294967296) / (duration * 1000000000);
    if (module->historical_hashrate_init < HISTORY_LENGTH) {
        module->current_hashrate = rolling_rate;
    } else {
        // More smoothing
        module->current_hashrate = ((module->current_hashrate * 9) + rolling_rate) / 10;
    }
#else
    double rolling_rate = (sum * 65536) / (duration * 1000000000);
    if (module->historical_hashrate_init < HISTORY_LENGTH) {
        module->current_hashrate = rolling_rate;
    } else {
        // More smoothing
        module->current_hashrate = ((module->current_hashrate * 11) + rolling_rate) / 12;
    }
#endif
#endif
    // logArrayContents(historical_hashrate, HISTORY_LENGTH);
    // logArrayContents(historical_hashrate_time_stamps, HISTORY_LENGTH);
    if(module->shares_accepted)
    _check_for_best_diff(GLOBAL_STATE, found_diff, job_id);
}

/*
 * Confirm an image that arrived over the air, so the bootloader stops holding
 * the previous one in reserve.
 *
 * With rollback enabled a freshly installed image boots once in
 * PENDING_VERIFY. If it never reaches this call -- it panics, hangs before the
 * system task runs, or reboots -- the bootloader puts the previous image back
 * on the next reset, without anyone having to open the case. That matters here
 * because the recovery of last resort is a socketed module behind a screwed-on
 * lid, and because these images go to people over a web flasher.
 *
 * The bar is deliberately "this build is alive and has stayed alive", not
 * "this build is mining". Tying it to shares would let an unreachable pool
 * revert a perfectly good update, which is a worse failure than the one being
 * guarded against.
 *
 * A serially flashed image is UNDEFINED rather than PENDING_VERIFY and needs
 * no confirmation, so this costs one partition read and then nothing.
 */
#define OTA_CONFIRM_SECONDS 60

static void confirm_running_image(int64_t uptime_seconds)
{
    static bool settled = false;

    if (settled || uptime_seconds < OTA_CONFIRM_SECONDS) {
        return;
    }

    const esp_partition_t * running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (running == NULL ||
        esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        settled = true;
        return;
    }

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGW(TAG, "update confirmed after %llds; keeping this image",
                 (long long)uptime_seconds);
    } else {
        ESP_LOGE(TAG, "could not confirm this image; it reverts on next restart");
    }

    settled = true;
}

void SYSTEM_notify_hw(GlobalState * GLOBAL_STATE, uint8_t chain_id, uint8_t chip_id, uint8_t core_id)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;
    module->recveived_hw += 1;

#ifdef HW_STATISTIC_FEATURE
    /*
     * What stood here was the vendor's scaffolding for multi-chip boards,
     * indexed by chip and guarded by ASIC_NUM 0 -- which made it both dead
     * and unbuildable, since enabling it as written divides 0x100 by zero.
     * These boards carry one BM1370, so the useful axis is the core.
     */
    if (core_id < CORE_STATS_CORES) {
        module->core_errors[core_id]++;
    }

    ESP_LOGD(TAG, "hw error: chip %d core %d (%lu on this core, %llu total)",
             chip_id, core_id,
             core_id < CORE_STATS_CORES ? module->core_errors[core_id] : 0,
             module->recveived_hw);
#endif
}

void SYSTEM_notify_error_info(
    GlobalState * GLOBAL_STATE, SYSTEM_ERROR system_err, const char* error_info)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    if(system_err >= SYSTEM_ERROR_MAX_NUM){
        ESP_LOGW(TAG, "system get a unknown error info.");
        return;
    }

    if(NULL != module->system_error){
        free(module->system_error);
    }
    module->system_error_code = system_err;

    switch (system_err)
    {
    case WRONG_ASIC_ERROR:
        module->system_error = strdup("Wrong asic error");
        inform_led(MSG_HASHBOARD_ERROR);
        break;
    
    case FAN_ERROR:
        module->system_error = strdup("fan error");
        inform_led(MSG_FAN_ERROR);
        break;
    
    case EEPROM_ERROR:
        module->system_error = strdup("eeprom error");
        inform_led(MSG_EEPROM_ERROR);
        break;

    case CONTROL_SYSTEM_ERROR:
        module->system_error = strdup("system error");
        inform_led(MSG_CONTROLBOARD_ERROR);
        break;

    case CONFIG_FILE_ERROR:
        module->system_error = strdup("config file error");
        inform_led(MSG_CONTROLBOARD_ERROR);
        break;
    
    case HIGH_TEMPERATURE_ERROR:
        module->system_error = strdup("temperature error");
        inform_led(MSG_HASHBOARD_ERROR);
        break;

    case HASHRATE_ERROR:
        module->system_error = strdup("hashrate error");
        inform_led(MSG_HASHBOARD_ERROR);
        break;
    
    case NETWORK_ERROR:
        module->system_error = strdup("network error");
        module->is_network_error = true;
        inform_led(MSG_NETWORK_ERROR);
        break;

    case WIFI_CONNETION_ERROR:
        module->system_error = strdup("wifi error");
        inform_led(MSG_NETWORK_ERROR);
        
        refresh_portal_data_from_system();
        inform_wifi_status(false);
        break;

    case POWER_BOARD_ERROR:
        module->system_error = strdup("power board error");
        inform_led(MSG_HASHBOARD_ERROR);
        break;

    case SPECIAL_ERROR:
        /* The only case that takes caller text, and the only one that can be
         * handed NULL. strdup(NULL) is a crash, not an empty string. */
        module->system_error = strdup(NULL != error_info ? error_info
                                                         : "unspecified error");
        inform_led(MSG_HASHBOARD_ERROR);
        break;

    default:
        /*
         * Every code needs a string, because the log line below prints this
         * with %s and the free() above has already let go of whatever was
         * here. An unhandled code used to leave it dangling or NULL and take
         * the miner down with a LoadProhibited -- which is worse than the
         * fault being reported.
         *
         * RUN_MODE_ERROR and BOARD_MISMATCH_ERROR are both in the enum and
         * both land here, so this was reachable before today.
         */
        module->system_error = strdup("system error");
        inform_led(MSG_CONTROLBOARD_ERROR);
        break;
    }

    /*Change the system status*/
    module->system_status = SYSTEM_ERROR_STATUS;
    //showErrorScreen(module->system_error,system_err);
    /* strdup() can still fail; %s on NULL is a crash on this libc. */
    ESP_LOGW(TAG, "system get a error %d, %s.", system_err,
             NULL != module->system_error ? module->system_error : "(none)");
}

void SYSTEM_notify_status_change(GlobalState * GLOBAL_STATE, SYSTEM_SATUS new_status)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    if(new_status >= SYSTEM_SATUS_MAX_NUM){
        ESP_LOGW(TAG, "system get a unknown status.");
        return;
    }
    //ESP_LOGD(TAG, "status change: %d", new_status);

    module->system_status = new_status;
    switch (module->system_status)
    {
        case SYSTEM_BOOT_UP:
            inform_led(MSG_NORMAL_BOOTING);
            break;
        case SYSTEM_WIFI_CONNECTED:
            inform_led(MSG_WIFI_CONNECTED);
            inform_wifi_status(true);
            break;
        case SYSTEM_NORMAL_MINING:
            inform_led(MSG_NORMAL_MINING);
            inform_mining_status();
            
            /*
             * Clear what mining actually disproves, and nothing else.
             *
             * This cleared every error. The status arrives from
             * SYSTEM_notify_get_work(), which fires when the pool hands us a
             * job -- so a miner with no hashboard at all connects, is given
             * work, declares itself normally mining and erases the hardware
             * fault it raised seconds earlier. Measured on a BC04 with a dead
             * hashboard domain: POWER_BOARD_ERROR was raised at 11.4 s and
             * the API reported no error at all by the time anyone looked.
             *
             * Being handed work proves the network works. It says nothing
             * about the hardware, so a hardware fault outlives it and has to
             * be cleared by a restart or by the fault going away.
             */
            module->is_network_error = false;
            if(NETWORK_ERROR == module->system_error_code ||
               WIFI_CONNETION_ERROR == module->system_error_code)
            {
                if(NULL != module->system_error){
                    free(module->system_error);
                    module->system_error = strdup("");
                }
                module->system_error_code = SYSTEM_ERROR_MAX_NUM;
            }
            break;       
        default:
            break;
    }  
}


static double _calculate_network_difficulty(uint32_t nBits)
{
    uint32_t mantissa = nBits & 0x007fffff;  // Extract the mantissa from nBits
    uint8_t exponent = (nBits >> 24) & 0xff; // Extract the exponent from nBits

    double target = (double) mantissa * pow(256, (exponent - 3)); // Calculate the target value

    double difficulty = (pow(2, 208) * 65535) / target; // Calculate the difficulty

    return difficulty;
}

double litecoin_difficulty_from_bits_simple(uint32_t nBits) {
    // 这是一个在社区广泛使用的简化公式
    int shift = (nBits >> 24) & 0xff;
    double diff = (double)0x0000ffff / (double)(nBits & 0x00ffffff);
    while (shift < 29) {
        diff *= 256.0;
        shift++;
    }
    while (shift > 29) {
        diff /= 256.0;
        shift--;
    }
    return diff;
}

static void _check_for_best_diff(GlobalState * GLOBAL_STATE, double diff, uint8_t job_id)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    if ((uint64_t) diff > module->best_session_nonce_diff) {
        module->best_session_nonce_diff = (uint64_t) diff;
        _suffix_string((uint64_t) diff, module->best_session_diff_string, DIFF_STRING_SIZE, 0);
        if(diff > 1000000)
        ESP_LOGI(TAG, "BD %.0f", diff);
    }

    if(diff > 1000000)
    {
        double network_diff = _calculate_network_difficulty(GLOBAL_STATE->ASIC_TASK_MODULE[0].active_jobs[job_id]->target);
        //double network_diff = (double)(GLOBAL_STATE->stratum_difficulty);
        if (diff > network_diff) {
            #if 0
            module->FOUND_BLOCK = true;
            module->BLOCK_NUM ++;
            ESP_LOGI(TAG, "FOUND BLOCK!!!!!!!!!!!!!!!!!!!!!! %f > %f", diff, network_diff);
            #else
            ESP_LOGI(TAG, "DIFF %f > %f, 0x%" PRIx32 "", diff, network_diff,GLOBAL_STATE->ASIC_TASK_MODULE[0].active_jobs[job_id]->target);
            #endif
        }
    }

    if ((uint64_t) diff <= module->best_nonce_diff) {
        return;
    }
    module->best_nonce_diff = (uint64_t) diff;

    nvs_config_set_u64(NVS_CONFIG_BEST_DIFF, module->best_nonce_diff);

    // make the best_nonce_diff into a string
    _suffix_string((uint64_t) diff, module->best_diff_string, DIFF_STRING_SIZE, 0);

    ESP_LOGI(TAG, "best_nonce_diff: %.0f", diff);
}

/* Convert a uint64_t value into a truncated string for displaying with its
 * associated suitable for Mega, Giga etc. Buf array needs to be long enough */
static void _suffix_string(uint64_t val, char * buf, size_t bufsiz, int sigdigits)
{
    const double dkilo = 1000.0;
    const uint64_t kilo = 1000ull;
    const uint64_t mega = 1000000ull;
    const uint64_t giga = 1000000000ull;
    const uint64_t tera = 1000000000000ull;
    const uint64_t peta = 1000000000000000ull;
    const uint64_t exa = 1000000000000000000ull;
    char suffix[2] = "";
    bool decimal = true;
    double dval;

    if (val >= exa) {
        val /= peta;
        dval = (double) val / dkilo;
        strcpy(suffix, "E");
    } else if (val >= peta) {
        val /= tera;
        dval = (double) val / dkilo;
        strcpy(suffix, "P");
    } else if (val >= tera) {
        val /= giga;
        dval = (double) val / dkilo;
        strcpy(suffix, "T");
    } else if (val >= giga) {
        val /= mega;
        dval = (double) val / dkilo;
        strcpy(suffix, "G");
    } else if (val >= mega) {
        val /= kilo;
        dval = (double) val / dkilo;
        strcpy(suffix, "M");
    } else if (val >= kilo) {
        dval = (double) val / dkilo;
        strcpy(suffix, "k");
    } else {
        dval = val;
        decimal = false;
    }

    if (!sigdigits) {
        if (decimal)
            snprintf(buf, bufsiz, "%.3g%s", dval, suffix);
        else
            snprintf(buf, bufsiz, "%d%s", (unsigned int) dval, suffix);
    } else {
        /* Always show sigdigits + 1, padded on right with zeroes
         * followed by suffix */
        int ndigits = sigdigits - 1 - (dval > 0.0 ? floor(log10(dval)) : 0);

        snprintf(buf, bufsiz, "%*.*f%s", sigdigits + 1, ndigits, dval, suffix);
    }
}

#if 0
static esp_err_t ensure_overheat_mode_config() {
    uint16_t overheat_mode = nvs_config_get_u16(NVS_CONFIG_OVERHEAT_MODE, UINT16_MAX);

    if (overheat_mode == UINT16_MAX) {
        // Key doesn't exist or couldn't be read, set the default value
        nvs_config_set_u16(NVS_CONFIG_OVERHEAT_MODE, 0);
        ESP_LOGI(TAG, "Default value for overheat_mode set to 0");
    } else {
        // Key exists, log the current value
        ESP_LOGI(TAG, "Existing overheat_mode value: %d", overheat_mode);
    }

    return ESP_OK;
}
#endif

void system_task(void *pvParameters) 
{
    ESP_LOGI("system task", "system_task started");

    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;
    uint16_t block_num = GLOBAL_STATE->SYSTEM_MODULE.BLOCK_NUM;
    uint32_t log_time = 0;
    double current_hashrate_max = 0;
    uint16_t hashrate_time_count = 0;
    uint16_t asic_init_time_count = 0;
    uint16_t miner_started_count = 0;
    
    while (1) 
    {
        /* Watch for the ASIC chain coming up short. This was three copies of
         * the same block, one per model, each redefining ASIC_INIT_TIME_OUT;
         * BC01 and BC02 had no branch and so were never checked. The expected
         * count now comes from ASIC_get_asic_count(). */
        #define ASIC_INIT_TIME_OUT (3*60*2)
        if(is_bc_family(GLOBAL_STATE->device_model))
        {
            if(GLOBAL_STATE->asic_count[0] != ASIC_get_asic_count(GLOBAL_STATE))
            {
                asic_init_time_count ++;
                if(asic_init_time_count == ASIC_INIT_TIME_OUT)
                {
                    char err_str[32];
                    sprintf(err_str,"ASIC err, num = %d",GLOBAL_STATE->asic_count[0]);
                    showErrorScreen(err_str,WRONG_ASIC_ERROR);
                }
                else if(asic_init_time_count > (ASIC_INIT_TIME_OUT + 1*60*2))
                {
                    restart_with_reason("ASIC num err");
                }
            }
            else
            {
                asic_init_time_count = 0;
            }
        }
        #if 1
        if(GLOBAL_STATE->SYSTEM_MODULE.FOUND_BLOCK)
        {
            if(block_num != GLOBAL_STATE->SYSTEM_MODULE.BLOCK_NUM)
            {
                block_num = GLOBAL_STATE->SYSTEM_MODULE.BLOCK_NUM;
                #if 1
                hideErrorScreen();
                showFoundBlockScreen();
                nvs_config_set_u16(NVS_CONFIG_BLOCK_NUM, block_num);
                #endif
            }
        }
        #endif

        if(GLOBAL_STATE->SYSTEM_MODULE.overheat_mode)
        {
            showErrorScreen("OVER HEATED",HIGH_TEMPERATURE_ERROR);
        }

        /*
         * current_hashrate is a rolling average, and in the first minutes it
         * reads far above the true rate -- a BC01 settling at 1.6 TH/s shows
         * 2.3 TH/s at twenty seconds, because the window holds a handful of
         * lucky shares. current_hashrate_max was a high-water mark that never
         * decayed, so that early overshoot became the reference for the rest
         * of the run and set the alarm at half of a number the miner was
         * never going to reach again. Ordinary variance then tripped
         * "Hashrate too low" on a perfectly healthy device.
         *
         * The average is left to settle before it is trusted, and the mark
         * bleeds off slowly so one spike cannot poison the whole session.
         */
        #define HASHRATE_WARMUP_SECONDS 600
        int64_t mining_seconds =
            (esp_timer_get_time() - GLOBAL_STATE->SYSTEM_MODULE.start_time) / 1000000;

        confirm_running_image(mining_seconds);

        /* Cheap between samples; takes one every HISTORY_INTERVAL_SECONDS. */
        history_tick(GLOBAL_STATE);

        if(mining_seconds < HASHRATE_WARMUP_SECONDS)
        {
            current_hashrate_max = 0;
            hashrate_time_count = 0;
        }
        else if(current_hashrate_max > 0)
        {
            /* about 1% a minute at this loop's 2 Hz */
            current_hashrate_max -= current_hashrate_max / 12000;
        }

        if( GLOBAL_STATE->SYSTEM_MODULE.current_hashrate > 1
            && mining_seconds >= HASHRATE_WARMUP_SECONDS)
        {
            if(GLOBAL_STATE->SYSTEM_MODULE.current_hashrate > current_hashrate_max)
            {
                current_hashrate_max = GLOBAL_STATE->SYSTEM_MODULE.current_hashrate;
            }
            if(GLOBAL_STATE->SYSTEM_MODULE.current_hashrate < current_hashrate_max/2)
            {
                hashrate_time_count ++;
                #define  HASHRATE_ERROR_TIME_OUT (10*60*2)
                if(hashrate_time_count > HASHRATE_ERROR_TIME_OUT)
                {
                    showErrorScreen("Hashrate too low",HASHRATE_ERROR);
                    if(hashrate_time_count > (HASHRATE_ERROR_TIME_OUT + 1*60*2))
                    {
                        restart_with_reason("Hashrate too low for extended period");
                    }
                }
            }
            else
            {
                hashrate_time_count = 0;
            }
        }

        /* Known at boot, so there is no reason to wait for a timeout to say
         * it. showErrorScreen ignores a repeat of the code already showing,
         * which keeps this cheap at the loop rate and keeps the message up. */
        if(GLOBAL_STATE->SYSTEM_MODULE.board_mismatch)
        {
            showErrorScreen("Wrong firmware\nfor this board", BOARD_MISMATCH_ERROR);
        }

        if(found_nonce_time_stamps)
        {
            if((esp_timer_get_time() - found_nonce_time_stamps) > 120000000)
            {
                if (!GLOBAL_STATE->SYSTEM_MODULE.is_network_error) {
                    showErrorScreen("Hashrate error",HASHRATE_ERROR);
                }
                if((esp_timer_get_time() - found_nonce_time_stamps) > 180000000)
                {
                    restart_with_reason("Hashrate error for extended period");
                }
            }
        }
        /* The check above only arms once a nonce has been seen, because
         * found_nonce_time_stamps stays zero until the first one arrives. A
         * bring-up that never yields a nonce therefore recovered from
         * nothing: the miner sat at 0 H/s for as long as it was left alone.
         *
         * Observed on a BC01, where the ASIC is detected, clocked and drawing
         * full power but never answers on the UART. It is intermittent, and a
         * restart clears it, so the same restart the stall path already
         * performs is applied to the case where hashing never started.
         *
         * Gated on work actually having arrived from the pool, so an idle or
         * unreachable pool cannot put the miner in a reboot loop. */
        else if (GLOBAL_STATE->ASIC_initalized &&
                 GLOBAL_STATE->SYSTEM_MODULE.work_received > 0 &&
                 !GLOBAL_STATE->SYSTEM_MODULE.is_network_error)
        {
            no_first_nonce_count++;
            #define FIRST_NONCE_WARN_TIME  (120*2)   /* loop runs at 2 Hz */
            #define FIRST_NONCE_FAIL_TIME  (180*2)
            if(no_first_nonce_count > FIRST_NONCE_WARN_TIME)
            {
                /* A board mismatch produces exactly this symptom -- the chip
                 * is found, clocked, and silent -- so without this the screen
                 * blames the ASIC for what is a wrong image, and the restart
                 * below turns it into a loop: rebooting cannot change which
                 * pins the firmware was compiled for, so it fails again three
                 * minutes later, forever, while the web interface needed to
                 * flash the right build keeps going away. */
                if(!GLOBAL_STATE->SYSTEM_MODULE.board_mismatch)
                {
                    showErrorScreen("No ASIC response",HASHRATE_ERROR);
                    if(no_first_nonce_count > FIRST_NONCE_FAIL_TIME)
                    {
                        restart_with_reason("ASIC returned no nonce after bring-up");
                    }
                }
            }
        }
        else
        {
            no_first_nonce_count = 0;
        }

        if(miner_started)
        {
            miner_started_count ++;
            if(miner_started_count > 10000)
            {
                miner_started_count = 10000;
            }
            else if(miner_started_count > 1*120)
            {
                if(GLOBAL_STATE->asic_vol_default > GLOBAL_STATE->HEALTH_MODULE.voltage)
                {
                    if((miner_started_count % 5) == 0)
                    {
                        float voltage = GLOBAL_STATE->asic_vol_default - GLOBAL_STATE->HEALTH_MODULE.voltage;
                        if(voltage > 3)
                        {
                            GLOBAL_STATE->asic_vol_default -= 3;
                            voltage = (float)GLOBAL_STATE->asic_vol_default / 100.0;
                            VCORE_set_voltage(voltage);
                        }
                        else
                        {
                            GLOBAL_STATE->asic_vol_default = GLOBAL_STATE->HEALTH_MODULE.voltage;
                            voltage = (float)GLOBAL_STATE->asic_vol_default / 100.0;
                            VCORE_set_voltage(voltage);
                            ESP_LOGI("system task", "vol set over");
                        }
                    }
                }
            }
        }
        else
        {
            miner_started_count = 0;
        }

        displayFlashPro();

        vTaskDelay(pdMS_TO_TICKS(500)); 
        
        static uint32_t time_last = 0;
        static uint32_t now = 0;
        now = esp_timer_get_time()/1000;

        log_time = now - time_last;
        if(log_time > 20*60*1000)
        {
            time_last = now;
            
            ESP_LOGI("system task", "temp: %.1f  %d  %" PRId8 " ", 
                    GLOBAL_STATE->HEALTH_MODULE.cpu_temperature,read_power_temp(),
                    GLOBAL_STATE->HEALTH_MODULE.board_temperature[0]
                );
            ESP_LOGI("system task", "Vin %.2f Vout %.2f Iout %.2f, Power %.0f",
                GLOBAL_STATE->HEALTH_MODULE.input_voltage,
                GLOBAL_STATE->HEALTH_MODULE.out_voltage,
                GLOBAL_STATE->HEALTH_MODULE.out_current/1000,
                GLOBAL_STATE->HEALTH_MODULE.power
                );
            ESP_LOGI("system task", "HASHRATE %.0fGH/S SHARES %"PRIu64 "",
                GLOBAL_STATE->SYSTEM_MODULE.current_hashrate,
                GLOBAL_STATE->SYSTEM_MODULE.shares_accepted
                );
        }
    }
}
