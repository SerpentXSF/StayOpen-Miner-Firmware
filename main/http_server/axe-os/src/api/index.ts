import { get, patch, post } from "@/util/http/axios";
import type { MinerStatusData, WifiNetwork, } from "./type.ts";
import { eASICModel, ResData, LoginResponse } from "./type.ts";
import { FORM_URLENCODED_HEADER, DEVICE_MODELS_INFO } from "@/util/const.ts";

enum URL {
    getMinerStatus = "/api/system/info",
    restartMiner = "/api/system/restart",
    updateSystem = "/api/system",
    setAutotuneStatus = "/api/system/autotune",
    performWWWOTAUpdate = "/api/system/OTAWWW",
    performOTAUpdate = "/api/system/OTA",
    scanWifi = "/api/system/wifi/scan",
    getKernelLog = "/api/get_log",
    getKernelLogFileList = "/api/get_log_file_list",
    syncTime = "/api/sync_time",
    ledOnOrOff = "/api/location",
    getNetworkInfo = "/api/get_network",
    setNetworkConf = "/api/set_network",
    getTheme = "/api/theme",
    setTheme = "/api/theme",
    downloadLog = "/api/system/log/download",
    login = "/api/system/login",
    getHistory = "/api/system/history"
}

export const wifiInfoList =
{
    "networks": [{
        "ssid": "GOKJ",
        "rssi": -43,
        "authmode": 3
    }, {
        "ssid": "Tasly_JuSheng",
        "rssi": -94,
        "authmode": 4
    }, {
        "ssid": "fengji",
        "rssi": -95,
        "authmode": 3
    }]
}

export const statusData: MinerStatusData = {
    // power_fault: "Voltage instability",
    currentTime: '2025-08-09 01:59:52',
    power: 121.670000076293945,
    voltage: 12208.75,
    current: 2237.5,
    temp: 35.7,
    vrTemp: 45,
    maxPower: 25,
    nominalVoltage: 5,
    hashRate: 0.236,
    bestDiff: "2",
    bestSessionDiff: "1",

    hwRate: "5",
    hwNumber: "100",
    nonceNumber: "2000",

    isStaticIP: "1",
    staticIP: "192.168.1.17",
    subnetMask: "255.255.255.0",
    gateway: "192.168.1.1",
    dns: "1.1.1.1",

    freeHeap: 2000,
    coreVoltage: 1200,
    coreVoltageActual: 1200,
    hostname: "SerpentX",
    macAddr: "2C:54:91:88:C9:E3",
    ssid: "default",
    wifiPass: "password",
    wifiStatus: "Connected!",
    wifiRSSI: -70,
    ethStatus: "ETH Link Up",
    ethConnected: 1,
    apEnabled: 0,
    sharesAccepted: 3,
    sharesRejected: 9,
    sharesRejectedReasons: [{ count: 2, message: "e" }, { count: 2, message: "a" }, { count: 3, message: "b" }, {
        count: 2,
        message: "c"
    }],
    uptimeSeconds: 38,
    asicCount: 1,
    smallCoreCount: 672,
    ASICModel: eASICModel.BM1366,
    stratumURL: "public-pool.io",
    stratumPort: 21496,
    fallbackStratumURL: "test.public-pool.io",
    fallbackStratumPort: 21497,
    stratumUser: "bc1q99n3pu025yyu0jlywpmwzalyhm36tg5u37w20d.bitaxe-U1",
    fallbackStratumUser: "bc1q99n3pu025yyu0jlywpmwzalyhm36tg5u37w20d.bitaxe-U1",
    stratumTLS: 1,
    stratumExtranonceSubscribe: 1,
    fallbackStratumTLS: 0,
    fallbackStratumExtranonceSubscribe: 0,
    isUsingFallbackStratum: true,
    frequency: 2000,
    version: "2.0.0 20260305",
    idfVersion: "v5.1.2",
    boardVersion: "204",
    flipscreen: 1,
    backlight: 1,
    invertscreen: 0,
    invertfanpolarity: 1,
    autofanspeed: 1,
    fanspeed: 80,
    fanrpm: 1000,
    fanrpm1: 2000,

    chipData: [
        { temp: 35, hashrate: 0.1, hw_errors: 0, frequency: 400 },
        { temp: 40, hashrate: 0.12, hw_errors: 2, frequency: 400 },
        { temp: 65, hashrate: 0.11, hw_errors: 5, frequency: 600 },
        { temp: 85, hashrate: 0.08, hw_errors: 10, frequency: 700 }
    ],
    bc04_individual_freq: true,
    frequency_0: 450,
    frequency_1: 480,
    frequency_2: 500,
    frequency_3: 520,

    boardtemp1: 30,
    boardtemp2: 40,
    overheat_mode: 0,
    boot_mode: 1,
    DeviceModel: 'DC06',
    ntpServerBackup: 'time.windows.com',
    ntpServer: 'ntp.aliyun.com',
    auth_enable: true, // Default Enable Mock
    autotuneActive: false,
    autotuneProgress: 0,
    autotuneLog: "Ready",
    autotuneProfile: JSON.stringify({
        "profiles": [
            { "freq": 400, "volt": 400, "hr_th": 4.0, "power_w": 55 },
            { "freq": 500, "volt": 440, "hr_th": 5.0, "power_w": 75 },
            { "freq": 600, "volt": 480, "hr_th": 6.0, "power_w": 90 },
            { "freq": 700, "volt": 490, "hr_th": 7.0, "power_w": 105 }
        ]
    })
}

