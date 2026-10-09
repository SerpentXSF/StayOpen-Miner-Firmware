#ifndef MAIN_NVS_CONFIG_H
#define MAIN_NVS_CONFIG_H

#include <stdint.h>

// Max length 15

#define NVS_CONFIG_WIFI_SSID "wifissid"
#define NVS_CONFIG_WIFI_PASS "wifipass"
#define NVS_CONFIG_HOSTNAME "hostname"

/* Shared secret for the HTTP API. Empty means authentication is disabled,
 * which is how the vendor firmware behaved. See main/http_server/api_auth.c. */
#define NVS_CONFIG_API_PASSWORD "apipassword"
#define NVS_CONFIG_STRATUM_URL "stratumurl"
#define NVS_CONFIG_STRATUM_PORT "stratumport"
#define NVS_CONFIG_FALLBACK_STRATUM_URL "fbstratumurl"
#define NVS_CONFIG_FALLBACK_STRATUM_PORT "fbstratumport"
#define NVS_CONFIG_STRATUM_USER "stratumuser"
#define NVS_CONFIG_STRATUM_PASS "stratumpass"

/* ---- the pools that are not pool A ----
 * No macros: their keys are built at runtime as "pool" + the pool's letter +
 * a suffix, from the one table in pool_fields.c -- "poolburl", "poolcfbport"
 * and so on. Thirteen settings across three pools is thirty-nine names, and
 * a list of thirty-nine #defines is a list that drifts from the code that
 * reads them.
 *
 * The spellings are unchanged: POOL_B is 1 and 'a' + 1 is 'b', so the keys
 * that table generates for pool B are byte for byte the ones this header
 * used to declare, and settings written by earlier firmware load as they
 * always did. NVS still caps a key at 15 characters; pool_key() says so
 * loudly if a suffix ever grows past it.
 *
 * Each pool has its own failover endpoint. Pool A already had one; before
 * that, pool B simply retried a dead host forever and donated its slices to
 * pool A.
 */
#define NVS_CONFIG_DUAL_ENABLE "dualenable"
#define NVS_CONFIG_DUAL_RATIO  "dualratioa"
#define NVS_CONFIG_DUAL_SLICE  "dualslicems"
#define NVS_CONFIG_STRATUM_EXTRANONCE_SUBSCRIBE "stratumxnsub"
#define NVS_CONFIG_STRATUM_TLS "stratumtls"
#define NVS_CONFIG_STRATUM_CERT "stratumcert"
#define NVS_CONFIG_STRATUM_DIFFICULTY "stratumdiff"
#define NVS_CONFIG_FALLBACK_STRATUM_USER "fbstratumuser"
#define NVS_CONFIG_FALLBACK_STRATUM_PASS "fbstratumpass"
#define NVS_CONFIG_FALLBACK_STRATUM_EXTRANONCE_SUBSCRIBE "stratumfbxnsub"
#define NVS_CONFIG_FALLBACK_STRATUM_TLS "stratumfbtls"
#define NVS_CONFIG_FALLBACK_STRATUM_CERT "stratumfbcert"
#define NVS_CONFIG_FALLBACK_STRATUM_DIFFICULTY "fbstratumdiff"
#define NVS_CONFIG_ASIC_VOLTAGE_DEF "asicovervdef"
#define NVS_CONFIG_ASIC_FREQ "asicfrequency"
#define NVS_CONFIG_ASIC_VOLTAGE "asicvoltage"
#define NVS_CONFIG_ASIC_NORMAL_FREQ "asicnormalf"
#define NVS_CONFIG_ASIC_NORMAL_VOLTAGE "asicnormalvol"
#define NVS_CONFIG_ASIC_OVER_FREQ "asicoverf"
#define NVS_CONFIG_ASIC_OVER_VOLTAGE "asicovervol"
#define NVS_CONFIG_ASIC_MODEL "asicmodel"
#define NVS_CONFIG_DEVICE_MODEL "devicemodel"
#define NVS_CONFIG_BOARD_VERSION "boardversion"
#define NVS_CONFIG_FLIP_SCREEN "flipscreen"
#define NVS_CONFIG_BACKLIGHT "backlight"
#define NVS_CONFIG_INVERT_SCREEN "invertscreen"
#define NVS_CONFIG_INVERT_FAN_POLARITY "invertfanpol"
#define NVS_CONFIG_AUTO_FAN_SPEED "autofanspeed"
#define NVS_CONFIG_FAN_SPEED "fanspeed"
#define NVS_CONFIG_BEST_DIFF "bestdiff"
#define NVS_CONFIG_SELF_TEST "selftest"
#define NVS_CONFIG_OVERHEAT_MODE "overheat_mode"
#define NVS_CONFIG_OVERCLOCK_ENABLED "oc_enabled"
#define NVS_CONFIG_SWARM "swarmconfig"

#define NVS_CONFIG_WIFI_ON "wifi_on"
#define NVS_CONFIG_ETH_ON "eth_on"


#define NVS_CONFIG_IS_STATIC_IP "isStaticIP"
#define NVS_CONFIG_STATIC_IP    "staticIP"
#define NVS_CONFIG_SUBNET_MASK "subnetMask"
#define NVS_CONFIG_GATEWAY      "gateway"
#define NVS_CONFIG_DNS          "dns"

#define NVS_CONFIG_ETH_IS_STATIC_IP "ETHisStaticIP"
#define NVS_CONFIG_ETH_STATIC_IP    "ETHstaticIP"
#define NVS_CONFIG_ETH_SUBNET_MASK "ETHsubnetMask"
#define NVS_CONFIG_ETH_GATEWAY      "ETHgateway"
#define NVS_CONFIG_ETH_DNS          "ETHdns"


// Theme configuration
#define NVS_CONFIG_THEME_SCHEME "themescheme"
#define NVS_CONFIG_THEME_NAME "themename"
#define NVS_CONFIG_THEME_COLORS "themecolors"

// ntp server
#define NVS_CONFIG_NTP_SERVER           "ntp_main"
#define NVS_CONFIG_NTP_SERVER_BACKUP    "ntp_backup"
#define NVS_CONFIG_TIME_ZONE            "time_zone"

#define NVS_CONFIG_BOOT_MODE            "boot_mode"
#define NVS_CONFIG_USERNAME             "username"
#define NVS_CONFIG_PASSWORD             "password"

#define NVS_CONFIG_SN                   "sn_str"
#define NVS_CONFIG_BLOCK_NUM            "BLOCK_NUM"

//influx configuration
#define NVS_CONFIG_INFLUX_ENABLE "influx_enable"
#define NVS_CONFIG_INFLUX_URL "influx_url"
#define NVS_CONFIG_INFLUX_TOKEN "influx_token"
#define NVS_CONFIG_INFLUX_PORT "influx_port"
#define NVS_CONFIG_INFLUX_BUCKET "influx_bucket"
#define NVS_CONFIG_INFLUX_ORG "influx_org"
#define NVS_CONFIG_INFLUX_PREFIX "influx_prefix"

#define NVS_CONFIG_RESTART_REASON "res_reason"

char * nvs_config_get_string(const char * key, const char * default_value);
void nvs_config_set_string(const char * key, const char * default_value);
uint16_t nvs_config_get_u16(const char * key, const uint16_t default_value);
void nvs_config_set_u16(const char * key, const uint16_t value);
uint64_t nvs_config_get_u64(const char * key, const uint64_t default_value);
void nvs_config_set_u64(const char * key, const uint64_t value);

#endif // MAIN_NVS_CONFIG_H
