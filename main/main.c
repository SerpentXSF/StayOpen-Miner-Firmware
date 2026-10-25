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
#include "esp_timer.h"       /* esp_timer_get_time(), for the heap marks */
#include "esp_task.h"        /* ESP_TASK_MAIN_STACK: the config size + padding */
#include "esp_wifi.h"
#include "mbedtls/platform.h"
#include <stdlib.h>
#include <time.h>

#include "nvs_config.h"
#include "nvs_device.h"
#include "main.h"
#include "global_state.h"
#include "pool_scheduler.h"
#include "miner.h"
#include "self_test.h"
#include "ethernet_init.h"
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
/*
 * Longer than the regulator can take to give up, which 90 s was not.
 *
 * This fires to start Ethernet once the hashboard is judged never to be
 * coming. The thing that decides that is TPS546_init(), which retries 100
 * times at 2 s apart -- 200 s. At 90 s it was entirely possible for a
 * regulator to answer *after* the watchdog had already started Ethernet, so
 * the core rail then stepped up with the W5500 live: the exact ordering the
 * deferral exists to prevent, and the one mechanism this project has direct
 * evidence for. A marginal connector after a board is moved is how a
 * regulator comes up slow.
 *
 * The comment this replaces promised Ethernet starts "either after the
 * hashboard is up, or after we know it is never coming up. Never in between."
 * The numbers allowed in between. These ones do not.
 *
 * The cost is that a board whose hashboard really is dead stays unreachable
 * for four minutes rather than ninety seconds. That is the right trade: being
 * slow to reach a broken board is an inconvenience, and energising a rail
 * underneath a live W5500 is how boards die.
 */
#define ETH_STALL_TIMEOUT_MS (240 * 1000)

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

/*
 * One compact line per task, because the figure that matters is how much of
 * each stack is never used.
 *
 * usStackHighWaterMark is the smallest free space that stack has ever had, in
 * words. A task created with 8192 bytes that still reports 6 KB free has been
 * over-allocated by that much, and on this chip that memory is internal RAM --
 * the scarce pool, not the eight megabytes of PSRAM the dashboard reports.
 * Internal RAM has been measured down to 1.1 KB free since boot here, so every
 * over-allocated stack is worth knowing about.
 */
