#include <stdio.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "mbedtls/platform.h"
#include <stdlib.h>
#include <time.h>

#include "nvs_config.h"
#include "nvs_device.h"
#include "main.h"
#include "global_state.h"
#include "miner.h"
#include "self_test.h"
#include "serial.h"
#include "asic.h"
#include "system.h"
#include "gpio_input_output.h"
#include "common.h"
#include "device.h"
#include "http_server.h"

#include "stratum_task.h"
#include "stratum_poolb_task.h"
#include "asic_task.h"
#include "create_jobs_task.h"
#include "asic_result_task.h"
#include "protocol_task.h"
#include "socket_api_task.h"
#include "influx_task.h"
#include "ping_task.h"
#include "http_task.h"

#include "rtc_sync.h"
#include "lvgl_porting.h"
#include "lv_input.h"

#include "network.h"


static const char * TAG = "serpentx";

static GlobalState GLOBAL_STATE = {
    .extranonce_str = NULL, 
    .extranonce_2_len = 0, 
    .abandon_work = 0, 
    .version_mask = 0,
    .ASIC_initalized = false,
    .interface_initalized = false
};

void show_mining_screen(void)
{
    SYSTEM_notify_status_change(&GLOBAL_STATE, SYSTEM_NORMAL_MINING);
}

/*
 * How long to let init_all_peripherals() run before deciding it is stuck.
 *
 * A healthy BC04 is well clear of this: the hashboard rail comes up at about
 * 32 s and the ASICs are detected by 37 s, so the flag is set around 35 s.
 * Ninety seconds leaves room for a slow board without leaving a broken one
 * unreachable for longer than it has to be.
 */
#define ETH_STALL_TIMEOUT_MS (90 * 1000)

static void eth_stall_watchdog(void *arg)
{
    GlobalState *state = (GlobalState *)arg;
    int waited = 0;

    while (waited < ETH_STALL_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;

        if (state->interface_initalized) {
            /* main() is past it and owns the Ethernet start. */
            vTaskDelete(NULL);
        }
    }

    if (!network_get_info().eth_on) {
        vTaskDelete(NULL);
    }

    ESP_LOGE(TAG, "Peripheral init has not finished in %d s -- the hashboard "
                  "is not coming up. Starting Ethernet anyway so this miner "
                  "can still be reached.", ETH_STALL_TIMEOUT_MS / 1000);

    if (network_eth_start() != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet was already started elsewhere");
    }

    vTaskDelete(NULL);
}

void restart_with_reason(const char *reason)
{
    ESP_LOGI(TAG, "Restarting with reason: %s", reason);
    nvs_config_set_string(NVS_CONFIG_RESTART_REASON, reason);
    power_off_hashboard(&GLOBAL_STATE);
    for (int i = 3; i >= 0; i--) {
        ESP_LOGI(TAG, "Restarting in %d seconds...", i);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    ESP_LOGI(TAG, "Restarting now.\n");
    fflush(stdout);
    esp_restart();
}

void restart(void)
{
    restart_with_reason("Normal restart");
}

void init_jobs(GlobalState *GLOBAL_STATE)
{
    for(int chain_num = 0; chain_num < MAX_CHAIN_NUM; chain_num++){
        if(GLOBAL_STATE->chain_pluged[chain_num]){
            //initialize the semaphore
            GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].semaphore = xSemaphoreCreateBinary();

            //GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].active_jobs = malloc(sizeof(bm_job *) * 128);
            //GLOBAL_STATE->valid_jobs[chain_num] = malloc(sizeof(uint8_t) * 128);
            GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].active_jobs = heap_caps_malloc(sizeof(bm_job *) * 128, MALLOC_CAP_SPIRAM);
            GLOBAL_STATE->valid_jobs[chain_num] = heap_caps_malloc(sizeof(uint8_t) * 128, MALLOC_CAP_SPIRAM);
            
            for (int i = 0; i < 128; i++)
            {
                GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].active_jobs[i] = NULL;
                GLOBAL_STATE->valid_jobs[chain_num][i] = 0;
            }
        }
    }
}

static void restart_callback(void)
{
    esp_rom_printf("\nSerpentX restarting...\n\n");
}

