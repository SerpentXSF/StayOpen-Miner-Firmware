#ifndef GLOBAL_STATE_H_
#define GLOBAL_STATE_H_

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include <freertos/semphr.h>

#include "common.h"
#include "asic_task.h"
#include "miner.h"
#include "stratum_api.h"
#include "work_queue.h"
#include "health_maintenance.h"
#include "statistic.h"
#include "hashrate_monitor_task.h"

#define STRATUM_USER CONFIG_STRATUM_USER
#define FALLBACK_STRATUM_USER CONFIG_FALLBACK_STRATUM_USER

#define HISTORY_LENGTH      100
#define DIFF_STRING_SIZE    10

/* BM1370 core ids are seven bits wide. */
#define CORE_STATS_CORES    128

/*TODO: replace the MAX_VALID_JOBS*/
#define MAX_VALID_JOBS      128

typedef enum
{
    DEVICE_UNKNOWN = -1,
    DEVICE_MODE1,
    DEVICE_MODE2,
    DEVICE_LOTTO,
    DEVICE_DC02,
    DEVICE_DC04,
    DEVICE_DC06,
    DEVICE_BC04,
    DEVICE_BC08,
    DEVICE_BC01,
    DEVICE_BC02,
    DEVICE_BC06,
    DEVICE_BC01_Pro,
} DeviceModel;

typedef enum
{
    ASIC_UNKNOWN = -1,
    ASIC_LT0051,
} AsicModel;

typedef struct {
    char message[64];
    uint32_t count;
} RejectedReasonStat;

typedef enum{
	SYSTEM_BOOT_UP = 0x0,
	SYSTEM_WIFI_CONNECTED,
	SYSTEM_HARDWARE_OK,
	SYSTEM_POOL_CONNECTED,
	SYSTEM_NORMAL_MINING,
	SYSTEM_ERROR_STATUS,
	SYSTEM_SATUS_MAX_NUM
}SYSTEM_SATUS;

typedef enum{
	SPECIAL_ERROR = 0x0,
	WRONG_ASIC_ERROR,
	FAN_ERROR,
	EEPROM_ERROR,
	RUN_MODE_ERROR,
	CONTROL_SYSTEM_ERROR, /*control borad error.*/
	CONFIG_FILE_ERROR,
	HASHRATE_ERROR,
	HIGH_TEMPERATURE_ERROR,
	WIFI_CONNETION_ERROR,
	NETWORK_ERROR,	/*could not connect to the pool.*/
	BOARD_MISMATCH_ERROR, /*image built for a different board's pinout.*/
	POWER_BOARD_ERROR, /*the voltage regulator did not answer on I2C.*/
	SYSTEM_ERROR_MAX_NUM
}SYSTEM_ERROR;

/*
 * One pool other than pool A. See SystemModule::pool below for why pool A is
 * not one of these yet.
 */
typedef struct {
    char *   url;
    uint16_t port;
    char *   user;
    char *   pass;
    uint16_t tls;
    char *   cert;
    /* uint16_t, not bool: the settings table in pool_fields.c has one
     * integer kind, and a bool member would be written through a
     * uint16_t pointer. Still read as a truth value everywhere. */
    uint16_t extranonce_subscribe;

    /* its own failover, stepped independently of every other pool's */
    char *   fb_url;
    uint16_t fb_port;
    char *   fb_user;
    char *   fb_pass;
    uint16_t fb_tls;
    bool     is_using_failover;

    /* runtime */
    bool     connected;
    uint64_t shares_accepted;
    uint64_t shares_rejected;

    /*
     * Share of slices wanted by this pool, relative to the others. The
     * scheduler normalises, so these need not add up to anything.
     */
    uint16_t weight;
} PoolConfig;

/*
 * One pool's live Stratum session, for the pools that are not pool A.
 *
 * Indexed by pool_id_t alongside SystemModule::pool, and [POOL_A] is unused
 * for the same reason: pool A's session state sits in the GlobalState fields
 * of the same names, because its own task owns it.
 *
 * Each pool owns its receive accumulator rather than sharing one, which is
 * what lets several sessions run at once -- see
 * STRATUM_V1_receive_jsonrpc_line_ctx, which exists for exactly that.
 */