// 1. 从环境变量读取 Mock 开关
// 注意：环境变量通常是字符串 'true'/'false'
const USE_MOCK = import.meta.env.VITE_USE_MOCK === 'true';

// 用于模拟累计数据
let mockUptime = 38;
let mockAccepted = 3;

const getDynamicMockStatus = (): MinerStatusData => {
    mockUptime += 2; // 假设每 2 秒一次查询
    if (Math.random() > 0.7) mockAccepted += 1;

    // 获取当前机型配置，以便限制波动范围
    const modelCfg = DEVICE_MODELS_INFO[statusData.DeviceModel || 'default'] || DEVICE_MODELS_INFO['default'];
    const refTable = modelCfg.ref_config_table || [];
    const bestRef = refTable.length > 0 ? refTable[refTable.length - 1] : null;

    // 随机抖动数值: 在 Ref/Best 和 Max 之间波动
    // 功率 (W)
    const maxPower = modelCfg.max_power_w;
    let minPower = bestRef?.power_w || (maxPower * 0.8);
    if (minPower >= maxPower) minPower = maxPower * 0.9;
    const power = minPower + Math.random() * (maxPower - minPower);

    // 电压 (mV)
    const maxVolt = modelCfg.max_voltage_v * 1000;
    const minVolt = modelCfg.ref_voltage_v * 1000;
    const voltage = minVolt + Math.random() * (maxVolt - minVolt);

    // 算力 (TH/s)
    let maxHashTH = modelCfg.max_hashrate_ghs / 1000;
    if (modelCfg.max_hashrate_ghs >= 7000000) maxHashTH = 7.0; // 应对潜在超大数值（若 7e6 为 GHs，则限制在合理范围内，例如 7TH/s）
    let minHashTH = bestRef?.hr_th || (maxHashTH * 0.8);
    if (minHashTH >= maxHashTH) minHashTH = maxHashTH * 0.9;
    const hashRate = minHashTH + Math.random() * (maxHashTH - minHashTH);

    // 频率 (MHz)
    const minFreq = modelCfg.ref_freq_mhz;
    const maxFreq = modelCfg.max_freq_mhz;
    const frequency = Math.floor(minFreq + Math.random() * (maxFreq - minFreq));

    const chipData = statusData.chipData?.map(chip => {
        // Temperature 波动：告警温度减去 15 到 告警温度之间 (如 55 ~ 70)
        const warnTemp = modelCfg.temp_warn_c || 75;
        const minTemp = warnTemp - 15;
        let temp = chip.temp + (Math.random() * 4 - 2);
        if (temp > warnTemp) temp = warnTemp;
        if (temp < minTemp) temp = Math.max(minTemp, 30);

        // Chip Hashrate: 按算力按比例稍微随机抖动
        const chipCount = statusData.chipData?.length || 1;
        let chipHash = (hashRate / chipCount) * (0.95 + Math.random() * 0.1);
        if (chipHash < 0) chipHash = 0;

        return { ...chip, temp, hashrate: chipHash, frequency };
    }) || [];

    // 时间戳
    const now = new Date();
    const pad = (n: number) => n.toString().padStart(2, '0');
    const timeStr = `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())} ${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`;

    return {
        ...statusData,
        currentTime: timeStr,
        uptimeSeconds: mockUptime,
        sharesAccepted: mockAccepted,
        power,
        voltage,
        hashRate,
        frequency,
        chipData,
        // 模拟风扇转速随温度变化
        fanspeed: 80 + Math.floor(Math.random() * 5 - 2),
        fanrpm: 1000 + Math.floor(Math.random() * 100 - 50),
    };
};

const getMinerStatus = async (baseUrl: string, data?: {}) => {
    // 2. 使用开关
    if (USE_MOCK) {
        console.log(`[Mock] getMinerStatus: Returning simulated data (Env: ${import.meta.env.MODE})`);
        // 模拟 500ms 网络延迟
        await new Promise(resolve => setTimeout(resolve, 500));
        return Promise.resolve(getDynamicMockStatus());
    }

    // 真实请求
    return get<MinerStatusData>({ baseURL: baseUrl, url: URL.getMinerStatus, data });
}

