<script setup lang="ts">
import {reactive, watch} from "vue";
import {computed, onMounted, ref} from "vue";
import {InfoCircleOutlined, WarningOutlined, ApiOutlined} from "@ant-design/icons-vue";
import {getMinerStatus, restartMiner, updateSystem, setAutotuneStatus} from "@/api";
import {MinerStatusData} from "@/api/type.ts";
import {WORK_MODE, ENABLE_STATE} from "@/util/const.ts";
import {getUrl, isEnable, showNotification, showNotificationLoading, validData} from "@/util/utils.ts";
import {useAppStore} from "@/store";
import {useI18n} from "vue-i18n";
import type {FormInstance} from "ant-design-vue";
const {t, locale} = useI18n();

const appStore = useAppStore();

const sel = (code: string): string => {
  return t(`settings.${code}`);
}

console.log('[SettingsEdit] setup started');

// ✅ 替换为从 store 读取
const referenceData = computed(() => {
  return appStore?.currentModelConfig?.ref_config_table || [];
});

const displayProfiles = computed(() => {
  if (appStore?.currentModelConfig?.support_autotune && tunedProfiles.value.length > 0) {
    return tunedProfiles.value.slice(0, 8);
  }
  return referenceData.value.slice(0, 8);
});

const isTunedData = computed(() => {
  return !!(appStore?.currentModelConfig?.support_autotune && tunedProfiles.value.length > 0);
});

// Move logic below to avoid TDZ
interface ActionStatus {
  saving: boolean;
  restarting: boolean;
  disablingOh: boolean;
}

const props = defineProps({
  url: String,
  title: String,
});

interface FormState {
  frequency: number;
  frequency_0?: number;
  frequency_1?: number;
  frequency_2?: number;
  frequency_3?: number;
  coreVoltage: number;
  fanspeed: number;
  autofanspeed: boolean,
  flipscreen: boolean;
  backlight: boolean;
  invertfanpolarity: boolean
  invertscreen: boolean;
  overheat_mode: number;
  boot_mode: number;
  ntpServerBackup: string;
  ntpServer: string;
}

const formState = reactive<FormState>({
  frequency: 0,
  frequency_0: 0,
  frequency_1: 0,
  frequency_2: 0,
  frequency_3: 0,
  coreVoltage: 0,
  fanspeed: 0,
  autofanspeed: true,
  flipscreen: true,
  backlight: true,
  invertfanpolarity: true,
  invertscreen: true,
  overheat_mode: 0,
  boot_mode: 0,
  ntpServerBackup: '',
  ntpServer: ''
});

const settingEditFormRef = ref<FormInstance>();

watch(
    () => locale.value,
    () => {
      setTimeout(() => {
        settingEditFormRef.value?.validate();
      }, 100);
    },
);

const actionStatus = reactive<ActionStatus>({saving: false, restarting: false, disablingOh: false});
const minerStatusRef = ref<MinerStatusData>();

const customizeMode = computed(() => {
  return formState.boot_mode == WORK_MODE.customize;
});

const overheatMode = computed(() => {
  return formState.overheat_mode == ENABLE_STATE.enable;
});

const getEditUrl = () => {
  // return props.url || getUrl('192.168.11.3');
  return props.url || getUrl(window.location.host);
}

const tunedProfiles = ref<any[]>([]);

watch(() => minerStatusRef.value?.autotuneProfile, (newVal) => {
  if (!newVal) {
    tunedProfiles.value = [];
    return;
  }
  try {
    const data = JSON.parse(newVal);
    if (data && data.profiles) {
      tunedProfiles.value = data.profiles;
    }
  } catch (e) {
    console.error("Failed to parse autotuneProfile", e);
  }
}, { immediate: true });

// Inline editing state
const editingIndex = ref<number>(-1);
const editingDraft = reactive({ freq: 0, volt: 0 });

const editProfile = (index: number, item: any) => {
  editingIndex.value = index;
  editingDraft.freq = item.freq;
  editingDraft.volt = item.volt;
};

