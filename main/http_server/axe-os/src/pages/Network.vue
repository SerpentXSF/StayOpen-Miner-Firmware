<script setup lang="ts">
import {computed, onMounted, onUnmounted, reactive, ref, watch} from "vue";
import {CloseCircleOutlined, ExclamationCircleFilled, ClusterOutlined} from "@ant-design/icons-vue";
import {useI18n} from "vue-i18n";
import {NetworkInfo, WifiNetwork} from "@/api/type.ts";
import {getMinerStatus, getNetworkInfo, restartMiner, scanWifi, setNetworkConf} from "@/api";
import {useAppStore} from "@/store";
import {showNotification, showNotificationLoading, validatorIP, validData} from "@/util/utils.ts";
import {FormInstance} from "ant-design-vue";
import SecuritySettings from "@/components/SecuritySettings.vue";
import {RES_CODE, WIFI_STATUS} from "@/util/const.ts";

const {t, locale} = useI18n();
const appStore = useAppStore();

const ncl = (code: string): string => t(`network.${code}`);
const ddl = (code: string): string => t(`dashboard.card.${code}`);

interface ActionStatus {
  saving: boolean;
  restarting: boolean;
  scanning: boolean;
}

// 扩展 FormState 以支持双网络
interface FormState {
  hostname: string;
  ssid: string;
  wifiPass: string;
  
  // Wi-Fi IP Config
  wifi_nettype: string;
  wifi_ip: string;
  wifi_mask: string;
  wifi_gateway: string;
  wifi_dns: string;

  // Ethernet IP Config
  eth_nettype: string;
  eth_ip: string;
  eth_mask: string;
  eth_gateway: string;
  eth_dns: string;
}

const nwWifiModalRef = ref<any>(null);
const minerStatusRef = ref<NetworkInfo>();
/* Starts true so the "no password" notice cannot flash up on a miner that
 * does have one, in the moment before the first status arrives. */
const authEnabled = ref<boolean>(true);
const activeTab = ref<string>('wifi'); // 控制当前激活的 Tab

const formState = reactive<FormState>({
  hostname: '',
  ssid: '',
  wifiPass: '',
  
  wifi_nettype: 'DHCP',
  wifi_ip: '',
  wifi_mask: '',
  wifi_gateway: '',
  wifi_dns: '',

  eth_nettype: 'DHCP',
  eth_ip: '',
  eth_mask: '',
  eth_gateway: '',
  eth_dns: '',
});

const NET_TYPE = { dhcp: "DHCP", static: "Static" };

const isWifiDhcp = computed(() => formState.wifi_nettype == NET_TYPE.dhcp);
const isEthDhcp = computed(() => formState.eth_nettype == NET_TYPE.dhcp);

const actionStatus = reactive<ActionStatus>({saving: false, restarting: false, scanning: false});
const wifiList = ref<{ label: string, value: string }[]>([]);
const showWifiList = ref<boolean>(false);

// [增量更新] 1. 优化数据同步逻辑：无条件加载所有网络配置
const syncNetworkConf = async () => {
  try {
    const res = await getNetworkInfo();
    if (res.code == RES_CODE.SUCCESS) {
      // 解析返回的数据 (根据 axios 配置，res.data 可能已经是对象或字符串)
      const info = typeof res.data === 'string' ? JSON.parse(res.data) : res.data;
      minerStatusRef.value = info;

      // === 基础设置 ===
      formState.hostname = info.hostname || '';
      formState.ssid = info.ssid || ''; // 当前连接的 SSID
      // formState.wifiPass 保持为空，不回填密码

      // === Wi-Fi 配置 (无论当前是 DHCP 还是 Static，都回填配置) ===
      // 后端现在会返回存储的静态 IP 配置，即使当前处于 DHCP 模式
      formState.wifi_nettype = info.wifi_conf_nettype || 'DHCP';
      formState.wifi_ip = info.wifi_conf_ipaddress || '';
      formState.wifi_mask = info.wifi_conf_netmask || '';
      formState.wifi_gateway = info.wifi_conf_gateway || '';
      formState.wifi_dns = info.wifi_conf_dnsservers || '';

      // === 有线配置 (同上) ===
      formState.eth_nettype = info.eth_conf_nettype || 'DHCP';
      formState.eth_ip = info.eth_conf_ipaddress || '';
      formState.eth_mask = info.eth_conf_netmask || '';
      formState.eth_gateway = info.eth_conf_gateway || '';
      formState.eth_dns = info.eth_conf_dnsservers || '';

    } else {
      showNotification(t('com.msg_sync_error'), 'error');
    }
  } catch (e) {
    console.error(e);
    showNotification(t('com.msg_sync_error'), 'error');
  }
}

