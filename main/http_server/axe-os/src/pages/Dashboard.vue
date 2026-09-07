<script setup lang="ts">
import uPlot from "uplot";
import "uplot/dist/uPlot.min.css";
import { useI18n } from "vue-i18n";
import { useAppStore } from "@/store";
import { computed, onMounted, onUnmounted, watch, ref, reactive } from "vue";
import { MinerStatusData } from "@/api/type.ts";
import { dateAgo, showNotification, transformFreq } from "../util/utils.ts";
import { WIFI_STATUS } from "@/util/const.ts";
import { WEB_VERSION } from "../util/const.ts";
import { WarningOutlined, CheckOutlined, MenuOutlined } from "@ant-design/icons-vue"; 
import ChipHeatmap from "@/components/ChipHeatmap.vue";

const appStore = useAppStore();
const { t, locale } = useI18n();

const ddl = (code: string): string => t(`dashboard.card.${code}`);
const dtl = (code: string): string => t(`dashboard.chart.${code}`);
const dsrl = (code: string): string => t(`dashboard.progress.power.${code}`);
const dstl = (code: string): string => t(`dashboard.progress.heat.${code}`);
const dll = (code: string): string => t(`dashboard.pool.${code}`);

/* Pool B status. statusRaw is the unshaped API payload, so it is read through
 * one accessor here rather than casting in half a dozen template expressions. */
const dualPool = computed(() => {
  const s: any = appStore.statusRaw ?? {};
  return {
    enabled: !!s.dualEnable,
    connected: !!s.poolBConnected,
    url: s.poolBUrl ?? '',
    port: s.poolBPort ?? 0,
    user: s.poolBUser ?? '',
    ratioA: s.dualRatioA ?? 50,
    sharesA: s.sharesAccepted ?? 0,
    rejectedA: s.sharesRejected ?? 0,
    sharesB: s.poolBSharesAccepted ?? 0,
    rejectedB: s.poolBSharesRejected ?? 0,
    diffA: s.stratumDiff ?? 0,
    diffB: s.poolBDiff ?? 0,
  };
});
const del = (code: string): string => t(`dashboard.uptime.${code}`);
const dal = (code: string): string => t(`dashboard.alert.${code}`);

const tooltipData = reactive({ show: false, left: 0, top: 0, time: '', hash: '', temp: '', power: '' });
const chartColors = reactive({ primary: '#19e1a5', secondary: '#38bdf8', power: '#eab308' });

// [新增] 获取当前机型配置 (前提是您已在 store 中实现了 currentModelConfig getter)
// 如果 store 中还没加，请参照上一条回答修改 store/modules/app/index.ts
const modelCfg = computed(() => appStore.currentModelConfig);

/*
 * Core voltage regulator temperature.
 *
 * Null when the firmware does not report it, so the row simply does not appear
 * on a board that has nothing to show rather than reading "0 °C" as though the
 * regulator were ice cold.
 *
 * The thresholds are deliberately not the chip's. A BC01 regulator sits around
 * 69 °C while the die is at 55 °C, so reusing the ASIC warning level would mark
 * a perfectly normal board as hot. 85 is a conservative warning for a TPS546,
 * whose junction rating is far above that -- it is chosen to flag an unusual
 * climb rather than to mark a limit, and it is not a measured figure for this
 * board.
 */
const VR_WARN_C = 85;
const VR_SCALE_C = 100;   // full-scale for the bar, not a limit

const vrTemp = computed<number | null>(() => {
  const v = (appStore.statusRaw as any)?.vrTemp;
  if (typeof v !== 'number' || !isFinite(v) || v <= 0) return null;
  return Math.round(v);
});

// --- uPlot 相关变量 ---
const chartContainer = ref<HTMLElement | null>(null);
let uplotInst: uPlot | null = null;
let resizeObserver: ResizeObserver | null = null;
let themeObserver: MutationObserver | null = null;

interface MinerStatusDataProc {
  maxPower: number,
  nominalVoltage: number,
  maxTemp: number,
  maxFrequency: number,

  power: number,
  voltage: number,
  current: number,
  coreVoltageActual: number,
  coreVoltageActual1: number,
  coreVoltage: number,
  temp: number,
  temp1: number,

  expectedHashRate: number,
  quickLink: string,
  fallbackQuickLink: string,

  wifi: {
    disconnected?: boolean;
    status: string;
    hint: string;
    rssi: number;
    level: string; 
  }
}

const getQuickLink = (stratumURL: string, stratumUser: string): string | undefined => {
  if (!stratumURL || !stratumUser) return undefined;
  const address = stratumUser.split('.')[0];
  if (stratumURL.includes('public-pool.io')) return `https://web.public-pool.io/#/app/${address}`;
  if (stratumURL.includes('ocean.xyz')) return `https://ocean.xyz/stats/${address}`;
  if (stratumURL.includes('solo.d-central.tech')) return `https://solo.d-central.tech/#/app/${address}`;
  if (/^eusolo[46]?.ckpool.org/.test(stratumURL)) return `https://eusolostats.ckpool.org/users/${address}`;
  if (/^solo[46]?.ckpool.org/.test(stratumURL)) return `https://solostats.ckpool.org/users/${address}`;
  if (stratumURL.includes('pool.noderunners.network')) return `https://noderunners.network/en/pool/user/${address}`;
  if (stratumURL.includes('satoshiradio.nl')) return `https://pool.satoshiradio.nl/user/${address}`;
  if (stratumURL.includes('solohash.co.uk')) return `https://solohash.co.uk/user/${address}`;
  return stratumURL.startsWith('http') ? stratumURL : `http://${stratumURL}`;
}

const calculateAverage = (data: number[]): any => {
  if (data.length === 0) return 0;
  const sum = data.reduce((sum, value) => sum + value, 0);
  return (sum / data.length);
}

const getWifiStatus = (status: MinerStatusData) => {
    if (!status || !status.wifiStatus) return { disconnected: true, status: ddl('w_not_connected'), hint: '', level: 'error', rssi: 0 };
    if (status.wifiStatus.startsWith(WIFI_STATUS.Connected)) {
        if (status.wifiRSSI <= 0 && status.wifiRSSI >= -60) { return { status: ddl('w_excellent'), hint: '', level: 'good', rssi: status.wifiRSSI } }
        else if (status.wifiRSSI < -60 && status.wifiRSSI >= -70) { return { status: ddl('w_normal'), hint: '', level: 'good', rssi: status.wifiRSSI } }
        else { return { status: ddl('w_bad'), hint: ddl('w_bad_hint'), level: 'bad', rssi: status.wifiRSSI } }
    }
    return { disconnected: true, status: ddl('w_not_connected'), hint: '', level: 'error', rssi: 0 };
}

