<script setup lang="ts">
import axios, {AxiosProgressEvent} from "axios";
import {PlusOutlined, ReloadOutlined, DownloadOutlined, ArrowUpOutlined, InfoCircleOutlined} from "@ant-design/icons-vue";
import SettingsEdit from "@/components/SettingsEdit.vue";
import SecuritySettings from "@/components/SecuritySettings.vue";
import {onMounted, ref} from "vue";
import {MinerStatusData} from "@/api/type.ts";
import {useI18n} from "vue-i18n";
import { WEB_VERSION, UPDATE_REPO_WWW, UPDATE_REPO_APP } from "../util/const.ts";
import {useAppStore} from "@/store";
import {getMinerStatus, URL as APP_URL} from "@/api";
import {showNotificationLoading, validData} from "@/util/utils.ts";

const {t} = useI18n();
const appStore = useAppStore(); 

const sel = (code: string): string => {
  return t(`settings.${code}`);
}

const minerStatusRef = ref<MinerStatusData>();

const fileInputRef = ref<HTMLInputElement | null>(null);

const updating = ref<boolean>(false);
const uploadProgress = ref<number>(0);
const uploadStatusMessage = ref<string>("");
const uploadFailed = ref<boolean>(false); 

// --- OTA Check Refs ---
const checkOtaStatus = ref<string>('');
const otaChecking = ref<boolean>(false);
const remoteWebUrl = ref<string | null>(null);
const remoteWebReleaseUrl = ref<string | null>(null);
const remoteWebVersion = ref<string>('');
const remoteWebNotes = ref<string>('');
const remoteFwUrl = ref<string | null>(null);
const remoteFwReleaseUrl = ref<string | null>(null);
const remoteFwVersion = ref<string>('');
const remoteFwNotes = ref<string>('');

const chooseFile = () => {
  if (updating.value) {
    return;
  }
  uploadProgress.value = 0;
  uploadStatusMessage.value = ""; 
  uploadFailed.value = false; 
  
  if (fileInputRef.value) {
    fileInputRef.value.value = '';
  }

  fileInputRef.value?.click();
};

const uploadFile = async (event: any) => {
  const files: FileList | null = (event.target as HTMLInputElement).files;
  const file: File | null = files ? files[0] : null;
  if (!file) {
    return;
  }
  await processFirmwareFile(file);
};