// [增量更新] 2. 简化协议切换逻辑：仅处理校验，不再清空数据
const onChangeProtocol = (type: 'wifi' | 'eth', value: string) => {
  // 切换时仅移除之前的校验状态，不需要清空 formState 中的值
  // 这样用户从 DHCP 切回 Static 时，之前的配置还在
  setTimeout(() => {
    const fields = type === 'wifi' 
      ? ['wifi_ip', 'wifi_mask', 'wifi_gateway', 'wifi_dns']
      : ['eth_ip', 'eth_mask', 'eth_gateway', 'eth_dns'];
    
    // 如果切换到 DHCP，清除 IP 字段的红色校验提示
    if (value == NET_TYPE.dhcp) {
      fields.forEach(f => netCfgFormRef.value?.clearValidate(f));
    } else {
      // 切换到 Static，如果已有数据则触发校验，没有则保持原样
      netCfgFormRef.value?.validate(fields).catch(() => {});
    }
  }, 100);
};

const updateSys = async (values: any) => {
  actionStatus.saving = true;
  try {
    let wifiPassStr = values.wifiPass || '';
    wifiPassStr = (wifiPassStr === '*****' ? undefined : wifiPassStr);
    
    const formData = {
      hostname: values.hostname,
      ssid: values.ssid.trim(),
      wifiPass: wifiPassStr,
      
      // Wi-Fi Specific
      wifi_conf_nettype: values.wifi_nettype,
      wifi_conf_ipaddress: values.wifi_ip,
      wifi_conf_netmask: values.wifi_mask,
      wifi_conf_gateway: values.wifi_gateway,
      wifi_conf_dnsservers: values.wifi_dns,

      // Ethernet Specific
      eth_conf_nettype: values.eth_nettype,
      eth_conf_ipaddress: values.eth_ip,
      eth_conf_netmask: values.eth_mask,
      eth_conf_gateway: values.eth_gateway,
      eth_conf_dnsservers: values.eth_dns,
    }

    const res = await setNetworkConf(formData);
    if (res.code == RES_CODE.SUCCESS) {
      showNotification(t('com.msg_save_success'), 'success')
      appStore.setInfo({ needsRestart: true });
    } else {
      /* The firmware sends a reason for a refused setting; a generic "Save
       * failed" would throw it away and leave the cause on the device. */
      showNotification(res.msg || t('com.msg_save_failed'), 'error')
    }
  } catch (err) {
    showNotification(t('com.msg_save_failed'), 'error')
    appStore.setInfo({ needsRestart: false });
  } finally {
    actionStatus.saving = false;
  }
}

const scanWifiInner = async () => {
  actionStatus.scanning = true;
  try {
    const resData = await scanWifi();
    const networks = resData.networks.sort((a, b) => b.rssi - a.rssi);
    const poorNetworks = networks.filter(network => network.rssi >= -80);
    const uniqueNetworks = poorNetworks.reduce((acc, network) => {
      if (!acc[network.ssid] || acc[network.ssid].rssi < network.rssi) {
        acc[network.ssid] = network;
      }
      return acc;
    }, {} as { [key: string]: WifiNetwork });
    
    wifiList.value = Object.values(uniqueNetworks).map(n => ({
      label: `${n.ssid} (${n.rssi}dBm)`,
      value: n.ssid
    }));
    showWifiList.value = true;
  } catch (e) {
    showNotification(ncl('notification_scan_error'), 'error')
  } finally {
    actionStatus.scanning = false;
  }
}

const selectWifiSSID = (selectedSsid: string) => {
  formState.ssid = selectedSsid;
  showWifiList.value = false;
}

const restart = async () => {
  if (actionStatus.restarting) return;
  actionStatus.restarting = true;
  try {
    appStore.setInfo({ needsRestart: false });
    await restartMiner('')
    showNotificationLoading(t('com.msg_restarting_system'), 30);
    setTimeout(() => {
      actionStatus.restarting = false;
      window.location.reload();
    }, 30000);
  } catch (e) {
    actionStatus.restarting = false;
    showNotification(t('com.msg_restart_failed'), 'error');
  }
}