const statusProc = computed((): MinerStatusDataProc => { 
    const status = appStore.statusRaw;
    // 使用配置中的值作为默认兜底
    const cfg = modelCfg.value;

    if (!status) {
      return {
        maxPower: cfg.max_power_w, nominalVoltage: 12000, maxTemp: 90, maxFrequency: cfg.max_freq_mhz,
        power: 0, voltage: 0, current: 0, coreVoltageActual: 0, coreVoltageActual1: 0,
        coreVoltage: 0, temp: 0, temp1: 0, expectedHashRate: 0,
        quickLink: '', fallbackQuickLink: '',
        wifi: { status: ddl('w_not_connected'), hint: '', level: 'error', rssi: 0, disconnected: true }
      };
    }
  
  // [修改] 使用配置中的最大值 (防止图表或进度条溢出)
  const maxPower = Math.max(cfg.max_power_w, status.power);
  const nominalVoltage = status.nominalVoltage;
  const maxTemp = Math.max(90, status.temp);
  const maxFrequency = Math.max(cfg.max_freq_mhz, status.frequency);

  const power = parseFloat(status.power.toFixed(1))
  const voltage = parseFloat((status.voltage / 1000).toFixed(1));
  const current = parseFloat((status.current / 1000).toFixed(1));
  const coreVoltageActual = parseFloat((status.coreVoltageActual / 1000).toFixed(2));
  const coreVoltageActual1 = parseFloat(((status.coreVoltageActual1 || 0) / 1000).toFixed(2));

  const coreVoltage = parseFloat((status.coreVoltage / 1000).toFixed(2));
  const temp = parseFloat(status.temp.toFixed(1));
  const temp1 = parseFloat((status.temp1 || 0).toFixed(1));

  const expectedHashRate = Math.floor(status.frequency * ((status.smallCoreCount * status.asicCount) / 1000));
  const quickLink = getQuickLink(status.stratumURL, status.stratumUser) || '';
  const fallbackQuickLink = getQuickLink(status.fallbackStratumURL, status.fallbackStratumUser) || '';
  
  const wifi = getWifiStatus(status);

  return {
    maxPower,
    nominalVoltage,
    maxTemp,
    maxFrequency,
    power,
    voltage,
    current,
    coreVoltageActual,
    coreVoltageActual1,
    coreVoltage,
    temp,
    temp1,
    expectedHashRate,
    quickLink,
    fallbackQuickLink,
    wifi
  }
});

const showWarningWrap = computed(() => {
  return appStore.statusRaw?.overheat_mode || appStore.statusRaw?.power_fault || (!appStore.statusRaw?.frequency || appStore.statusRaw?.frequency < 400);
})

const hashrateInfo = computed(() => {
  return transformFreq((appStore.statusRaw?.hashRate || 0) * 1000000000);
})

const avgHashrateInfo = computed(() => {
  return transformFreq(calculateAverage(appStore.hashrateData));
})

const efficiency = computed(() => {
  const power = statusProc.value.power;
  const hashrateG = appStore.statusRaw?.hashRate || 0;
  if (hashrateG <= 0) return '0.0';
  return (power / (hashrateG / 1000)).toFixed(1);
});

const HISTORY_RANGES = [
  { label: '1H', seconds: 3600 },
  { label: '4H', seconds: 14400 },
  { label: '24H', seconds: 86400 },
];

const selectRange = async (seconds: number) => {
  if (appStore.historyLoading) return;
  await appStore.loadHistory(seconds);
  renderChart();
};

const resetChartData = () => {
  if (appStore.chartResetting) {
    return;
  }
  appStore.setInfo({chartResetting: true});
  appStore.resetChartData();
  appStore.maintainDataset();

  setTimeout(() => {
    renderChart(); 
    appStore.setInfo({chartResetting: false});
    showNotification(t('com.msg_reset_success'), 'success');
  }, 1000);
}

// --- uPlot 核心逻辑 ---

const visibleSeries = reactive({
  hash: true,
  temp: true,
  power: true
});

const toggleSeries = (key: 'hash' | 'temp' | 'power') => {
  visibleSeries[key] = !visibleSeries[key];
  if (!uplotInst) return;
  
  const idxMap = { hash: 1, temp: 2, power: 3 };
  const idx = idxMap[key];
  
  uplotInst.setSeries(idx, { show: visibleSeries[key] });

  const uObj = uplotInst as any; 
  if (uObj.opt.axes && uObj.opt.axes[idx]) {
    uObj.opt.axes[idx].show = visibleSeries[key];
  }

  uplotInst.redraw(true, true);
};

const getUplotData = () => {
  const len = appStore.chartData.length;
  if (len === 0) return [[], [], [], []];

  const times = new Float64Array(len);
  const hashs = new Float64Array(len);
  const temps = new Float64Array(len);
  const powers = new Float64Array(len);
  
  const srcHash = appStore.hashrateData || [];
  const srcTemp = appStore.temperatureData || [];
  const srcPower = appStore.powerData || [];
  
  const offHash = len - srcHash.length;
  const offTemp = len - srcTemp.length;
  const offPower = len - srcPower.length;

  for (let i = 0; i < len; i++) {
     times[i] = Math.floor(appStore.dataLabel[i] / 1000);
     hashs[i] = (i >= offHash) ? (srcHash[i - offHash] ?? 0) : 0;
     temps[i] = (i >= offTemp) ? (srcTemp[i - offTemp] ?? 0) : 0;
     powers[i] = (i >= offPower) ? (srcPower[i - offPower] ?? 0) : 0;
  }
  
  return [times, hashs, temps, powers];
}

