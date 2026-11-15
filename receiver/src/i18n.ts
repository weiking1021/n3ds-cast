// The words the page shows to people (the numbers and technical readouts stay English). The language is chosen
// automatically: see resolveLocale().

export type Locale = 'en' | 'zh-TW' | 'ja';

const en = {
  'splash.title': 'New 3DS Direct Cast Receiver',
  'splash.waiting': 'Ready. Waiting for the New 3DS to send the picture...',
  'splash.connected': 'Connected to the New 3DS. Waiting for the stream to start...',
  'splash.waitingPc': 'PC test receiver: waiting for the 3DS to connect...',
  'audio.enable': 'Enable sound (press OK)',
  'state.ready': 'READY',
  'state.live': 'LIVE',
  'state.configured': 'CONFIGURED',
  'mode.label': 'Mode',
  'mode.top': 'Top Only',
  'mode.bottom': 'Bottom Only',
  'mode.vertical': 'Vertical Dual',
  'mode.side': 'Side-by-Side',
  'mode.pip': 'Picture-in-Picture',
  'version.both': '3DS v{sender} / receiver v{receiver}',
  'version.receiver': 'receiver v{receiver}',
  'audio.off': 'Audio: OFF',
  'audio.waiting': 'Audio: {kbps} kbps, waiting',
  'audio.waitingPackets': 'Audio: waiting for packets',
  'audio.buffering': 'Audio: buffering',
  'audio.pressOk': 'Audio: press OK to enable',
} as const;

export type MessageKey = keyof typeof en;
type Catalog = Record<MessageKey, string>;

const zhTW: Catalog = {
  'splash.title': 'New 3DS 直接投放接收端',
  'splash.waiting': '已就緒，等待 New 3DS 傳送畫面資料...',
  'splash.connected': '已連線至 New 3DS，等待串流開始...',
  'splash.waitingPc': 'PC 測試接收端：等待 3DS 連線...',
  'audio.enable': '啟用聲音（按確認鍵）',
  'state.ready': '就緒',
  'state.live': '串流中',
  'state.configured': '已設定',
  'mode.label': '版面',
  'mode.top': '僅上螢幕',
  'mode.bottom': '僅下螢幕',
  'mode.vertical': '上下排列',
  'mode.side': '左右排列',
  'mode.pip': '子母畫面',
  'version.both': '3DS v{sender} / 接收端 v{receiver}',
  'version.receiver': '接收端 v{receiver}',
  'audio.off': '音訊：關閉',
  'audio.waiting': '音訊：{kbps} kbps，等待中',
  'audio.waitingPackets': '音訊：等待封包',
  'audio.buffering': '音訊：緩衝中',
  'audio.pressOk': '音訊：請按確認鍵啟用',
};

const ja: Catalog = {
  'splash.title': 'New 3DS ダイレクトキャスト受信側',
  'splash.waiting': '準備完了。New 3DSからの映像を待っています...',
  'splash.connected': 'New 3DSに接続しました。配信の開始を待っています...',
  'splash.waitingPc': 'PCテスト受信側：3DSの接続を待っています...',
  'audio.enable': '音声を有効にする（OKボタン）',
  'state.ready': '準備完了',
  'state.live': '配信中',
  'state.configured': '設定済み',
  'mode.label': 'レイアウト',
  'mode.top': '上画面のみ',
  'mode.bottom': '下画面のみ',
  'mode.vertical': '上下に並べる',
  'mode.side': '左右に並べる',
  'mode.pip': '子画面',
  'version.both': '3DS v{sender} / 受信側 v{receiver}',
  'version.receiver': '受信側 v{receiver}',
  'audio.off': '音声：オフ',
  'audio.waiting': '音声：{kbps} kbps、待機中',
  'audio.waitingPackets': '音声：パケット待ち',
  'audio.buffering': '音声：バッファ中',
  'audio.pressOk': '音声：OKボタンで有効にします',
};

export const CATALOGS: Record<Locale, Catalog> = { en, 'zh-TW': zhTW, ja };

export const DEFAULT_LOCALE: Locale = 'en';

/** The supported language for a language tag such as "zh-TW", "zh_hant", "en-US"; null when there is none. */
export function matchLocale(tag: string | null | undefined): Locale | null {
  if (!tag) return null;
  const primary = tag.toLowerCase().replace(/_/g, '-').split('-')[0];
  if (primary === 'en') return 'en';
  if (primary === 'zh') return 'zh-TW'; // the only Chinese there is: Traditional is read by people who use Simplified too
  if (primary === 'ja') return 'ja';
  return null;
}

/**
 * The language to show: the first of the candidates that is supported. Put the candidates in order of priority, for
 * example [the address's ?lang=, the language the 3DS asked for, ...the browser's or the TV's preferences]; one that
 * is missing or not supported is skipped, and without any match the default (English) is used.
 */
export function resolveLocale(candidates: ReadonlyArray<string | null | undefined>): Locale {
  for (const candidate of candidates) {
    const found = matchLocale(candidate);
    if (found) return found;
  }
  return DEFAULT_LOCALE;
}

export function translate(locale: Locale, key: MessageKey, params: Record<string, string | number> = {}): string {
  return CATALOGS[locale][key].replace(/\{(\w+)\}/g, (all, name: string) => String(params[name] ?? all));
}

// ---- the language of this page ----
let current: Locale = DEFAULT_LOCALE;
const listeners = new Set<() => void>();

export function t(key: MessageKey, params?: Record<string, string | number>): string {
  return translate(current, key, params);
}

export function currentLocale(): Locale {
  return current;
}

/** Changes the language; everything that registered with onLocaleChange redraws its text. */
export function setLocale(locale: Locale): void {
  document.documentElement.lang = locale;
  if (locale === current) return;
  current = locale;
  listeners.forEach((listener) => listener());
}

export function onLocaleChange(listener: () => void): void {
  listeners.add(listener);
}

/** Puts the current language into every element of `root` marked with data-i18n="key". */
export function applyStaticText(root: ParentNode): void {
  root.querySelectorAll<HTMLElement>('[data-i18n]').forEach((element) => {
    element.textContent = t(element.dataset.i18n as MessageKey);
  });
}
