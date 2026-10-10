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

  secondary: SecondaryPool[];
  poolCount: number;
  dualEnable: number;
  dualRatioA: number;
  dualSliceMs: number;
}

/*
 * One pool that is not pool A. Three of them, for B, C and D -- the same
 * shape, because the miner treats them the same way: every configured pool
 * stays connected and the chain is time-sliced between them.
 */
interface SecondaryPool {
  URL: string;
  User: string;
  Password: string | null;
  TLS: number;
  ExtranonceSubscribe: number;
  FbURL: string;
  FbUser: string;
  FbPassword: string | null;
  FbTLS: number;
  Weight: number;
}

const POOL_LETTERS = ['B', 'C', 'D'];
const blankSecondary = (): SecondaryPool => ({
  URL: '', User: '', Password: null, TLS: 0, ExtranonceSubscribe: 0,
  FbURL: '', FbUser: '', FbPassword: null, FbTLS: 0, Weight: 0,
});

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

  secondary: [blankSecondary(), blankSecondary(), blankSecondary()],
  poolCount: 2,
  dualEnable: 0,
  dualRatioA: 50,
  dualSliceMs: 2000,
});

const cfgsFormRef = ref<FormInstance>();
const minerStatusRef = ref<MinerStatusData>();

/* Which endpoint each secondary pool is actually on. Read from the status the
 * page already fetches, so it reflects the miner rather than the form. */
const usingFailover = (i: number) => (((minerStatusRef.value as any)
    ?.[`pool${POOL_LETTERS[i]}UsingFailover`] ?? 0) === 1);

/*
 * Pools beyond B need four ASICs. That is a product decision rather than a
 * technical one -- work is time-sliced across the whole chain rather than
 * aimed at a chip, so pool count and ASIC count are independent -- but the
 * miner enforces it, so the page must not offer what would be ignored.
 */
const asicCount = computed(() => Number(appStore.statusRaw?.asicCount ?? 0));
const canQuad = computed(() => asicCount.value >= 4);

/* How many secondary pools are on show: one for Dual, three for Quad. */
const shownPools = computed(() => Math.max(0, formState.poolCount - 1));

/*
 * The split the miner will actually run, mirroring build_pool_weights() in
 * create_jobs_task.c: pool A takes its percentage, and whatever is left is
 * divided among the configured pools -- by their own weights where any have
 * been set, evenly where none have. Shown live, because three weights and a
 * percentage are hard to hold in your head.
 */
const splitPreview = computed(() => {
  const active = formState.secondary
      .slice(0, shownPools.value)
      .map((p, i) => ({ letter: POOL_LETTERS[i], url: p.URL.trim(), weight: Number(p.Weight) || 0 }))
      .filter((p) => p.url !== '');

  if (!formState.dualEnable || active.length === 0) {
    return [{ letter: 'A', pct: 100 }];
  }

  const ratioA = Math.min(100, Math.max(0, Number(formState.dualRatioA) || 0));
  const stated = active.reduce((sum, p) => sum + p.weight, 0);
  const rest = 100 - ratioA;

  const weights = [
    { letter: 'A', weight: ratioA },
    ...active.map((p) => ({
      letter: p.letter,
      weight: stated > 0 ? p.weight : Math.floor(rest / active.length),
    })),
  ];
  const total = weights.reduce((sum, w) => sum + w.weight, 0);
  if (total <= 0) {
    return [{ letter: 'A', pct: 100 }];
  }
  return weights.map((w) => ({ letter: w.letter, pct: Math.round((w.weight / total) * 1000) / 10 }));
});

/*
 * Reducing the pool count clears the pools it drops.
 *
 * The miner has no notion of a "mode": it connects every pool that has a
 * host, so a pool left filled in but hidden here would quietly keep mining.
 * Clearing the host is what removes it, and doing that here keeps the page
 * showing what the miner will do.
 */
const onPoolCountChange = (next: number) => {
  for (let i = next - 1; i < formState.secondary.length; i++) {
    formState.secondary[i] = blankSecondary();
  }
  formState.poolCount = next;
  formState.dualEnable = next > 1 ? 1 : 0;
};
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