const netCfgFormRef = ref<FormInstance>();

watch(() => locale.value, () => {
  setTimeout(() => netCfgFormRef.value?.validate(), 100);
});
const ipValidator = (_: string, value: string, callback: Function) => {
  if (validatorIP(value)) callback();
  else callback(ncl("check_ip_fmt"));
};

// Two interfaces cannot share one static address: two MACs answering for one
// IP on one subnet makes the ARP entry flap, and the page loads its title and
// then stays blank while the miner carries on hashing. The firmware refuses to
// store it; this is so nobody has to send it to find out.
const duplicateIpValidator = (rule: any, value: string, callback: Function) => {
  if (!validatorIP(value)) {
    callback(ncl("check_ip_fmt"));
    return;
  }
  const other = String(rule?.field || '').startsWith('wifi_')
    ? {ip: formState.eth_ip, dhcp: isEthDhcp.value}
    : {ip: formState.wifi_ip, dhcp: isWifiDhcp.value};
  if (!other.dhcp && other.ip && other.ip === value) {
    callback(ncl("check_ip_duplicate"));
    return;
  }
  callback();
};

const hostnameValidator = (_: string, value: string, callback: Function) => {
  if (value && value.indexOf('-') == -1) callback();
  else callback(ncl("check_houstname_fmt"));
};

const getNotification = (wifiStatus: string) => {
  if (wifiStatus.startsWith(WIFI_STATUS.Connected)) return null;
  if (wifiStatus.startsWith(WIFI_STATUS.Connecting)) return {msg: ddl('w_connecting'), level: 'info'};
  if (wifiStatus.startsWith(WIFI_STATUS.PasswordError)) return {msg: ddl('w_pwd_err'), level: 'error'};
  if (wifiStatus.startsWith(WIFI_STATUS.NoApFound)) return {msg: ddl('w_ap_not_found'), level: 'error'};
  if (wifiStatus.startsWith(WIFI_STATUS.Error)) return {msg: ddl('w_error'), level: 'error'};
  return {msg: ddl('w_not_connected'), level: 'error'};
}

const checkWifiStatus = async () => {
  try {
    const resData = await getMinerStatus('');
    if (validData(resData)) {
      authEnabled.value = !!(resData as any).authEnabled;
      const info = getNotification(resData.wifiStatus);
      if(info) showNotification(info.msg, info.level);
    }
  } catch (e) {
  }
}

let refreshIntervalRef: any = 0;
onUnmounted(() => clearInterval(refreshIntervalRef));
/*
 * What the Ethernet port is actually doing, rather than a fixed instruction.
 *
 * ethStatus is the firmware's own string -- "ETH GetIP", "ETH Link Up",
 * "ETH Link Down", "ETH Started", "ETH Stopped". ethConnected folds the first
 * two together, and ethIP is the address it is reachable at.
 *
 * An empty status early in a boot is expected rather than a fault: on a BC04
 * Ethernet is deliberately started after the hashboard is powered, because a
 * controller already running through that transient stops answering.
 */
const ethState = computed(() => {
  const st: any = minerStatusRef.value || {};
  const status: string = st.ethStatus || '';

  if (st.eth_on === 0) {
    return {text: t('network.eth_off'), tone: 'is-off'};
  }
  if (st.ethConnected === 1 && st.ethIP) {
    return {text: `${t('network.eth_connected')} ${st.ethIP}`, tone: 'is-up'};
  }
  if (status.includes('Link Up')) {
    return {text: t('network.eth_linked'), tone: 'is-up'};
  }
  if (status.includes('Link Down') || status.includes('Stopped')) {
    return {text: t('network.eth_no_cable'), tone: 'is-down'};
  }
  if (!status) {
    return {text: t('network.eth_starting'), tone: ''};
  }
  return {text: t('network.eth_no_cable'), tone: 'is-down'};
});