const renderChart = () => {
  if (!chartContainer.value) return;

  const UNIT_GHS = 1000000;
  const style = getComputedStyle(document.documentElement);
  const themeVar = style.getPropertyValue('--theme').trim().replace(/['"]/g, '');
  const colorScheme = style.getPropertyValue('color-scheme').trim();
  const isDarkMode = themeVar.includes('dark') || colorScheme === 'dark';

  const textColor = isDarkMode ? '#FFFFFF' : '#000000';
  const gridColor = isDarkMode ? 'rgba(255, 255, 255, 0.1)' : 'rgba(0, 0, 0, 0.1)';
  const primaryColor = style.getPropertyValue('--ant-primary-color').trim() || '#19e1a5';
  const secondaryColor = '#38bdf8';
  const powerColor = '#eab308'; 

  chartColors.primary = primaryColor;
  chartColors.secondary = secondaryColor;
  chartColors.power = powerColor;

  // [新增] 获取配置
  const cfg = modelCfg.value;

  const rect = chartContainer.value.getBoundingClientRect();
  
  const opts: uPlot.Options = {
    width: rect.width,
    height: rect.height, 
    title: "",
    mode: 1,
    legend: { show: false }, 
    cursor: {
      points: {
        show: true,
        size: 8,
        width: 2,
        stroke: (u, seriesIdx) => u.series[seriesIdx].stroke as string,
        fill: isDarkMode ? '#1e1e1e' : '#ffffff'
      }
    },
    hooks: {
      setCursor: [
        u => {
          const { left, top, idx } = (u.cursor as any);
          if (left === undefined || top === undefined || idx === null || left < 0) {
            tooltipData.show = false;
            return;
          }
          tooltipData.show = true;
          tooltipData.left = left;
          tooltipData.top = top;
          
          if (u.data && u.data[0] && u.data[0][idx] !== undefined) {
             const d = new Date(u.data[0][idx] * 1000);
             const pad = (n: number) => n.toString().padStart(2, '0');
             tooltipData.time = `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
          }

          if (u.data && u.data[1] && u.data[1][idx] !== null && u.data[1][idx] !== undefined) {
             tooltipData.hash = transformFreq(u.data[1][idx]).value;
          } else {
             tooltipData.hash = '0.00';
          }
          
          if (u.data && u.data[2] && u.data[2][idx] !== null && u.data[2][idx] !== undefined) {
             tooltipData.temp = u.data[2][idx].toFixed(1) + ' °C';
          } else {
             tooltipData.temp = '-- °C';
          }

          if (u.data && u.data[3] && u.data[3][idx] !== null && u.data[3][idx] !== undefined) {
             tooltipData.power = u.data[3][idx].toFixed(1) + ' W';
          } else {
             tooltipData.power = '-- W';
          }
        }
      ],
    },
    scales: {
      x: { time: true },
      // [修改] 动态读取最大算力
      hash: { range: [0, cfg.max_hashrate_ghs * UNIT_GHS] }, 
      temp: { range: [0, 100] },
      // [修改] 动态读取最大功率
      power: { range: [0, cfg.chart_power_max] }, 
    },
    axes: [
      {
        stroke: textColor,
        grid: { show: false },
        ticks: { show: true, stroke: gridColor, width: 1 },
        space: 50, size: 50, gap: 10,
      },
      // 左轴：算力
      {
        scale: 'hash',
        show: visibleSeries.hash, 
        stroke: primaryColor,
        grid: { show: true, stroke: gridColor, width: 1 },
        // [修改] 使用配置表中的 chart_hash_incr_ghs
        incrs: [ cfg.chart_hash_incr_ghs * UNIT_GHS ], 
        space: 25,
        values: (_self, ticks) => ticks.map(v => transformFreq(v).value),
        size: 75,
      },
      // 右轴1：温度
      {
        scale: 'temp',
        side: 1,
        show: visibleSeries.temp, 
        stroke: secondaryColor,
        grid: { show: false },
        space: 25,
        incrs: [10],
        values: (_self, ticks) => ticks.map(v => v.toFixed(0) + "°C"),
      },
      // 右轴2：功率
      {
        scale: 'power',
        side: 1,
        show: visibleSeries.power, 
        stroke: powerColor,
        grid: { show: false },
        space: 25,
        // [关键修复] 动态计算步进值，始终保持 10 个刻度
        // DC02(50W)->5, DC04(100W)->10, DC08(200W)->20
        incrs: [ cfg.chart_power_max / 10 ],
        values: (_self, ticks) => ticks.map(v => v.toFixed(0) + "W"),
      }
    ],
    series: [
      {}, 
      {
        scale: 'hash',
        stroke: primaryColor,
        width: 2,
        show: visibleSeries.hash,
        points: { show: false } // 默认隐藏静态点
      }, 
      {
        scale: 'temp',
        stroke: secondaryColor,
        width: 2,
        show: visibleSeries.temp,
        points: { show: false } // 默认隐藏静态点
      }, 
      {
        scale: 'power',
        stroke: '#eab308', 
        width: 2,
        show: visibleSeries.power,
        dash: [10, 5],
        points: { show: false } // 默认隐藏静态点
      }  
    ]
  };

  const data = getUplotData();

  if (uplotInst) {
    uplotInst.destroy();
    // 不要使用 chartContainer.value.innerHTML = '';
    // 因为 chartContainer 内还包含了 tooltip 的独立 div
  }
  
  // 重新创建实例，注意要在 tooltip div 之前插入 canvas，或者仅仅是将实例附加到父级而不覆盖
  uplotInst = new uPlot(opts, data as any, chartContainer.value);
}

watch(() => appStore.chartDataVersion, () => {
  if (uplotInst) {
    uplotInst.setData(getUplotData() as any);
  } else {
    renderChart();
  }
});

watch(() => locale.value, () => {
  renderChart();
});

onMounted(async () => {
  /*
   * Ask the miner what it has been doing before drawing anything. Without
   * this the chart opens empty on every fresh page and fills only at the
   * speed of the poll, which is what made it a record of the tab rather
   * than of the device.
   */
  await appStore.loadHistory(appStore.historyWindow);

  themeObserver = new MutationObserver(() => {
    renderChart();
  });
  themeObserver.observe(document.documentElement, { attributes: true, attributeFilter: ['style', 'class'] });

  resizeObserver = new ResizeObserver(entries => {
    if (uplotInst && entries[0]) {
      const { width, height } = entries[0].contentRect;
      uplotInst.setSize({ width, height });
    }
  });
  if (chartContainer.value) {
    resizeObserver.observe(chartContainer.value);
  }

  setTimeout(() => {
    if (appStore.statusRaw) {
        renderChart();
    }
  }, 100);
});

onUnmounted(() => {
  if (uplotInst) uplotInst.destroy();
  if (resizeObserver) resizeObserver.disconnect();
  if (themeObserver) themeObserver.disconnect();
});

// --- SVG 仪表盘计算逻辑 ---
const pathLength = 100; 
const gaugeDashOffset = computed(() => {
  const percent = Math.min(Math.max(statusProc.value.temp / statusProc.value.maxTemp, 0), 1);
  return pathLength * (1 - percent);
});

// [修改] 使用配置的温度告警阈值 (temp_warn_c)
const gaugeColor = computed(() => {
  const temp = statusProc.value.temp;
  const warn = modelCfg.value.temp_warn_c;
  if (temp >= warn) return '#f97316'; 
  if (temp >= (warn - 15)) return '#facc15'; // 比如预警线
  return '#22d3ee'; 
});

</script>

<template>
  <div class="db-wrap">
    <a-row :gutter="[0, 20]">
      <a-col :span="24" v-if="showWarningWrap">
        <a-space direction="vertical" style="width: 100%">
          <a-alert v-if="appStore.statusRaw?.overheat_mode" :showIcon="true" type="error" :message="dal('overheated')" class="db-alert"></a-alert>
          <a-alert v-if="appStore.statusRaw?.power_fault" :showIcon="true" type="error" :message="`${appStore.statusRaw?.power_fault} ${dal('power_fault')}`" class="db-alert"></a-alert>
          <a-alert v-if="!appStore.statusRaw?.frequency || appStore.statusRaw?.frequency < 400" :showIcon="true" type="warning" :message="dal('low_freq')" class="db-alert"></a-alert>
        </a-space>
      </a-col>

      <a-col :span="24">
        <section class="status-strip" :class="{ 'debug-mode': appStore.isDebugMode }">
          <div class="pill">
            <div class="pill-label" style="display: flex; justify-content: space-between;">
              <span>{{ ddl('hash_rate') }}</span>
              <span>Efficiency</span>
            </div>
            <div class="pill-main">
              <div v-if="!appStore.statusRaw?.power_fault">
                <span class="pill-value">{{ hashrateInfo.data }}</span>
                <span class="pill-unit">{{ hashrateInfo.suffix }}</span>
              </div>
              <div v-else class="pill-status error">{{ ddl('power_fault') }}</div>

              <div v-if="!appStore.statusRaw?.power_fault" style="text-align: right;">
                  <span class="pill-value" style="font-size: 16px;">{{ efficiency }}</span>
                  <span class="pill-unit" style="font-size: 10px;">J/TH</span>
              </div>
            </div>
            <div class="pill-sub">{{ ddl('average') }}: {{ avgHashrateInfo.value }}</div>
          </div>

          <div class="pill">
            <div class="pill-label">
              {{ appStore.statusRaw?.autotuneActive ? 'Autotune Protocol' : ddl('shares') }}
            </div>
            <div class="pill-main" v-if="!appStore.statusRaw?.autotuneActive">
              <div>
                <span class="pill-value">{{ appStore.statusRaw?.sharesAccepted?.toFixed(0) || 0 }}</span>
              </div>
              <span class :class="appStore.statusRaw?.sharesRejected > 0 ? 'pill-tag error' : 'pill-tag'">
                {{ appStore.statusRaw?.sharesRejected || 0 }} {{ ddl('rejected') }}
              </span>
            </div>
            <div class="pill-sub" v-if="!appStore.statusRaw?.autotuneActive">
              {{ (appStore.statusRaw?.sharesAccepted > 0 || appStore.statusRaw?.sharesRejected > 0) ? 
                (appStore.statusRaw?.sharesAccepted / (appStore.statusRaw?.sharesAccepted + appStore.statusRaw?.sharesRejected) * 100).toFixed(2) : '100.00' 
              }}% accepted
            </div>
            
            <div class="pill-main" v-if="appStore.statusRaw?.autotuneActive" style="flex-direction: column; align-items: stretch; gap: 4px; margin-top: 4px;">
              <div style="font-size: 14px; font-weight: 500; color: #38bdf8; display: flex; justify-content: space-between;">
                <span>Tuning in progress</span>
                <span>{{ appStore.statusRaw?.autotuneProgress?.toFixed(1) || 0 }}%</span>
              </div>
              <div class="meter" style="width: 100%; height: 6px; background: rgba(56,189,248,0.2); border-radius: 4px; overflow: hidden;">
                <div class="meter-fill" :style="{ width: (appStore.statusRaw?.autotuneProgress || 0) + '%', background: '#38bdf8', height: '100%', transition: 'width 0.3s' }"></div>
              </div>
            </div>
            <div class="pill-sub" v-if="appStore.statusRaw?.autotuneActive" style="color: #10b981; font-family: monospace; font-size: 10px; margin-top: 4px; white-space: normal; line-height: 1.2;">
              > {{ appStore.statusRaw?.autotuneLog || 'Initializing...' }}
            </div>
          </div>

          <div class="pill" :class="{ 'pill-split': appStore.hasEthernet }">
            <template v-if="appStore.hasEthernet">
              <div class="pill-split-row">
                <div class="pill-half">
                  <div class="pill-label">{{ ddl('wifi_status') }}</div>
                  <div class="pill-main" style="justify-content: flex-start;">
                    <span class="pill-value">{{ statusProc.wifi.status }}</span>
                    <span v-if="!statusProc.wifi.disconnected" class="pill-status" :class="statusProc.wifi.level">
                      {{ statusProc.wifi.level === 'bad' ? 'Bad' : (statusProc.wifi.level === 'good' ? 'Good' : 'N/A') }}
                    </span>
                  </div>
                  <div class="pill-sub">{{ ddl('rssi') }}: {{ statusProc.wifi.disconnected ? 'N/A' : statusProc.wifi.rssi + ' dBm' }}</div>
                </div>
                <div class="pill-half">
                  <div class="pill-label">{{ ddl('ethernet_status') }}</div>
                  <div class="pill-main" style="justify-content: flex-start;">
                    <span class="pill-value" :style="{ color: appStore.statusRaw?.ethConnected ? '#34d0b6' : '#808080' }">
                      {{ appStore.statusRaw?.ethConnected ? ddl('eth_on') : ddl('eth_off') }}
                    </span>
                  </div>
                  <div class="pill-sub">{{ appStore.statusRaw?.ethStatus || 'N/A' }}</div>
                </div>
              </div>
            </template>
            <template v-else>
              <div class="pill-label">{{ ddl('wifi_status') }}</div>
              <div class="pill-main">
                <div>
                  <span class="pill-value">{{ statusProc.wifi.status }}</span>
                </div>
                <span v-if="!statusProc.wifi.disconnected" class="pill-status" :class="statusProc.wifi.level">
                  {{ statusProc.wifi.level === 'bad' ? 'Bad' : (statusProc.wifi.level === 'good' ? 'Good' : 'N/A') }}
                </span>
              </div>
              <div class="pill-sub">{{ ddl('rssi') }}: {{ statusProc.wifi.disconnected ? 'N/A' : statusProc.wifi.rssi + ' dBm' }}</div>
            </template>
          </div>

          <div class="pill">
            <div class="pill-label">{{ ddl('best_difficulty') }} & {{ del('title') }}</div>
            <div class="pill-main">
              <div>
                <span class="pill-value">{{ appStore.statusRaw?.bestDiff || 'N/A' }}</span>
                <span class="pill-unit">{{ ddl('all_time_best') }}</span>
              </div>
            </div>
            <div class="pill-sub">{{ ddl('since_system_root') }}: {{ appStore.statusRaw?.bestSessionDiff || 'N/A' }}</div>
            <div class="pill-sub">{{ del('title') }}: {{ dateAgo(appStore.statusRaw?.uptimeSeconds) }}</div>
          </div>

          <div class="pill" v-if="appStore.isDebugMode">
             <div class="pill-label">HW Errors</div>
             <div class="pill-main">
               <div>
                 <span class="pill-value">{{ appStore.statusRaw?.hwRate || '0' }}%</span>
               </div>
             </div>
             <div class="pill-sub">
                HW: {{ appStore.statusRaw?.hwNumber || '0' }} / Nonce: {{ appStore.statusRaw?.nonceNumber || '0' }}
             </div>
          </div>
        </section>
      </a-col>

      <a-col :span="24">
        <section class="content-row">
          <div class="card card-chart">
            <div class="card-header">
              <div class="card-title">{{ dtl('Performance') }}</div>
              
              <div class="chart-actions">
                
                <div class="chart-legend-inline desktop-only">
                  <div class="legend-item" @click="toggleSeries('hash')" :class="{ disabled: !visibleSeries.hash }">
                    <div class="checkbox-custom" :style="{ borderColor: 'var(--ant-primary-color)', backgroundColor: visibleSeries.hash ? 'var(--ant-primary-color)' : 'transparent' }">
                      <CheckOutlined v-if="visibleSeries.hash" style="font-size: 10px; color: #000;" />
                    </div>
                    <span>{{ dtl('hash_title') }}</span>
                  </div>

                  <div class="legend-item" @click="toggleSeries('temp')" :class="{ disabled: !visibleSeries.temp }">
                    <div class="checkbox-custom" :style="{ borderColor: '#38bdf8', backgroundColor: visibleSeries.temp ? '#38bdf8' : 'transparent' }">
                      <CheckOutlined v-if="visibleSeries.temp" style="font-size: 10px; color: #000;" />
                    </div>
                    <span>{{ dtl('temp_title') }}</span>
                  </div>

                  <div class="legend-item" @click="toggleSeries('power')" :class="{ disabled: !visibleSeries.power }">
                    <div class="checkbox-custom" :style="{ borderColor: '#eab308', backgroundColor: visibleSeries.power ? '#eab308' : 'transparent' }">
                      <CheckOutlined v-if="visibleSeries.power" style="font-size: 10px; color: #000;" />
                    </div>
                    <span>{{ dsrl('power') }}</span>
                  </div>
                </div>

                <div class="mobile-only">
                  <a-dropdown :trigger="['click']" placement="bottomRight">
                    <div class="btn-icon-ref">
                      <MenuOutlined />
                    </div>
                    <template #overlay>
                      <a-menu class="chart-legend-menu">
                        <a-menu-item key="hash" @click="toggleSeries('hash')">
                          <div class="menu-item-content">
                            <div class="checkbox-custom" :style="{ borderColor: 'var(--ant-primary-color)', backgroundColor: visibleSeries.hash ? 'var(--ant-primary-color)' : 'transparent' }">
                              <CheckOutlined v-if="visibleSeries.hash" style="font-size: 10px; color: #000;" />
                            </div>
                            <span>{{ dtl('hash_title') }}</span>
                          </div>
                        </a-menu-item>
                        <a-menu-item key="temp" @click="toggleSeries('temp')">
                          <div class="menu-item-content">
                            <div class="checkbox-custom" :style="{ borderColor: '#38bdf8', backgroundColor: visibleSeries.temp ? '#38bdf8' : 'transparent' }">
                              <CheckOutlined v-if="visibleSeries.temp" style="font-size: 10px; color: #000;" />
                            </div>
                            <span>{{ dtl('temp_title') }}</span>
                          </div>
                        </a-menu-item>
                        <a-menu-item key="power" @click="toggleSeries('power')">
                          <div class="menu-item-content">
                            <div class="checkbox-custom" :style="{ borderColor: '#eab308', backgroundColor: visibleSeries.power ? '#eab308' : 'transparent' }">
                              <CheckOutlined v-if="visibleSeries.power" style="font-size: 10px; color: #000;" />
                            </div>
                            <span>{{ dsrl('power') }}</span>
                          </div>
                        </a-menu-item>
                      </a-menu>
                    </template>
                  </a-dropdown>
                </div>

                <div class="divider-vertical"></div>

                <!-- The device keeps a day of samples; these ask it for a
                     slice of them. Ranges longer than the miner has been up
                     simply return what it has. -->
                <div class="range-group">
                  <button v-for="r in HISTORY_RANGES" :key="r.seconds"
                          class="btn-range"
                          :class="{ active: appStore.historyWindow === r.seconds }"
                          :disabled="appStore.historyLoading"
                          @click="selectRange(r.seconds)">
                    {{ r.label }}
                  </button>
                </div>

                <div class="divider-vertical"></div>

                <button class="btn-reset" @click="resetChartData" :disabled="appStore.chartResetting">
                  {{ dtl('reset') }}
                </button>
              </div>
            </div>
            
            <div class="chart">
              <div class="chart-lines" ref="chartContainer" style="position: relative;">
                 <!-- Hover Tooltip -->
                 <div v-show="tooltipData.show" class="uplot-tooltip" :style="{ left: tooltipData.left + 'px', top: tooltipData.top + 'px' }">
                    <div class="tt-time">{{ tooltipData.time }}</div>
                    <div class="tt-item" v-if="visibleSeries.hash"><span class="tt-dot" :style="{ background: chartColors.primary }"></span> <span class="tt-lbl">{{ dtl('hash_title') }}</span> <span class="tt-val">{{ tooltipData.hash }}</span></div>
                    <div class="tt-item" v-if="visibleSeries.temp"><span class="tt-dot" :style="{ background: chartColors.secondary }"></span> <span class="tt-lbl">{{ dtl('temp_title') }}</span> <span class="tt-val">{{ tooltipData.temp }}</span></div>
                    <div class="tt-item" v-if="visibleSeries.power"><span class="tt-dot" :style="{ background: chartColors.power }"></span> <span class="tt-lbl">{{ dsrl('power') }}</span> <span class="tt-val">{{ tooltipData.power }}</span></div>
                 </div>
              </div>
            </div>
          </div>

          <div class="vitals-column">
            <div class="card">
              <div class="card-header">
                <div class="card-title">{{ dsrl('title') }}</div>
              </div>
              <div class="metric-row">
                <span class="metric-label">{{ dsrl('power') }}</span>
                <span class="metric-value">{{ statusProc.power }} W</span>
              </div>
              <div class="meter">
                <div class="meter-fill" :style="{width: (statusProc.power / statusProc.maxPower) * 100 + '%'}"></div>
              </div>

              <div class="metric-row">
                <span class="metric-label">{{ dsrl('input_voltage') }}</span>
                <span class="metric-value">{{ statusProc.voltage }} V</span>
              </div>
              <div class="meter">
                <div class="meter-fill" :style="{width: Math.min((statusProc.voltage / 15) * 100, 100) + '%'}"></div>
              </div>

              <div class="metric-row">
                <span class="metric-label">
                  {{ dsrl('asic_freq') }}
                  </span>
                <span class="metric-value" :class="{ 'warn-text': (appStore.statusRaw?.frequency || 0) > modelCfg.ref_freq_mhz }">
                  {{ appStore.statusRaw?.frequency || 0 }} MHz
                </span>
              </div>
              <div class="meter">
                <div class="meter-fill" :style="{
                  width: ((appStore.statusRaw?.frequency || 0) / statusProc.maxFrequency) * 100 + '%',
                  background: (appStore.statusRaw?.frequency || 0) > modelCfg.ref_freq_mhz ? '#f97316' : ''
                }"></div>
                
                <div class="meter-marker" :style="{ left: (modelCfg.ref_freq_mhz / statusProc.maxFrequency) * 100 + '%' }">
                   <span class="meter-ref-text">REF:{{ modelCfg.ref_freq_mhz }}</span>
                </div>
              </div>

              <div class="metric-row">
                <span class="metric-label">
                  {{ dsrl('hashboard_voltage') }}
                  </span>
                <span class="metric-value" :class="{ 'warn-text': statusProc.coreVoltageActual > modelCfg.ref_voltage_v }">
                  {{ statusProc.coreVoltageActual }} V
                </span>
              </div>
              <div class="meter">
                 <div class="meter-fill" :style="{
                  width: (statusProc.coreVoltageActual / modelCfg.max_voltage_v) * 100 + '%',
                  background: statusProc.coreVoltageActual > (modelCfg.ref_voltage_v + 0.05) ? '#f97316' : ''
                }"></div>
                
                <div class="meter-marker" :style="{ left: (modelCfg.ref_voltage_v / modelCfg.max_voltage_v) * 100 + '%' }">
                   <span class="meter-ref-text">REF:{{ modelCfg.ref_voltage_v }}</span>
                </div>
              </div>
            </div>


            <div class="card">
              <div class="card-header">
                <div class="card-title">{{ dstl('title') }}</div>
              </div>

              <div class="gauge-container">
                <div class="gauge-info-top">
                   <span class="gauge-label">{{ dstl('hashboard_temp') }}</span>
                   <span class="gauge-value">{{ statusProc.temp > 0 ? statusProc.temp : '--' }} °C</span>
                </div>

                <div class="gauge-info-bottom">
                    <span class="gauge-sub" v-if="statusProc.temp >= modelCfg.temp_warn_c" style="color: #fb7185;">
                       <WarningOutlined /> {{ dstl('temp_danger') }}
                    </span>
                    <span class="gauge-sub" v-else>Safe</span>
                </div>
                
                <svg class="gauge-svg" viewBox="0 40 300 85" preserveAspectRatio="xMidYMid meet">
                  <defs>
                    <linearGradient id="gaugeGradient" x1="0%" y1="0%" x2="100%" y2="0%">
                      <stop offset="0%" stop-color="#22d3ee" />
                      <stop offset="100%" :stop-color="gaugeColor" />
                    </linearGradient>
                  </defs>
                  
                  <path 
                    d="M 30 110 A 50 50 0 0 1 80 60 L 220 60 A 50 50 0 0 1 270 110" 
                    fill="none" 
                    stroke="var(--surface-border)" 
                    stroke-width="6" 
                    stroke-linecap="round"
                  />

                  <path 
                    d="M 30 110 A 50 50 0 0 1 80 60 L 220 60 A 50 50 0 0 1 270 110" 
                    fill="none" 
                    stroke="url(#gaugeGradient)" 
                    stroke-width="6" 
                    stroke-linecap="round"
                    pathLength="100"
                    :stroke-dasharray="pathLength"
                    :stroke-dashoffset="gaugeDashOffset"
                    style="transition: stroke-dashoffset 1s ease-out;"
                  />
                </svg>
              </div>
              
              <!--
                The regulator, shown only when the firmware reports it. It runs
                well above the die -- around 69C against 55C on a BC01 -- and is
                the part that limits a voltage increase, so it belongs next to
                the chip temperature rather than buried in an API response.
              -->
              <div class="metric-row" style="margin-top:4px;" v-if="vrTemp !== null">
                <span class="metric-label" :title="dstl('vr_temp_hint')">{{ dstl('vr_temp') }}</span>
                <span class="metric-value" :style="vrTemp >= VR_WARN_C ? 'color:#fb7185' : ''">
                  {{ vrTemp }} °C
                </span>
              </div>
              <div class="meter" v-if="vrTemp !== null">
                <div class="meter-fill"
                     :style="{ width: Math.min(100, (vrTemp / VR_SCALE_C) * 100) + '%',
                               background: vrTemp >= VR_WARN_C ? '#f97316' : '' }"></div>
              </div>

              <div class="metric-row" style="margin-top:4px;" v-if="appStore.hasSecondHashBord">
                <span class="metric-label">{{ dstl('hashboard_temp1') }}</span>
                <span class="metric-value">{{ statusProc.temp1 > 0 ? statusProc.temp1 : '--' }} °C</span>
              </div>
              <div class="meter" v-if="appStore.hasSecondHashBord">
                <div class="meter-fill" :style="{width: (statusProc.temp1 / statusProc.maxTemp) * 100 + '%', background: statusProc.temp1 >= modelCfg.temp_warn_c ? '#f97316' : ''}"></div>
              </div>

              <div class="metric-row" style="margin-top:4px;">
                <span class="metric-label">{{ dstl('rpm_of_fan') }}</span>
                <span class="metric-value">{{ appStore.statusRaw?.fanspeed || 0 }}% ({{ appStore.statusRaw?.fanrpm || 0 }} RPM)</span>
              </div>
              <div class="meter">
                <div class="meter-fill" :style="{width: (appStore.statusRaw?.fanspeed || 0) + '%'}"></div>
              </div>
              
              <div class="metric-row" style="margin-top:4px;" v-if="appStore.hasSecondFan">
                <span class="metric-label">{{ dstl('rpm_of_fan1') }}</span>
                <span class="metric-value">{{ appStore.statusRaw?.fanspeed || 0 }}% ({{ appStore.statusRaw?.fanrpm1 || 0 }} RPM)</span>
              </div>
              <div class="meter" v-if="appStore.hasSecondFan">
                <div class="meter-fill" :style="{width: (appStore.statusRaw?.fanspeed || 0) + '%'}"></div>
              </div>
            </div>
          </div>
        </section>
      </a-col>

      <a-col :span="24" v-if="modelCfg.support_chip_monitoring && appStore.statusRaw?.chipData && appStore.statusRaw.chipData.length > 0">
        <ChipHeatmap />
      </a-col>

      <a-col :span="24">
        <section class="bottom-row">
          <div class="card">
            <div class="card-header">
              <div class="card-title">{{ dll('title') }} [{{ appStore.statusRaw?.isUsingFallbackStratum ? dll('fallback') : dll('primary') }}]</div>
            </div>
            <div class="info-list">
              <div class="info-row">
                <div class="info-key">{{ dll('url') }}</div>
                <div class="info-value">
                     <a :href="appStore.statusRaw?.isUsingFallbackStratum ? (statusProc.fallbackQuickLink|| appStore.statusRaw?.fallbackStratumURL) : (statusProc.quickLink || appStore.statusRaw?.stratumURL)" target="_blank">
                       {{ appStore.statusRaw?.isUsingFallbackStratum ? appStore.statusRaw?.fallbackStratumURL : appStore.statusRaw?.stratumURL }}
                     </a>
                </div>
              </div>
              <div class="info-row">
                <div class="info-key">{{ dll('port') }}</div>
                <div class="info-value">{{ appStore.statusRaw?.isUsingFallbackStratum ? appStore.statusRaw?.fallbackStratumPort : appStore.statusRaw?.stratumPort }}</div>
              </div>
              <div class="info-row">
                <div class="info-key">{{ dll('user') }}</div>
                <div class="info-value">{{ appStore.statusRaw?.isUsingFallbackStratum ? appStore.statusRaw?.fallbackStratumUser : appStore.statusRaw?.stratumUser }}</div>
              </div>
            </div>
          </div>

          <div class="card" v-if="dualPool.enabled">
            <div class="card-header">
              <div class="card-title">{{ dll('poolB_title') }}</div>
              <span class="dual-badge" :class="{ 'is-live': dualPool.connected }">
                {{ dualPool.connected ? dll('poolB_connected') : dll('poolB_waiting') }}
              </span>
            </div>
            <div class="info-list">
              <div class="info-row">
                <div class="info-key">{{ dll('url') }}</div>
                <div class="info-value">{{ dualPool.url }}:{{ dualPool.port }}</div>
              </div>
              <div class="info-row">
                <div class="info-key">{{ dll('user') }}</div>
                <div class="info-value dual-user">{{ dualPool.user }}</div>
              </div>
            </div>

            <div class="dual-split-row">
              <span class="dual-leg pool-a">A {{ dualPool.ratioA }}%</span>
              <span class="dual-leg pool-b">B {{ 100 - dualPool.ratioA }}%</span>
            </div>
            <div class="meter dual-meter">
              <div class="meter-fill" :style="{ width: dualPool.ratioA + '%' }"></div>
            </div>

            <div class="info-list">
              <div class="info-row">
                <div class="info-key">{{ dll('poolA_shares') }}</div>
                <div class="info-value">{{ dualPool.sharesA }} / {{ dualPool.rejectedA }}
                  <span class="dual-diff">@ {{ dualPool.diffA }}</span>
                </div>
              </div>
              <div class="info-row">
                <div class="info-key">{{ dll('poolB_shares') }}</div>
                <div class="info-value">{{ dualPool.sharesB }} / {{ dualPool.rejectedB }}
                  <span class="dual-diff">@ {{ dualPool.diffB }}</span>
                </div>
              </div>
            </div>
          </div>

          <div class="card">
            <div class="card-header">
              <div class="card-title">{{ t('logs.overview') }}</div>
            </div>
            <div class="info-list">
              <div class="info-row">
                <div class="info-key">{{ t('logs.model') }}</div>
                <div class="info-value">{{ appStore.statusRaw?.ASICModel }}</div>
              </div>
              <div class="info-row">
                <div class="info-key">{{ del('title') }}</div>
                <div class="info-value">{{ dateAgo(appStore.statusRaw?.uptimeSeconds) }}</div>
              </div>
              <div class="info-row">
                <div class="info-key">{{ t('logs.version') }}</div>
                <div class="info-value">{{ appStore.statusRaw?.version}}</div>
              </div>
              <div class="info-row">
                <div class="info-key">{{ t('settings.version_website') }}</div>
                <div class="info-value">{{WEB_VERSION}}</div>
              </div>              
              <div class="info-row">
                <div class="info-key">{{ t('logs.bord_version') }}</div>
                <div class="info-value">{{ appStore.statusRaw?.boardVersion }}</div>
              </div>
            </div>
            <div class="system-extra">
              <span class="badge">{{ appStore.statusRaw?.DeviceModel }}</span>
            </div>
          </div>
        </section>
      </a-col>
    </a-row>
  </div>
</template>

<style scoped lang="scss">
/* uPlot Tooltip */
.uplot-tooltip {
  position: absolute;
  background: var(--surface-card, rgba(0, 0, 0, 0.8));
  border: 1px solid var(--surface-border, rgba(255, 255, 255, 0.1));
  box-shadow: 0 4px 12px rgba(0,0,0,0.15);
  pointer-events: none;
  padding: 8px 12px;
  border-radius: 8px;
  z-index: 100;
  transform: translate(-50%, -105%);
  display: flex;
  flex-direction: column;
  gap: 4px;
  min-width: 140px;
  font-size: 13px;
  color: var(--text-color, #fff);
  backdrop-filter: blur(4px);
  margin-top: -15px;
  transition: top 30ms linear, left 30ms linear;
}
.tt-time {
  font-size: 11px;
  color: var(--text-secondary, #9ca3af);
  margin-bottom: 4px;
  border-bottom: 1px dashed var(--surface-border, rgba(255, 255, 255, 0.1));
  padding-bottom: 4px;
  text-align: center;
}
.tt-item {
  display: flex;
  align-items: center;
  gap: 6px;
  white-space: nowrap;
}
.tt-lbl {
  color: var(--text-secondary, #ccc);
  font-size: 12px;
}
.tt-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  display: inline-block;
  box-shadow: 0 0 2px rgba(0,0,0,0.3);
}
.tt-val {
  font-weight: bold;
  margin-left: auto;
  font-family: monospace;
  font-size: 12px;
}

/* 原样式 */
.db-alert {
  border-radius: var(--card-border-radius, 14px) !important;
  border: 1px solid var(--surface-border) !important;
  background-color: var(--surface-card) !important;

  &.ant-alert-error {
    background-color: rgba(239, 68, 68, 0.1) !important;
    border-color: rgba(248, 113, 113, 0.45) !important;
    :deep(.ant-alert-message), :deep(.ant-alert-icon) {
      color: #fb7185 !important;
    }
  }
  &.ant-alert-warning {
    background-color: rgba(249, 115, 22, 0.1) !important;
    border-color: rgba(251, 146, 60, 0.45) !important;
    :deep(.ant-alert-message), :deep(.ant-alert-icon) {
      color: #f97316 !important;
    }
  }
}

.status-strip {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 12px;
  
  &.debug-mode {
      grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  }
}

.pill {
  background: var(--pill-bg, radial-gradient(circle at top left, #1d2633, #050910));
  border: 1px solid var(--pill-border, var(--surface-border));
  border-radius: 12px;
  padding: 22px 26px; 
  display: flex;
  flex-direction: column;
  gap: 8px;
  box-shadow: var(--pill-shadow, none);
}

/* Split layout: WiFi | Ethernet side by side */
.pill-split {
  padding: 16px 0 !important;
}

.pill-split-row {
  display: flex;
  flex-direction: row;
  width: 100%;
}

.pill-half {
  flex: 1;
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding: 6px 22px;
}

.pill-half:first-child {
  border-right: 1px solid var(--surface-border);
}

.pill-half .pill-label {
  font-size: 10px;
}

.pill-half .pill-value {
  font-size: 16px;
}

.pill-half .pill-sub {
  font-size: 10px;
}

.pill-label {
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: 0.08em;
  color: var(--text-color-secondary);
}

.pill-main {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: 8px;
}

.pill-value {
  font-size: 20px;
  font-weight: 500;
  color: var(--pill-text, var(--text-color));
  white-space: nowrap;
}

.pill-unit {
  font-size: 12px;
  color: var(--text-color-secondary);
  margin-left: 4px;
}

.pill-sub {
  font-size: 11px;
  color: var(--text-color-secondary);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.pill-status {
  font-size: 11px;
  padding: 3px 8px;
  border-radius: 999px;
  font-weight: 500;
  white-space: nowrap;
  
  &.good {
    background: rgba(25, 225, 165, 0.12);
    color: #a7f3d0;
    border: 1px solid rgba(16, 185, 129, 0.5);
  }
  :root[color-scheme='light'] &.good {
     color: #10b981;
     background: #ecfdf5;
     border-color: #a7f3d0;
  }

  &.bad {
    background: rgba(249, 115, 22, 0.1);
    color: #fdba74;
    border: 1px solid rgba(251, 146, 60, 0.5);
  }
  :root[color-scheme='light'] &.bad {
     color: #f97316;
     background: #fff7ed;
     border-color: #fdba74;
  }

  &.error {
    background: rgba(239, 68, 68, 0.1);
    color: #fb7185;
    border: 1px solid rgba(248, 113, 113, 0.45);
  }
  :root[color-scheme='light'] &.error {
     color: #ef4444;
     background: #fef2f2;
     border-color: #fca5a5;
  }
}

.pill-tag {
  font-size: 11px;
  padding: 2px 8px;
  border-radius: 999px;
  white-space: nowrap;
  font-weight: 500;
  background: rgba(25, 225, 165, 0.12);
  color: #a7f3d0;
  border: 1px solid rgba(16, 185, 129, 0.5);
  
  :root[color-scheme='light'] & {
     color: #10b981;
     background: #ecfdf5;
     border-color: #a7f3d0;
  }

  &.error {
    background: rgba(239, 68, 68, 0.1);
    color: #fb7185;
    border: 1px solid rgba(248, 113, 113, 0.45);
    
    :root[color-scheme='light'] & {
       color: #ef4444;
       background: #fef2f2;
       border-color: #fca5a5;
    }
  }
}

.content-row {
  display: grid;
  grid-template-columns: minmax(0, 2.3fr) minmax(0, 1fr);
  gap: 16px;
  align-items: stretch;
}

.card {
  gap: 10px;
  display: flex;
  flex-direction: column;
}

.card-chart {
  height: auto; 
  display: flex;
  flex-direction: column;
}

.card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.dual-badge {
  font-size: 0.72rem;
  font-weight: 700;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  padding: 2px 10px;
  border-radius: 999px;
  color: var(--text-color-secondary);
  border: 1px solid var(--surface-border);
}

.dual-badge.is-live {
  color: var(--primary-color);
  border-color: var(--primary-color);
}

.dual-split-row {
  display: flex;
  justify-content: space-between;
  margin: 10px 0 4px;
  font-size: 0.8rem;
  font-weight: 700;
  font-variant-numeric: tabular-nums;
}

.dual-leg.pool-a { color: var(--primary-color); }
.dual-leg.pool-b { color: #ffb81c; }

.dual-meter .meter-fill {
  background: var(--primary-color);
}

/* the unfilled remainder stands for pool B */
.dual-meter {
  background: #ffb81c;
}

.dual-user {
  word-break: break-all;
}

/* the pools rarely share a difficulty, which is why the share counts differ */
.dual-diff {
  color: var(--text-color-secondary);
  font-size: 0.82em;
  margin-left: 0.35rem;
}

.card-title {
  font-size: 13px;
  text-transform: uppercase;
  letter-spacing: 0.08em;
  color: var(--text-color-secondary);
}

.btn-ghost {
  border: none;
  background: none;
  color: var(--text-color-secondary);
  font-size: 11px;
  cursor: pointer;
  padding: 4px 8px;
  &:hover {
    color: var(--text-color);
    text-decoration: underline;
  }
}

/* [新增] Header 右侧布局 */
.chart-actions {
  display: flex;
  align-items: center;
  gap: 16px;
}

.chart-legend-inline {
  display: flex;
  gap: 16px;
  align-items: center;
}

.legend-item {
  display: flex;
  align-items: center;
  gap: 6px;
  cursor: pointer;
  user-select: none;
  font-size: 12px;
  color: var(--text-color);
  transition: opacity 0.2s;

  &.disabled {
    opacity: 0.5;
    .checkbox-custom {
      background-color: transparent !important;
    }
  }
}

/* [新增] 自定义 Checkbox 样式 */
.checkbox-custom {
  width: 14px;
  height: 14px;
  border-width: 1px;
  border-style: solid;
  border-radius: 3px;
  display: flex;
  align-items: center;
  justify-content: center;
  transition: all 0.2s;
}

.divider-vertical {
  width: 1px;
  height: 16px;
  background-color: var(--surface-border);
  
  @media (max-width: 640px) {
    display: none;
  }
}

/* 响应式显示控制 */
.desktop-only {
  @media (max-width: 640px) {
    display: none !important;
  }
}

.mobile-only {
  display: none;
  @media (max-width: 640px) {
    display: block;
  }
}

.btn-icon-ref {
  width: 28px;
  height: 28px;
  display: flex;
  align-items: center;
  justify-content: center;
  border: 1px solid var(--surface-border);
  border-radius: 4px;
  background: var(--surface-ground);
  color: var(--text-color-secondary);
  cursor: pointer;
  
  &:hover {
    color: var(--ant-primary-color);
    border-color: var(--ant-primary-color);
  }
}

.chart-legend-menu {
  background: var(--surface-overlay) !important;
  border: 1px solid var(--surface-border) !important;
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.2) !important;
  
  :deep(.ant-dropdown-menu-item) {
    padding: 8px 12px;
    
    &:hover {
      background: rgba(255, 255, 255, 0.05) !important;
    }
  }
}

.menu-item-content {
  display: flex;
  align-items: center;
  gap: 10px;
  color: var(--text-color);
  font-size: 13px;
}

/* [修改] Reset 按钮样式升级 */
.range-group {
  display: inline-flex;
  gap: 4px;
}

.btn-range {
  border: 1px solid var(--surface-border);
  background: var(--surface-ground);
  color: var(--text-color-secondary);
  border-radius: 4px;
  padding: 4px 10px;
  font-size: 11px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s;
  letter-spacing: 0.5px;
}

.btn-range.active {
  border-color: var(--ant-primary-color);
  color: var(--ant-primary-color);
}

.btn-range:disabled {
  opacity: 0.5;
  cursor: default;
}

.btn-reset {
  border: 1px solid var(--surface-border);
  background: var(--surface-ground);
  color: var(--text-color-secondary);
  border-radius: 4px;
  padding: 4px 12px;
  font-size: 11px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s;
  text-transform: uppercase;
  letter-spacing: 0.5px;

  &:hover:not(:disabled) {
    border-color: var(--ant-primary-color);
    color: var(--ant-primary-color);
    background: rgba(25, 225, 165, 0.05);
  }

  &:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }
}

.chart {
  margin-top: 10px;
  flex: 1;
  min-height: 0; 
  border-radius: 12px;
  background: var(--chart-bg, radial-gradient(circle at top, rgba(25, 225, 165, 0.2), transparent 55%), linear-gradient(180deg, var(--surface-ground), var(--surface-ground)));
  border: 1px solid var(--surface-border);
  position: relative;
  overflow: hidden; 
}

.chart-lines {
  position: absolute;
  inset: 0;
  /* 确保 uPlot 容器填满父元素 */
  width: 100% !important;
  height: 100% !important;
}

.chart-legend {
  position: absolute;
  right: 16px;
  top: 12px;
  display: flex;
  gap: 8px;
  font-size: 10px;
  color: var(--text-color-secondary);
  z-index: 10; /* 确保在图表上方 */
}

.legend-item {
  display: flex;
  align-items: center;
  gap: 4px;
}

.legend-dot {
  width: 8px;
  height: 8px;
  border-radius: 999px;
  border: 1px solid var(--ant-primary-color);
  background: var(--ant-primary-color);
  opacity: 0.5;
}

.legend-dot.secondary {
  border-color: #38bdf8;
  background: #38bdf8;
}

.vitals-column {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

/* 1. 增加标题行底部的间距，为 REF 文字腾出位置，防止重叠 */
.metric-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 12px;
  /* [修改] 增加到底部 20px (原先可能是 4px 或 8px) */
  margin-bottom: 20px; 
  color: var(--text-color);
}

.metric-label {
  color: var(--text-color-secondary);
}

.metric-value {
  font-weight: 500;
  color: var(--text-color);
}

/* 2. 修改进度条容器：允许溢出显示 */
.meter {
  width: 100%;
  height: 6px;
  border-radius: 999px;
  background: var(--surface-ground);
  border: 1px solid var(--surface-border);
  /* [关键修改] 必须删除 overflow: hidden，或者设置为 visible */
  overflow: visible !important; 
  margin-bottom: 8px;
  position: relative;
}

/* 3. 修改填充条：手动保持圆角 */
.meter-fill {
  height: 100%;
  width: 0%;
  background: linear-gradient(90deg, var(--ant-primary-color), #22d3ee);
  transition: width 0.3s ease-out;
  /* [新增] 因为父容器不再裁剪，这里必须手动加圆角 */
  border-radius: 999px; 
}

/* [新增] 参考值样式：稍微淡一点，紧跟在标签后面 */
.metric-ref {
  font-size: 0.8em;
  color: var(--text-color-secondary);
  opacity: 0.7;
  margin-left: 4px;
  font-weight: 400;
}

/* [新增] 告警文字颜色 */
.warn-text {
  color: #f97316 !important; /* 橙色 */
}

/* 5. 保持刻度线样式 */
.meter-marker {
  position: absolute;
  top: 0;
  height: 100%;
  width: 4px;
  background-color: #facc15; /* 黄色 */
  z-index: 5;
  transform: translateX(-50%);
  box-shadow: 0 0 2px rgba(0,0,0,0.5);
  pointer-events: none;
}

/* 4. 优化文字显示 */
.meter-ref-text {
  position: absolute;
  top: -18px; /* 稍微再往上提一点，因为加了背景 */
  left: 50%;
  transform: translateX(-50%);
  
  font-size: 10px;
  font-weight: 600;
  color: #facc15; /* 黄色字体 */
  
  /* [新增] 黑色半透明背景，确保白底可见 */
  background-color: rgba(0, 0, 0, 0.75); 
  padding: 1px 4px; /* 内边距 */
  border-radius: 4px; /* 圆角胶囊 */
  
  white-space: nowrap;
  line-height: 1.2;
  z-index: 10;
  
  /* 可选：加个小箭头指向下面 (纯 CSS 画三角形) */
  &::after {
    content: '';
    position: absolute;
    bottom: -3px;
    left: 50%;
    transform: translateX(-50%);
    border-width: 3px 3px 0;
    border-style: solid;
    border-color: rgba(0, 0, 0, 0.75) transparent transparent transparent;
  }
}

.gauge-container {
  width: 100%;
  height: 100px;
  position: relative;
  margin-bottom: 4px;
  display: flex;
  justify-content: center;
}

.gauge-svg {
  width: 100%; 
  height: 100%;
  max-width: none; 
}

.gauge-info-top {
  position: absolute;
  top: -20px; 
  left: 0;
  width: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  z-index: 2;
}

.gauge-info-bottom {
  position: absolute;
  bottom: 25px;
  left: 0;
  width: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  z-index: 2;
}

.gauge-label {
  font-size: 11px;
  color: var(--text-color-secondary);
}

.gauge-value {
  font-size: 18px;
  font-weight: 500;
  color: var(--text-color);
  line-height: 1.2;
}

.gauge-sub {
  font-size: 10px;
  color: var(--text-color-secondary);
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 4px;
}

.bottom-row {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 16px;
  align-items: stretch;
  
  .card {
    height: 100%;
    display: flex;
    flex-direction: column;

    .info-list {
      flex-grow: 1;
    }
  }
}

.info-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
  margin-top: 4px;
  font-size: 12px;
}

.info-row {
  display: flex;
  justify-content: space-between;
  gap: 10px;
  align-items: center;
}

.info-key {
  color: var(--text-color-secondary);
  min-width: 80px;
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: 0.08em;
  flex-shrink: 0;
}

.info-value {
  font-family: "SF Mono", ui-monospace, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
  font-size: 12px;
  word-break: break-all;
  text-align: right;
  color: var(--text-color);
  
  a {
    color: var(--ant-primary-color);
    text-decoration: none;
    &:hover {
      text-decoration: underline;
    }
  }
}

.system-extra {
  margin-top: 10px;
  display: flex;
  justify-content: flex-end;
}

.badge {
  font-size: 11px;
  padding: 3px 8px;
  border-radius: 999px;
  background: rgba(37, 99, 235, 0.15);
  border: 1px solid rgba(59, 130, 246, 0.6);
  color: #bfdbfe;
}

@media (max-width: 960px) {
  .content-row,
  .bottom-row {
    grid-template-columns: minmax(0, 1fr);
  }
  
  .status-strip {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .chart {
     flex: none;
     min-height: 400px;
     height: auto;
  }
}

@media (max-width: 640px) {
  .status-strip {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}

@media (max-width: 480px) {
  .status-strip {
    grid-template-columns: minmax(0, 1fr);
  }
}

/* uPlot 样式覆盖 */
:deep(.uplot) {
  font-family: inherit !important;
}
/* 隐藏原生图例 */
:deep(.u-legend) {
  display: none;
}
</style>