// Extracted core logic so it can be called by both manual upload and remote OTA
const processFirmwareFile = async (file: File | Blob) => {
  if (updating.value) return;
  updating.value = true;
  
  appStore.setInfo({ isPollingPaused: true });
  uploadFailed.value = false; 
  uploadStatusMessage.value = t('settings.upload_status_detecting') + '\n';

  const reader = new FileReader();
  reader.onload = async (eventReader: any) => {
    const firstByte = new Uint8Array(eventReader.target.result)[0];
    let uploadUrl = "";
    let isFirmware = false;
    
    const verStr = minerStatusRef.value?.version || '0';
    const verPrefix = verStr.split(' ')[0];
    const verParts = verPrefix.split('.').map(Number);
    const major = verParts[0] || 0;
    
    // 判定是否为加密模式：>= 2.0
    const isEncryptedMode = major >= 2;
    
    if (isEncryptedMode) {
      if (firstByte === 0xAA) {
        uploadStatusMessage.value += t('settings.upload_status_firmware') + '\n';
        uploadUrl = APP_URL.performOTAUpdate;
        isFirmware = true;
      } else if (firstByte === 0x55) {
        uploadStatusMessage.value += t('settings.upload_status_website') + '\n';
        uploadUrl = APP_URL.performWWWOTAUpdate;
        isFirmware = false;
      } else {
        uploadStatusMessage.value += t('settings.upload_status_invalid_firmware') + '\n';
        uploadFailed.value = true;
        updating.value = false;
        appStore.setInfo({ isPollingPaused: false });
        return;
      }
    } else {
      if (firstByte === 0xE9) {
        uploadStatusMessage.value += t('settings.upload_status_firmware') + '\n';
        uploadUrl = APP_URL.performOTAUpdate;
        isFirmware = true;
      } else {
        uploadStatusMessage.value += t('settings.upload_status_website') + '\n';
        uploadUrl = APP_URL.performWWWOTAUpdate;
        isFirmware = false;
      }
    }

    try {
      await axios.post(uploadUrl, file, {
        headers: {
          'Content-Type': 'application/octet-stream'
        },
        responseType: 'text',
        onUploadProgress: (progressEvent: AxiosProgressEvent) => {
           if (progressEvent.total) {
              uploadProgress.value = (progressEvent.loaded / progressEvent.total);
           }
        }
      });
      
      if (isFirmware) {
        uploadStatusMessage.value += t('settings.upload_status_success_firmware') + '\n';
        showNotificationLoading(t('com.msg_restarting_system'), 30);
        setTimeout(() => {
          updating.value = false;
          window.location.reload();
        }, 30000);
      } else {
        uploadStatusMessage.value += t('settings.upload_status_success_website') + '\n';

        /*
         * The miner restarts itself once the write verifies, so there is no
         * restart to send from here any more. Sending one anyway raced the
         * reboot it was asking for and reported "Failed to send restart
         * command" for an update that had in fact succeeded.
         *
         * Leaving it to the device also covers the callers that are not this
         * page: the partition is written but the running server still serves
         * the old mount, so anything that posted to /api/system/OTAWWW and
         * did not restart was left looking at an interface that had not
         * changed, or at the recovery page.
         */
        showNotificationLoading(t('com.msg_restarting_system'), 30);
        
        setTimeout(() => {
           updating.value = false;
           appStore.setInfo({ isPollingPaused: false });
           window.location.hash = "";
           window.location.reload();
        }, 30000); 
      }
    } catch (e: any) {
      console.error("Upload failed:", e);
      if (e.response && e.response.data && e.response.data.includes("SHA256 Mismatch")) {
        uploadStatusMessage.value += t('settings.upload_status_md5_mismatch');
      } else {
        uploadStatusMessage.value += t('settings.upload_status_fail_retry');
      }
      uploadFailed.value = true; 
      updating.value = false;
      appStore.setInfo({ isPollingPaused: false });
    }
  };
  
  reader.readAsArrayBuffer(file.slice(0, 1));
};

// --- Remote OTA Code ---
const isVersionNewer = (remoteVer: string, localVerStr: string) => {
  let rBase = remoteVer.replace(/^v/i, '').split(/[ \-_]/)[0];
  let lBase = localVerStr.replace(/^v/i, '').split(/[ \-_]/)[0];
  
  const vR = rBase.split('.').map(Number);
  const vL = lBase.split('.').map(Number);
  
  for (let i = 0; i < Math.max(vR.length, vL.length); i++) {
    const r = vR[i] || 0;
    const l = vL[i] || 0;
    if (r > l) return true;
    if (r < l) return false;
  }
  
  // If base versions are identical, compare the 8-digit date string if present
  const rDateMatch = remoteVer.match(/\d{8}/);
  const lDateMatch = localVerStr.match(/\d{8}/);
  if (rDateMatch && lDateMatch) {
     return parseInt(rDateMatch[0]) > parseInt(lDateMatch[0]);
  } else if (rDateMatch && !lDateMatch) {
     return true; // remote has date, local doesn't = newer
  }
  return false;
};

/*
 * Pick the asset an update actually accepts.
 *
 * A release carries five images. Two of them are update containers the miner
 * will take -- the application as `-ota.bin`, the interface as `-www-ota.bin`.
 * The others are raw partition images for a serial cable, and this same page
 * rejects them: it routes an upload by its first byte, and a partition image
 * starts 0xE9 rather than 0xAA or 0x55.
 *
 * The previous selection took the first asset whose name ended in .bin, which
 * on an alphabetically ordered release is `-app.bin`. So checking for updates
 * here offered a file that this page refuses -- the one path where the miner
 * hands the user the wrong thing and then blames the file.
 */
/*
 * Boards that run another board's build, deliberately.
 *
 * A BC01 Pro is a BC01 as far as the image is concerned -- same ASIC pinout,
 * same driver -- and no artifact is named for it, so without this it would
 * find nothing and be told there is no update. Anything not listed here has
 * to match its own name.
 */
const ASSET_MODEL_ALIAS: Record<string, string> = {
  'BC01-Pro': 'BC01',
};