typedef struct {
    work_queue             queue;
    char *                 extranonce_str;
    int                    extranonce_2_len;
    uint32_t               stratum_difficulty;
    uint32_t               version_mask;
    int                    send_uid;

    /*
     * Published only once the connection is up, and swapped under its lock:
     * asic_result_task can be submitting a share from another task at any
     * moment and must never read a handle that is being destroyed.
     */
    esp_transport_handle_t transport;
    pthread_mutex_t        transport_lock;
    pthread_mutex_t        extranonce_lock;

    /*
     * Set by the share-submit path when a write to this pool fails, read by
     * the pool task as "tear this session down".
     *
     * A write failing is the earliest sign a peer has gone. The receive side
     * cannot tell: a socket whose peer vanished without a FIN -- a NAT drop,
     * a WiFi roam, a pool host dying -- simply goes quiet, which is
     * indistinguishable from a quiet pool until lwIP exhausts its
     * retransmits minutes later. Until then the pool keeps its full share of
     * the slices and every share built for it is dropped.
     *
     * A flag rather than a teardown in place: asic_result_task does not own
     * these sessions, and closing one from under the task that is reading it
     * is how the transport_lock dance started.
     */
    volatile bool          write_failed;

    /*
     * Set by the API to end this session on purpose, read by the pool task
     * in the same place as write_failed and torn down the same way.
     *
     * Kept separate from write_failed rather than reusing it so the log says
     * which happened. A requested drop recorded as "a share could not be
     * written" would read as a network fault in the one record anyone has to
     * work from afterwards, and this flag exists to make faults legible.
     *
     * There is no equivalent for pool A. Its transport has no lock, and its
     * receive timeout is ten minutes, so neither closing the socket from
     * another task nor polling a flag in its loop would be safe or prompt.
     * Giving pool A one means locking its transport first.
     */
    volatile bool          drop_requested;
} PoolSession;

typedef struct
{
    double duration_start;
    int historical_hashrate_rolling_index;
    double historical_hashrate_time_stamps[HISTORY_LENGTH];
    double historical_hashrate[HISTORY_LENGTH];
    int historical_hashrate_init;
    double current_hashrate;
    int64_t start_time;
    uint64_t shares_accepted;
    uint64_t shares_rejected;
    uint64_t work_received;
#ifdef HW_STATISTIC_FEATURE
    uint64_t recveived_nonce;
    uint64_t recveived_hw;
    /*
     * Per-core accounting. The BM1370 reports the core that produced each
     * result in seven bits, so 128 slots covers every id it can emit --
     * including any the datasheet does not claim exist, which is the case
     * worth catching rather than dropping.
     *
     * A core that has gone bad returns nonces that fail the difficulty-1
     * check. Counting good and bad per core turns "the chip has hardware
     * errors" into "core 63 produces them and nothing else does".
     */
    uint32_t core_nonces[CORE_STATS_CORES];
    uint32_t core_errors[CORE_STATS_CORES];
#endif
    RejectedReasonStat rejected_reason_stats[10];
    int rejected_reason_stats_count;
    /* set when the NVS board model cannot work with this image's pins */
    bool board_mismatch;
    int screen_page;
    uint64_t best_nonce_diff;
    char best_diff_string[DIFF_STRING_SIZE];
    uint64_t best_session_nonce_diff;
    char best_session_diff_string[DIFF_STRING_SIZE];
    bool FOUND_BLOCK;
    uint16_t BLOCK_NUM;
	
    char ssid[32];
    char wifi_status[20];
    char ip_addr_str[16]; // IP4ADDR_STRLEN_MAX
    char ap_ssid[32];
    bool ap_enabled;
	
    char * pool_url;
    char * fallback_pool_url;
    uint16_t pool_port;
    uint16_t fallback_pool_port;
    char * pool_user;
    char * fallback_pool_user;
    char * pool_pass;
    char * fallback_pool_pass;
    uint16_t pool_difficulty;
    uint16_t fallback_pool_difficulty;
    bool pool_extranonce_subscribe;
    bool fallback_pool_extranonce_subscribe;
    uint16_t pool_tls;
    uint16_t fallback_pool_tls;
    char * pool_cert;
    char * fallback_pool_cert;
    bool is_using_fallback;

    /* ---- dual mining, and beyond ----
     * Pools that are not pool A: permanently connected, not failovers for it.
     * The ASIC's single hashrate is time-sliced between whichever of them are
     * configured -- see components/dual_pool/pool_scheduler.c.
     *
     * Indexed by pool_id_t, so pool[POOL_B], pool[POOL_C] and pool[POOL_D]
     * line up with the scheduler's ids and with job_pool[]: the slot a job
     * records when it is built is the slot that scores it when it comes back.
     *
     * pool[POOL_A] is reserved and unused. Pool A's configuration still lives
     * in the pool_url / pool_port / pool_user fields above, because it is
     * served by its own task rather than by the secondary one, and moving it
     * would touch the display, the API, NVS and the primary stratum path
     * without changing any behaviour. Keeping its index here means nothing
     * needs renumbering when it does move.
     *
     * The literal 4 is POOL_MAX from pool_scheduler.h, which this header
     * cannot include: components/asic and components/bc_hal include
     * global_state.h and do not see dual_pool's include directory. A
     * _Static_assert in create_jobs_task.c, which includes both, keeps the
     * two from drifting apart.
     */
    PoolConfig pool[4];

    uint16_t overheat_mode;
    uint16_t power_fault;
    uint32_t lastClockSync;
    bool is_screen_active;
    bool is_firmware_update;
    char firmware_update_filename[20];
    char firmware_update_status[20];
    char * asic_status;
    bool is_sleep_mode;
    uint16_t boot_mode;
    char * system_error;
    /* Which error system_error describes, so a recovery can tell what it
     * actually proves. SYSTEM_ERROR_MAX_NUM means "none". */
    SYSTEM_ERROR system_error_code;
    SYSTEM_SATUS system_status;
    bool is_network_error;      // NETWORK_ERROR 时置 true，跳过 HASHRATE_ERROR 误报
    char *sn[MAX_CHAIN_NUM];
} SystemModule;

