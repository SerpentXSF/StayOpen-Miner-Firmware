<script setup lang="ts">
import {onMounted, reactive, ref, watch, computed} from "vue";
import {MinerStatusData} from "@/api/type.ts";
import {getMinerStatus, restartMiner, updateSystem} from "@/api";
import {showNotification, showNotificationLoading, validData} from "@/util/utils.ts";
import {useAppStore} from "@/store";
import {useI18n} from "vue-i18n";
import {FormInstance} from "ant-design-vue";
import { POOL_MODELS } from "@/util/pools.ts";

const {t, locale} = useI18n();

const pl = (code: string): string => t(`pool.${code}`);
const dpl = (code: string): string => t(`dashboard.pool.${code}`); // 用于获取 Primary/Backup 翻译

const appStore = useAppStore();

interface ActionStatus {
  saving: boolean;
  restarting: boolean;
}

interface FormState {
  stratumURL: string;
  fallbackStratumURL: string;
  stratumUser: string;
  stratumPassword: string;
  fallbackStratumUser: string;
  fallbackStratumPassword: string;
  stratumTLS: number;
  stratumExtranonceSubscribe: number;
  fallbackStratumTLS: number;
  fallbackStratumExtranonceSubscribe: number;

  poolBURL: string;
  poolBUser: string;
  poolBPassword: string | null;
  poolBTLS: number;
  poolBExtranonceSubscribe: number;
  poolBFbURL: string;
  poolBFbUser: string;
  poolBFbPassword: string | null;
  poolBFbTLS: number;
  dualEnable: number;
  dualRatioA: number;
  dualSliceMs: number;
}

const formState = reactive<FormState>({
  stratumURL: '',
  fallbackStratumURL: '',
  stratumUser: '',
  stratumPassword: '',
  fallbackStratumUser: '',
  fallbackStratumPassword: null,
  stratumTLS: 0,
  stratumExtranonceSubscribe: 0,
  fallbackStratumTLS: 0,
  fallbackStratumExtranonceSubscribe: 0,

  poolBURL: '',
  poolBUser: '',
  poolBPassword: null,
  poolBTLS: 0,
  poolBExtranonceSubscribe: 0,
  poolBFbURL: '',
  poolBFbUser: '',
  poolBFbPassword: null,
  poolBFbTLS: 0,
  dualEnable: 0,
  dualRatioA: 50,
  dualSliceMs: 2000,
});

const cfgsFormRef = ref<FormInstance>();
const minerStatusRef = ref<MinerStatusData>();

/* Which pool B endpoint is actually live. Read from the status the page
 * already fetches, so it reflects the miner rather than the form. */
const poolBUsingFailover = computed(
    () => ((minerStatusRef.value as any)?.poolBUsingFailover ?? 0) === 1);
const actionStatus = reactive<ActionStatus>({saving: false, restarting: false});
const activeTab = ref<string>('primary'); // 控制 Tab 切换

// --- 推荐矿池逻辑 ---

const recommendedPools = computed(() => {
    const model = appStore.statusRaw?.DeviceModel || '';
    let type = 'LTC';
    if (model.startsWith('BC') || model.includes('SHA256') || model.includes('BM13')) {
        type = 'BTC';
    }
    return POOL_MODELS.filter(p => p.type === type);
});

const onPoolSelect = (value: string) => {
    if (activeTab.value === 'primary') {
        formState.stratumURL = value;
    } else {
        formState.fallbackStratumURL = value;
    }
};

// -------------------

watch(
    () => locale.value,
    () => {
      setTimeout(() => {
        cfgsFormRef.value?.validate();
      }, 100);
    },
);

const syncMinerStatus = async () => {
  try {
    const resData = await getMinerStatus('');
    minerStatusRef.value = validData(resData);
  } catch (e) {
    console.log(e);
  }
}

const onPoolBSelect = (value: string) => {
  formState.poolBURL = value;
};