const saveProfile = async (index: number) => {
  if (editingDraft.freq <= 0 || editingDraft.volt <= 0) {
    showNotification('Invalid frequency or voltage', 'error');
    return;
  }
  
  // Update local array
  tunedProfiles.value[index].freq = editingDraft.freq;
  tunedProfiles.value[index].volt = editingDraft.volt;
  editingIndex.value = -1;

  // Save to backend
  actionStatus.saving = true;
  try {
    const newProfileRaw = JSON.stringify({ profiles: tunedProfiles.value });
    await updateSystem(getEditUrl(), { autotuneProfile: newProfileRaw });
    showNotification('Preset saved to device', 'success');
  } catch (err) {
    showNotification('Failed to save preset', 'error');
  } finally {
    actionStatus.saving = false;
  }
};

const cancelEdit = () => {
  editingIndex.value = -1;
};

// Check if a profile is currently active
const currentFreq = computed(() => appStore?.statusRaw?.frequency || 0);
const currentVolt = computed(() => appStore?.statusRaw?.coreVoltage || 0);

const isProfileActive = (item: any) => {
  // If boot_mode is not customize, technically no preset is actively "forced"
  // but we matching freq/volt to show what it is running at right now.
  return currentFreq.value === item.freq && currentVolt.value === item.volt;
};

const applyTunedProfile = (item: any) => {
  formState.boot_mode = WORK_MODE.customize;
  formState.frequency = item.freq;
  formState.frequency_0 = item.freq;
  formState.frequency_1 = item.freq;
  formState.frequency_2 = item.freq;
  formState.frequency_3 = item.freq;
  formState.coreVoltage = item.volt; // UI operates in cV!
  showNotification(`Applied tuned profile: ${item.hr_th}T @ ${item.power_w}W`, 'success');
};

const startAutotune = async () => {
  try {
    const editUrl = getEditUrl();
    await setAutotuneStatus(editUrl, { active: true });
    showNotification('Autotune started. Check dashboard for progress.', 'success');
    if (minerStatusRef.value) {
      minerStatusRef.value.autotuneActive = true;
    }
  } catch (err) {
    showNotification('Failed to start autotune', 'error');
  }
};

const updateSys = async () => {
  actionStatus.saving = true;
  const editUrl = getEditUrl();
  try {
    const payload: any = { ...formState };
    payload.autofanspeed = formState.autofanspeed ? ENABLE_STATE.enable : ENABLE_STATE.disable;
    payload.flipscreen = formState.flipscreen ? ENABLE_STATE.enable : ENABLE_STATE.disable;
    payload.backlight = formState.backlight ? ENABLE_STATE.enable : ENABLE_STATE.disable;
    payload.invertfanpolarity = formState.invertfanpolarity ? ENABLE_STATE.enable : ENABLE_STATE.disable;
    payload.invertscreen = formState.invertscreen ? ENABLE_STATE.enable : ENABLE_STATE.disable;
    
    await updateSystem(editUrl, payload);
    showNotification(t('com.msg_save_success'), 'success')
    appStore.setInfo({ needsRestart: true });
  } catch (err) {
    showNotification(t('com.msg_save_failed'), 'error')
    appStore.setInfo({ needsRestart: false });
  } finally {
    actionStatus.saving = false;
  }
}

const onFinishFailed = (errorInfo: any) => {
  let errContent = 'Validation failed';
  if (errorInfo && errorInfo.errorFields && errorInfo.errorFields.length > 0) {
    errContent = errorInfo.errorFields.map((f: any) => f.errors.join(', ')).join('; ');
  }
  showNotification(t('com.msg_save_failed') + ': ' + errContent, 'error');
};

const syncMinerStatus = async () => {
  try {
    const resData = await getMinerStatus(getEditUrl());
    minerStatusRef.value = validData(resData);
  } catch (e) {
    console.log(e);
  }
}

const disableOverheatMode = async () => {
  actionStatus.disablingOh = true;
  formState.overheat_mode = ENABLE_STATE.disable;
  await updateSys();
  await syncMinerStatus();
  actionStatus.disablingOh = false
}

const restart = async () => {
  if(actionStatus.restarting){
    return;
  }

  const editUrl = getEditUrl();
  actionStatus.restarting = true;
  try {
    appStore.setInfo({ needsRestart: false });
    await restartMiner(editUrl)
    showNotificationLoading(t('com.msg_restarting_system'), 30);
    setTimeout(() => {
      actionStatus.restarting = false;
      window.location.reload();
    }, 30000);
  } catch (e) {
    actionStatus.restarting = false;
    showNotification(t('com.msg_restart_failed'), 'error');
  } finally {
  }
}



