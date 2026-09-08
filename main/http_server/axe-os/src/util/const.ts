import { default as langEn } from '@/i18n/en.ts'
import { default as langZh } from '@/i18n/zh.ts'

/**
 * 当前 Web 版本号
 * 详细变更记录请查阅: src/CHANGELOG.md
 */
const WEB_VERSION = '1.6.1 20260908';

// [新增] 机型配置接口
export interface ModelConfig {
    // 硬件基础信息
    hashboard_count: number;
    fan_count: number;
    flip_screen: boolean;
    backlight: boolean;
    invert_fan_duty_cycle: boolean;
    has_ethernet: boolean; // [新增] 是否支持有线网络

    // 仪表盘 & 图表显示参数
    max_power_w: number;          // 功率表盘最大值
    chart_power_max: number;      // 曲线图功率轴最大值
    max_hashrate_ghs: number;     // 曲线图算力轴最大值
    chart_hash_incr_ghs: number;  // [新增] 曲线图算力轴刻度步进值

    // [新增] 是否支持自动调优与登录签权与芯片监控与OTA
    support_autotune?: boolean;
    support_login?: boolean;
    support_chip_monitoring?: boolean;
    support_ota_download?: boolean;
    support_tls?: boolean;
    support_xnsub?: boolean;

    // 仪表盘参考线 (REF) 与量程
    max_freq_mhz: number;         // 频率表盘最大刻度
    ref_freq_mhz: number;         // 频率 REF 标记位置

    max_voltage_v: number;        // 电压表盘最大刻度
    ref_voltage_v: number;        // 电压 REF 标记位置

    // 告警阈值
    temp_warn_c: number;          // 温度告警阈值 (变红)

    // 设置页面参考表 (包含预估算力与功耗可选字段)
    ref_config_table: Array<{ freq: number; volt: number, hr_th?: number, power_w?: number }>;

    // [新增] 是否在该机型显示参考配置面板
    showRefConfig?: boolean;

    // 自定义模式默认电压 (毫伏)
    default_volts_mv: number;
}


const tempEn = langEn.dashboard.chart.temp_title;
const tempZh = langZh.dashboard.chart.temp_title;
const hashEn = langEn.dashboard.chart.hash_title;
const hashZh = langZh.dashboard.chart.hash_title;
const CHART_VALUE = {
    [tempEn]: tempZh,
    [tempZh]: tempEn,
    [hashEn]: hashZh,
    [hashZh]: hashEn,
}

const RES_CODE = {
    SUCCESS: "200",
    PWD_ERROR: "501",
};

const WIFI_STATUS = {
    Connecting: 'Connecting...',
    Connected: 'Connected!',
    NoApFound: 'No AP found',
    PasswordError: 'Password error',
    Error: 'Error'
}

const ENABLE_STATE = {
    disable: 0,
    enable: 1,
};

const RET_STATUS = {
    OK: 'OK',
}

const WORK_MODE = {
    normal: 0, // NORMAL_MODE
    over_freq: 1, // OVER_FREQ_MODE
    customize: 2, // USER_CUSTOMIZED_MODE
    debug: 3, // DEMO_DEBUG_MODE
    lower_power: 4, // LOWER_POWER_MODE
    super_low_power: 5, // SUPER_LOW_POWER_MODE
    sleep: 6, // SLEEP_MODE
    power_debug: 256 // POWER_DEBUG_MODE (0x100)
};

const MULTIPART_HEADER = {
    "Content-Type": "multipart/form-data",
};

const FORM_URLENCODED_HEADER = {
    "Content-Type": "application/x-www-form-urlencoded",
};

const DEVICE_MODEL = {
    DC02: 'DC02',
    DC04: 'DC04',
    DC06: 'DC06',
    DC08: 'DC08',
    BC01: 'BC01',
    'BC01-Pro': 'BC01-Pro',
    BC02: 'BC02',
    BC04: 'BC04',
    BC08: 'BC08'
}