onMounted(async () => {
  await checkWifiStatus();
  refreshIntervalRef = setInterval(checkWifiStatus, 15000);
  await syncNetworkConf();

  const info = minerStatusRef.value;
  if (info) {
    formState.hostname = info.hostname || '';
    formState.ssid = info.ssid || '';
    formState.wifiPass = '*****';

    // === Wi-Fi 初始化 ===
    formState.wifi_nettype = info.wifi_conf_nettype || info.wifi_conf_nettype || 'DHCP';
    formState.wifi_ip = info.wifi_conf_ipaddress || info.wifi_conf_ipaddress || '';
    formState.wifi_mask = info.wifi_conf_netmask || info.wifi_conf_netmask || '';
    formState.wifi_gateway = info.wifi_conf_gateway || info.wifi_conf_gateway || '';
    formState.wifi_dns = info.wifi_conf_dnsservers || info.wifi_conf_dnsservers || '';

    // === Ethernet 初始化 ===
    formState.eth_nettype = info.eth_conf_nettype || 'DHCP';
    formState.eth_ip = info.eth_conf_ipaddress || '';
    formState.eth_mask = info.eth_conf_netmask || '';
    formState.eth_gateway = info.eth_conf_gateway || '';
    formState.eth_dns = info.eth_conf_dnsservers || '';
    
    // 如果有有线网络，默认聚焦到 Ethernet Tab (可选)
    // if (appStore.hasEthernet) activeTab.value = 'ethernet';
  }
});
</script>