void analyze_task_memory(void) {
    ESP_LOGI("TASKS", "=== 任务内存分析 ===");
    
    // 获取任务数量
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    ESP_LOGI("TASKS", "总任务数: %d", task_count);
    
    // 分配数组存储任务状态
    TaskStatus_t *task_status = (TaskStatus_t *)pvPortMalloc(task_count * sizeof(TaskStatus_t));
    
    if (task_status != NULL) {
        // 获取任务状态数组
        task_count = uxTaskGetSystemState(task_status, task_count, NULL);
        
        // 打印每个任务的信息
        for (int i = 0; i < task_count; i++) {
            ESP_LOGI("TASKS", "任务: %s", task_status[i].pcTaskName);
            ESP_LOGI("TASKS", "  优先级: %d", task_status[i].uxCurrentPriority);
            ESP_LOGI("TASKS", "  栈高水位线: %d", task_status[i].usStackHighWaterMark);
            ESP_LOGI("TASKS", "  任务编号: %d", task_status[i].xTaskNumber);
        }
        
        vPortFree(task_status);
    }
}

void improved_memory_monitor(void) 
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    
    ESP_LOGI("MEMORY", "=== 改进后内存状态 ===");
    ESP_LOGI("MEMORY", "内部RAM空闲: %d bytes", internal_free);
    ESP_LOGI("MEMORY", "SPIRAM空闲: %d bytes", spiram_free);
    ESP_LOGI("MEMORY", "总堆空闲: %d bytes", esp_get_free_heap_size());
    ESP_LOGI("MEMORY", "最小堆: %d bytes", esp_get_minimum_free_heap_size());
    
    if (internal_free < 20000) { // 如果内部RAM少于20KB
        ESP_LOGW("MEMORY", "警告: 内部RAM仍然紧张!");
    }
}

// 内存监控任务
void memory_monitor_task(void *pvParameters) {
    ESP_LOGI("MONITOR", "内存监控任务启动");
    
    while (1) {
        ESP_LOGI("MONITOR", "=== 实时内存状态 ===");
        
        // 基础内存信息
        size_t free_heap = xPortGetFreeHeapSize();
        size_t min_free_heap = xPortGetMinimumEverFreeHeapSize();
        
        ESP_LOGI("MONITOR", "当前堆: %d bytes, 最小堆: %d bytes", 
                 free_heap, min_free_heap);
        
        // 堆碎片分析
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_8BIT);
        
        ESP_LOGI("MONITOR", "最大空闲块: %d bytes", info.largest_free_block);
        ESP_LOGI("MONITOR", "空闲块数量: %d", info.free_blocks);
        
        // 内存使用率计算
        float fragmentation = 0.0;
        if (info.largest_free_block > 0) {
            fragmentation = (1.0 - ((float)info.largest_free_block / info.total_free_bytes)) * 100.0;
        }
        ESP_LOGI("MONITOR", "内存碎片率: %.2f%%", fragmentation);
        
        vTaskDelay(pdMS_TO_TICKS(10000)); // 每10秒检查一次

        analyze_task_memory();
        vTaskDelay(pdMS_TO_TICKS(10000)); // 每10秒检查一次
        improved_memory_monitor();

        vTaskDelay(pdMS_TO_TICKS(10000)); // 每10秒检查一次
    }
}

void showLastResetReason(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    static const char* m_lastResetReason;
    switch (reason) {
        case ESP_RST_UNKNOWN: m_lastResetReason = "Unknown"; break;
        case ESP_RST_POWERON: m_lastResetReason = "Power on reset"; break;
        case ESP_RST_EXT: m_lastResetReason = "External reset"; break;
        case ESP_RST_SW: m_lastResetReason = "Software reset"; break;
        case ESP_RST_PANIC: m_lastResetReason = "Software panic reset"; break;
        case ESP_RST_INT_WDT: m_lastResetReason = "Interrupt watchdog reset"; break;
        case ESP_RST_TASK_WDT: m_lastResetReason = "Task watchdog reset"; break;
        case ESP_RST_WDT: m_lastResetReason = "Other watchdog reset"; break;
        case ESP_RST_DEEPSLEEP: m_lastResetReason = "Exiting deep sleep"; break;
        case ESP_RST_BROWNOUT: m_lastResetReason = "Brownout reset"; break;
        case ESP_RST_SDIO: m_lastResetReason = "SDIO reset"; break;
        default: m_lastResetReason = "Not specified"; break;
    }
    ESP_LOGI(TAG, "Reset reason: %s", m_lastResetReason);
}

