import { MinerStatusData, ChartData, DomainData } from "@/api/type.ts";

export interface AppState {
  isAPMode: boolean;
  /* set when the miner answers 401: it has a password and we hold no valid
   * token, so the router must offer the login page */
  authRequired?: boolean;
  staticMenuDesktopInactive: boolean;
  staticMenuMobileActive: boolean;
  isDebugMode: boolean;
  windowInnerWidth: number;
  currentTime: Date;
  localTime: Date;
  deviceModel: string;
  statusRaw: MinerStatusData;

  dataLabel: number[];
  hashrateData: number[];
  temperatureData: number[];
  temperatureData2: number[]; // [新增] 第二块算力板温度
  powerData: number[];
  chartData: ChartData[];
  chartDataVersion: number;
  domainsOrigin: DomainData;
  domainsDst: DomainData;
  chartResetting: boolean;
  historyWindow: number;
  historyLoading: boolean;

  // WebSocket Global State
  ws: WebSocket | null;
  wsConnecting: boolean;
  wsConnected: boolean;
  logContent: string;

  needsRestart: boolean;
  /* Settings written to NVS that the running miner has not picked up yet.
   * Keyed by the field name the API uses. */
  pendingSettings: Record<string, any>;
  consecutiveFailures: number; // [新增] 连续失败计数

  isPollingPaused: boolean;

  // Auth & General
  token: string;
  isDataLoaded: boolean;
}