const updateSys = async (values: any) => {
  actionStatus.saving = true;
  try {
    const stratumPasswordStr = (values.stratumPassword === '*****' ? undefined : values.stratumPassword);
    
    // 解析 URL 和 Port
    const parseAddress = (addr: string) => {
        let url = addr.trim();
        let port = 3333; 

        // 移除协议前缀
        if (url.startsWith('stratum+ssl://')) {
            url = url.substring(14);
        } else if (url.startsWith('stratum+tcp://')) {
            url = url.substring(14);
        }

        const parts = url.split(':');
        if (parts.length > 1) {
            const p = parseInt(parts[parts.length - 1]);
            if (!isNaN(p)) {
                port = p;
                // 重组 URL (去除端口部分)
                url = parts.slice(0, parts.length - 1).join(':');
            }
        }
        return { url, port };
    };

    const fallbackStratumPasswordStr =
        (values.fallbackStratumPassword === '*****' ? undefined : values.fallbackStratumPassword);

    const primary = parseAddress(formState.stratumURL);
    const fallback = parseAddress(formState.fallbackStratumURL);

    const formData = {
      stratumURL: primary.url,
      stratumPort: primary.port,
      stratumUser: values.stratumUser,
      stratumPassword: stratumPasswordStr,
      stratumTLS: values.stratumTLS,
      stratumExtranonceSubscribe: values.stratumExtranonceSubscribe,

      fallbackStratumURL: fallback.url,
      fallbackStratumPort: fallback.port,
      fallbackStratumUser: values.fallbackStratumUser,
      /* Same sentinel the other two use. This one was sent raw against
         an initial value of the literal 'password', so opening this page
         and pressing Save replaced a configured fallback password with
         that string without anyone touching the field. */
      fallbackStratumPassword: fallbackStratumPasswordStr,
      fallbackStratumTLS: values.fallbackStratumTLS,
      fallbackStratumExtranonceSubscribe: values.fallbackStratumExtranonceSubscribe,
    }

    // Dual pool. Only send the endpoint when one is set, so saving the page
    // with the tab untouched cannot blank a configured pool B.
    const poolB = parseAddress(formState.poolBURL);
    if (poolB.url) {
      Object.assign(formData, {
        poolBUrl: poolB.url,
        poolBPort: poolB.port,
        poolBUser: formState.poolBUser,
        poolBTLS: formState.poolBTLS,
        poolBExtranonceSubscribe: formState.poolBExtranonceSubscribe,
      });
      if (formState.poolBPassword && formState.poolBPassword !== '*****') {
        Object.assign(formData, { poolBPass: formState.poolBPassword });
      }
    }

    /* Pool B's failover is independent of pool A's, and of pool B's primary --
     * it is cleared by emptying the field, not by clearing the primary. */
    const poolBFb = parseAddress(formState.poolBFbURL);
    Object.assign(formData, {
      poolBFbUrl: poolBFb.url,
      poolBFbPort: poolBFb.url ? poolBFb.port : 0,
      poolBFbUser: formState.poolBFbUser,
      poolBFbTLS: formState.poolBFbTLS,
    });
    if (formState.poolBFbPassword && formState.poolBFbPassword !== '*****') {
      Object.assign(formData, { poolBFbPass: formState.poolBFbPassword });
    }
    Object.assign(formData, {
      dualEnable: formState.dualEnable,
      dualRatioA: formState.dualRatioA,
      dualSliceMs: formState.dualSliceMs,
    });

    await updateSystem('', formData);
    showNotification(t('com.msg_save_success'), 'success')
    /* Pool settings are stored in NVS but reported from the running miner, so
     * until it restarts the API still answers with the old values. Remember
     * what was sent, or leaving this page and coming back shows the previous
     * worker name and reads as a save that silently failed. */
    appStore.markPending(formData);
  } catch (err) {
    showNotification(t('com.msg_save_failed'), 'error')
  } finally {
    actionStatus.saving = false;
  }
}

