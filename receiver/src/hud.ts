import { applyStaticText, onLocaleChange, t, type MessageKey } from './i18n';
import type { DisplayConfig, StatsMessage } from './protocol';

/** When this page was built (set by vite.config.mts), shown next to the version so a new build can be told apart. */
declare const __BUILD__: string;

function element<T extends HTMLElement>(id: string): T {
  const found = document.getElementById(id);
  if (!found) throw new Error(`index.html has no #${id}`);
  return found as T;
}

const LAYOUT_CLASSES = ['mode-top', 'mode-bottom', 'mode-vertical', 'mode-side', 'mode-pip'];
const LAYOUT_NAMES: MessageKey[] = ['mode.top', 'mode.bottom', 'mode.vertical', 'mode.side', 'mode.pip'];

/** Everything the page shows besides the two screens: the splash, the OSD, the debug panel, the layout classes. */
export class Hud {
  /** JPEG quality the 3DS used: current value, and the range over the last second when it moved. */
  lastQuality = '';

  // What is shown in words is remembered as a key (and a number), so that it can be drawn again in another language.
  private stateKey: MessageKey = 'state.ready';
  private splashKey: MessageKey = 'splash.waiting';
  private layout = 0;
  private senderVersion = '';

  private readonly stage = element('stage');
  private readonly splash = element('splash');
  private readonly splashStatus = element('splash-status');
  private readonly osd = element('osd');
  private readonly state = element('osd-state');
  private readonly mode = element('osd-mode');
  private readonly stats = element('osd-stats');
  private readonly jank = element('osd-jank');
  private readonly audio = element('osd-audio');
  private readonly versions = element('osd-version');
  private readonly debug = element('dbg');
  private readonly audioEnable = element<HTMLButtonElement>('audio-enable');

  constructor(private readonly receiverVersion: string) {
    element('splash-version').textContent = `v${receiverVersion}`;
    onLocaleChange(() => this.refresh());
    this.refresh();
  }

  /** Draws all the words again, in the current language. */
  refresh(): void {
    applyStaticText(document);
    this.state.textContent = t(this.stateKey);
    this.splashStatus.textContent = t(this.splashKey);
    this.mode.textContent = `${t('mode.label')}: ${t(LAYOUT_NAMES[this.layout] || 'mode.top')}`;
    this.versions.textContent = this.senderVersion
      ? t('version.both', { sender: this.senderVersion, receiver: `${this.receiverVersion} (${__BUILD__})` })
      : t('version.receiver', { receiver: `${this.receiverVersion} (${__BUILD__})` });
  }

  /** The 3DS program announces its version in its CAPS message; both are shown. */
  showVersions(senderVersion: string, build = ''): void {
    this.senderVersion = senderVersion && build ? `${senderVersion} (${build})` : senderVersion;
    this.refresh();
  }

  /** Layout, filter, aspect and OSD visibility as the 3DS asked for. (The audio part is the caller's.) */
  applyLayout(cfg: DisplayConfig): void {
    this.layout = Number(cfg.layout ?? 0);
    this.stage.className = LAYOUT_CLASSES[this.layout] || 'mode-top';
    this.refresh();
    document.body.classList.toggle('filter-pixelated', cfg.filter !== 1);
    document.body.classList.toggle('filter-smooth', cfg.filter === 1);
    document.body.classList.toggle('stretch-fill', cfg.stretch === 1);
    if (cfg.show_osd !== undefined) {
      this.osd.classList.toggle('hidden', cfg.show_osd === 0);
      this.debug.classList.toggle('hidden', cfg.show_osd === 0);
    }
  }

  setState(key: MessageKey): void {
    this.stateKey = key;
    this.state.textContent = t(key);
  }

  setSplashStatus(key: MessageKey): void {
    this.splashKey = key;
    this.splashStatus.textContent = t(key);
  }

  showSplash(): void {
    this.splash.classList.remove('hidden');
  }

  hideSplash(): void {
    if (!this.splash.classList.contains('hidden')) this.splash.classList.add('hidden');
  }

  setFrameStats(text: string): void {
    this.stats.textContent = text;
  }

  setJank(text: string): void {
    this.jank.textContent = text;
  }

  setAudioStatus(status: string, needsGesture: boolean): void {
    this.audio.textContent = status;
    this.audioEnable.hidden = !needsGesture;
    if (needsGesture && document.activeElement !== this.audioEnable) this.audioEnable.focus();
  }

  onAudioEnable(handler: () => void): void {
    this.audioEnable.addEventListener('click', handler);
  }

  qualityText(m: StatsMessage): string {
    if (typeof m.q !== 'number' || !m.q) return 'Q -';
    const range =
      typeof m.qn === 'number' && typeof m.qx === 'number' && m.qn !== m.qx ? ` (${m.qn}-${m.qx})` : '';
    return `Q${m.q}${range}`;
  }

  /** The 3DS program's own numbers (once a second). */
  showStats(m: StatsMessage): void {
    this.lastQuality = this.qualityText(m);
    const tenths = (n: number | undefined) => (typeof n === 'number' ? (n / 10).toFixed(1) : '-'); // tenths of ms
    const num = (n: number | string | undefined) => (n === undefined ? '-' : n);
    const cpu = typeof m.tn === 'number' && typeof m.t === 'number' ? (m.t - m.tn / 10).toFixed(1) : '-';
    const lines = [
      `R${num(m.r)} J${num(m.j)} T${num(m.t)} ms`,
      typeof m.wr === 'number' && typeof m.tn === 'number' && typeof m.t === 'number'
        ? // the send time per frame in four parts: the network itself, the TLS work around it (encryption), building
          // the messages, and the rest (waiting, being preempted)
          `T net ${tenths(m.tn)} / tls ${tenths(m.wr - m.tn)} / build ${tenths(m.bd)} / other ${(m.t - m.wr / 10 - (m.bd || 0) / 10).toFixed(1)}`
        : `T net ${tenths(m.tn)} / cpu ${cpu}`,
      `J conv ${tenths(m.jc)} / jpeg ${tenths(m.jj)} / b64 ${tenths(m.jb)}`,
      `jpeg = gather ${tenths(m.pg)} / dct ${tenths(m.pd)} / entropy ${tenths(m.pe)}`,
      `budget ${m.bp ? `${m.bp}% = ${((m.bb || 0) / 1024).toFixed(1)} KB/frame` : '-'}`,
      // the link speed the pictures are planned for, and what the 3DS actually measured while sending
      `network ${m.la ? 'auto' : 'set'} ${m.lk ? `${(m.lk / 1000).toFixed(2)} MB/s` : '-'}  (measured ${m.lm ? `${(m.lm / 1000).toFixed(2)} MB/s` : '-'})`,
      `TLS ${m.ci || '-'}`,
      `core ${m.core || '-'}${m.fb ? ' (fell back)' : ''}  rejected ${num(m.rej)}  reconnects ${num(m.rc)}`,
      `audio sent ${num(m.sent)}  dropped ${num(m.drop)}`,
    ];
    this.debug.textContent = lines.join('\n');
  }
}