const pickUpdateAsset = (assets: any[] | undefined, kind: 'app' | 'www') => {
  const bins = (assets || []).filter((a: any) => a?.name?.endsWith('.bin'));
  const isWww = (n: string) => /-www-ota\.bin$/i.test(n);
  const isApp = (n: string) => /-ota\.bin$/i.test(n) && !isWww(n);
  const wanted = kind === 'www' ? isWww : isApp;
  // Only ever an update container. Falling back to "any .bin" is what produced
  // the bug; a release without the right container has no update to offer.
  return bins.find((a: any) => wanted(a.name)) || null;
};

const checkGithubUpdates = async () => {
  otaChecking.value = true;
  checkOtaStatus.value = sel('remote_ota_checking');
  remoteWebUrl.value = null;
  remoteWebReleaseUrl.value = null;
  remoteFwUrl.value = null;
  remoteFwReleaseUrl.value = null;
  remoteWebVersion.value = '';
  remoteFwVersion.value = '';
  remoteWebNotes.value = '';
  remoteFwNotes.value = '';

  const currentModel = minerStatusRef.value?.DeviceModel || appStore.deviceModel || 'DC02';
  const ts = Date.now();
  
  try {
    const [webRes, fwRes] = await Promise.allSettled([
      axios.get(`https://api.github.com/repos/${UPDATE_REPO_WWW}/releases/latest?t=${ts}`),
      axios.get(`https://api.github.com/repos/${UPDATE_REPO_APP}/releases/latest?t=${ts}`)
    ]);

    let foundUpdate = false;

    if (webRes.status === 'fulfilled' && webRes.value.data) {
      /*
       * Compared against the running FIRMWARE version, not against
       * WEB_VERSION.
       *
       * The release tag numbers the firmware -- 2.0.13 -- while the interface
       * carries its own series, 1.5.3. Comparing one to the other asks whether
       * 2.0.13 is newer than 1.5.3, which is true forever, so this reported an
       * interface update on every check no matter what was installed.
       *
       * Both images ship together in one release, so the question that actually
       * has an answer is whether the release is newer than what the miner is
       * running.
       */
      let gitWebVersion = webRes.value.data.tag_name;
      /*
       * The web interface is genuinely board-independent -- every board's
       * -www-ota.bin in a release is byte-identical -- so any of them is the
       * right one, and preferring this board's is presentation rather than
       * correctness. Unlike the application container below, where taking
       * another board's file installs the wrong firmware.
       */
      const wwwAssets = webRes.value.data.assets || [];
      const wwwModel = (ASSET_MODEL_ALIAS[currentModel] || currentModel).toLowerCase();
      const binAsset = pickUpdateAsset(
        wwwAssets.filter((a: any) => a?.name?.toLowerCase().includes(wwwModel)), 'www')
        || pickUpdateAsset(wwwAssets, 'www');
      
      if (binAsset) {
        // extract string preceding .bin, split by _, typically `www_1.3.5_20260310.bin` gives `20260310`
        const nameParts = binAsset.name.replace('.bin', '').split('_');
        if (nameParts.length > 2) {
          gitWebVersion += ` ${nameParts[nameParts.length - 1]}`;
        }
        
        const localFwForWeb = minerStatusRef.value?.version || '0';
        if (isVersionNewer(gitWebVersion, localFwForWeb)) {
          remoteWebUrl.value = binAsset.browser_download_url;
          remoteWebReleaseUrl.value = webRes.value.data.html_url || `https://github.com/${UPDATE_REPO_WWW}/releases/tag/${webRes.value.data.tag_name}`;
          remoteWebVersion.value = gitWebVersion;
          remoteWebNotes.value = webRes.value.data.body || '';
          foundUpdate = true;
        }
      }
    }

    if (fwRes.status === 'fulfilled' && fwRes.value.data) {
      let gitFwVersion = fwRes.value.data.tag_name;
      /*
       * The application container has to name this board, with no fallback.
       *
       * A release carries every published board now, so "this board's file, or
       * else any file" resolves to whichever board sorts first -- BC02, BC08
       * and BC01-Pro were all being offered the BC01 image. The boards do not
       * share an ASIC pinout, so installing another board's build produces a
       * miner that boots, detects its chip and never returns a share. The
       * board-mismatch guard catches it and says so on screen, but it is still
       * a wasted update and a miner that looks broken.
       *
       * A board with no build published is offered nothing, which is the
       * truthful answer.
       */
      const fwAssets = fwRes.value.data.assets || [];
      const assetModel = (ASSET_MODEL_ALIAS[currentModel] || currentModel).toLowerCase();
      const forThisModel = fwAssets.filter((a: any) =>
        a?.name?.toLowerCase().includes(assetModel));
      const binAsset = pickUpdateAsset(forThisModel, 'app');

      if (binAsset) {
        // extract string preceding .bin, split by _
        const nameParts = binAsset.name.replace('.bin', '').split('_');
        if (nameParts.length > 2) {
          gitFwVersion += ` ${nameParts[nameParts.length - 1]}`;
        }
        
        const localFw = minerStatusRef.value?.version || '0';
        if (isVersionNewer(gitFwVersion, localFw)) {
          remoteFwUrl.value = binAsset.browser_download_url;
          remoteFwReleaseUrl.value = fwRes.value.data.html_url || `https://github.com/${UPDATE_REPO_APP}/releases/tag/${fwRes.value.data.tag_name}`;
          remoteFwVersion.value = gitFwVersion;
          remoteFwNotes.value = fwRes.value.data.body || '';
          foundUpdate = true;
        }
      }
    }

    if (foundUpdate) {
      checkOtaStatus.value = ''; // Clear to show buttons
    } else {
      checkOtaStatus.value = sel('remote_ota_latest');
    }
  } catch (error) {
    console.error("Github Check Error:", error);
    checkOtaStatus.value = "Network error checking Github repository.";
  } finally {
    otaChecking.value = false;
  }
};