// Since-boot history for the dashboard chart, averaged by the device into the
// number of points we say we can draw.
const getHistory = async (windowSeconds: number, points = 180) => {
    if (USE_MOCK) {
        return Promise.resolve({ interval: 30, age: 0, count: 0, span: 0,
                                 hashrate: [], temp: [], vrTemp: [], power: [] });
    }
    return get<any>({ url: `${URL.getHistory}?window=${windowSeconds}&points=${points}` });
}

const restartMiner = async (baseUrl: string, data?: {}) => {
    if (USE_MOCK) {
        console.log(`[Mock] restartMiner`);
        await new Promise(r => setTimeout(r, 500));
        return Promise.resolve({ code: "200", msg: "Success" });
    }
    return post<any>({ baseURL: baseUrl, url: URL.restartMiner, data });
}

const updateSystem = async (baseUrl: string, data?: any) => {
    if (USE_MOCK) {
        console.log(`[Mock] updateSystem`, data);
        if (data) {
            Object.assign(statusData, data);
        }
        await new Promise(r => setTimeout(r, 500));
        return Promise.resolve({ code: "200", msg: "Success" });
    }
    return patch<any>({ baseURL: baseUrl, url: URL.updateSystem, data });
}

const setAutotuneStatus = async (baseUrl: string, data: { active: boolean }) => {
    if (USE_MOCK) {
        console.log(`[Mock] setAutotuneStatus: ${data.active}`);
        await new Promise(r => setTimeout(r, 500));
        return Promise.resolve({ code: "200", msg: "Success" });
    }
    return post<any>({ baseURL: baseUrl, url: URL.setAutotuneStatus, data });
}

const scanWifi = async (data?: {}) => {
    if (USE_MOCK) {
        return Promise.resolve({ networks: wifiInfoList.networks });
    }
    return get<{ networks: WifiNetwork[] }>({ url: URL.scanWifi, data });
}

const getKernelLog = async (data?: {}) => {
    if (USE_MOCK) {
        return Promise.resolve("Mock Kernel Log: [OK]");
    }
    return post<string>({ url: URL.getKernelLog, data, headers: { ...FORM_URLENCODED_HEADER } });
}

const getKernelLogFileList = async (data?: {}) => {
    if (USE_MOCK) {
        return Promise.resolve("log1.txt\nlog2.txt");
    }
    return post<string>({ url: URL.getKernelLogFileList, data });
}

const syncUTCTime = async (data?: {}) => {
    if (USE_MOCK) {
        return Promise.resolve("OK");
    }
    return post<string>({ url: URL.syncTime, data });
}

const ledOnOrOff = async (data?: {}) => {
    if (USE_MOCK) {
        return Promise.resolve("OK");
    }
    return post<string>({ url: URL.ledOnOrOff, data });
}

const getNetworkInfo = async (data?: {}) => {
    if (USE_MOCK) {
        console.log(`[Mock] getNetworkInfo`);
        await new Promise(r => setTimeout(r, 300));
        return Promise.resolve({
            code: "200",
            msg: "success",
            data: JSON.stringify({
                is_static_ip: 0,
                ip: "192.168.1.17",
                mask: "255.255.255.0",
                gateway: "192.168.1.1",
                dns: "1.1.1.1",
                hostname: "SerpentX"
            })
        } as unknown as ResData<string>);
    }
    return post<ResData<string>>({ url: URL.getNetworkInfo, data });
};

const setNetworkConf = async (data?: {}) => {
    if (USE_MOCK) {
        console.log(`[Mock] setNetworkConf`, data);
        await new Promise(r => setTimeout(r, 800));
        return Promise.resolve({ code: "200", msg: "Mock Save Success" } as unknown as ResData<any>);
    }
    return post<ResData<any>>({ url: URL.setNetworkConf, data });
};
// --- 新增内容开始 ---
const getTheme = async (data?: {}) => {
    if (USE_MOCK) {
        return Promise.resolve({ "theme-color": "blue" });
    }
    return get<any>({ url: URL.getTheme, data });
}

const setTheme = async (data?: {}) => {
    if (USE_MOCK) {
        return Promise.resolve({ code: "200" });
    }
    return post<any>({ url: URL.setTheme, data });
}

// The device has a single shared password, not user accounts.
const login = async (password: string) => {
    if (USE_MOCK) {
        await new Promise(resolve => setTimeout(resolve, 500));
        if (password === 'mock') {
            return Promise.resolve({ token: 'mock-token-12345' });
        }
        return Promise.reject({ response: { status: 401 } });
    }
    return post<LoginResponse>({ url: URL.login, data: { password } });
}
// --- 新增内容结束 ---

export {
    URL,
    getMinerStatus,
    getHistory,
    restartMiner,
    updateSystem,
    setAutotuneStatus,
    scanWifi,
    getKernelLog,
    getKernelLogFileList,
    syncUTCTime,
    ledOnOrOff,
    getNetworkInfo,
    setNetworkConf,
    // --- 新增内容开始 ---
    getTheme,
    setTheme,
    login
    // --- 新增内容结束 ---
};