void analyze_task_memory(void) {
    UBaseType_t count = uxTaskGetNumberOfTasks();

    /*
     * The census goes in PSRAM, not internal RAM.
     *
     * pvPortMalloc() takes internal, and this wants about 40 bytes per task --
     * a kilobyte and more on a BC04, asked for by the very measurement that
     * exists to find out why internal RAM is short. Taking it from the pool
     * under study changes the answer. Nothing here is touched by an ISR or by
     * DMA, so PSRAM is simply the right place for it.
     */
    TaskStatus_t *tasks = heap_caps_malloc(count * sizeof(TaskStatus_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tasks == NULL) {
        /* No PSRAM, or none left: internal is better than no reading at all. */
        tasks = (TaskStatus_t *) pvPortMalloc(count * sizeof(TaskStatus_t));
    }
    if (tasks == NULL) {
        ESP_LOGW("TASKS", "cannot allocate %u task records", (unsigned) count);
        return;
    }

    count = uxTaskGetSystemState(tasks, count, NULL);

    uint32_t unused_total = 0;
    ESP_LOGI("TASKS", "--- %u tasks: unused stack, bytes ---", (unsigned) count);
    for (UBaseType_t i = 0; i < count; i++) {
        uint32_t unused = (uint32_t) tasks[i].usStackHighWaterMark * sizeof(StackType_t);
        unused_total += unused;
        ESP_LOGI("TASKS", "  %-20s prio %2u  unused %6" PRIu32,
                 tasks[i].pcTaskName, (unsigned) tasks[i].uxCurrentPriority, unused);
    }
    ESP_LOGI("TASKS", "  total unused stack across all tasks: %" PRIu32 " bytes (%" PRIu32 " KB)",
             unused_total, unused_total / 1024);

    heap_caps_free(tasks);
}

/*
 * Where the internal pool actually goes, which nothing has ever measured.
 *
 * Ten reboots put minFreeHeapInternal between 131 and 1,639 bytes, every time,
 * and in two of them the floor fell after the 70-second mark -- so it is not
 * only a start-up cost. The census that exists fires once at two minutes, long
 * after the dip, and cannot say when or where.
 *
 * Both run on the caller's stack, which is the point: the task that used to do
 * this work is deliberately not started because its 3 KB of internal stack was
 * once enough to stop asic_result_0 being created.
 *
 * THEY ARE NOT FREE, and an earlier version of this comment claimed they were.
 * init_logging_system() is app_main's first statement and installs
 * log_to_queue() as the vprintf hook, which malloc()s twice per line -- the
 * formatted string and a timestamped copy for the web log queue. With
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192 those go to the internal pool
 * first, so each line reported here costs that pool roughly twice the line's
 * length, transiently, and the queued copy until the websocket drain runs.
 *
 * Two consequences worth holding on to:
 *
 *   - a reported floor may include this instrument's own allocations, and
 *     cannot be separated from them after the fact;
 *   - heap_watermark_moved() can therefore push the floor down itself and
 *     report its own effect on the next pass, so "monotonic, so the line count
 *     is bounded" is weaker than it sounds.
 *
 * It is still worth having -- it is the only reading of this window that
 * exists -- but the figures are an upper bound on the pressure, not a clean
 * one. Bypassing the hook with esp_rom_printf() would fix that and would also
 * take the lines off the kernel log the API serves, which is the only way they
 * can be read on a board with no serial attached.
 */
/*
 * DMA figures as well as internal, on the same line.
 *
 * MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL is not the pool that fails. It counts
 * the 8 KB RTC fast region, which carries no MALLOC_CAP_DMA, and its
 * largest-block is a maximum across heaps while its minimum is a sum of
 * per-heap minima. What actually runs out is DMA-capable DRAM: esp-aes
 * allocates its descriptor array with MALLOC_CAP_DMA per record, tens of
 * bytes, and that is the allocation that fails on every boot.
 *
 * On this chip the query is unambiguous -- esp_psram registers PSRAM with
 * SPIRAM|DEFAULT and 8BIT|32BIT|SIMD and no DMA capability, so MALLOC_CAP_DMA
 * is internal DRAM and nothing else.
 *
 * Same line, not another one: every log line costs two mallocs from the pool
 * being measured, so adding figures to a line already being printed is free
 * where a second line is not.
 */
void heap_mark(const char *stage)
{
    ESP_LOGI("MEMORY", "[%8lld ms] %-22s internal free %u, largest block %u, "
                       "min since boot %u | DMA free %u, largest %u, min %u",
             esp_timer_get_time() / 1000, stage,
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));
}

/*
 * Logs only when the floor drops, so it is silent on a healthy run and says
 * exactly when it is not. Cheap enough to leave on: the minimum is monotonic,
 * so the number of lines this can ever produce is bounded by how far the pool
 * has to fall, and in practice that is a handful per boot.
 *
 * Returns true when it logged, so a caller can do something more expensive on
 * the way down if it wants to.
 */
bool heap_watermark_moved(void)
{
    /*
     * Two floors, because they are not the same pool and only one of them has
     * ever been seen to break anything. Either falling is worth a line; both
     * are reported on it, so a reader can see which moved.
     */
    static size_t floor_internal = SIZE_MAX;
    static size_t floor_dma = SIZE_MAX;

    size_t now_int = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t now_dma = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);

    if (now_int >= floor_internal && now_dma >= floor_dma) {
        return false;
    }
    const char *which = (now_int < floor_internal && now_dma < floor_dma) ? "both"
                      : (now_dma < floor_dma) ? "DMA" : "internal";
    floor_internal = now_int < floor_internal ? now_int : floor_internal;
    floor_dma = now_dma < floor_dma ? now_dma : floor_dma;

    ESP_LOGW("MEMORY", "[%8lld ms] %s floor fell -- internal min %u (free %u, "
                       "largest %u) | DMA min %u (free %u, largest %u)",
             esp_timer_get_time() / 1000, which,
             (unsigned) floor_internal,
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (unsigned) floor_dma,
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return true;
}

