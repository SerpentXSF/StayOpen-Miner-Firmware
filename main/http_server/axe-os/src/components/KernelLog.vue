<script setup lang="ts">
import { ref, onMounted, nextTick, watch, computed } from "vue";
import { useI18n } from "vue-i18n";
import { message } from "ant-design-vue";
import { DownloadOutlined, PauseCircleOutlined, PlayCircleOutlined } from "@ant-design/icons-vue";
import { useAppStore } from "@/store";
import { storeToRefs } from "pinia";
import { URL } from "@/api"; // 导入 URL 以使用下载链接

const { t } = useI18n();
const appStore = useAppStore();
const {wsConnected, wsConnecting, logContent } = storeToRefs(appStore);

const ll = (code: string): string => {
  return t(`logs.${code}`);
}

// --- 状态 ---
const logContentRef = ref<HTMLDivElement | null>(null);
const stopScroll = ref<boolean>(false);
const logFilterText = ref<string>("");

// --- HTML转义 ---
const escapeHTML = (str: string) => {
  if (!str) return '';
  return str.replace(/[&<>"']/g, match => {
    const map: { [key: string]: string } = {
      '&': '&amp;',
      '<': '&lt;',
      '>': '&gt;',
      '"': '&quot;',
      "'": '&#39;'
    };
    return map[match as keyof typeof map];
  });
};

// --- 格式化日志内容 ---
const formattedLogContent = computed(() => {
  const lines = logContent.value.split('\n');
  return lines
    .filter(line => line.toLowerCase().includes(logFilterText.value.toLowerCase()))
    .map(line => {
      const escapedLine = escapeHTML(line);
      let className = 'log-line log-info'; // 默认样式

      // 【修改】：使用 includes 匹配，因为现在开头可能是时间戳 [2025-...]
      if (escapedLine.includes('E (')) {
        className = 'log-line log-error';
      } else if (escapedLine.includes('W (')) {
        className = 'log-line log-warn';
      } else if (escapedLine.includes('D (')) {
        className = 'log-line log-debug';
      } else if (escapedLine.includes('I (')) {
        className = 'log-line log-info';
      } else if (escapedLine.includes('[WebSocket]')) {
        className = 'log-line log-system';
      }
      
      return `<div class="${className}">${escapedLine}</div>`;
    })
    .join('');
});

/*
 * Download the log.
 *
 * This used to be window.location.href = URL.downloadLog, which is a top-level
 * navigation: the browser asks for the file itself and sends no Authorization
 * header, because the token lives in localStorage and is attached by the HTTP
 * layer rather than stored as a cookie. The endpoint requires authentication,
 * so it answered "Authentication required" -- to a signed-in user, on the page
 * that was streaming the very same log over an authenticated socket.
 *
 * Fetch it with the header and hand the browser a blob instead. Deliberately
 * not a token in the query string: it would leak the credential into history,
 * logs and the address bar.
 */
const downloadLogFile = async () => {
    const token = localStorage.getItem("auth_token");
    try {
        const res = await fetch(URL.downloadLog, {
            headers: token ? { Authorization: "Bearer " + token } : {},
        });
        if (!res.ok) {
            message.error(res.status === 401 ? ll("downloadUnauthorised")
                                             : ll("downloadFailed"));
            return;
        }
        const blob = await res.blob();
        const href = window.URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = href;
        a.download = "kernel-log.txt";
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        window.URL.revokeObjectURL(href);
    } catch {
        message.error(ll("downloadFailed"));
    }
};

onMounted(async () => {
  // 1. 自动连接 WebSocket (如果未连接)
  if (!wsConnected.value && !wsConnecting.value) {
      appStore.connectWebSocket({
          connecting: ll('ws_status_connecting'),
          connected: ll('ws_status_connected'),
          disconnected: ll('ws_status_disconnected'),
          expired: ll('ws_status_expired'),
          error: ll('ws_status_error'),
          create_error: ll('ws_status_create_error')
      });
  }

  // 2. 自动滚动逻辑
  watch([formattedLogContent, stopScroll], async () => {
      if (stopScroll.value) return;
      await nextTick();
      if (logContentRef.value) {
        logContentRef.value.scrollTop = logContentRef.value.scrollHeight;
      }
  }, { immediate: true });
});
</script>

