<script setup lang="ts">
import { reactive, onMounted, ref } from "vue";
import { useI18n } from "vue-i18n";
import { useAppStore } from "@/store";
import { updateSystem, getMinerStatus } from "@/api";
import { showNotification, getUrl, validData } from "@/util/utils.ts";
import { useRouter } from "vue-router";
import { LogoutOutlined } from "@ant-design/icons-vue";

const { t } = useI18n();
const appStore = useAppStore();
const router = useRouter();

const formState = reactive({
  apiPassword: '',
  confirmPassword: ''
});

// Reflects the device's actual state, reported by /api/system/info.
const authEnabled = ref(false);
const loading = ref(false);
const error = ref('');

const handleLogout = () => {
    appStore.logout();
    router.push('/login');
};

/*
 * Every string in this component was written in English in the source.
 *
 * Which left the whole of it untranslated on a Chinese interface -- and this
 * is the page that tells an owner their miner has no password, and asks them
 * to confirm turning authentication off. Those are the worst two strings in
 * the interface to leave in a language the reader may not have, because
 * misreading either one leaves a miner open on their network. The rest of
 * the interface has gone through i18n from the start; this was never wired
 * up.
 */
const sec = (code: string): string => t(`security.${code}`);

const updateSecurity = async () => {
    error.value = '';

    if (formState.apiPassword !== formState.confirmPassword) {
        error.value = sec('err_mismatch');
        return;
    }

    if (formState.apiPassword !== '' && formState.apiPassword.length < 8) {
        error.value = sec('err_short');
        return;
    }

    /* Two keys joined here rather than one carrying a blank line, so no
     * translation has to reproduce the paragraph break. */
    if (formState.apiPassword === '' &&
        !window.confirm(`${sec('clear_warn')}\n\n${sec('clear_confirm')}`)) {
        return;
    }

    loading.value = true;
    try {
        await updateSystem(getUrl(window.location.host), { apiPassword: formState.apiPassword });
        showNotification(t('com.msg_save_success'), 'success');
        // The change revoked this session's token along with the rest.
        appStore.logout();
        router.push('/login');
    } catch (e) {
        showNotification(t('com.msg_save_failed'), 'error');
    } finally {
        loading.value = false;
    }
};

const syncStatus = async () => {
    try {
        const res = await getMinerStatus(getUrl(window.location.host));
        const data = validData(res);
        if (data) {
            authEnabled.value = !!data.authEnabled;
        }
    } catch(e) {
        console.error(e);
    }
}

onMounted(() => {
    syncStatus();
});
</script>

<template>
  <div class="card security-card">
    <div class="security-header">
        <div class="security-title">{{ sec('title') }}</div>
        <a-button type="primary" danger @click="handleLogout" class="logout-btn">
            <template #icon><LogoutOutlined /></template>
            {{ sec('logout') }}
        </a-button>
    </div>
    
    <a-alert
      v-if="!authEnabled"
      type="warning"
      show-icon
      class="auth-warning"
      :message="sec('no_pass_title')"
      :description="sec('no_pass_body')"
    />

    <a-form :model="formState" layout="vertical" @finish="updateSecurity">
      <a-form-item :label="sec('api_password')">
         <a-input-password v-model:value="formState.apiPassword"
                           :placeholder="sec('api_password_ph')" />
      </a-form-item>

      <a-form-item :label="sec('confirm')">
         <a-input-password v-model:value="formState.confirmPassword"
                           :placeholder="sec('confirm_ph')" />
      </a-form-item>

      <div v-if="error" class="error-message">{{ error }}</div>

      <a-button type="primary" html-type="submit" :loading="loading">
        {{ sec('save') }}
      </a-button>

      <p class="auth-note">{{ sec('note') }}</p>
    </a-form>

  </div>
</template>

<style scoped lang="scss">
.auth-warning {
    margin-bottom: 1.5rem;
}

.error-message {
    color: var(--ant-error-color, #ff4d4f);
    margin-bottom: 1rem;
}

.auth-note {
    margin-top: 1rem;
    font-size: 0.85rem;
    opacity: 0.75;
}

.security-card {
    padding: 2.2rem 1.2rem; 
    margin-top: 2rem;
}

.security-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 1.5rem;
}

.security-title {
    font-size: 1.5rem;
    font-weight: 500;
    color: var(--text-color);
    margin-bottom: 0;
}

.logout-btn {
    font-size: 1rem;
    display: flex;
    align-items: center;
}

:deep(.auth-switch.ant-switch-checked) {
    background-color: var(--ant-primary-color) !important; 
}
</style>