void improved_memory_monitor(void)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t internal_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t internal_blk  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t spiram_free   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI("MEMORY", "internal free %u, min since boot %u, largest block %u",
             (unsigned) internal_free, (unsigned) internal_min, (unsigned) internal_blk);
    ESP_LOGI("MEMORY", "PSRAM free %u, total heap %u",
             (unsigned) spiram_free, (unsigned) esp_get_free_heap_size());

    /*
     * A TLS session wants tens of kilobytes of internal RAM and will not take
     * PSRAM. Below about 40 KB the next handshake is the one that fails, which
     * is how a broken price fetch hid behind eight megabytes of free heap.
     */
    if (internal_min < 40000) {
        ESP_LOGW("MEMORY", "internal RAM reached %u bytes since boot -- a TLS "
                           "handshake needs more than that", (unsigned) internal_min);
    }
}

/*
 * Reports where internal RAM is going, every two minutes.
 *
 * Started only where it earns its keep -- see the call site. It exists because
 * nothing else on this device measures the internal pool, and a figure that
 * only appears when a TLS handshake fails is a figure that arrives too late.
 */
void memory_monitor_task(void *pvParameters) {
    /* Let start-up finish before the first reading, or every line describes
     * the boot rather than how the miner actually runs. */
    vTaskDelay(pdMS_TO_TICKS(90 * 1000));

    while (1) {
        improved_memory_monitor();
        analyze_task_memory();
        vTaskDelay(pdMS_TO_TICKS(120 * 1000));
    }
}

/*
 * Held at file scope so the API can report them. They used to be local to the
 * two functions below, which logged them and let them go -- and a log is not
 * evidence once the 262 KB ring has turned over. Both boards restarted inside
 * the same minute on 2026-10-21 and by the time anyone looked, three days
 * later, the only record of why had scrolled away.
 */
static const char *s_last_reset_reason = "Unknown";
static char s_last_restart_reason[48] = "None";

const char *system_last_reset_reason(void)
{
    return s_last_reset_reason;
}

const char *system_last_restart_reason(void)
{
    return s_last_restart_reason;
}

void showLastResetReason(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    const char *m_lastResetReason;
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
        /*
         * These five were missing and fell through to "Not specified", which
         * is the least useful thing this function can say. Three of them name
         * failures this project has actually spent time chasing:
         *
         *   USB         -- a BC01 that needed a USB cable to boot looped for
         *                  days, and a reset caused by the USB peripheral
         *                  reported as nothing at all.
         *   PWR_GLITCH  -- a supply disturbance too brief to trip the brownout
         *                  detector, on boards whose whole story is a core
         *                  rail stepping hard.
         *   CPU_LOCKUP  -- the shape of the unbounded I2C wait that hung a
         *                  BC04 silently at 7.2 seconds.
         *
         * A reset reason costs nothing to report and is often the only
         * evidence left after a board comes back.
         */
        case ESP_RST_USB: m_lastResetReason = "USB peripheral reset"; break;
        case ESP_RST_JTAG: m_lastResetReason = "JTAG reset"; break;
        case ESP_RST_EFUSE: m_lastResetReason = "eFuse error reset"; break;
        case ESP_RST_PWR_GLITCH: m_lastResetReason = "Power glitch reset"; break;
        case ESP_RST_CPU_LOCKUP: m_lastResetReason = "CPU lockup reset"; break;
        /* Carry the number, so a reason added by a later IDF is still actionable. */
        default: {
            static char unknown[32];
            snprintf(unknown, sizeof(unknown), "Not specified (%d)", (int) reason);
            m_lastResetReason = unknown;
            break;
        }
    }
    s_last_reset_reason = m_lastResetReason;
    ESP_LOGI(TAG, "Reset reason: %s", m_lastResetReason);
}