const triggerNativeDownload = (url: string | null, filename: string) => {
  if (!url) return;
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.target = '_blank';
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
};

const syncMinerStatus = async () => {
  try {
    const resData = await getMinerStatus('');
    minerStatusRef.value = validData(resData);
  } catch (e) {
    console.log(e);
  }
}

onMounted(async () => {
  await syncMinerStatus();
  if (appStore.currentModelConfig?.support_ota_download) {
    checkGithubUpdates();
  }
})
</script>

<template>
  <settings-edit :title="''" :url="''"></settings-edit>
  <a-card :title="sel('update_combined')" class="card set-status-wrap" style="border: 1px solid var(--surface-border); box-shadow: none;">
    <!-- Shown where someone is about to change firmware, which is the moment
         it is worth knowing whose firmware this is and what it does not
         promise. The README and the web flasher carry the same statement. -->
    <a-alert type="warning" show-icon class="set-unofficial-notice"
             :message="sel('unofficial_notice')" />
    <a-descriptions bordered :column="{ xs: 1, sm: 1, md: 1 }" size="small">
      <a-descriptions-item :label="sel('remote_ota_model') || 'Model'">
        {{ minerStatusRef?.DeviceModel || appStore.deviceModel || 'DC02' }}
      </a-descriptions-item>

      <a-descriptions-item :label="sel('version_website')">
        <div style="display: flex; align-items: center; gap: 12px; flex-wrap: wrap;">
          <span>{{ WEB_VERSION }}</span>
          <template v-if="remoteWebUrl">
            <a-tag color="warning"><ArrowUpOutlined /> {{ remoteWebVersion }}</a-tag>
          </template>
          <div v-if="remoteWebUrl" style="display: flex; align-items: center; gap: 8px;">
            <a-popover v-if="remoteWebNotes || remoteWebReleaseUrl" :title="sel('remote_ota_release_notes') || 'Notes'" trigger="hover">
              <template #content>
                <div style="max-width: 300px; max-height: 50vh; overflow-y: auto;">
                  <div style="white-space: pre-wrap; margin-bottom: 12px;">{{ remoteWebNotes }}</div>
                  <a v-if="remoteWebReleaseUrl" :href="remoteWebReleaseUrl" target="_blank">{{ sel('open_update_page') }}</a>
                </div>
              </template>
              <InfoCircleOutlined style="color: #1a81ff; cursor: pointer;" />
            </a-popover>
            <a-button type="primary" size="small" @click="triggerNativeDownload(remoteWebUrl, `web_update_${remoteWebVersion}.bin`)" :disabled="updating">
              <DownloadOutlined /> {{ sel('download') }}
            </a-button>
          </div>
        </div>
      </a-descriptions-item>

      <a-descriptions-item :label="sel('version_firmware')">
        <div style="display: flex; align-items: center; gap: 12px; flex-wrap: wrap;">
          <span>{{ minerStatusRef?.version || 'Unknown' }}</span>
          <template v-if="remoteFwUrl">
            <a-tag color="warning"><ArrowUpOutlined /> {{ remoteFwVersion }}</a-tag>
          </template>
          <div v-if="remoteFwUrl" style="display: flex; align-items: center; gap: 8px;">
            <a-popover v-if="remoteFwNotes || remoteFwReleaseUrl" :title="sel('remote_ota_release_notes') || 'Notes'" trigger="hover">
              <template #content>
                <div style="max-width: 300px; max-height: 50vh; overflow-y: auto;">
                  <div style="white-space: pre-wrap; margin-bottom: 12px;">{{ remoteFwNotes }}</div>
                  <a v-if="remoteFwReleaseUrl" :href="remoteFwReleaseUrl" target="_blank">{{ sel('open_update_page') }}</a>
                </div>
              </template>
              <InfoCircleOutlined style="color: #1a81ff; cursor: pointer;" />
            </a-popover>
            <a-button type="primary" size="small" @click="triggerNativeDownload(remoteFwUrl, `fw_update_${remoteFwVersion}.bin`)" :disabled="updating">
              <DownloadOutlined /> {{ sel('download') }}
            </a-button>
          </div>
        </div>
      </a-descriptions-item>
    </a-descriptions>

    <div style="margin-top: 24px; padding-top: 16px; border-top: 1px solid var(--surface-border);">
      <div style="display: flex; gap: 12px; align-items: center; flex-wrap: wrap;">
        <input ref="fileInputRef" type="file" style="display: none" accept=".bin" @change="uploadFile" />
        <a-button type="primary" :loading="updating" @click="chooseFile">
          <PlusOutlined /> {{ sel('browse') }}
        </a-button>
        <span style="color: var(--text-color-secondary); font-size: 0.9rem;">{{ sel('update_combined_hint') }}</span>
        <a-button v-if="appStore.currentModelConfig?.support_ota_download" @click="checkGithubUpdates" :loading="otaChecking" :disabled="updating">
          <ReloadOutlined /> {{ sel('remote_ota_check') }}
        </a-button>
      </div>

      <div v-if="updating || checkOtaStatus || uploadStatusMessage" style="margin-top: 16px;">
        <a-alert :message="checkOtaStatus || uploadStatusMessage" :type="uploadFailed ? 'error' : 'info'" show-icon />
        <a-progress v-if="updating" :percent="Number((uploadProgress * 100).toFixed(2))" stroke-color="#1A81FF" :trail-color="'rgba(128, 128, 128, 0.2)'" size="small" style="margin-top: 8px;" />
      </div>
    </div>
  </a-card>

  
  <!-- Shown unconditionally: this firmware always supports a device password,
       and hiding the only way to set one is how a fresh miner ended up with no
       route to authentication at all. -->
  <SecuritySettings />