const workModes = computed(() => {
  let modes = [
    { value: WORK_MODE.normal, label: sel('m_normal') },
    { value: WORK_MODE.over_freq, label: sel('m_over_freq') },
    { value: WORK_MODE.customize, label: sel('m_cust_voltage_freq') },
    // { value: WORK_MODE.lower_power, label: sel('m_lower_power') },
    // { value: WORK_MODE.super_low_power, label: sel('m_super_low_power') },
    // { value: WORK_MODE.sleep, label: sel('m_sleep') },
  ];

  const debugModes = [
    { value: WORK_MODE.debug, label: sel('m_debug') },
    { value: WORK_MODE.power_debug, label: sel('m_power_debug') },
  ];

  if (appStore?.isDebugMode) {
    modes.push(...debugModes);
  }
  
  if(minerStatusRef.value) {
    const curMode = minerStatusRef.value.boot_mode;
    const isDebugModeSelected = debugModes.some(m => m.value === curMode);
    const isAlreadyInList = modes.some(m => m.value === curMode);

    if (isDebugModeSelected && !isAlreadyInList) {
        const modeToAdd = debugModes.find(m => m.value === curMode);
        if (modeToAdd) {
          modes.push(modeToAdd);
        }
    }
  }

  return modes.map(mode => {
    return {value: mode.value, label: mode.label}
  });
});

// [新增] 硬件限制计算
const maxFreq = computed(() => appStore?.currentModelConfig?.max_freq_mhz || 2600);
const maxVolt = computed(() => {
  const cfg = appStore?.currentModelConfig;
  // The settable cap where the model has one, the dial's top of scale
  // otherwise. On a BC04 they differ: the regulator reaches 5.20 V, the
  // vendor caps the chips at 4.80, and the firmware discards anything above
  // it -- so offering 5.20 here meant a value that saved and did nothing.
  return Math.round((cfg?.max_settable_voltage_v || cfg?.max_voltage_v || 1.4) * 100);
});

const freqRules = computed(() => [
  { required: true, message: t('com.rule_required') }
]);

const voltRules = computed(() => [
  { required: true, message: t('com.rule_required') }
]);

onMounted(async () => {
  await syncMinerStatus();
   if (minerStatusRef.value) { // <--- 添加这个检查
    formState.frequency = Number(minerStatusRef.value.frequency) || 0;
    formState.frequency_0 = Number(minerStatusRef.value.frequency_0) || formState.frequency;
    formState.frequency_1 = Number(minerStatusRef.value.frequency_1) || formState.frequency;
    formState.frequency_2 = Number(minerStatusRef.value.frequency_2) || formState.frequency;
    formState.frequency_3 = Number(minerStatusRef.value.frequency_3) || formState.frequency;
    formState.coreVoltage = Number(minerStatusRef.value.coreVoltage) || 0;
    formState.fanspeed = Number(minerStatusRef.value.fanspeed) || 0;
    formState.autofanspeed = isEnable(minerStatusRef.value.autofanspeed);
    formState.flipscreen = isEnable(minerStatusRef.value.flipscreen);
    formState.backlight = isEnable(minerStatusRef.value.backlight);
    formState.invertfanpolarity = isEnable(minerStatusRef.value.invertfanpolarity);
    formState.invertscreen = isEnable(minerStatusRef.value.invertscreen);
    formState.overheat_mode = Number(minerStatusRef.value.overheat_mode) || 0;
    formState.boot_mode = Number(minerStatusRef.value.boot_mode) || 0;
    formState.ntpServer = minerStatusRef.value.ntpServer;
    formState.ntpServerBackup = minerStatusRef.value.ntpServerBackup;
   }
});
</script>