/* Which secondary pool a Quick Select dropdown is filling in. Set on focus,
 * because the select itself only reports the value chosen, not which of the
 * three rows it belongs to. */
const secondaryQuickSelectIndex = ref<number>(0);

const onSecondaryPoolSelect = (value: string) => {
  formState.secondary[secondaryQuickSelectIndex.value].URL = value;
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

    /*
     * Every secondary pool. Pools past the chosen count were blanked by
     * onPoolCountChange, and a blank host is sent as a blank -- that is what
     * removes a pool from the miner, which otherwise connects anything with a
     * host in it.
     */
    formState.secondary.forEach((pool, i) => {
      const L = POOL_LETTERS[i];
      const primary = parseAddress(pool.URL);

      Object.assign(formData, {
        [`pool${L}Url`]: primary.url,
        [`pool${L}Port`]: primary.url ? primary.port : 3333,
        [`pool${L}User`]: pool.User,
        [`pool${L}TLS`]: pool.TLS,
        [`pool${L}ExtranonceSubscribe`]: pool.ExtranonceSubscribe,
        [`pool${L}Weight`]: Number(pool.Weight) || 0,
      });
      /* Only sent when the user typed one: the API never hands a password
       * back, so an untouched field holds the '*****' placeholder and
       * sending that would overwrite the stored secret with five asterisks. */
      if (pool.Password && pool.Password !== '*****') {
        Object.assign(formData, { [`pool${L}Pass`]: pool.Password });
      }

      /* Each pool's failover is independent of pool A's and of its own
       * primary -- it is cleared by emptying the field, not by clearing the
       * primary. */
      const fb = parseAddress(pool.FbURL);
      Object.assign(formData, {
        [`pool${L}FbUrl`]: fb.url,
        [`pool${L}FbPort`]: fb.url ? fb.port : 0,
        [`pool${L}FbUser`]: pool.FbUser,
        [`pool${L}FbTLS`]: pool.FbTLS,
      });
      if (pool.FbPassword && pool.FbPassword !== '*****') {
        Object.assign(formData, { [`pool${L}FbPass`]: pool.FbPassword });
      }
    });

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

  POOL_LETTERS.forEach((L, i) => {
    const pool = formState.secondary[i];
    if (pending[`pool${L}Url`] !== undefined) {
      pool.URL = pending[`pool${L}Url`]
          ? `${pending[`pool${L}Url`]}:${pending[`pool${L}Port`] ?? 3333}` : '';
    }
    if (pending[`pool${L}FbUrl`] !== undefined) {
      pool.FbURL = pending[`pool${L}FbUrl`]
          ? `${pending[`pool${L}FbUrl`]}:${pending[`pool${L}FbPort`] ?? 3333}` : '';
    }
    (['User', 'TLS', 'ExtranonceSubscribe', 'FbUser', 'FbTLS', 'Weight'] as const)
        .forEach((field) => {
          if (pending[`pool${L}${field}`] !== undefined) {
            (pool as any)[field] = pending[`pool${L}${field}`];
          }
        });
  });
  formState.poolCount = 1 + formState.secondary.filter((p) => p.URL !== '').length;

  if (pending.stratumURL) {
    formState.stratumURL = `${pending.stratumURL}:${pending.stratumPort ?? 3333}`;
  }
  if (pending.fallbackStratumURL) {
    formState.fallbackStratumURL =
        `${pending.fallbackStratumURL}:${pending.fallbackStratumPort ?? 3333}`;
  }

  /* The secondary pools are handled above; these are pool A's and the
   * settings that apply to the split as a whole. */
  const direct: Array<keyof typeof formState> = [
    'stratumUser', 'fallbackStratumUser', 'stratumTLS', 'fallbackStratumTLS',
    'stratumExtranonceSubscribe', 'fallbackStratumExtranonceSubscribe',
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
    POOL_LETTERS.forEach((L, i) => {
      const pool = formState.secondary[i];
      pool.URL = st[`pool${L}Url`] ? `${st[`pool${L}Url`]}:${st[`pool${L}Port`] ?? 3333}` : '';
      pool.User = st[`pool${L}User`] ?? '';
      /* The API never returns a password. A placeholder stands in so the
       * field is not blank for a pool that has one, and updateSys only sends
       * it back when the user has replaced it. */
      pool.Password = st[`pool${L}Url`] ? '*****' : '';
      pool.TLS = st[`pool${L}TLS`] ?? 0;
      pool.ExtranonceSubscribe = st[`pool${L}ExtranonceSubscribe`] ?? 0;
      pool.FbURL = st[`pool${L}FbUrl`] ? `${st[`pool${L}FbUrl`]}:${st[`pool${L}FbPort`] ?? 3333}` : '';
      pool.FbUser = st[`pool${L}FbUser`] ?? '';
      pool.FbPassword = st[`pool${L}FbUrl`] ? '*****' : '';
      pool.FbTLS = st[`pool${L}FbTLS`] ?? 0;
      pool.Weight = st[`pool${L}Weight`] ?? 0;
    });

    /* The mode is read from the configuration rather than stored: a pool is
     * in play when it has a host, so counting them is what the miner itself
     * does. One configured secondary shows as Dual, three as Quad. */
    formState.poolCount = 1 + formState.secondary.filter((p) => p.URL !== '').length;
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

              <!-- How many pools to divide the chain between. The miner has
                   no stored "mode": a pool is in play when it has a host, so
                   this adds and removes panels and the hosts decide. -->
              <div class="pool-mode">
                <div class="pool-mode-head">
                  <span class="ps-switch-label">{{ dpl('mode') }}</span>
                </div>
                <a-radio-group :value="formState.poolCount" button-style="solid"
                               @change="(e: any) => onPoolCountChange(e.target.value)">
                  <a-radio-button :value="1">{{ dpl('mode_single') }}</a-radio-button>
                  <a-radio-button :value="2">{{ dpl('mode_dual') }}</a-radio-button>
                  <a-radio-button :value="3" :disabled="!canQuad">{{ dpl('mode_triple') }}</a-radio-button>
                  <a-radio-button :value="4" :disabled="!canQuad">{{ dpl('mode_quad') }}</a-radio-button>
                </a-radio-group>
                <div class="dual-hint" style="margin-top: .6rem;">
                  {{ canQuad ? dpl('mode_hint') : dpl('mode_needs_four') }}
                </div>
              </div>

              <!-- One panel per secondary pool. Identical, because the miner
                   treats them identically. -->
              <div v-for="(pool, i) in formState.secondary.slice(0, shownPools)" :key="i"
                   class="pool-panel">
                <div class="pool-panel-head">
                  <span class="pool-panel-title">{{ dpl('pool_named').replace('%s', POOL_LETTERS[i]) }}</span>
                  <span v-if="pool.FbURL" class="dual-fb-state" :class="{ 'is-live': usingFailover(i) }">
                    {{ usingFailover(i) ? dpl('dual_fb_active') : dpl('dual_fb_primary') }}
                  </span>
                </div>

                <a-form-item label="Quick Select">
                  <a-select class="pool-quick-select" placeholder="Select a recommended solo pool"
                            @focus="secondaryQuickSelectIndex = i"
                            @change="onSecondaryPoolSelect" style="width: 100%"
                            dropdownClassName="pool-quick-select-dropdown">
                    <a-select-option v-for="p in recommendedPools" :key="p.id" :value="p.value">
                      <div class="pool-option">
                        <img :src="p.logo" class="pool-logo" />
                        <div class="pool-info">
                          <div class="pool-name">{{ p.label }}</div>
                          <div class="pool-addr">{{ p.value }}</div>
                        </div>
                      </div>
                    </a-select-option>
                  </a-select>
                </a-form-item>

                <a-form-item :label="pl('stratum_host')">
                  <a-input v-model:value="pool.URL" placeholder="host:port"></a-input>
                </a-form-item>
                <a-form-item :label="pl('stratum_user')">
                  <a-input v-model:value="pool.User"></a-input>
                </a-form-item>
                <a-form-item :label="pl('stratum_password')">
                  <a-input-password v-model:value="pool.Password"></a-input-password>
                </a-form-item>

                <!-- Only shown with more than two pools. With two, the A/B
                     slider below says the same thing more plainly. -->
                <a-form-item v-if="shownPools > 1" :label="dpl('weight')">
                  <a-input-number v-model:value="pool.Weight" :min="0" :max="100" :step="5" />
                  <span class="dual-hint" style="margin-left: .75rem;">{{ dpl('weight_hint') }}</span>
                </a-form-item>

                <a-form-item v-if="appStore.currentModelConfig?.support_tls" :colon="false" style="margin-bottom: 0;">
                  <div class="ps-switch-row">
                    <span class="ps-switch-label">{{ pl('stratum_tls') }}</span>
                    <a-switch :checked="pool.TLS === 1"
                              @change="(checked: boolean) => pool.TLS = checked ? 1 : 0" />
                  </div>
                </a-form-item>
                <!-- A secondary pool could always receive an extranonce
                     rotation; it just never asked for one, and there was no
                     way to ask. -->
                <a-form-item v-if="appStore.currentModelConfig?.support_xnsub" :colon="false" style="margin-bottom: 0;">
                  <div class="ps-switch-row">
                    <span class="ps-switch-label">{{ pl('stratum_xnsub') }}</span>
                    <a-switch :checked="pool.ExtranonceSubscribe === 1"
                              @change="(checked: boolean) => pool.ExtranonceSubscribe = checked ? 1 : 0" />
                  </div>
                </a-form-item>

                <div class="dual-fb">
                  <div class="dual-fb-head">
                    <span class="ps-switch-label">{{ dpl('dual_fb') }}</span>
                  </div>
                  <div class="dual-hint" style="margin-bottom: .85rem;">{{ dpl('dual_fb_hint') }}</div>

                  <a-form-item :label="pl('stratum_host')">
                    <a-input v-model:value="pool.FbURL" placeholder="host:port"></a-input>
                  </a-form-item>
                  <a-form-item :label="pl('stratum_user')">
                    <a-input v-model:value="pool.FbUser"></a-input>
                  </a-form-item>
                  <a-form-item :label="pl('stratum_password')">
                    <a-input-password v-model:value="pool.FbPassword"></a-input-password>
                  </a-form-item>
                  <a-form-item v-if="appStore.currentModelConfig?.support_tls" :colon="false" style="margin-bottom: 0;">
                    <div class="ps-switch-row">
                      <span class="ps-switch-label">{{ pl('stratum_tls') }}</span>
                      <a-switch :checked="pool.FbTLS === 1"
                                @change="(checked: boolean) => pool.FbTLS = checked ? 1 : 0" />
                    </div>
                  </a-form-item>
                </div>
              </div>

              <div v-if="formState.dualEnable === 1" class="dual-split">
                <div class="dual-split-head">
                  <span class="ps-switch-label">{{ dpl('dual_split') }}</span>
                  <span class="dual-split-value">
                    <template v-for="(s, idx) in splitPreview" :key="s.letter">
                      <span class="sep" v-if="idx > 0">/</span>
                      <span :class="s.letter === 'A' ? 'pool-a' : 'pool-b'">{{ s.letter }} {{ s.pct }}%</span>
                    </template>
                  </span>
                </div>
                <a-slider v-model:value="formState.dualRatioA" :min="0" :max="100" :step="5"
                          :tip-formatter="(v: number) => `A ${v}%`" />
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
/* The pool-count selector, above the panels it adds and removes. */
.pool-mode {
  margin-bottom: 1.5rem;
}

.pool-mode-head {
  margin-bottom: .5rem;
}

/*
 * One per secondary pool. Boxed rather than separated by a rule, because with
 * three of them on the page a reader needs to see at a glance where pool C
 * ends and pool D begins -- and each one carries its own failover block,
 * which is itself rule-separated.
 */
.pool-panel {
  border: 1px solid var(--surface-border);
  border-radius: var(--card-border-radius);
  padding: 1rem 1.1rem .4rem;
  margin-bottom: 1.25rem;
}

.pool-panel-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: .75rem;
  margin-bottom: .9rem;
}

.pool-panel-title {
  font-weight: 600;
  letter-spacing: .02em;
}

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
