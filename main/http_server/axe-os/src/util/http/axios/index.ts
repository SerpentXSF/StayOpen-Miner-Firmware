import axios from "axios";
import type {
    AxiosInstance,
    AxiosRequestConfig,
    AxiosResponse,
    AxiosError,
    InternalAxiosRequestConfig,
} from "axios";
import { showMessage } from "./status";
import type { IResponse } from "./type";
import i18n from "@/i18n";
// import {statusData} from "@/api";

const t = i18n.global.t;

const lang = (code: string) => {
    return t(`err.${code}`);
};

const service: AxiosInstance = axios.create({
    timeout: 15000,
    headers: { "Content-Type": "application/json" },
});

service.interceptors.request.use(
    (config: InternalAxiosRequestConfig) => {
        const token = localStorage.getItem('auth_token');
        if (token) {
            config.headers.Authorization = `Bearer ${token}`;
        }
        return config;
    },
    (error: AxiosError) => {
        return Promise.reject(error);
    },
);

// axios实例拦截响应
service.interceptors.response.use(
    (response: AxiosResponse) => {
        if (response.status === 200) {
            return response;
        }
        console.log(showMessage(response.status));
        return response;
    },
    // 请求失败
    (error: any) => {
        // 不是2xx的都在这
        const { response } = error;
        if (response) {
            if (response.status === 401) {
                localStorage.removeItem('auth_token');
                if (!window.location.hash.includes('#/login')) {
                    window.location.href = '#/login';
                }
            }
            /*
             * Carry the status with the message.
             *
             * Rejecting with a bare string loses the one field callers test.
             * The status poll in App.vue asks for e.response.status so that a
             * 401 -- a session that needs signing in, which the branch above
             * has already acted on -- does not also claim the miner is
             * broken. Against a string that read undefined, so sitting at the
             * login screen raised "data synchronization failed" every time,
             * which is the alarm that suppression was written to prevent.
             *
             * The message stays the message, so callers that log or display
             * it are unaffected.
             */
            const failure = new Error(showMessage(response.status)) as Error & { response?: any };
            failure.response = response;
            return Promise.reject(failure);
        } else {
            return Promise.reject(lang("errxxx1"));
        }
    },
);

const request = <T = any>(config: AxiosRequestConfig): Promise<T> => {
    const conf = config;
    // if(!conf.baseURL) {
    //     conf.baseURL = 'http://192.168.11.3/'
    // }
    return new Promise((resolve, reject) => {
        service
            .request<any, AxiosResponse<IResponse>>(conf)
            .then((res: AxiosResponse<IResponse>) => {
                let dataObj: any = res.data;
                resolve(dataObj as T);
            })
            .catch((err: any) => {
                reject(err);
            });
    });
};

export function get<T = any>(config: AxiosRequestConfig): Promise<T> {
    return request({ ...config, method: "GET" });
}

export function post<T = any>(config: AxiosRequestConfig): Promise<T> {
    return request({ ...config, method: "POST" });
}

export function patch<T = any>(config: AxiosRequestConfig): Promise<T> {
    return request({ ...config, method: "PATCH" });
}

export default request;

export type { AxiosInstance, AxiosResponse };