void showLastResetReasonCustom(void)
{
    char *custom_reason = nvs_config_get_string(NVS_CONFIG_RESTART_REASON, "None");
    if (custom_reason != NULL) {
        /* Copied before the key is cleared below -- otherwise the reason is
         * gone from NVS and from memory in the same breath, and the API has
         * nothing to report for the rest of the run. */
        snprintf(s_last_restart_reason, sizeof(s_last_restart_reason), "%s",
                 custom_reason);
        ESP_LOGI(TAG, "Custom restart reason: %s", custom_reason);
        nvs_config_set_string(NVS_CONFIG_RESTART_REASON, "None");
        /* nvs_config_get_string() hands back malloc'd memory on every path,
         * including its strdup'd defaults. This one was never freed. */
        free(custom_reason);
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

/*
 * Did a task the miner cannot mine without fail to start?
 *
 * File scope rather than a local, because the answer has to outlive the loop
 * that sets it and reach the SYSTEM_HARDWARE_OK call at the end of app_main().
 */
static bool s_critical_task_failed = false;

/*
 * Task creation that cannot fail quietly.
 *
 * Every xTaskCreate() in app_main() either ignored its return value outright
 * or logged one ESP_LOGW and carried on. That is how a BC04 on this bench
 * spent twenty minutes reporting four ASICs, 12 V in, a healthy network and
 * 0 GH/s: asic_result_0 had failed to create for want of internal RAM -- the
 * entire record of it was the line "Create asic_result_0 failed -1." -- and
 * nothing else in the firmware knew. Jobs were built, work was sent to the
 * chips, and the nonces came back to a task that did not exist.
 *
 * FreeRTOS fails this call for one reason: it could not allocate the stack.
 * Stacks come out of internal RAM, which on a BC04 has been measured down to
 * 203 bytes free at its low-water mark since boot, so this is a live failure
 * mode rather than a theoretical one -- and the memory work that follows will
 * be moving exactly these allocations around.
 *
 * `critical` marks a task the miner cannot mine without. Those raise a fault
 * and stop the miner reporting itself healthy. Where that fault shows up,
 * stated exactly, because two of the obvious places are not among them:
 *
 *   - `systemError` in /api/system/info, and /api/get_err. Durable: the
 *     string is only cleared for NETWORK_ERROR and WIFI_CONNETION_ERROR, so
 *     a SPECIAL_ERROR outlives SYSTEM_NORMAL_MINING.
 *   - the dashboard's alert column, which reads that same field.
 *   - the status LED, but only until the pool hands over a job:
 *     SYSTEM_notify_get_work() raises SYSTEM_NORMAL_MINING, which puts the
 *     LED back to its mining pattern. The string survives; the light does
 *     not.
 *   - NOT the T-Display. Nothing under main/displays reads system_error and
 *     showErrorScreen() is commented out in system.c.
 *
 * The rest are logged at ESP_LOGE and left alone -- losing the price fetcher
 * is not a reason to tell an owner their hardware is broken.
 *
 * Reporting only. Nothing here retries, and nothing powers the hashboard
 * down -- at this point in boot the heap is at its most contended, so a
 * retry would be the same roll of the dice, and a chain that is hashing into
 * a missing reader is still a chain whose owner may want to look at it
 * before it stops.
 */
static bool create_task_checked(GlobalState *state, TaskFunction_t fn,
                                const char *name, uint32_t stack, void *arg,
                                UBaseType_t prio, bool critical)
{
    if (pdPASS == xTaskCreate(fn, name, stack, arg, prio, NULL)) {
        return true;
    }

    ESP_LOGE(TAG, "task '%s' did not start: no internal RAM for its %" PRIu32
                  " byte stack. Internal heap: %u free, largest block %u.",
             name, stack,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));

    if (critical) {
        char info[48];
        snprintf(info, sizeof(info), "%s did not start", name);
        ESP_LOGE(TAG, "'%s' is required in order to mine. Raising a fault: "
                      "this miner will not hash and must not report itself "
                      "healthy.", name);
        SYSTEM_notify_error_info(state, SPECIAL_ERROR, info);
        s_critical_task_failed = true;
    }
    return false;
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

    heap_mark("before display init");
    /*init the lcd display.*/
    dev_display_init(&GLOBAL_STATE);
    heap_mark("after display init");

    heap_mark("before i2c init");
	ESP_ERROR_CHECK(bc_i2c_init());

    /*
     * Before anything switches the core rail on -- the self test below does it
     * directly, the normal boot does it in init_all_peripherals() -- take the
     * Ethernet controller out of the path of that step.
     *
     * Deferring the driver was only ever half of it. The W5500 runs from its
     * own supply and is live and linked whether or not this firmware has
     * opened the SPI bus, which is exactly how the first BC04's controller was
     * sitting when it stopped answering 70 ms into the transient.
     *
     * The BC01 family has no W5500; leave its GPIO alone.
     */
    if (!device_is_bc01_family(GLOBAL_STATE.device_model)) {
        eth_phy_hold_in_reset();
    }

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
    create_task_checked(&GLOBAL_STATE, health_maintenance_task, "health_maintener",
                        4096, (void*)&GLOBAL_STATE, 12, false);

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

    heap_mark("before network init");
    network_init(&GLOBAL_STATE);
    heap_mark("after network init");

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
            create_task_checked(&GLOBAL_STATE, rtc_sync_task, "rtc-sync",
                                4096, NULL, 5, false);
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
    create_task_checked(&GLOBAL_STATE, eth_stall_watchdog, "eth-stall",
                        3072, &GLOBAL_STATE, 3, false);

    /*
     * Not ESP_ERROR_CHECK. The only failure this reports is a voltage
     * regulator that did not answer, and it has already logged that, raised
     * the error and left the hashboard unpowered. Aborting here would restore
     * the reboot loop that change exists to remove.
     */
    heap_mark("before peripherals");
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
    /*
     * Unconditional, because network_init() can defer Ethernet even when
     * eth_on is 0 -- that is the last-resort path for a board with neither
     * interface configured. network_eth_start() answers ESP_ERR_INVALID_STATE
     * when nothing was deferred, so this is a no-op on a board without it.
     */
    {
        esp_err_t eth_started = network_eth_start();
        if (ESP_OK != eth_started && network_get_info().eth_on) {
            network_eth_recover();
        }
        /* Both of those run once and never again. From here on the controller
         * is watched, because one that wedges an hour later keeps its IP,
         * passes nothing, and reports nothing. Safe on boards with no
         * controller: it returns without starting anything. */
        network_eth_start_health_watch();
    }

    /*Serial Init and detect the asic.*/
    if(ASIC_detect(&GLOBAL_STATE))
    {
        power_off_hashboard(&GLOBAL_STATE);
        create_task_checked(&GLOBAL_STATE, system_task, "system task",
                            4096, (void*)&GLOBAL_STATE, 4, true);
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

    /*
     * Every secondary pool's queue and the locks guarding its session state.
     *
     * All of them, unconditionally, and not just the ones configured right
     * now: a pool can be filled in from the web UI while the miner is running,
     * and the task would reach for a mutex that was never initialised. Pool A
     * is skipped because its session lives in the GlobalState fields of the
     * same names, not in this array.
     */
    for (pool_id_t id = POOL_B; id < POOL_MAX; id++) {
        queue_init(&GLOBAL_STATE.pool_session[id].queue);
        pthread_mutex_init(&GLOBAL_STATE.pool_session[id].transport_lock, NULL);
        pthread_mutex_init(&GLOBAL_STATE.pool_session[id].extranonce_lock, NULL);
    }
    for(int i = 0; i < MAX_CHAIN_NUM; i++){
        if(GLOBAL_STATE.chain_pluged[i])
            queue_init(&GLOBAL_STATE.ASIC_jobs_queue[i]);
    }
    init_jobs(&GLOBAL_STATE);

    heap_mark("before task fleet");

    /*
     * Stack sizes below are measured, not guessed.
     *
     * analyze_task_memory() prints each task's unused stack two minutes in.
     * Read on three boots and kept at its WORST, that gave:
     *
     *   stratum_manager  8192, unused 1144    left alone
     *   stratum_poolb    8192, unused 1240    left alone
     *   asic_result_0    6144, unused 1440    left alone
     *   http task        6144, unused 2428    left alone
     *   stratum_worker   6144, unused 3316 -> 4608
     *   asic_task_0      5120, unused 2964 -> 3584
     *   hashrate_task_0  4096, unused 2940 -> 2560
     *
     * Three were cut, each left with about 1.4 KB spare. The rest were not:
     * the two stratum tasks and asic_result_0 are already inside 1.5 KB, and
     * asic_result_0 is the one that failed to be created on a board that ran
     * out -- taking anything from it would be aiming at the wrong target.
     *
     * http task was left alone despite 2428 spare. It is the only one of these
     * that does TLS, its retry paths are not all exercised by a healthy boot,
     * and it was worth the least of the four.
     *
     * Unlike the main task's stack these are held for the life of the miner,
     * so what they give back is steady-state internal RAM rather than a
     * start-up peak. A figure from one boot would not be enough to size any of
     * them: a task that has not yet taken its deepest path reports slack it
     * does not have.
     *
     * Measured on BOTH boards, three boots each, and they agree closely --
     * every task within about a hundred bytes, and the main task identical at
     * 3,448. So one set of sizes covers both rather than a per-board branch:
     *
     *                  BC04 worst used   BC01 worst used   now   spare
     *   stratum_worker       2,828            2,904       4608   1,704
     *   asic_task_0          2,156            2,340       3584   1,244
     *   hashrate_task_0      1,156              752       2560   1,808
     *
     * These were briefly scoped to the BC04 alone, when only that board had
     * been read. That was the right default while the BC01 was unmeasured and
     * is unnecessary now that it is not -- the figures turned out to be the
     * same board to board.
     */
    /*create the stratum task.*/
    create_task_checked(&GLOBAL_STATE, stratum_task, "stratum_manager",
                        8192, (void *)&GLOBAL_STATE, 5, true);
    /* pool B idles cheaply when dual mining is off, so it is always started */
    /* Pool B is optional: create_jobs_task checks whether its transport is
     * up before selecting it, so pool A carries on alone. */
    create_task_checked(&GLOBAL_STATE, stratum_poolb_task, "stratum_poolb",
                        8192, (void *)&GLOBAL_STATE, 5, false);
    /*create the uart task.*/
    create_task_checked(&GLOBAL_STATE, create_jobs_task, "stratum_worker",
                        4608, (void *)&GLOBAL_STATE, 10, true);
    /*create the socket api task.*/
    #ifdef SOCKET_API_FEAUTRE
    create_task_checked(&GLOBAL_STATE, socket_api_task, "socket-api",
                        4096, (void*)&GLOBAL_STATE, 16, false);
    #endif
    /*create influx task*/
    //xTaskCreate(influx_task, "influx", 8192, (void*)&GLOBAL_STATE, 23, NULL);
    /*create ping pool task*/
    //xTaskCreate(ping_task, "ping task", 4096, (void*)&GLOBAL_STATE, 20, NULL);
    /*create http task*/
    create_task_checked(&GLOBAL_STATE, http_task, "http task",
                        6144, (void*)&GLOBAL_STATE, 3, false);
    /*create memory monitor task*/
    /*
     * Deliberately not started.
     *
     * Starting it cost the miner its hashrate. This task is 3 KB and at 25 s
     * into the boot there was not 3 KB of internal RAM to spare, so the next
     * xTaskCreate -- asic_result_0, the task that reads nonces back from the
     * chips -- failed with -1. The miner then detected four ASICs, built jobs
     * and sent them, and read nothing back: hashrate zero, shares zero, no
     * error beyond one warning line.
     *
     * The figures it would have printed are in /api/system/info instead, as
     * freeHeapInternal, minFreeHeapInternal and largestBlockInternal, which
     * cost nothing because they are read on request rather than by a task of
     * their own. Do not start this on a board until internal RAM has real
     * headroom.
     */
    //xTaskCreate(memory_monitor_task, "memory-monitor", 3072, (void*)&GLOBAL_STATE, 1, NULL);
    /*create system task*/
    create_task_checked(&GLOBAL_STATE, system_task, "system task",
                        4096, (void*)&GLOBAL_STATE, 4, true);

    /*
     * static, because its address is handed to tasks that outlive this
     * function. app_main() is allowed to return -- ESP-IDF deletes the main
     * task when it does -- and the moment it did, every ASIC task would be
     * holding a pointer into a stack frame that no longer exists.
     */
    static AsicParam chain_param[2] = {{.chain_num = 0, .p_global_state = &GLOBAL_STATE}, {.chain_num = 1, .p_global_state = &GLOBAL_STATE}};
    for(uint32_t i = 0; i < MAX_CHAIN_NUM; i++)
    {
        if(GLOBAL_STATE.chain_pluged[i]){
            ESP_LOGI(TAG, "create tasks for chain %"PRIu32"", i);
            /*create the asic work.*/
            char tmp_name[30];

            {
                /* Sends work to the chips. */
                sprintf(tmp_name, "asic_task_%"PRIu32"", i);
                create_task_checked(&GLOBAL_STATE, ASIC_task, tmp_name, 3584,
                                    (void *)&(chain_param[i]), 10, true);
            }

            {
                /* Reads nonces back. Without it the chain hashes and every
                 * share it finds is dropped on the floor. */
                sprintf(tmp_name, "asic_result_%"PRIu32"", i);
                create_task_checked(&GLOBAL_STATE, ASIC_result_task, tmp_name,
                                    6144, (void *)&(chain_param[i]), 15, true);
            }
            #if 1
            {
                /* The stall watchdog. Its absence costs the detection, not
                 * the mining, so it is reported rather than raised. */
                sprintf(tmp_name, "hashrate_task_%"PRIu32"", i);
                create_task_checked(&GLOBAL_STATE, hashrate_monitor_task,
                                    tmp_name, 2560,
                                    (void *)&(chain_param[i]), 5, false);
            }
            #endif
        }
    }

    /*
     * SYSTEM_HARDWARE_OK used to be unconditional, which was the other half
     * of the silent failure: SYSTEM_notify_error_info() sets the status to
     * SYSTEM_ERROR_STATUS, and this line ran afterwards and cleared it. The
     * fault was raised and then immediately overwritten with "healthy".
     */
    if (!s_critical_task_failed) {
        SYSTEM_notify_status_change(&GLOBAL_STATE, SYSTEM_HARDWARE_OK);
    } else {
        ESP_LOGE(TAG, "not reporting SYSTEM_HARDWARE_OK: a task this miner "
                      "needs in order to mine did not start. The fault "
                      "stands.");
    }
    show_mining_screen();
    
    /*
     * Free and largest-block, not just free.
     *
     * These two diverge badly on this board and only the second one decides
     * anything: a task stack has to come out of one contiguous block, so
     * "17875 free" says nothing about whether an 8 KB stack can be had. The
     * boot that refused asic_result_0 had 10863 free and a largest block of
     * 3840. This is the number that sizes the pools' stratum task.
     */
    ESP_LOGI(TAG, "heap free heap size: %" PRIu32 " bytes, largest block %" PRIu32,
             (uint32_t) heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (uint32_t) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Minimum free heap size: %" PRIu32 " bytes", esp_get_minimum_free_heap_size());

    /*
     * How much of the main task's own stack was ever used.
     *
     * All of it is internal RAM held for the whole of app_main -- including
     * the moment the ASIC tasks are created, which is exactly where this board
     * runs out. Whether that figure can come down is the question the log has
     * never answered, so it answers it here. The reading is taken last, after
     * the deepest call this function makes.
     *
     * The figure this comment used to quote, 16000, was wrong: sdkconfig has
     * said 8192 for as long as it has been checked. Printing
     * CONFIG_ESP_MAIN_TASK_STACK_SIZE rather than a number written in prose is
     * why that could be noticed at all.
     */
    /*
     * The denominator is the configured size PLUS TASK_EXTRA_STACK_SIZE.
     * app_startup.c creates "main" with ESP_TASK_MAIN_STACK, which
     * esp_task.h defines as CONFIG_ESP_MAIN_TASK_STACK_SIZE + 512 unless
     * CONFIG_NEWLIB_NANO_FORMAT is set, and it is not on this build. Printing
     * the config value alone understated the stack by 512 and so understated
     * use by the same -- it read 2,936 when the real figure is 3,448. The
     * differences were unaffected, so the trim this sized was not, but anyone
     * repeating the measurement from the old line would have been 512 out.
     */
    ESP_LOGI(TAG, "main task stack: %u of %d bytes still unused "
                  "(%d configured + %d padding)",
             (unsigned) (uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (int) ESP_TASK_MAIN_STACK,
             CONFIG_ESP_MAIN_TASK_STACK_SIZE,
             (int) (ESP_TASK_MAIN_STACK - CONFIG_ESP_MAIN_TASK_STACK_SIZE));

    /*
     * The last word app_main has on the internal pool. Everything after this
     * belongs to the tasks, so a floor lower than this one was reached while
     * the miner was running rather than while it was starting -- which the
     * survey says does happen, and which nothing could previously tell apart.
     */
    heap_mark("end of app_main");
    /*restart();*/
}
