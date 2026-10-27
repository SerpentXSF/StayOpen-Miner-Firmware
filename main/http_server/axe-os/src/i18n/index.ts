import { createI18n } from "vue-i18n";
import en from "./en";
import zh from "./zh";

const messages = {
  en,
  zh,
};

/*
 * fallbackLocale, because without one vue-i18n renders a missing key as the
 * key path itself.
 *
 * zh was four keys short of en, and three of those are on the dashboard's
 * Ethernet pill -- so a BC04 in Chinese printed the literal text
 * "dashboard.card.ethernet_status" and "dashboard.card.eth_on" where the
 * wired-network status belongs. The keys are filled in now, but the shape of
 * the bug is that en gains a key and zh is not updated in the same change,
 * which will happen again. With a fallback the worst case is a label in
 * English, which is readable; without one it is a developer's key path, which
 * is not.
 */
const i18n = createI18n({
  legacy: false,
  messages,
  locale: localStorage.getItem("lang") || "en",
  fallbackLocale: "en",
  globalInjection: true,
});

export const LANG_LIST = {
    en : 'English',
    zh : '中文'
};

export const langChange = (lang: any) => {
  localStorage.setItem("lang", lang);
  i18n.global.locale.value = lang;
};

export default i18n;