const restart = async () => {
  if(actionStatus.restarting) {
    return;
  }
  actionStatus.restarting = true;
  try {
    appStore.clearPending();
    await restartMiner('');
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

watch(
    () => minerStatusRef.value,
    (newValue, oldValue) => {
      if (newValue && !oldValue) {
        // Ready state check if needed
      }
    },
);

/*
 * Show settings that were saved but are not live yet.
 *
 * The API reports pool settings from GLOBAL_STATE, which is populated at boot,
 * so a saved change reads back as the old value until the miner restarts.
 * Repopulating the form from that made a successful save look like it had been
 * discarded -- the user renames a worker, saves, comes back, and sees the old
 * name. Both pools behave this way; it is not specific to pool B.
 */
const applyPendingOverFormState = () => {
  const pending = appStore.pendingSettings || {};
  if (!Object.keys(pending).length) {
    return;
  }

  if (pending.poolBUrl) {
    formState.poolBURL = `${pending.poolBUrl}:${pending.poolBPort ?? 3333}`;
  }
  if (pending.poolBFbUrl) {
    formState.poolBFbURL = `${pending.poolBFbUrl}:${pending.poolBFbPort ?? 3333}`;
  }
  if (pending.stratumURL) {
    formState.stratumURL = `${pending.stratumURL}:${pending.stratumPort ?? 3333}`;
  }
  if (pending.fallbackStratumURL) {
    formState.fallbackStratumURL =
        `${pending.fallbackStratumURL}:${pending.fallbackStratumPort ?? 3333}`;
  }

  const direct: Array<keyof typeof formState> = [
    'stratumUser', 'fallbackStratumUser', 'stratumTLS', 'fallbackStratumTLS',
    'stratumExtranonceSubscribe', 'fallbackStratumExtranonceSubscribe',
    'poolBUser', 'poolBTLS', 'poolBExtranonceSubscribe', 'poolBFbUser', 'poolBFbTLS',
    'dualEnable', 'dualRatioA', 'dualSliceMs',
  ] as any;

  direct.forEach((k) => {
    if (pending[k as string] !== undefined) {
      (formState as any)[k] = pending[k as string];
    }
  });
};

onMounted(async () => {
  await syncMinerStatus();
  if (minerStatusRef.value) {
    // 合并显示
    formState.stratumURL = `${minerStatusRef.value.stratumURL}:${minerStatusRef.value.stratumPort}`;
    formState.fallbackStratumURL = `${minerStatusRef.value.fallbackStratumURL}:${minerStatusRef.value.fallbackStratumPort}`;
    
    formState.stratumUser = minerStatusRef.value.stratumUser;
    formState.stratumPassword = '*****';
    formState.fallbackStratumUser = minerStatusRef.value.fallbackStratumUser;
    formState.fallbackStratumPassword = '*****';
    formState.stratumTLS = minerStatusRef.value.stratumTLS ?? 0;
    formState.stratumExtranonceSubscribe = minerStatusRef.value.stratumExtranonceSubscribe ?? 0;
    formState.fallbackStratumTLS = minerStatusRef.value.fallbackStratumTLS ?? 0;
    formState.fallbackStratumExtranonceSubscribe = minerStatusRef.value.fallbackStratumExtranonceSubscribe ?? 0;

    /* What the miner is running. Anything saved since is layered over it
     * below -- see appStore.pendingSettings. */
    const st: any = minerStatusRef.value;
    formState.poolBURL = st.poolBUrl ? `${st.poolBUrl}:${st.poolBPort ?? 3333}` : '';
    formState.poolBUser = st.poolBUser ?? '';
    formState.poolBPassword = st.poolBUrl ? '*****' : '';
    formState.poolBTLS = st.poolBTLS ?? 0;
    formState.poolBExtranonceSubscribe = st.poolBExtranonceSubscribe ?? 0;
    formState.poolBFbURL = st.poolBFbUrl ? `${st.poolBFbUrl}:${st.poolBFbPort ?? 3333}` : '';
    formState.poolBFbUser = st.poolBFbUser ?? '';
    formState.poolBFbPassword = st.poolBFbUrl ? '*****' : '';
    formState.poolBFbTLS = st.poolBFbTLS ?? 0;
    formState.dualEnable = st.dualEnable ?? 0;
    formState.dualRatioA = st.dualRatioA ?? 50;
    formState.dualSliceMs = st.dualSliceMs ?? 2000;

    applyPendingOverFormState();
  }
})
</script>

<template>
  <div>
    <a-card :title="pl('title')" class="card ps-card" style="border: 1px solid var(--surface-border); box-shadow: none;">
      <div class="ps-form-wrap">
      <!-- Saved settings live in NVS but the miner reports what it booted
           with, so say plainly that a restart is still owed. -->
      <a-alert v-if="appStore.needsRestart" type="warning" show-icon
               class="ps-pending-alert" :message="t('com.restart_pending_banner')"/>
      <a-form ref="cfgsFormRef" :wrapper-col="{xs:24, sm: 12}" :model="formState" :hideRequiredMark="true"
              @finish="updateSys">
        
        <a-tabs v-model:activeKey="activeTab" type="card">
          
          <a-tab-pane key="primary" :tab="dpl('primary')">
            <div class="tab-content">
              <!-- 推荐矿池下拉选择 -->
               <a-form-item label="Quick Select">
                  <a-select class="pool-quick-select" placeholder="Select a recommended solo pool" @change="onPoolSelect" style="width: 100%" dropdownClassName="pool-quick-select-dropdown">
                      <a-select-option v-for="pool in recommendedPools" :key="pool.id" :value="pool.value">
                          <div class="pool-option">
                              <img :src="pool.logo" class="pool-logo" />
                              <div class="pool-info">
                                  <div class="pool-name">{{ pool.label }}</div>
                                  <div class="pool-addr">{{ pool.value }}</div>
                              </div>
                          </div>
                      </a-select-option>
                  </a-select>
               </a-form-item>

              <a-form-item :label="pl('stratum_host')" name="stratumURL" :rules="[{required: true, message: t('com.rule_required')}]">
                <a-input v-model:value="formState.stratumURL" placeholder="host:port"></a-input>
              </a-form-item>
              
              <a-form-item :label="pl('stratum_user')" name="stratumUser"
                           :rules="[{required: true, message: t('com.rule_required')}]">
                <a-input v-model:value="formState.stratumUser"></a-input>
              </a-form-item>
              <a-form-item :label="pl('stratum_password')" name="stratumPassword"
                           :rules="[{required: true, message: t('com.rule_required')}]">
                <a-input-password v-model:value="formState.stratumPassword"></a-input-password>
              </a-form-item>

              <a-form-item v-if="appStore.currentModelConfig?.support_tls" name="stratumTLS" :colon="false" style="margin-bottom: 0;">
              <div class="ps-switch-row">
                <span class="ps-switch-label">{{ pl('stratum_tls') }}</span>
                <a-switch :checked="formState.stratumTLS === 1" @change="(checked: boolean) => formState.stratumTLS = checked ? 1 : 0" />
              </div>
              </a-form-item>
              <a-form-item v-if="appStore.currentModelConfig?.support_xnsub" name="stratumExtranonceSubscribe" :colon="false" style="margin-bottom: 0;">
              <div class="ps-switch-row">
                <span class="ps-switch-label">{{ pl('stratum_xnsub') }}</span>
                <a-switch :checked="formState.stratumExtranonceSubscribe === 1" @change="(checked: boolean) => formState.stratumExtranonceSubscribe = checked ? 1 : 0" />
              </div>
              </a-form-item>
            </div>
          </a-tab-pane>

          <a-tab-pane key="fallback" :tab="dpl('fallback')">
            <div class="tab-content">
               <!-- 推荐矿池下拉选择 (Fallback) -->
               <a-form-item label="Quick Select">
                  <a-select class="pool-quick-select" placeholder="Select a recommended solo pool" @change="onPoolSelect" style="width: 100%" dropdownClassName="pool-quick-select-dropdown">
                      <a-select-option v-for="pool in recommendedPools" :key="pool.id" :value="pool.value">
                          <div class="pool-option">
                              <img :src="pool.logo" class="pool-logo" />
                              <div class="pool-info">
                                  <div class="pool-name">{{ pool.label }}</div>
                                  <div class="pool-addr">{{ pool.value }}</div>
                              </div>
                          </div>
                      </a-select-option>
                  </a-select>
               </a-form-item>

              <a-form-item :label="pl('fallback_stratum_host')" name="fallbackStratumURL">
                <a-input v-model:value="formState.fallbackStratumURL" placeholder="host:port"></a-input>
              </a-form-item>
              
              <a-form-item :label="pl('fallback_stratum_user')" name="fallbackStratumUser">
                <a-input v-model:value="formState.fallbackStratumUser"></a-input>
              </a-form-item>
              <a-form-item :label="pl('fallback_stratum_password')" name="fallbackStratumPassword">
                <a-input-password v-model:value="formState.fallbackStratumPassword"></a-input-password>
              </a-form-item>

              <a-form-item v-if="appStore.currentModelConfig?.support_tls" name="fallbackStratumTLS" :colon="false" style="margin-bottom: 0;">
              <div class="ps-switch-row">
                <span class="ps-switch-label">{{ pl('stratum_tls') }}</span>
                <a-switch :checked="formState.fallbackStratumTLS === 1" @change="(checked: boolean) => formState.fallbackStratumTLS = checked ? 1 : 0" />
              </div>
              </a-form-item>
              <a-form-item v-if="appStore.currentModelConfig?.support_xnsub" name="fallbackStratumExtranonceSubscribe" :colon="false" style="margin-bottom: 0;">
              <div class="ps-switch-row">
                <span class="ps-switch-label">{{ pl('stratum_xnsub') }}</span>
                <a-switch :checked="formState.fallbackStratumExtranonceSubscribe === 1" @change="(checked: boolean) => formState.fallbackStratumExtranonceSubscribe = checked ? 1 : 0" />
              </div>
              </a-form-item>
            </div>
          </a-tab-pane>

          <a-tab-pane key="dual" :tab="dpl('dual')">
            <div class="tab-content">

              <div class="dual-intro">
                <div class="dual-intro-title">{{ dpl('dual_intro_title') }}</div>
                <div class="dual-intro-body">{{ dpl('dual_intro_body') }}</div>
              </div>

              <a-form-item :colon="false" style="margin-bottom: 1.25rem;">
                <div class="ps-switch-row">
                  <span class="ps-switch-label">{{ dpl('dual_enable') }}</span>
                  <a-switch :checked="formState.dualEnable === 1"
                            @change="(checked: boolean) => formState.dualEnable = checked ? 1 : 0" />
                </div>
              </a-form-item>

              <a-form-item label="Quick Select">
                <a-select class="pool-quick-select" placeholder="Select a recommended solo pool"
                          @change="onPoolBSelect" style="width: 100%"
                          dropdownClassName="pool-quick-select-dropdown">
                  <a-select-option v-for="pool in recommendedPools" :key="pool.id" :value="pool.value">
                    <div class="pool-option">
                      <img :src="pool.logo" class="pool-logo" />
                      <div class="pool-info">
                        <div class="pool-name">{{ pool.label }}</div>
                        <div class="pool-addr">{{ pool.value }}</div>
                      </div>
                    </div>
                  </a-select-option>
                </a-select>
              </a-form-item>

              <a-form-item :label="pl('stratum_host')" name="poolBURL">
                <a-input v-model:value="formState.poolBURL" placeholder="host:port"></a-input>
              </a-form-item>
              <a-form-item :label="pl('stratum_user')" name="poolBUser">
                <a-input v-model:value="formState.poolBUser"></a-input>
              </a-form-item>
              <a-form-item :label="pl('stratum_password')" name="poolBPassword">
                <a-input-password v-model:value="formState.poolBPassword"></a-input-password>
              </a-form-item>

              <a-form-item v-if="appStore.currentModelConfig?.support_tls" name="poolBTLS" :colon="false" style="margin-bottom: 0;">
                <div class="ps-switch-row">
                  <span class="ps-switch-label">{{ pl('stratum_tls') }}</span>
                  <a-switch :checked="formState.poolBTLS === 1"
                            @change="(checked: boolean) => formState.poolBTLS = checked ? 1 : 0" />
                </div>
              </a-form-item>
              <!-- Pool B could always receive an extranonce rotation; it just
                   never asked for one, and there was no way to ask. -->
              <a-form-item v-if="appStore.currentModelConfig?.support_xnsub" name="poolBExtranonceSubscribe" :colon="false" style="margin-bottom: 0;">
                <div class="ps-switch-row">
                  <span class="ps-switch-label">{{ pl('stratum_xnsub') }}</span>
                  <a-switch :checked="formState.poolBExtranonceSubscribe === 1"
                            @change="(checked: boolean) => formState.poolBExtranonceSubscribe = checked ? 1 : 0" />
                </div>
              </a-form-item>

              <div class="dual-fb">
                <div class="dual-fb-head">
                  <span class="ps-switch-label">{{ dpl('dual_fb') }}</span>
                  <span v-if="formState.poolBFbURL"
                        class="dual-fb-state"
                        :class="{ 'is-live': poolBUsingFailover }">
                    {{ poolBUsingFailover ? dpl('dual_fb_active') : dpl('dual_fb_primary') }}
                  </span>
                </div>
                <div class="dual-hint" style="margin-bottom: .85rem;">{{ dpl('dual_fb_hint') }}</div>

                <a-form-item :label="pl('stratum_host')" name="poolBFbURL">
                  <a-input v-model:value="formState.poolBFbURL" placeholder="host:port"></a-input>
                </a-form-item>
                <a-form-item :label="pl('stratum_user')" name="poolBFbUser">
                  <a-input v-model:value="formState.poolBFbUser"></a-input>
                </a-form-item>
                <a-form-item :label="pl('stratum_password')" name="poolBFbPassword">
                  <a-input-password v-model:value="formState.poolBFbPassword"></a-input-password>
                </a-form-item>
                <a-form-item v-if="appStore.currentModelConfig?.support_tls" name="poolBFbTLS"
                             :colon="false" style="margin-bottom: 0;">
                  <div class="ps-switch-row">
                    <span class="ps-switch-label">{{ pl('stratum_tls') }}</span>
                    <a-switch :checked="formState.poolBFbTLS === 1"
                              @change="(checked: boolean) => formState.poolBFbTLS = checked ? 1 : 0" />
                  </div>
                </a-form-item>
              </div>

              <div class="dual-split">
                <div class="dual-split-head">
                  <span class="ps-switch-label">{{ dpl('dual_split') }}</span>
                  <span class="dual-split-value">
                    <span class="pool-a">A {{ formState.dualRatioA }}%</span>
                    <span class="sep">/</span>
                    <span class="pool-b">B {{ 100 - formState.dualRatioA }}%</span>
                  </span>
                </div>
                <a-slider v-model:value="formState.dualRatioA" :min="0" :max="100" :step="5"
                          :tip-formatter="(v: number) => `A ${v}% / B ${100 - v}%`" />
                <div class="dual-hint">{{ dpl('dual_split_hint') }}</div>
              </div>

              <a-form-item :label="dpl('dual_slice')" name="dualSliceMs">
                <a-input-number v-model:value="formState.dualSliceMs" :min="100" :max="60000" :step="100" />
              </a-form-item>
              <div class="dual-hint">{{ dpl('dual_slice_hint') }}</div>

            </div>
          </a-tab-pane>

        </a-tabs>

        <div class="ps-action">
          <a-button :loading="actionStatus.saving" class="ps-action-btn" type="primary" html-type="submit">
            {{ t('com.save') }}
          </a-button>
          <a-button :loading="actionStatus.restarting" class="ps-action-btn" :disabled="!appStore.needsRestart" type="primary"
                    @click="restart">{{ appStore.needsRestart ? t('com.restart_pending') : t('com.restart') }}
          </a-button>
          <div class="ps-restart-hint">{{ t('com.restart_hint') }}</div>
        </div>
      </a-form>
      </div>
    </a-card>
  </div>
</template>

<style scoped lang="scss">
.ps-card {
  margin-top: 2rem;
  margin-bottom: 2rem;
}

.ps-title {
  font-size: 1.5rem;
  font-weight: 500;
  margin-bottom: 0.5rem;
}

.ps-form-wrap {
  margin-top: 1rem;
}

.tab-content {
  padding-top: 20px;
}

/* ---- dual pool ---- */
.dual-fb {
  margin: 1.5rem 0 .5rem;
  padding-top: 1.25rem;
  border-top: 1px solid var(--surface-border);
}

.dual-fb-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: .75rem;
  margin-bottom: .35rem;
}

.dual-fb-state {
  font-size: .78rem;
  font-weight: 600;
  letter-spacing: .02em;
  padding: .15rem .6rem;
  border-radius: 999px;
  border: 1px solid var(--surface-border);
  color: var(--text-color-secondary);
  white-space: nowrap;
}

.dual-fb-state.is-live {
  color: var(--primary-color);
  border-color: var(--primary-color);
}

.dual-intro {
  border: 1px solid var(--surface-border);
  border-left: 3px solid var(--primary-color);
  border-radius: var(--card-border-radius);
  padding: 0.85rem 1.1rem;
  margin-bottom: 1.5rem;
  background: var(--surface-overlay);
}

.dual-intro-title {
  font-weight: 600;
  margin-bottom: 0.25rem;
  color: var(--text-color);
}

.dual-intro-body {
  color: var(--text-color-secondary);
  font-size: 0.9rem;
  line-height: 1.5;
}

.dual-split {
  border-top: 1px solid var(--surface-border);
  margin-top: 1.5rem;
  padding-top: 1.25rem;
}

.dual-split-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 0.25rem;
}