void showLastResetReasonCustom(void)
{
    char *custom_reason = nvs_config_get_string(NVS_CONFIG_RESTART_REASON, "None");
    if (custom_reason != NULL) {
        ESP_LOGI(TAG, "Custom restart reason: %s", custom_reason);
        nvs_config_set_string(NVS_CONFIG_RESTART_REASON, "None");
    }
}

// Custom calloc function that allocates from PSRAM
void *psram_calloc(size_t num, size_t size) {
    void *ptr = heap_caps_calloc(num, size, MALLOC_CAP_SPIRAM);
    if (!ptr) {
        ESP_LOGE(TAG, "PSRAM allocation failed! Falling back to internal RAM.");
        ptr = heap_caps_calloc(num, size, MALLOC_CAP_DEFAULT);
    }
    return ptr;
}

void free_psram(void *ptr) {
    heap_caps_free(ptr);
}

/*
 * The mining path logs at debug level: every job dequeued, every nonce the
 * ASIC returns with its difficulty, every share submitted and the pool's
 * answer. That is the view ESP-Miner and its forks give, and without it the
 * log page can only say the miner is connected -- it cannot show it working,
 * which is the thing an operator actually wants to watch.
 *
 * Raised per tag rather than globally: at debug the wifi and lwip stacks
 * out-log the miner several times over and bury it.
 */
static void enable_mining_logs(void)
{
    static const char *const mining_tags[] = {
        "asic_result",       /* nonces, their difficulty, share submission */
        "create_jobs_task",  /* work dequeued and built */
        "stratum_task",      /* pool responses, accept/reject, timing */
        "stratum_api",       /* the stratum messages themselves */
    };

    for (size_t i = 0; i < sizeof(mining_tags) / sizeof(mining_tags[0]); i++) {
        esp_log_level_set(mining_tags[i], ESP_LOG_DEBUG);
    }
}

/*
 * The board model is read from NVS at runtime. The ASIC's UART pins are fixed
 * when the image is built. Nothing ties the two together, so a BC01 image
 * whose settings say BC04 takes every BC04 code path over BC01 wiring: it
 * boots, serves its web interface, detects the chip, and never returns a
 * single share. That is indistinguishable from dead hardware, and it cost a
 * long debugging session to find from the other end -- so it is worth saying
 * plainly at boot.
 *
 * Only boards whose pinout is actually known are checked. The BC01 was
 * measured on hardware; the BC04 comes from the vendor's own reference
 * configuration. Anything else passes, because guessing here would block a
 * board that works.
 */
static void check_board_matches_image(GlobalState * state)
{
    static const struct {
        DeviceModel model;
        int tx;
        int rx;
        const char *name;
    } known[] = {
        { DEVICE_BC01,     18, 17, "BC01" },
        { DEVICE_BC01_Pro, 18, 17, "BC01 Pro" },
        { DEVICE_BC04,     17, 18, "BC04" },
    };

    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (known[i].model != state->device_model) {
            continue;
        }
        if (known[i].tx == UART_CHAIN_0_TXD0 && known[i].rx == UART_CHAIN_0_RXD0) {
            return;
        }

        state->SYSTEM_MODULE.board_mismatch = true;
        ESP_LOGE(TAG, "================ BOARD MISMATCH ================");
        ESP_LOGE(TAG, "Settings say this is a %s.", known[i].name);
        ESP_LOGE(TAG, "This image drives the ASIC on TX %d / RX %d;",
                 UART_CHAIN_0_TXD0, UART_CHAIN_0_RXD0);
        ESP_LOGE(TAG, "a %s needs TX %d / RX %d.",
                 known[i].name, known[i].tx, known[i].rx);
        ESP_LOGE(TAG, "The chip will be detected and will never return a");
        ESP_LOGE(TAG, "share. Flash the %s build instead.", known[i].name);
        ESP_LOGE(TAG, "================================================");
        return;
    }
}

