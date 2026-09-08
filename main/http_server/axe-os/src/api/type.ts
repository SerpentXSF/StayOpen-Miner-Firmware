export interface ResData<T> {
  code: string;
  msg: string;
  data: T;
}

export interface NetworkInfo {
  hostname: string;
  ssid: string;
  wifiPass: string;

  // [新增] 显式 Wi-Fi 配置
  wifi_conf_nettype?: string;
  wifi_conf_ipaddress?: string;
  wifi_conf_netmask?: string;
  wifi_conf_gateway?: string;
  wifi_conf_dnsservers?: string;

  // [新增] 显式 Ethernet 配置
  eth_conf_nettype?: string;
  eth_conf_ipaddress?: string;
  eth_conf_netmask?: string;
  eth_conf_gateway?: string;
  eth_conf_dnsservers?: string;
}

export enum eASICModel {
  BM1366 = 'BM1366',
  BM1368 = 'BM1368',
  BM1397 = 'BM1397',
  BM1370 = 'BM1370'
}


export interface ISharesRejectedStat {
  message: string;
  count: number;
}

export interface ChartData {
  time: number,
  hashrate: number,
  temperature: number
}

export interface DomainData {
  temp: number[],
  hash: number[]
}

export interface ChipData {
  temp: number;
  hashrate: number;
  hw_errors: number;
  frequency: number;
}

export interface MinerStatusData {
  currentTime: string;
  flipscreen: number;
  backlight: number;
  invertscreen: number;
  power: number,
  voltage: number,
  current: number,
  temp: number,
  temp1?: number,
  vrTemp: number,
  autotuneActive?: boolean,
  autotuneProgress?: number,
  autotuneLog?: string,
  autotuneProfile?: string,
  maxPower: number,
  nominalVoltage: number,
  hashRate: number,
  bestDiff: string,
  bestSessionDiff: string,

  hwRate: string,
  hwNumber: string,
  nonceNumber: string,

  freeHeap: number,
  coreVoltage: number,
  hostname: string,
  macAddr: string,
  ssid: string,
  wifiPass: string,

  isStaticIP: string,
  staticIP: string,
  subnetMask: string,
  gateway: string,
  dns: string,

  wifiStatus: string,
  wifiRSSI: number;
  ethStatus: string;
  ethIP?: string;
  eth_on?: number;
  ethConnected: number;
  apEnabled: number,
  sharesAccepted: number,
  sharesRejected: number,
  sharesRejectedReasons: ISharesRejectedStat[];
  uptimeSeconds: number,
  asicCount: number,
  smallCoreCount: number,
  ASICModel: eASICModel,
  stratumURL: string,
  stratumPort: number,
  fallbackStratumURL: string,
  fallbackStratumPort: number,
  isUsingFallbackStratum: boolean,
  stratumUser: string,
  fallbackStratumUser: string,
  stratumTLS: number,
  stratumExtranonceSubscribe: number,
  fallbackStratumTLS: number,
  fallbackStratumExtranonceSubscribe: number,
  poolBExtranonceSubscribe?: number,
  frequency: number,
  version: string,
  idfVersion: string,
  boardVersion: string,
  invertfanpolarity: number,
  autofanspeed: number,
  fanspeed: number,
  fanrpm: number,
  fanrpm1?: number,
  coreVoltageActual: number,
  coreVoltageActual1?: number,

  boardtemp1?: number,
  boardtemp2?: number,
  overheat_mode: number,
  power_fault?: string,
  boot_mode: number,
  DeviceModel: string,
  ntpServerBackup: string,
  ntpServer: string,
  auth_enable?: boolean,
  chipData?: ChipData[],
  bc04_individual_freq?: boolean;
  frequency_0?: number;
  frequency_1?: number;
  frequency_2?: number;
  frequency_3?: number;
}

export interface LoginResponse {
  token: string;
}

export interface WifiNetwork {
  ssid: string;
  rssi: number;
  authmode: number;
}
