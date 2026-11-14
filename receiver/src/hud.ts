import { applyStaticText, onLocaleChange, t, type MessageKey } from './i18n';
import type { DisplayConfig, StatsMessage } from './protocol';

function element<T extends HTMLElement>(id: string): T {
  const found = document.getElementById(id);
  if (!found) throw new Error(`index.html has no #${id}`);
  return found as T;
}

const LAYOUT_CLASSES = ['mode-top', 'mode-bottom', 'mode-vertical', 'mode-side', 'mode-pip'];
const LAYOUT_NAMES: MessageKey[] = ['mode.top', 'mode.bottom', 'mode.vertical', 'mode.side', 'mode.pip'];
const AUDIO_STATE = ['off', 'waiting for game DSP', 'DSP layout unsupported', 'live'];
const DSP_KIND = ['none', 'pipe table', 'std firmware 0x7FFE'];

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
      ? t('version.both', { sender: this.senderVersion, receiver: this.receiverVersion })
      : t('version.receiver', { receiver: this.receiverVersion });
  }

  /** The 3DS program announces its version in its CAPS message; both are shown. */
  showVersions(senderVersion: string): void {
    this.senderVersion = senderVersion;
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
    const hex = (n: number | undefined) =>
      n === undefined ? '-' : Number(n).toString(16).toUpperCase().padStart(4, '0');
    const tenths = (n: number | undefined) => (typeof n === 'number' ? (n / 10).toFixed(1) : '-'); // tenths of ms
    const list = (a: number[] | undefined) => (Array.isArray(a) ? a : []);
    const num = (n: number | string | undefined) => (n === undefined ? '-' : n);
    const cpu = typeof m.tn === 'number' && typeof m.t === 'number' ? (m.t - m.tn / 10).toFixed(1) : '-';
    const lines = [
      `R${num(m.r)} J${num(m.j)} T${num(m.t)} ms   ${num(m.fps)} FPS   ${this.qualityText(m)}`,
      `T net ${tenths(m.tn)} / cpu ${cpu}   frame ${tenths(m.kb)} KB`,
      `J conv ${tenths(m.jc)} / jpeg ${tenths(m.jj)} / b64 ${tenths(m.jb)}`,
      `core ${m.core || '-'}${m.fb ? ' (fell back)' : ''}  rejected ${num(m.rej)}  reconnects ${num(m.rc)}`,
      `title ${num(m.title)}`,
      `audio: ${(m.as !== undefined && AUDIO_STATE[m.as]) || num(m.as)}`,
      ` layout: ${(m.ak !== undefined && DSP_KIND[m.ak]) || num(m.ak)}`,
      ` probes ${num(m.ap)}  real ${num(m.ar)}  silent ${num(m.sl)}`,
      ` sent ${num(m.sent)}  dropped ${num(m.drop)}`,
      'DSP counters:',
      ` ${list(m.c).map(hex).join(' ')}`,
      'DSP non-zero /4096:',
      ` ${list(m.nz).join(' ')}`,
      `pipe hdr ${list(m.h)[0] ?? '-'}  valid ${list(m.h)[1] ?? '-'}`,
    ];
    this.debug.textContent = lines.join('\n');
  }
}