<template>
  <div>
    <div class="card set-edit-card">
      <div class="set-edit-header">
        <div class="set-edit-title">{{ props.title || sel('gen_settings') || 'General Settings' }}</div>
      </div>

      <!-- Same reason as Network and Poolsettings: what is saved and what is
           running are two different things until the miner restarts. -->
      <a-alert v-if="appStore?.needsRestart" type="warning" show-icon
               class="set-edit-pending-alert" :message="t('com.restart_pending_banner')"/>
      <a-row class="set-edit-form-wrap" :gutter="[40, 20]">
        <a-col :xs="24" :lg="14">
          <a-form ref='settingEditFormRef' :model="formState" hide-required-mark @finish="updateSys" @finishFailed="onFinishFailed">
            <a-descriptions bordered :column="{ xs: 1, sm: 1, md: 1 }" size="small" style="margin-bottom: 24px;">
              <a-descriptions-item :label="sel('work_mode')">
                <a-form-item name="boot_mode" style="margin-bottom: 0;">
                  <a-select v-model:value="formState.boot_mode" :options="workModes"></a-select>
                </a-form-item>
              </a-descriptions-item>

              <template v-if="customizeMode">
                <template v-if="appStore.statusRaw?.bc04_individual_freq">
                  <a-descriptions-item :label="sel('freq') + ' (Chip 0)'">
                    <a-form-item name="frequency_0" :rules="freqRules" style="margin-bottom: 0;">
                      <a-input-number v-model:value="formState.frequency_0" :min="100" :max="maxFreq" class="set-edit-input"></a-input-number>
                    </a-form-item>
                  </a-descriptions-item>
                  <a-descriptions-item :label="sel('freq') + ' (Chip 1)'">
                    <a-form-item name="frequency_1" :rules="freqRules" style="margin-bottom: 0;">
                      <a-input-number v-model:value="formState.frequency_1" :min="100" :max="maxFreq" class="set-edit-input"></a-input-number>
                    </a-form-item>
                  </a-descriptions-item>
                  <a-descriptions-item :label="sel('freq') + ' (Chip 2)'">
                    <a-form-item name="frequency_2" :rules="freqRules" style="margin-bottom: 0;">
                      <a-input-number v-model:value="formState.frequency_2" :min="100" :max="maxFreq" class="set-edit-input"></a-input-number>
                    </a-form-item>
                  </a-descriptions-item>
                  <a-descriptions-item :label="sel('freq') + ' (Chip 3)'">
                    <a-form-item name="frequency_3" :rules="freqRules" style="margin-bottom: 0;">
                      <a-input-number v-model:value="formState.frequency_3" :min="100" :max="maxFreq" class="set-edit-input"></a-input-number>
                    </a-form-item>
                  </a-descriptions-item>
                </template>
                <template v-else>
                  <a-descriptions-item :label="sel('freq') + ' (MHz)'">
                    <a-form-item name="frequency" :rules="freqRules" style="margin-bottom: 0;">
                      <a-input-number v-model:value="formState.frequency" :min="0" :max="maxFreq" class="set-edit-input"></a-input-number>
                    </a-form-item>
                  </a-descriptions-item>
                </template>

                <a-descriptions-item :label="sel('voltage') + ' (10mV)'">
                  <a-form-item name="coreVoltage" :rules="voltRules" style="margin-bottom: 0;">
                    <a-input-number v-model:value="formState.coreVoltage" :min="0" :max="maxVolt" class="set-edit-input"></a-input-number>
                  </a-form-item>
                </a-descriptions-item>
              </template>

              <a-descriptions-item :label="sel('ntp_server')">
                <a-form-item name="ntpServer" :rules="[{required: true, message: t('com.rule_required')}]" style="margin-bottom: 0;">
                  <a-input v-model:value="formState.ntpServer" class="set-edit-input"></a-input>
                </a-form-item>
              </a-descriptions-item>

              <a-descriptions-item :label="sel('ntp_server_backup')">
                <a-form-item name="ntpServerBackup" :rules="[{required: true, message: t('com.rule_required')}]" style="margin-bottom: 0;">
                  <a-input v-model:value="formState.ntpServerBackup" class="set-edit-input"></a-input>
                </a-form-item>
              </a-descriptions-item>

              <a-descriptions-item :label="sel('auto_fan_ctrl')">
                <a-form-item name="autofanspeed" style="margin-bottom: 0;">
                  <div style="display: flex; align-items: center; gap: 8px;">
                    <a-switch v-model:checked="formState.autofanspeed" />
                    <span class="set-edit-checkbox-hint">{{ sel('auto_fan_ctrl') }}</span>
                  </div>
                </a-form-item>
              </a-descriptions-item>

              <a-descriptions-item v-if="!formState.autofanspeed" :label="`${sel('fan_speed')} (${formState.fanspeed}%)`">
                <a-form-item name="fanspeed" style="margin-bottom: 0;">
                  <a-slider v-model:value="formState.fanspeed"/>
                  <span v-show="formState.fanspeed < 33" class="set-edit-slider-hint" style="color: red; font-size: 0.85rem;"><WarningOutlined/> &nbsp;{{ sel('danger_oh') }}</span>
                  <span v-show="formState.fanspeed == 100" class="set-edit-slider-hint" style="color:#F2A900; font-size: 0.85rem;"><WarningOutlined/>{{ sel('simulator') }} </span>
                </a-form-item>
              </a-descriptions-item>

              <a-descriptions-item v-if="appStore?.hasFlipScreen" :label="sel('flip_screen')">
                <a-form-item name="flipscreen" style="margin-bottom: 0;">
                  <div style="display: flex; align-items: center; gap: 8px;">
                    <a-switch v-model:checked="formState.flipscreen" />
                    <a-tooltip>
                      <template #title>{{ sel('flip_screen_tip') }}</template>
                      <InfoCircleOutlined class="set-edit-checkbox-info" style="color: var(--text-color-secondary);" />
                    </a-tooltip>
                  </div>
                </a-form-item>
              </a-descriptions-item>

              <a-descriptions-item v-if="appStore?.hasBacklight" :label="sel('backlight')">
                <a-form-item name="backlight" style="margin-bottom: 0;">
                  <div style="display: flex; align-items: center; gap: 8px;">
                    <a-switch v-model:checked="formState.backlight" />
                    <a-tooltip>
                      <template #title>{{ sel('backlight_tip') }}</template>
                      <InfoCircleOutlined class="set-edit-checkbox-info" style="color: var(--text-color-secondary);" />
                    </a-tooltip>
                  </div>
                </a-form-item>
              </a-descriptions-item>

              <a-descriptions-item v-if="appStore?.hasInvertFanDutyCycle" :label="sel('invert_fan_duty_cycle')">
                <a-form-item name="invertfanpolarity" style="margin-bottom: 0;">
                  <div style="display: flex; align-items: center; gap: 8px;">
                    <a-switch v-model:checked="formState.invertfanpolarity" />
                    <a-tooltip>
                      <template #title>{{ sel('invert_fan_duty_cycle_tip') }}</template>
                      <InfoCircleOutlined class="set-edit-checkbox-info" style="color: var(--text-color-secondary);" />
                    </a-tooltip>
                  </div>
                </a-form-item>
              </a-descriptions-item>

              <a-descriptions-item v-if="overheatMode" :label="sel('disable_oh')">
                <div style="display: flex; flex-direction: column; align-items: flex-start; gap: 8px;">
                  <a-button danger class="set-edit-disable-btn" type="primary" :loading="actionStatus.disablingOh" @click="disableOverheatMode" style="width: auto;">
                    {{ sel('disable_oh') }}
                  </a-button>
                  <div class="set-edit-disable-overheat-hint" style="text-align: left;">{{ sel('disable_oh_hint') }}</div>
                </div>
              </a-descriptions-item>
            </a-descriptions>

            <div style="margin-top: 24px; padding-top: 16px; border-top: 1px solid var(--surface-border); display: flex; align-items: center; gap: 12px; flex-wrap: wrap;">
              <a-button class="set-edit-action-btn" type="primary" :loading="actionStatus.saving" html-type="submit" style="margin: 0;">
                {{ t('com.save') }}
              </a-button>
              <a-button class="set-edit-action-btn" :disabled="!appStore?.needsRestart" type="primary"
                        :loading="actionStatus.restarting" @click="restart" style="margin: 0;">{{ appStore?.needsRestart ? t('com.restart_pending') : t('com.restart') }}
              </a-button>
              <span class="set-edit-restart-hint" style="color: var(--text-color-secondary); font-size: 0.9rem;">{{ t('com.restart_hint') }}</span>
            </div>
          </a-form>
        </a-col>
        <a-col :xs="24" :lg="10" v-if="appStore.currentModelConfig?.showRefConfig !== false">
          <div class="ref-container">
            <div class="ref-section">
              <div class="ref-title" :style="isTunedData ? 'color: #38bdf8; display: flex; align-items: center; gap: 8px;' : ''">
                {{ isTunedData ? '✨ Autotuned Profiles' : sel('ref_config_title') }}
              </div>
              <div class="ref-table-wrap">
                <table class="ref-table">
                  <thead>
                    <tr>
                      <th>{{ sel('freq') }}</th>
                      <th>{{ sel('voltage') }} (/100)V</th>
                      <th v-if="displayProfiles[0] && displayProfiles[0].hr_th !== undefined">{{ sel('hashrate') }} (TH)</th>
                      <th v-if="displayProfiles[0] && displayProfiles[0].power_w !== undefined">{{ sel('power') }} (W)</th>
                      <th>Action</th>
                    </tr>
                  </thead>
                  <tbody>
                    <tr v-for="(item, index) in displayProfiles" :key="'dp'+index" :class="{'active-row': isProfileActive(item)}">
                      <td>
                        <a-input-number v-if="isTunedData && editingIndex === index" v-model:value="editingDraft.freq" :min="0" :max="maxFreq" size="small" style="width: 70px" />
                        <span v-else style="font-weight: 500;">{{ item.freq }}</span>
                      </td>
                      <td>
                        <a-input-number v-if="isTunedData && editingIndex === index" v-model:value="editingDraft.volt" :min="0" :max="maxVolt" size="small" style="width: 70px" />
                        <span v-else>{{ item.volt }}</span>
                      </td>
                      <td v-if="displayProfiles[0] && displayProfiles[0].hr_th !== undefined">
                        <span v-if="isTunedData && editingIndex === index">--</span>
                        <span v-else style="color: #52C41A;">{{ item.hr_th }}</span>
                      </td>
                      <td v-if="displayProfiles[0] && displayProfiles[0].power_w !== undefined">
                        <span v-if="isTunedData && editingIndex === index">--</span>
                        <span v-else style="color: #faad14;">{{ item.power_w }}</span>
                      </td>
                      <td>
                        <div v-if="isTunedData && editingIndex === index" style="display: flex; gap: 4px; justify-content: center;">
                          <a-button size="small" type="primary" @click="saveProfile(index)">Save</a-button>
                          <a-button size="small" @click="cancelEdit">Cancel</a-button>
                        </div>
                        <div v-else style="display: flex; gap: 4px; justify-content: center;">
                          <a-button size="small" type="primary" style="font-size: 10px;" :class="{'active-btn-success': isProfileActive(item)}" @click="!isProfileActive(item) && applyTunedProfile(item)">
                            {{ isProfileActive(item) ? 'Active' : 'Apply' }}
                          </a-button>
                          <a-button v-if="isTunedData" size="small" type="dashed" style="font-size: 10px;" @click="editProfile(index, item)">Edit</a-button>
                        </div>
                      </td>
                    </tr>
                  </tbody>
                </table>
              </div>
            </div>

            <div class="ref-section" v-if="appStore?.currentModelConfig?.support_autotune">
              <a-button type="primary" style="width: 100%; background: #0ea5e9; border: none; font-weight: 500;" 
                :loading="minerStatusRef?.autotuneActive" :disabled="minerStatusRef?.autotuneActive" 
                @click="startAutotune">
                <template #icon><ApiOutlined /></template>
                {{ minerStatusRef?.autotuneActive ? 'Tuning in Progress...' : 'Start Autotune Protocol' }}
              </a-button>
              <div style="font-size: 12px; color: #888; text-align: center; margin-top: 8px;">
                Automatically finds the best voltage for each frequency.
              </div>
            </div>
          </div>
        </a-col>
      </a-row>
    </div>
    
  </div>