<template>
  <div ref="nwWifiModalRef">
    <a-modal
        :getContainer="() => (nwWifiModalRef as any)"
        class="nw-wifi-modal"
        :closable="true"
        v-model:open="showWifiList"
        footer=""
        width="580px"
        :body-style="{overflow:'auto', maxHeight: '60vh'}"
        :destroy-on-close="true"
        title="Select Wifi Network"
    >
      <template #closeIcon><CloseCircleOutlined style="font-size: 1.5rem"/></template>
      <div class="nw-warning-wrap">
        <ExclamationCircleFilled class="nw-warning-icon"/>
        <span class="nw-warning">{{ ncl('warning') }}</span>
      </div>
      <a-list :bordered="false" :data-source="wifiList">
        <template #renderItem="{ item }">
          <div class="nw-wifi-item-wrap">
            <a-button class="nw-wifi-item" type="primary" @click="selectWifiSSID(item.value)">
              {{ item.label }}
            </a-button>
          </div>
        </template>
      </a-list>
    </a-modal>

    <a-card :title="ncl('title')" class="card nw-card" style="border: 1px solid var(--surface-border); box-shadow: none;">
      <!--
        Shown only while no API password is set. This is the page people open
        first on a new miner, and an unset password leaves every endpoint on
        the network unauthenticated -- which is how the vendor firmware
        shipped. Saying it here, at the moment the miner is being put on a
        network, is the only place it reliably gets read.
      -->
      <div v-if="!authEnabled" class="nw-auth-notice">
        <ExclamationCircleFilled class="nw-auth-notice-icon" />
        <div>
          <div class="nw-auth-notice-title">{{ ncl('no_password_title') }}</div>
          <div class="nw-auth-notice-body">
            {{ ncl('no_password_body') }}
            <router-link to="/settings">{{ ncl('no_password_link') }}</router-link>
          </div>
        </div>
      </div>

      <div class="nw-form-wrap">
      <!-- Saved settings live in NVS but the miner reports what it booted
           with, so say plainly that a restart is still owed. Poolsettings has
           shown this since it was written; Network did not, and "Restart to
           Apply" on a button is easy to read past. -->
      <a-alert v-if="appStore.needsRestart" type="warning" show-icon
               class="nw-pending-alert" :message="t('com.restart_pending_banner')"/>
      <a-form ref="netCfgFormRef" :wrapper-col="{xs:24, sm: 12}" :model="formState" :hideRequiredMark="true" @finish="updateSys">
        
        <div class="nw-section-body" style="margin-bottom: 20px;">
            <a-form-item :label="ncl('hostname')" name="hostname"
                        :rules="[{required: true, message: t('com.rule_required')},{ validator: hostnameValidator }]">
              <a-input v-model:value="formState.hostname"></a-input>
            </a-form-item>
        </div>

        <a-tabs v-model:activeKey="activeTab" type="card">
          
          <a-tab-pane key="wifi" :tab="t('network.wifi_settings')">
             <div class="tab-content">
                <a-form-item :label="ncl('wifi_ssid')" name="ssid" :rules="[{required: true, message: t('com.rule_required')}]">
                  <a-input-search v-model:value="formState.ssid" enter-button :loading="actionStatus.scanning"
                                  @search="scanWifiInner"></a-input-search>
                </a-form-item>
                <a-form-item :label="ncl('wifi_pwd')" name="wifiPass">
                  <a-input-password v-model:value="formState.wifiPass"></a-input-password>
                </a-form-item>

                <a-divider style="margin: 20px 0; font-size: 12px; color: #999;">IP Configuration (Wi-Fi)</a-divider>

                <a-form-item :label="ncl('is_static_ip')" name="wifi_nettype">
                  <a-select v-model:value="formState.wifi_nettype" @change="(val: any) => onChangeProtocol('wifi', val)">
                    <a-select-option value="DHCP">DHCP (Automatic)</a-select-option>
                    <a-select-option value="Static">Static IP (Manual)</a-select-option>
                  </a-select>
                </a-form-item>
                
                <template v-if="!isWifiDhcp">
                  <a-form-item :label="ncl('static_ip')" name="wifi_ip"
                               :rules="[{required: true, message: t('com.rule_required')}, { validator: duplicateIpValidator }]">
                    <a-input v-model:value="formState.wifi_ip"></a-input>
                  </a-form-item>
                  <a-form-item :label="ncl('subnet_mask')" name="wifi_mask"
                               :rules="[{required: true, message: t('com.rule_required')}, { validator: ipValidator }]">
                    <a-input v-model:value="formState.wifi_mask"></a-input>
                  </a-form-item>
                  <a-form-item :label="ncl('gateway')" name="wifi_gateway"
                               :rules="[{required: true, message: t('com.rule_required')}, { validator: ipValidator }]">
                    <a-input v-model:value="formState.wifi_gateway"></a-input>
                  </a-form-item>
                  <a-form-item :label="ncl('dns')" name="wifi_dns"
                               :rules="[{required: true, message: t('com.rule_required')}, { validator: ipValidator }]">
                    <a-input v-model:value="formState.wifi_dns"></a-input>
                  </a-form-item>
                </template>
             </div>
          </a-tab-pane>

          <a-tab-pane key="ethernet" :tab="t('network.ethernet')" v-if="appStore.hasEthernet">
             <div class="tab-content">
               <!-- Live, not a fixed instruction. This read "plug in the
                    network cable" whether or not one was plugged in, and
                    whether or not Ethernet was even enabled, so a working
                    link looked identical to a broken one. -->
               <div class="nw-static-hint" :class="ethState.tone">
                  <ClusterOutlined />
                  {{ ethState.text }}
               </div>

               <a-form-item :label="ncl('is_static_ip')" name="eth_nettype">
                  <a-select v-model:value="formState.eth_nettype" @change="(val: any) => onChangeProtocol('eth', val)">
                    <a-select-option value="DHCP">DHCP (Automatic)</a-select-option>
                    <a-select-option value="Static">Static IP (Manual)</a-select-option>
                  </a-select>
                </a-form-item>
                
                <template v-if="!isEthDhcp">
                  <a-form-item :label="ncl('static_ip')" name="eth_ip"
                               :rules="[{required: true, message: t('com.rule_required')}, { validator: duplicateIpValidator }]">
                    <a-input v-model:value="formState.eth_ip"></a-input>
                  </a-form-item>
                  <a-form-item :label="ncl('subnet_mask')" name="eth_mask"
                               :rules="[{required: true, message: t('com.rule_required')}, { validator: ipValidator }]">
                    <a-input v-model:value="formState.eth_mask"></a-input>
                  </a-form-item>
                  <a-form-item :label="ncl('gateway')" name="eth_gateway"
                               :rules="[{required: true, message: t('com.rule_required')}, { validator: ipValidator }]">
                    <a-input v-model:value="formState.eth_gateway"></a-input>
                  </a-form-item>
                  <a-form-item :label="ncl('dns')" name="eth_dns"
                               :rules="[{required: true, message: t('com.rule_required')}, { validator: ipValidator }]">
                    <a-input v-model:value="formState.eth_dns"></a-input>
                  </a-form-item>
                </template>
             </div>
          </a-tab-pane>

        </a-tabs>

        <div class="nw-action">
          <a-button class="nw-action-btn" type="primary" html-type="submit" :loading="actionStatus.saving">
            {{ t('com.save') }}
          </a-button>
          <a-button class="nw-action-btn" :disabled="!appStore.needsRestart" type="primary" :loading="actionStatus.restarting"
                    @click="restart">
            {{ appStore.needsRestart ? t('com.restart_pending') : t('com.restart') }}
          </a-button>
          <div class="nw-restart-hint">{{ t('com.restart_hint') }}</div>
        </div>
      </a-form>
      </div>
    </a-card>

    <!--
      First run happens here: the miner is its own access point on
      192.168.4.1 and this is the page the owner lands on to give it a
      network. It is also the only moment they are guaranteed to be looking
      at the device, so it is where the password belongs. Without this a
      fresh miner joined the network with authentication switched off and
      nothing prompting anyone to turn it on.
    -->
    <SecuritySettings v-if="appStore.statusRaw?.apEnabled" />

  </div>