.dual-split-value {
  font-variant-numeric: tabular-nums;
  font-weight: 600;

  .pool-a { color: var(--primary-color); }
  .pool-b { color: #ffb81c; }
  .sep { color: var(--text-color-secondary); margin: 0 0.4rem; }
}

.dual-hint {
  color: var(--text-color-secondary);
  font-size: 0.85rem;
  line-height: 1.5;
  margin-bottom: 1.25rem;
}

:deep(.ant-form-item .ant-form-item-label >label) {
  color: var(--text-color);
  font-size: 1rem;
  font-weight: 500;
  text-align: left;
  width: 15rem;
}

:deep(.ant-input) {
  height: 35px;
}

:deep(.ant-input-number) {
  width: 100%;
  height: 35px;
  line-height: 35px;
}

:deep(.ant-input-affix-wrapper.ant-input-password ) {
  padding-top: 0 !important;
  padding-bottom: 0 !important;;
}

.ps-action {
  margin-top: 2rem;
  border-top: 1px solid var(--surface-border);
  padding-top: 1.5rem;
  margin-bottom: 1rem;
}

.ps-action-btn {
  margin-right: 10px;
  margin-bottom: 10px;
}

.ps-pending-alert {
  margin-bottom: 16px;
}

.ps-restart-hint {
  font-size: 0.9rem;
}

/* Tabs 样式修复：高亮选中项，适配暗黑模式 */
/* The option contents render inside a dropdown that Ant Design portals to
 * the end of the body, so a scoped rule cannot reach them. They are styled in
 * styles/layout/_antd-fixes.scss instead. */

:deep(.pool-quick-select .ant-select-selector) {
  height: auto !important;
  min-height: 50px;
  padding-top: 4px;
  padding-bottom: 4px;
  display: flex !important;
  align-items: center;
}

:deep(.pool-quick-select .ant-select-selection-item) {
  display: flex !important;
  align-items: center;
  line-height: normal !important;
}

.ps-switch-row {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 15px;
}

.ps-switch-label {
  font-size: 1rem;
  font-weight: 500;
  color: var(--text-color);
  min-width: 15rem;
}


</style>