</template>

<style scoped lang="scss">
.set-edit-card {
  padding: 0.5rem 1.2rem;
}

.set-edit-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.set-edit-title {
  font-size: 1.5rem;
  font-weight: 500;
}

.set-edit-form-wrap {
  margin-top: 1rem;
}

:deep(.ant-form-item .ant-form-item-label >label) {
  color: var(--text-color);
  font-size: 1rem;
  font-weight: 500;
  text-align: left;
  width: 12rem;
}

:deep(.ant-input) {
  height: 35px;
}

:deep(.ant-select-selector) {
  height: 37px !important;
  line-height: 37px !important;
}

:deep(.ant-select-selection-item) {
  line-height: 37px !important;
}

:deep(.ant-input-number .ant-input-number-input) {
  height: 35px;
  line-height: 35px;
}


.set-edit-checkbox-wrap {
  display: flex;
  flex-direction: column;
  margin-top: 1rem;

}

.set-edit-checkbox-hint {
  font-size: 1rem;
  color: var(--text-color);
  font-weight: 500;
}

.set-edit-checkbox-info {
  font-size: 0.9rem;
  vertical-align: middle;
}

.set-edit-input {
  width: 100%;
}

:deep(.ant-modal .ant-modal-content) {
  padding: 0 !important;
}