</template>

<style scoped lang="scss">
.nw-card {
  margin-top: 2rem;
  margin-bottom: 2rem;
}

.nw-title {
  font-size: 1.5rem;
  font-weight: 500;
  margin-bottom: 0.5rem;
}

.nw-auth-notice {
  display: flex;
  align-items: flex-start;
  gap: 0.75rem;
  margin-bottom: 1.5rem;
  padding: 0.9rem 1.1rem;
  border: 1px solid var(--nw-warn-border, #b45309);
  border-radius: var(--card-border-radius, 12px);
  background: rgba(180, 83, 9, 0.12);
}

.nw-auth-notice-icon {
  font-size: 1.25rem;
  color: #f59e0b;
  flex: 0 0 auto;
  margin-top: 0.1rem;
}

.nw-auth-notice-title {
  font-weight: 600;
  color: var(--text-color);
  margin-bottom: 0.15rem;
}

.nw-auth-notice-body {
  color: var(--text-color-secondary);
  font-size: 0.9rem;
  line-height: 1.5;
}

.nw-auth-notice-body a {
  color: var(--ant-primary-color);
  font-weight: 600;
}

.nw-form-wrap {
  margin-top: 1rem;
}

/* Tab 内容增加内边距 */
.tab-content {
  padding-top: 20px;
}

/* Tones for the live Ethernet status: connected, no link, switched off. */
.nw-static-hint.is-up { color: #22c55e; }
.nw-static-hint.is-down { color: #eab308; }
.nw-static-hint.is-off { opacity: .65; }

.nw-static-hint {
  font-size: 0.9rem;
  color: var(--text-color-secondary);
  background: rgba(128, 128, 128, 0.05);
  padding: 8px 12px;
  border-radius: 4px;
  margin-bottom: 1.5rem;
  display: flex;
  align-items: center;
  gap: 8px;
}

:deep(.ant-form-item .ant-form-item-label >label) {
  color: var(--text-color);
  font-size: 1rem;
  font-weight: 500;
  text-align: left;
  width: 10rem;
}

:deep(.ant-input), :deep(.ant-input-search .ant-input-search-button) {
  height: 35px;
}

:deep(.ant-input-affix-wrapper.ant-input-password ) {
  padding-top: 0 !important;
  padding-bottom: 0 !important;;
}

.nw-action {
  margin-top: 2rem;
  border-top: 1px solid var(--surface-border);
  padding-top: 1.5rem;
}

.nw-action-btn {
  margin-right: 10px;
  margin-bottom: 10px;
}

.nw-pending-alert {
  margin-bottom: 16px;
}

.nw-restart-hint {
  font-size: 0.9rem;
}

.nw-warning-wrap {
  margin-bottom: 1.1rem;
  font-size: 0.9rem;
  color: #F00000;
  padding: 0 1.1rem;
}
.nw-warning-icon { margin-right: 0.5rem; }
.nw-wifi-item-wrap { padding: 0 1rem; }
.nw-wifi-item {
  width: 100%;
  text-align: left;
  margin-bottom: 0.8rem;
  height: 2.9rem;
  line-height: 1;
}
:deep(.nw-wifi-modal .ant-modal-header) { margin-bottom: 2rem; }

/* Card tab colours are corrected globally in
 * styles/layout/_antd-fixes.scss: Ant Design paints the label on an inner
 * element, so setting a colour on the tab left it invisible. */
</style>