</template>

<style scoped lang="scss">
.set-status-wrap {
  margin-top: 2rem;
}

.set-status-card {
  padding: 2.2rem 1.2rem;
  min-height: 13rem;
  height: auto;
}

.set-status-item {
  display: flex;
  flex-direction: column;
  width: 100%;
}

.set-status-update-label {
  height: 2.8rem;
  font-size: 1.4rem;
  color: var(--text-color);
}

.set-status-upload-controls {
  margin-bottom: 1rem; 
}


.set-status-upload-row {
  display: flex;
  align-items: center;
  gap: 1rem;
}

.set-status-update-btn {
  width: 7rem;
  height: 2.2rem;
  line-height: 1;
  padding: 0;
  flex-shrink: 0; 
}

.set-status-hint {
  color: var(--text-color);
  font-size: 0.9rem;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.set-status-progress {
  width: 100%; 
  max-width: 100%; 
  display: block;
  margin-top: 0.5rem ;
}


.set-status-upload-status {
  font-size: 0.85rem;
  color: var(--text-color-secondary);
  margin-top: 0;
  white-space: pre-line;
  background-color: rgba(0, 0, 0, 0.05);
  border: 1px solid var(--surface-border);
  border-radius: 4px;
  padding: 0.5rem;
  max-height: 100px;
  overflow-y: auto;
  width: 100%; 
}

.set-status-upload-status.upload-failed {
  color: #F82C5F; 
  font-weight: 500;
}


.set-status-version-display-wrapper {
  margin-top: 1rem;
  display: flex;
  flex-direction: column; 
  gap: 0.25rem; 
}

.set-status-label {
  color: #000000;
  font-size: 1.1rem;
  font-weight: 500;
  margin-right: 0.3rem;
  white-space: nowrap;
}

.set-status-value {
  color: #1A81FF;
  font-size: 1.4rem;
  font-weight: 500;
  white-space: nowrap;
}

.set-status-version-display {
  color: var(--text-color, #000000);
  font-size: 0.9rem;
  font-weight: 500;
  margin-top: 0.5rem;
}

.settings-page-header {
  margin-top: 2rem;
  margin-bottom: -1rem;
}
.professional-ota {
  margin-top: 1rem;
}

.ota-description {
  font-size: 0.95rem;
  color: var(--text-color-secondary);
  line-height: 1.5;
}

.ota-info-box {
  background: transparent;
  border-radius: 8px;
  height: 100%;
}

.info-section {
  background-color: rgba(120, 120, 120, 0.05);
  border: 1px solid var(--surface-border, rgba(120, 120, 120, 0.15));
  border-radius: 8px;
  padding: 1.2rem;
  margin-bottom: 2rem;
}

.manual-upload-section {
  background-color: rgba(120, 120, 120, 0.05);
  border: 1px dashed var(--surface-border, rgba(120, 120, 120, 0.25));
  border-radius: 8px;
  padding: 1.2rem;
}

.manual-title {
  font-weight: 600;
  font-size: 1.05rem;
  color: var(--text-color);
  margin-bottom: 1rem;
}

.ota-status-header-box {
  background-color: rgba(26, 129, 255, 0.05);
  border: 1px solid rgba(26, 129, 255, 0.2);
  border-radius: 8px;
  padding: 1.2rem;
  margin-bottom: 1.5rem;
}

.ota-info-row {
  display: flex;
  margin-bottom: 0.8rem;
  align-items: center;
}

.ota-info-row:last-child {
  margin-bottom: 0;
}

.ota-info-row .info-label {
  min-width: 140px;
  color: var(--text-color-secondary);
}

.ota-info-row .info-value {
  color: var(--text-color);
  font-weight: 500;
}

.ota-updates-container {
  display: flex;
  flex-direction: column;
  gap: 1.2rem;
  margin-bottom: 1.5rem;
}

.ota-update-card {
  border: 1px solid rgba(120, 120, 120, 0.15);
  border-radius: 8px;
  background-color: rgba(0, 0, 0, 0.02);
  overflow: hidden;
}

.ota-update-card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 1rem 1.2rem;
  background-color: rgba(120, 120, 120, 0.05);
  border-bottom: 1px solid rgba(120, 120, 120, 0.1);
}

.ota-update-card-header .update-title {
  font-weight: 600;
  font-size: 1.05rem;
  color: var(--text-color);
}

.ota-release-notes {
  padding: 1rem 1.2rem;
}

.ota-release-notes .notes-title {
  font-weight: 600;
  margin-bottom: 0.5rem;
  color: var(--text-color);
}

.ota-release-notes .notes-content {
  font-family: inherit;
  font-size: 0.9rem;
  color: var(--text-color-secondary);
  white-space: pre-wrap;
  margin: 0;
  background: transparent;
  padding: 0;
  border: none;
}

.ota-download-progress-wrap {
  margin-top: 1rem;
}

.ota-download-status {
  margin-bottom: 0.5rem;
  font-size: 0.9rem;
  color: #1A81FF;
}

.set-unofficial-notice {
  /* Colours live in styles/layout/_antd-fixes.scss with the other alert
     types, because the contrast problem was never specific to this notice. */
  margin-bottom: 16px;
}


</style>