<template>
  <div>
    <a-row class="bs-syslog-wrap">
      <a-col :xs="24" :md="24" :lg="24" class="bs-syslog-content-wrap">
        
        <div class="bs-block-title-container">
          <div class="bs-block-title">
            {{ ll("log_content") }}
          </div>
          
          <a-space wrap class="bs-log-toolbar">
            <a-button @click="downloadLogFile">
              <template #icon><DownloadOutlined /></template>
              {{ ll('download') || 'Download' }} 
            </a-button>
            
            <a-button
              @click="stopScroll = !stopScroll"
              :danger="stopScroll" 
            >
              <template #icon>
                <PauseCircleOutlined v-if="!stopScroll" />
                <PlayCircleOutlined v-else />
              </template>
              {{ stopScroll ? ll('ws_scroll_start') : ll('ws_scroll_stop') }}
            </a-button>
            
            <a-button @click="logFilterText = ''">
              {{ ll('ws_clear_filter') }}
            </a-button>

            <a-input
              v-model:value="logFilterText"
              :placeholder="ll('ws_filter_placeholder')"
              style="width: 200px"
              allow-clear
            />
          </a-space>
        </div>

        <a-spin :spinning="wsConnecting" tip="connecting...">
          <div class="bs-syslog-content" ref="logContentRef">
            <div v-html="formattedLogContent"></div>
          </div>
        </a-spin>
      </a-col>
    </a-row>
  </div>
</template>

<style scoped lang="scss">
.bs-syslog-wrap {
  display: flex;
}

.bs-block-title {
  color: var(--text-color);
  font-weight: 500;
  font-size: 1.1rem;
}

.bs-block-title-container {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 1.5rem;
  flex-wrap: wrap;
  gap: 1rem;
}

.bs-log-toolbar {
  margin-left: auto;
  flex-wrap: wrap;
  gap: 1rem;
}

.bs-syslog-content-wrap {
  position: relative;
  padding: 0px 0rem 2.1rem 0rem; 
}

.bs-syslog-content {
  position: relative;
  padding: 1rem;
  background-color: var(--surface-ground);
  color: var(--text-color);
  //background: #1e1e1e; /* 确保深色背景适合阅读日志 */
  border: 1px solid var(--surface-border);
  border-radius: 0.8rem;
  height: 100%;
  min-height: 36rem;
  font-size: 0.9rem;
  //color: #d4d4d4;
  line-height: 1.4rem;
  white-space: normal;
  word-break: break-all;
  max-height: 600px;
  overflow: auto;
  font-family: 'Courier New', Courier, monospace;
}

:deep(.log-line) {
  white-space: pre-wrap;
  word-break: break-all;
  padding: 0 2px; 
}

// :deep(.log-line.log-info) { color: #d4d4d4; }
// :deep(.log-line.log-system) { color: #00ffff; }
// :deep(.log-line.log-warn) { color: #cca700; }
// :deep(.log-line.log-error) { color: #f44336; }
// :deep(.log-line.log-debug) { color: #569cd6; }

/* 同时需要为日志级别颜色适配亮色背景 */
:deep(.log-line.log-info) { color: var(--text-color); }
:deep(.log-line.log-error) { color: #d32f2f; } /* 亮色下深红 */
:deep(.log-line.log-warn) { color: #f57f17; }  /* 亮色下深黄 */
:deep(.log-line.log-debug) { color: #0288d1; }  /* 亮色下深蓝 */
:deep(.log-line.log-system) { color: #0097a7; }
</style>