void app_main(void)
{
    init_logging_system();
    enable_mining_logs();

    esp_rom_printf("\nSerpentX starting....\n\n");
    ESP_LOGI(TAG, "APP: %s %s", CONFIG_APP_PROJECT_VER, __TIME__);
	
    showLastResetReason();

    // use PSRAM because TLS costs a lot of internal RAM
    mbedtls_platform_set_calloc_free(psram_calloc, free_psram);

    esp_err_t ret = esp_register_shutdown_handler(restart_callback);
    if (ret != ESP_OK) {
        ESP_LOGE("RESTART", "Failed to register restart callback: %s", esp_err_to_name(ret));
    }

    //initialize the ESP32 NVS
    if (NVSDevice_init() != ESP_OK){
        ESP_LOGE(TAG, "Failed to init NVS");
        return;
    }

    /*
     * Apply the configured timezone here rather than leaving it to the network
     * task. Log lines are stamped with localtime_r(), and the timezone used to
     * arrive only when rtc_sync() ran -- which waits on the network -- so every
     * line logged before that point was stamped UTC and everything after it
     * local. A single log file stepped four hours sideways partway down, which
     * makes it unsortable and makes correlating a fault with a wall clock a
     * guess. Nothing here needs the network: the value is already in NVS.
     */
    {
        /* same fallback as rtc_sync(), so an unset timezone still gives one
         * consistent stamp rather than two different wrong ones */
        char * tz = nvs_config_get_string(NVS_CONFIG_TIME_ZONE, "CST-8");
        if (tz != NULL) {
            setenv("TZ", tz, 1);
            tzset();
            ESP_LOGI(TAG, "Timezone applied: %s", tz);
            free(tz);
        }
    }

    showLastResetReasonCustom();

    /*get the nvs config, parse the NVS config into GLOBAL_STATE*/
    if (NVSDevice_parse_config(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse NVS config");
        return;
    }

    check_board_matches_image(&GLOBAL_STATE);

    GLOBAL_STATE.global_parameter_mutex = xSemaphoreCreateMutex();   
    SYSTEM_init_system(&GLOBAL_STATE);
    #ifdef STATISTIC_SYSTEM_FEATURE
    statistic_init_system_by_device(&GLOBAL_STATE);
    #endif

    /*init the lcd display.*/
    dev_display_init(&GLOBAL_STATE);

	ESP_ERROR_CHECK(bc_i2c_init());

    #if 1
    if(should_test()){
        /*
         * The self test measures the fan and the core regulator, and on the
         * BC01 family both sit behind the USB-PD gate: until the supply is
         * negotiated they have no power, do not answer, and cannot be
         * measured. The test ran before any of that and duly reported a fan
         * turning at 0 rpm on every setting -- a correct measurement of an
         * unpowered fan, read as a broken board.
         *
         * A self-test boot restarts before init_all_peripherals() would
         * otherwise do this, so it is the only bring-up on this path.
         */
        if (device_is_bc01_family(GLOBAL_STATE.device_model) &&
            bc01_pd_bringup(&GLOBAL_STATE) != ESP_OK) {
            ESP_LOGE(TAG, "USB-PD bring-up failed; self test cannot measure "
                          "the hashboard");
        }

        self_test(&GLOBAL_STATE);
        power_off_hashboard(&GLOBAL_STATE);
        if(GLOBAL_STATE.SELF_TEST_MODULE.result)
            vTaskDelay(pdMS_TO_TICKS(5000));
        else
            vTaskDelay(pdMS_TO_TICKS(10000));
        restart_with_reason("Self test completed");
        return;
    }
    #else
    {
        nvs_config_set_u16(NVS_CONFIG_SELF_TEST,1);
    }
    #endif

	/*create the health task*/
    xTaskCreate(health_maintenance_task, "health_maintener", 4096, (void*)&GLOBAL_STATE, 12, NULL);

    /*
     * Secure the supply before switching the radio on.
     *
     * WiFi transmit is the largest current step this board makes, and until
     * this call the BC01 family is running on whatever USB-C offers before a
     * PD contract exists. network_init() used to bring the radio up around
     * two seconds and negotiation did not happen until twelve, so for ten
     * seconds the board took its biggest load on its smallest supply.
     *
     * With the display module's own USB-C plugged into a computer that
     * second 5 V source carries it and nothing shows. On its own -- which is
     * how a miner actually runs -- a BC01 with a replacement LilyGO module
     * reset at five to ten seconds and did it again on every boot, forever.
     * It was reproduced on the bench: loops with the module's USB-C out,
     * boots with it in, regardless of where the miner is sitting.
     *
     * The self test already did this for the same reason -- everything
     * behind the PD gate is unpowered until the contract exists. The rest of
     * the boot needs it just as much.
     */
    if (device_is_bc01_family(GLOBAL_STATE.device_model) &&
        bc01_pd_bringup(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGW(TAG, "USB-PD not negotiated yet; bringing the network up on "
                      "the default supply and trying again with the "
                      "peripherals");
    }

    network_init(&GLOBAL_STATE);

    /*sync the locol time.*/
    vSemaphoreCreateBinary(xSyncTimeSemaphore);
    vSemaphoreCreateBinary(xSyncTimeDoneSemaphore);
    if(NULL == xSyncTimeSemaphore || NULL == xSyncTimeDoneSemaphore){
        ESP_LOGE(TAG, "Could not create xSyncTimeSemaphore or xSyncTimeDoneSemaphore.");
        ESP_ERROR_CHECK(ESP_FAIL);
    }else{
        if (pdFALSE == xSemaphoreTake(xSyncTimeSemaphore, 0)) {
            ESP_LOGI(TAG, "Could not take xSyncTimeSemaphore.");
            ESP_ERROR_CHECK(ESP_FAIL);
        }else{
            xTaskCreate(rtc_sync_task, "rtc-sync", 4096, NULL, 5, NULL);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
   
    xSemaphoreTake(xSyncTimeDoneSemaphore, portMAX_DELAY);

    /*Should enter sleep mode?*/
    if(SLEEP_MODE == GLOBAL_STATE.SYSTEM_MODULE.boot_mode){
        ESP_LOGI(TAG, "Enter sleep mode....");
        GLOBAL_STATE.SYSTEM_MODULE.is_sleep_mode = true;
        return;
    }

    /*
     * Ethernet must not be hostage to the hashboard.
     *
     * init_all_peripherals() talks to the hashboard over I2C, and on a board
     * whose bus is dead it does not come back -- observed on a BC04 with a
     * shorted bus, still sitting inside it after four minutes. Everything
     * below, including the Ethernet start, is then unreachable. A miner in
     * that state with no WiFi credentials has no management interface at all:
     * no API, no web interface, nothing but USB. That is precisely the
     * situation where being able to reach it matters most.
     *
     * So a watchdog starts Ethernet if init has not finished in time. It
     * cannot simply run on a timer, because the reason Ethernet is deferred
     * at all is that a W5500 running when the core rail steps up stops
     * answering for good. Waiting on interface_initalized keeps that
     * guarantee: Ethernet starts either after the hashboard is up, or after
     * we know it is never coming up. Never in between.
     */
    xTaskCreate(eth_stall_watchdog, "eth-stall", 3072, &GLOBAL_STATE, 3, NULL);

    /*
     * Not ESP_ERROR_CHECK. The only failure this reports is a voltage
     * regulator that did not answer, and it has already logged that, raised
     * the error and left the hashboard unpowered. Aborting here would restore
     * the reboot loop that change exists to remove.
     */
    if (ESP_OK != init_all_peripherals(&GLOBAL_STATE)) {
        ESP_LOGE(TAG, "hashboard peripherals did not initialise; carrying on "
                      "so the miner stays reachable");
    }

    /* The hashboard rail has just come up and settled.
     *
     * If network_init() left Ethernet alone because WiFi could carry the boot,
     * this is where it starts -- after the transient rather than through it.
     * Otherwise the controller has been running all along and, on a BC04, has
     * already stopped answering; try restarting it. Boards with no Ethernet,
     * or nothing deferred, return ESP_ERR_INVALID_STATE and are unaffected. */
    if (network_get_info().eth_on) {
        if (network_eth_start() != ESP_OK) {
            network_eth_recover();
        }
        /* Both of those run once and never again. From here on the controller
         * is watched, because one that wedges an hour later keeps its IP,
         * passes nothing, and reports nothing. */
        network_eth_start_health_watch();
    }

    /*Serial Init and detect the asic.*/
    if(ASIC_detect(&GLOBAL_STATE))
    {
        power_off_hashboard(&GLOBAL_STATE);
        xTaskCreate(system_task, "system task", 4096, (void*)&GLOBAL_STATE, 4, NULL);
        show_mining_screen();
        return;
    }
    else
    {
        if((strlen(GLOBAL_STATE.SYSTEM_MODULE.pool_url) < 5)&&(strlen(GLOBAL_STATE.SYSTEM_MODULE.fallback_pool_url) < 5))
        {
            ESP_LOGI(TAG, "error pool : %s , %s ",GLOBAL_STATE.SYSTEM_MODULE.pool_url, GLOBAL_STATE.SYSTEM_MODULE.fallback_pool_url);
            power_off_hashboard(&GLOBAL_STATE);
        }
    }

    /*create the queue.*/
    queue_init(&GLOBAL_STATE.stratum_queue);

    /* dual mining: pool B's own queue and the locks guarding its session state.
     * Initialised unconditionally so enabling dual mining at runtime does not
     * need a restart to have somewhere to put work. */
    queue_init(&GLOBAL_STATE.stratum_queueB);
    pthread_mutex_init(&GLOBAL_STATE.transportB_lock, NULL);
    pthread_mutex_init(&GLOBAL_STATE.extranonceB_lock, NULL);
    for(int i = 0; i < MAX_CHAIN_NUM; i++){
        if(GLOBAL_STATE.chain_pluged[i])
            queue_init(&GLOBAL_STATE.ASIC_jobs_queue[i]);
    }
    init_jobs(&GLOBAL_STATE);

    /*create the stratum task.*/
    xTaskCreate(stratum_task, "stratum_manager", 8192, (void *)&GLOBAL_STATE, 5, NULL);
    /* pool B idles cheaply when dual mining is off, so it is always started */
    xTaskCreate(stratum_poolb_task, "stratum_poolb", 8192, (void *)&GLOBAL_STATE, 5, NULL);
    /*create the uart task.*/
    xTaskCreate(create_jobs_task, "stratum_worker", 6144, (void *)&GLOBAL_STATE, 10, NULL);
    /*create the socket api task.*/
    #ifdef SOCKET_API_FEAUTRE
    xTaskCreate(socket_api_task, "socket-api", 4096, (void*)&GLOBAL_STATE, 16, NULL);
    #endif
    /*create influx task*/
    //xTaskCreate(influx_task, "influx", 8192, (void*)&GLOBAL_STATE, 23, NULL);
    /*create ping pool task*/
    //xTaskCreate(ping_task, "ping task", 4096, (void*)&GLOBAL_STATE, 20, NULL);
    /*create http task*/
    xTaskCreate(http_task, "http task", 6144, (void*)&GLOBAL_STATE, 3, NULL);
    /*create memory monitor task*/
    //xTaskCreate(memory_monitor_task, "memory monitor task", 4096, (void*)&GLOBAL_STATE, 1, NULL);
    /*create system task*/
    xTaskCreate(system_task, "system task", 4096, (void*)&GLOBAL_STATE, 4, NULL);

    AsicParam chain_param[2] = {{.chain_num = 0, .p_global_state = &GLOBAL_STATE}, {.chain_num = 1, .p_global_state = &GLOBAL_STATE}};
    for(uint32_t i = 0; i < MAX_CHAIN_NUM; i++)
    {
        if(GLOBAL_STATE.chain_pluged[i]){
            ESP_LOGI(TAG, "create tasks for chain %"PRIu32"", i);
            /*create the asic work.*/
            char tmp_name[30];
            BaseType_t xReturn;

            {
                sprintf(tmp_name, "asic_task_%"PRIu32"", i);
                xReturn = xTaskCreate(ASIC_task, tmp_name, 5120, (void *)&(chain_param[i]), 10, NULL);
                if(pdPASS != xReturn){
                    ESP_LOGW(TAG, "Create %s failed %d.", tmp_name, xReturn);
                }
            }
            
            {
                sprintf(tmp_name, "asic_result_%"PRIu32"", i);
                xReturn = xTaskCreate(ASIC_result_task, tmp_name, 6144, (void *)&(chain_param[i]), 15, NULL);
                if(pdPASS != xReturn){
                    ESP_LOGW(TAG, "Create %s failed %d.", tmp_name, xReturn);
                }
            }
            #if 1
            {
                sprintf(tmp_name, "hashrate_task_%"PRIu32"", i);
                xReturn = xTaskCreate(hashrate_monitor_task, tmp_name, 4096, (void *)&(chain_param[i]), 5, NULL);
                if(pdPASS != xReturn){
                    ESP_LOGW(TAG, "Create %s failed %d.", tmp_name, xReturn);
                }
            }
            #endif
        }
    }

    SYSTEM_notify_status_change(&GLOBAL_STATE, SYSTEM_HARDWARE_OK);
    show_mining_screen();
    
    ESP_LOGI(TAG, "heap free heap size: %" PRIu32 " bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Minimum free heap size: %" PRIu32 " bytes", esp_get_minimum_free_heap_size());
    /*restart();*/
}