// [修改] 扩展机型详细配置
const DEVICE_MODELS_INFO: Record<string, ModelConfig> = {
    [DEVICE_MODEL.DC08]: {
        hashboard_count: 1,
        fan_count: 1,
        flip_screen: false,
        backlight: true,
        invert_fan_duty_cycle: true,
        has_ethernet: true, // [新增] DC08 支持有线
        support_autotune: false,
        support_login: false,
        support_chip_monitoring: false,
        support_ota_download: false, // 默认关闭，目前只开通BC04
        support_tls: false,
        support_xnsub: false,

        max_power_w: 200,
        chart_power_max: 200,
        max_hashrate_ghs: 600,
        chart_hash_incr_ghs: 60, // 每 200G 一个刻度

        max_freq_mhz: 2600,
        ref_freq_mhz: 2300,

        max_voltage_v: 2.80,
        ref_voltage_v: 2.60,

        temp_warn_c: 70,
        default_volts_mv: 2600,

        ref_config_table: [
            { freq: 800, volt: 216 },
            { freq: 1200, volt: 226 },
            { freq: 1600, volt: 234 },
            { freq: 1800, volt: 240 },
            { freq: 2000, volt: 250 },
            { freq: 2200, volt: 258 },
            { freq: 2300, volt: 264 },
            { freq: 2400, volt: 270 },
        ]
    },
    [DEVICE_MODEL.DC06]: {
        hashboard_count: 1,
        fan_count: 1,
        flip_screen: false,
        backlight: true,
        invert_fan_duty_cycle: true,
        has_ethernet: true, // [新增] DC08 支持有线
        support_autotune: false,
        support_login: false,
        support_chip_monitoring: false,
        support_ota_download: false, // 默认关闭，目前只开通BC04
        support_tls: false,
        support_xnsub: false,

        max_power_w: 100,
        chart_power_max: 100,
        max_hashrate_ghs: 450,
        chart_hash_incr_ghs: 45, // 每 200G 一个刻度

        max_freq_mhz: 2400,
        ref_freq_mhz: 2300,

        max_voltage_v: 4.05,
        ref_voltage_v: 3.90,

        temp_warn_c: 70,
        default_volts_mv: 3900,

        ref_config_table: [
            { freq: 700, volt: 315 },
            { freq: 1000, volt: 330 },
            { freq: 1200, volt: 339 },
            { freq: 1600, volt: 351 },
            { freq: 2000, volt: 375 },
            { freq: 2200, volt: 384 },
            { freq: 2300, volt: 390 },
            { freq: 2400, volt: 405 },
        ]
    },
    [DEVICE_MODEL.DC04]: {
        hashboard_count: 1,
        fan_count: 1,
        flip_screen: false,
        backlight: true,
        invert_fan_duty_cycle: true,
        has_ethernet: true, // [新增] DC08 支持有线
        support_autotune: false,
        support_login: false,
        support_chip_monitoring: false,
        support_ota_download: false, // 默认关闭，目前只开通BC04
        support_tls: false,
        support_xnsub: false,

        max_power_w: 100,
        chart_power_max: 100,
        max_hashrate_ghs: 300,
        chart_hash_incr_ghs: 30, // 每 200G 一个刻度

        max_freq_mhz: 2600,
        ref_freq_mhz: 2300,

        max_voltage_v: 2.80,
        ref_voltage_v: 2.60,

        temp_warn_c: 70,
        default_volts_mv: 2600,

        ref_config_table: [
            { freq: 800, volt: 216 },
            { freq: 1200, volt: 226 },
            { freq: 1600, volt: 234 },
            { freq: 1800, volt: 240 },
            { freq: 2000, volt: 250 },
            { freq: 2200, volt: 258 },
            { freq: 2400, volt: 270 },
            { freq: 2600, volt: 280 },
        ]
    },
    [DEVICE_MODEL.DC02]: {
        hashboard_count: 1,
        fan_count: 1,
        flip_screen: false,
        backlight: true,
        invert_fan_duty_cycle: false,
        has_ethernet: false, // [新增] DC02 仅 WiFi
        support_autotune: false,
        support_login: false,
        support_chip_monitoring: false,
        support_ota_download: false, // 默认关闭，目前只开通BC04
        support_tls: false,
        support_xnsub: false,

        max_power_w: 50,
        chart_power_max: 50,
        max_hashrate_ghs: 150,
        chart_hash_incr_ghs: 15, // 每 200G 一个刻度

        max_freq_mhz: 2600,
        ref_freq_mhz: 2300,

        max_voltage_v: 1.4,
        ref_voltage_v: 1.32,

        temp_warn_c: 70,
        default_volts_mv: 1270,

        ref_config_table: [
            { freq: 800, volt: 108 },
            { freq: 1200, volt: 113 },
            { freq: 1600, volt: 117 },
            { freq: 1800, volt: 120 },
            { freq: 2000, volt: 125 },
            { freq: 2200, volt: 129 },
            { freq: 2400, volt: 135 },
            { freq: 2600, volt: 140 },
        ]
    },
    [DEVICE_MODEL.BC04]: {
        hashboard_count: 1,
        fan_count: 1,
        flip_screen: true,
        backlight: true,
        invert_fan_duty_cycle: true,
        has_ethernet: true,
        support_autotune: false,
        support_login: false,
        support_chip_monitoring: false,
        support_ota_download: true, // 机型独立开关
        support_tls: true,
        support_xnsub: true,

        max_power_w: 120,
        chart_power_max: 120,
        max_hashrate_ghs: 7000000,
        chart_hash_incr_ghs: 700000, // 每 200G 一个刻度

        max_freq_mhz: 820,
        ref_freq_mhz: 750,

        max_voltage_v: 5.20,
        ref_voltage_v: 4.90,

        temp_warn_c: 70,
        default_volts_mv: 4900,
        showRefConfig: false, // BC04 默认不显示参考配置面板

        ref_config_table: [
            { freq: 480, volt: 450, hr_th: 3.6, power_w: 48 },
            { freq: 620, volt: 480, hr_th: 4.8, power_w: 72 },
            { freq: 750, volt: 490, hr_th: 6.0, power_w: 90 },
            { freq: 800, volt: 500, hr_th: 6.5, power_w: 100 },
        ]
    },
    [DEVICE_MODEL.BC08]: {
        hashboard_count: 1,
        fan_count: 1,
        flip_screen: false,
        backlight: true,
        invert_fan_duty_cycle: true,
        has_ethernet: true,
        support_autotune: false,
        support_login: false,
        support_chip_monitoring: true,
        support_ota_download: false,
        support_tls: false,
        support_xnsub: false,

        max_power_w: 240,
        chart_power_max: 240,
        max_hashrate_ghs: 14000000,
        chart_hash_incr_ghs: 1400000,

        max_freq_mhz: 1000,
        ref_freq_mhz: 750,

        max_voltage_v: 6.00,
        ref_voltage_v: 4.90,

        temp_warn_c: 70,
        default_volts_mv: 4900,
        showRefConfig: false,

        ref_config_table: [
            { freq: 480, volt: 450, hr_th: 7.2, power_w: 96 },
            { freq: 620, volt: 480, hr_th: 9.6, power_w: 144 },
            { freq: 750, volt: 490, hr_th: 12.0, power_w: 180 },
            { freq: 800, volt: 500, hr_th: 13.0, power_w: 200 },
        ]
    },
    [DEVICE_MODEL.BC02]: {
        hashboard_count: 1,
        fan_count: 1,
        flip_screen: false,
        backlight: true,
        invert_fan_duty_cycle: true,
        has_ethernet: true,
        support_autotune: false,
        support_login: false,
        support_chip_monitoring: true,
        support_ota_download: false,
        support_tls: false,
        support_xnsub: false,

        max_power_w: 60,
        chart_power_max: 60,
        max_hashrate_ghs: 3500000,
        chart_hash_incr_ghs: 350000,

        max_freq_mhz: 820,
        ref_freq_mhz: 750,

        max_voltage_v: 1.30,
        ref_voltage_v: 1.23,

        temp_warn_c: 70,
        default_volts_mv: 1225,
        showRefConfig: false,

        ref_config_table: [
            { freq: 480, volt: 113, hr_th: 1.8, power_w: 24 },
            { freq: 620, volt: 120, hr_th: 2.4, power_w: 36 },
            { freq: 750, volt: 123, hr_th: 3.0, power_w: 45 },
            { freq: 800, volt: 125, hr_th: 3.25, power_w: 50 },
        ]
    },
    [DEVICE_MODEL.BC01]: {
        hashboard_count: 1,
        fan_count: 1,
        flip_screen: false,
        backlight: true,
        invert_fan_duty_cycle: true,
        has_ethernet: false,
        support_autotune: false,
        support_login: false,
        support_chip_monitoring: false,
        support_ota_download: true,  // release assets are published for this board
        support_tls: false,
        support_xnsub: false,

        max_power_w: 45,
        chart_power_max: 45,
        max_hashrate_ghs: 1750000,
        chart_hash_incr_ghs: 175000,

        max_freq_mhz: 820,
        ref_freq_mhz: 750,

        max_voltage_v: 1.30,
        ref_voltage_v: 1.2,

        temp_warn_c: 70,
        default_volts_mv: 1200,
        showRefConfig: false,

        ref_config_table: [
            { freq: 480, volt: 113, hr_th: 0.9, power_w: 12 },
            { freq: 620, volt: 120, hr_th: 1.2, power_w: 18 },
            { freq: 750, volt: 123, hr_th: 1.5, power_w: 22.5 },
            { freq: 800, volt: 125, hr_th: 1.6, power_w: 25 },
        ]
    },
    [DEVICE_MODEL['BC01-Pro']]: {
        hashboard_count: 1,
        fan_count: 1,
        flip_screen: false,
        backlight: false,
        invert_fan_duty_cycle: true,
        has_ethernet: false,
        support_autotune: false,
        support_login: false,
        support_chip_monitoring: false,
        support_ota_download: false,
        support_tls: false,
        support_xnsub: false,

        max_power_w: 45,
        chart_power_max: 45,
        max_hashrate_ghs: 4500000,
        chart_hash_incr_ghs: 450000,

        max_freq_mhz: 500,
        ref_freq_mhz: 380,

        max_voltage_v: 1.15,
        ref_voltage_v: 1.00,

        temp_warn_c: 70,
        default_volts_mv: 1000,
        showRefConfig: false,

        ref_config_table: [
            { freq: 300, volt: 900,  hr_th: 2.0, power_w: 22 },
            { freq: 350, volt: 950,  hr_th: 2.8, power_w: 30 },
            { freq: 400, volt: 1000, hr_th: 3.5, power_w: 38 },
            { freq: 450, volt: 1050, hr_th: 4.0, power_w: 45 },
        ]
    },
    // 默认兜底配置
    'default': {
        hashboard_count: 1,
        fan_count: 1,
        flip_screen: false,
        backlight: true,
        invert_fan_duty_cycle: false,
        has_ethernet: false, // [新增] DC02 仅 WiFi
        support_autotune: false,
        support_login: false,
        support_chip_monitoring: false,
        support_ota_download: false, // 默认关闭，目前只开通BC04
        support_tls: false,
        support_xnsub: false,

        max_power_w: 50,
        chart_power_max: 50,
        max_hashrate_ghs: 150,
        chart_hash_incr_ghs: 15, // 每 200G 一个刻度

        max_freq_mhz: 2600,
        ref_freq_mhz: 2300,

        max_voltage_v: 1.4,
        ref_voltage_v: 1.32,

        temp_warn_c: 70,
        default_volts_mv: 1270,

        ref_config_table: [
            { freq: 800, volt: 108 },
            { freq: 1200, volt: 113 },
            { freq: 1600, volt: 117 },
            { freq: 1800, volt: 120 },
            { freq: 2000, volt: 125 },
            { freq: 2200, volt: 129 },
            { freq: 2400, volt: 135 },
        ]
    }
}

const TIMER_WORKER = '/js/timer.worker.js';

export {
    RES_CODE,
    MULTIPART_HEADER,
    FORM_URLENCODED_HEADER,
    ENABLE_STATE,
    WORK_MODE,
    DEVICE_MODELS_INFO,
    RET_STATUS,
    DEVICE_MODEL,
    WEB_VERSION,
    WIFI_STATUS,
    CHART_VALUE,
    TIMER_WORKER
};

/* GitHub repo consulted for firmware/web release checks.
 * Was the vendor's HammerMiner/BC04-* repositories, which this build
 * does not track. Change here if the repository is renamed. */
export const UPDATE_REPO_WWW = "SerpentXSF/StayOpen-Miner-Firmware";
export const UPDATE_REPO_APP = "SerpentXSF/StayOpen-Miner-Firmware";