typedef struct
{
    bool active;
    char *message;
    bool result;
    bool finished;
} SelfTestModule;

typedef enum{
    SHARE_ACCEPTED,
    SHARE_REJECTED,
    SHARE_DISCARDED,
    SHARE_STALE
}SHARE_STATUS;

typedef struct
{
    DeviceModel device_model;
    char * device_model_str;
    int board_version;
    AsicModel asic_model;
    char * asic_model_str;
    uint16_t asic_count[MAX_CHAIN_NUM];
    uint16_t voltage_domain;
    double asic_job_frequency_ms;
    uint32_t asic_difficulty;
    uint32_t asic_freqency;
#ifdef CONFIG_BC04_INDIVIDUAL_FREQ
    uint16_t bc04_chip_freqs[4];
#endif
    uint16_t asic_vol_max;
    uint16_t asic_vol_min;
	uint16_t asic_vol_default;

    work_queue stratum_queue;
    work_queue ASIC_jobs_queue[MAX_CHAIN_NUM];

    SystemModule SYSTEM_MODULE;
    AsicTaskModule ASIC_TASK_MODULE[MAX_CHAIN_NUM];
    HealthMaintenceModule HEALTH_MODULE;
    SelfTestModule SELF_TEST_MODULE;
#ifdef STATISTIC_SYSTEM_FEATURE    
    StatisticModule STATISTIC_MODULE;
#endif
    HashrateMonitorModule HASHRATE_MONITOR_MODULE;

    SemaphoreHandle_t global_parameter_mutex;
    char *extranonce_str;
    int extranonce_2_len;
    int abandon_work;

    uint8_t * valid_jobs[MAX_CHAIN_NUM];
    /*
     * Which pool owns each ASIC job slot, so a clean_jobs from one pool can
     * spare the other's in-flight work. Recorded here rather than read back
     * from active_jobs[] because that array is written outside
     * valid_jobs_lock -- following those pointers from another task would be
     * a use-after-free waiting to happen. Written under the same lock as
     * valid_jobs, and read under it too.
     */
    uint8_t job_pool[MAX_CHAIN_NUM][128];
    pthread_mutex_t valid_jobs_lock[MAX_CHAIN_NUM];

    uint32_t stratum_difficulty;
    bool new_set_mining_difficulty_msg;
    uint32_t version_mask;
    bool new_stratum_version_rolling_msg;

    esp_transport_handle_t transport;

    /* ---- how the hashrate is divided ----
     * dual_ratio_a is the two-pool form, kept because that is what is stored
     * in NVS and shown in the UI today; it is handed to the scheduler as
     * {ratio_a, 100 - ratio_a}. With more than two pools in play the per-pool
     * weights in SystemModule::pool are what count instead. */
    bool dual_enable;
    uint8_t dual_ratio_a;        /* percent of slices given to pool A */
    uint16_t dual_interval_ms;   /* slice length */

    /*
     * Jobs the scheduler assigned to each pool, and jobs actually built for
     * it. Indexed by pool id.
     *
     * The gap between them is what a pool lost to another, because it had
     * nothing queued to build from. That is what these are for, and it is a
     * number worth having: without it a shortfall could only be inferred from
     * share counts against several moving vardiffs.
     *
     * What they are NOT is the hashrate split, and an earlier comment here
     * claimed they were. They count job-building events, not time, so a pool
     * whose work is invalidated often rebuilds often and is over-reported.
     * Measured on a BC04 mining four pools at an even 25% each: pool A, a
     * local solo node issuing clean_jobs far more often than the three
     * remote pools, built 8.06 jobs per slice against 6.49, 6.44 and 6.91 --
     * 22% more, reading as a 29/23/23/25 split when every pool in fact owned
     * exactly a quarter of the slices.
     *
     * The hashrate split is decided by pool_scheduler_select(), which divides
     * wall-clock slices. To measure what each pool actually received, use
     * accepted shares times that pool's difficulty.
     */
    uint32_t jobs_selected[4];
    uint32_t jobs_served[4];

    PoolSession pool_session[4];

    // A message ID that must be unique per request that expects a response.
    // For requests not expecting a response (called notifications), this is null.
    int send_uid;

    bool ASIC_initalized;
    bool interface_initalized;
    bool chain_pluged[MAX_CHAIN_NUM];
    float real_freq[MAX_CHAIN_NUM];
    uint32_t baud_rate[MAX_CHAIN_NUM];
    uint32_t tmmusk;

    bool screen_flash;

    int block_height;
    char * scriptsig;
    char network_diff_string[DIFF_STRING_SIZE];
    char pd_state;
} GlobalState;

typedef struct{
    uint32_t chain_num; /*uart num*/
    GlobalState *p_global_state;
}AsicParam;

#endif /* GLOBAL_STATE_H_ */