:deep(.set-edit-form-wrap .ant-slider) {
  margin-bottom: 7px;
  height: 16px; 
}

:deep(.set-edit-form-wrap .ant-slider-rail) {
  background-color: rgba(255, 255, 255, 0.15) !important;
  border: 1px solid rgba(255, 255, 255, 0.2) !important;
  height: 8px !important;
  border-radius: 4px !important;
}

:deep(.set-edit-form-wrap .ant-slider-track) {
  background-color: #1A81FF !important;
  height: 8px !important;
  border-radius: 4px !important;
}

:deep(.set-edit-form-wrap .ant-slider-handle) {
  margin-top: -6px !important;
  width: 16px !important;
  height: 16px !important;
}

:deep(.set-edit-form-wrap .ant-slider-handle::after) {
  box-shadow: 0 0 0 2px #1A81FF !important;
  width: 12px !important;
  height: 12px !important;
  inset: 1px !important;
}

.set-edit-slider-label {
  font-size: 1rem;
  color: var(--text-color);
  font-weight: 500;
}

.set-edit-disable-wrap {
  margin-bottom: 2rem;
}

.set-edit-disable-btn {
  width: 100%;
  height: 2.3rem;
  background: rgba(255, 47, 24, 1);
  color: #ffffff;
}

.set-edit-disable-overheat-hint {
  width: 100%;
  text-align: center;
  color: rgba(240, 0, 0, 1);
  font-size: 0.9rem;
  margin-top: 0.2rem;
}

