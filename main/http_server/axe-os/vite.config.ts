import { fileURLToPath, URL } from "node:url";
import { defineConfig, Plugin } from 'vite'
import vue from '@vitejs/plugin-vue'
import viteCompression from 'vite-plugin-compression';
import AutoImport from 'unplugin-auto-import/vite';
import Components from 'unplugin-vue-components/vite';
import { AntDesignVueResolver } from 'unplugin-vue-components/resolvers';

// 全局捕获 ECONNRESET 防止 vite dev 进程因为底层 socket 出错而直接退出
process.on('uncaughtException', (err: any) => {
    if (err.code === 'ECONNRESET') {
        console.warn('⚠️ [Vite Global] Caught unhandled ECONNRESET. Ignored to prevent server crash.');
    } else {
        console.error('Unhandled Exception:', err);
    }
});

// [保持不变] 强制注入 modulepreload
const injectPreloadLinks = (): Plugin => ({
    name: 'inject-preloads',
    transformIndexHtml(html, ctx) {
        if (!ctx.bundle) return html;

        const links = Object.values(ctx.bundle)
            .filter((chunk: any) => {
                return chunk.fileName.endsWith('.js') &&
                    !chunk.fileName.includes('worker') &&
                    !chunk.fileName.includes('index-');
            })
            .map((chunk: any) => {
                return `<link rel="modulepreload" crossorigin href="/${chunk.fileName}">`;
            })
            .join('');

        return html.replace('</head>', `${links}\n</head>`);
    }
});

export default defineConfig({
    envDir: 'src',

    server: {
        proxy: {
            '/api': {
                target: 'http://192.168.4.1', // 此处配置无关紧要，主要是为了挂载全局容错
                changeOrigin: true,
                secure: false,
                configure: (proxy, _options) => {
                    // 捕获 ECONNRESET，避免 Nodejs 崩溃退出
                    proxy.on('error', (err: any, _req, _res) => {
                        if (err.code === 'ECONNRESET') {
                            console.warn('[Proxy Warn] Connection Reset by peer. Ignored.');
                        } else {
                            console.warn('[Proxy Warn] Request error:', err.message);
                        }
                    });
                }
            }
        }
    },

    build: {
        sourcemap: false,
        chunkSizeWarningLimit: 2000, // 调高警告阈值，因为我们要合并大包
        emptyOutDir: true,
        minify: 'terser', // 使用 terser 获得更小的体积
        modulePreload: false, // [修改] 禁用默认的 modulePreload，完全由我们手动控制或串行加载

        terserOptions: {
            compress: {
                drop_console: true,  // 移除 console
                drop_debugger: true, // 移除 debugger
                pure_funcs: ['console.log', 'console.info'], // 进一步移除无用函数
            },
            format: {
                comments: false, // 移除注释
            },
        },
        /*
         * CSS is its own hashed asset again.
         *
         * With cssCodeSplit off, vite-plugin-css-injected-by-js appended the
         * whole app's CSS into the vendor chunk after Rollup had already
         * computed that chunk's content hash. The filename therefore never
         * moved when the styles did: two builds with different CSS both
         * shipped as vendor-BefK_L50.js. The firmware serves /assets with
         * Cache-Control: max-age=2592000, so a browser that had been to the
         * miner before kept a month-old copy across an update.
         *
         * That is worse than stale colours. Scoped-style ids are derived from
         * component source, so an edited component gets a new id in its
         * freshly named chunk while the cached CSS still carries the old one,
         * and the selectors stop matching at all -- an update that appears to
         * do nothing, or to break the interface.
         *
         * Splitting CSS back out costs one request and restores the property
         * the cache header assumes: a changed file has a changed name.
         */
        cssCodeSplit: true,

        rollupOptions: {
            output: {
                // [关键优化] 极简分包策略：所有 node_modules 合并为一个 vendor.js
                // 这样 Gzip 压缩率最高，且减少 HTTP 请求开销
                manualChunks(id) {
                    if (id.includes('node_modules')) {
                        return 'vendor';
                    }
                },
                // 确保入口文件名称固定
                entryFileNames: 'assets/[name]-[hash].js',
                chunkFileNames: 'assets/[name]-[hash].js',
                assetFileNames: 'assets/[name]-[hash].[ext]'
            }
        }
    },
    plugins: [
        vue(),
        AutoImport({
            imports: ['vue', 'vue-router', 'vue-i18n'],
            dts: 'src/auto-imports.d.ts',
        }),
        Components({
            resolvers: [
                AntDesignVueResolver({
                    importStyle: false, // 不导入样式文件，依靠全局样式或按需
                }),
            ],
            dts: 'src/components.d.ts',
        }),
        injectPreloadLinks(),
        // [关键优化] Gzip 压缩配置
        viteCompression({
            algorithm: 'gzip',
            ext: '.gz',
            // [警告] 设置为 true 会删除原文件，只保留 .gz。
            // 只有当您的后端 http_server.c 支持自动处理 .gz 请求时才开启。
            // 如果后端不支持，请改为 false，但通过脚本只烧录 .gz 文件。
            deleteOriginFile: true,
            threshold: 1, // 对所有文件进行压缩（哪怕很小）
            filter: /\.(js|css|html|svg|json)$/i // 压缩这些类型
        })
    ],
    resolve: {
        alias: {
            "@": fileURLToPath(new URL("./src", import.meta.url)),
        },
    },
})