.set-edit-action-btn {
  margin-right: 10px;
  margin-bottom: 10px;
}

/* 新增：参考表样式 */
.ref-container {
  padding: 1.5rem;
  background: var(--surface-ground); 
  border-radius: 8px;
  border: 1px solid var(--surface-border);
  height: 100%;
  color: var(--text-color);
}

.ref-section {
  margin-bottom: 1.5rem;
}

.ref-title {
  font-weight: 600;
  font-size: 1rem;
  margin-bottom: 0.5rem;
  color: var(--text-color);
}

.ref-desc {
  font-size: 0.9rem;
  color: var(--text-color-secondary);
  line-height: 1.5;
}

.ref-table-wrap {
  overflow-x: auto;
  overflow-y: auto; /* 允许垂直滚动 */
  max-height: 350px; /* [关键] 限制最大高度，大概显示 8-9 行 */
  border: 1px solid var(--surface-border); /* 加个边框让滚动区域更明显 */
  border-radius: 4px;
}

/* [新增] 表头固定样式 */
.ref-table thead th {
  position: sticky;
  top: 0;
  background: var(--surface-ground); /* 必须指定背景色，防止内容透视 */
  z-index: 2; /* 确保在内容之上 */
  box-shadow: 0 1px 0 var(--surface-border); /* 下边框 */
}

.ref-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 0.9rem;
  text-align: center;

  thead {
    th {
      padding: 8px;
      border-bottom: 2px solid var(--surface-border);
      color: var(--text-color-secondary);
      font-weight: 600;
    }
  }

  tbody {
    td {
      padding: 6px;
      border-bottom: 1px solid var(--surface-border);
      color: var(--text-color);
    }
    
    tr:last-child td {
      border-bottom: none;
    }
    
    tr:nth-child(even) {
      background-color: rgba(128, 128, 128, 0.05); 
    }
    
    tr.active-row {
      background-color: rgba(16, 185, 129, 0.15) !important;
      border: 1px solid rgba(16, 185, 129, 0.3) !important;
    }
  }
}

.active-btn-success {
  background-color: #10b981 !important;
  border-color: #10b981 !important;
  color: #fff !important;
  opacity: 1 !important;
  cursor: default !important;
}


:deep(.set-edit-form-wrap .ant-switch:not(.ant-switch-checked)) {
  background-color: rgba(255, 255, 255, 0.15) !important;
  border: 1px solid rgba(255, 255, 255, 0.1) !important;
}

:deep(.set-edit-form-wrap .ant-switch-checked) {
  background-color: #34d0b6 !important;
  border-color: #34d0b6 !important;
}
.set-edit-pending-alert {
  margin-bottom: 16px;
}

</style>