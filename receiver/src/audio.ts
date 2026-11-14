import { t } from './i18n';
import type { AudioMessage } from './protocol';

// Independent IMA ADPCM blocks, decoded without loading an extra codec.
const STEPS = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
  60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
  337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
  1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
  4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
  12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];
const INDEX_CHANGES = [-1, -1, -1, -1, 2, 4, 6, 8];
const RATES = [8000, 16000, 32000]; // samples per second of the ADPCM blocks (10 blocks a second)

/** Decodes one block (100 ms of mono audio) to float samples. Throws for a block that is not well formed. */
export function decodeAdpcm(msg: AudioMessage): Float32Array {
  if (msg.codec !== 'ima-adpcm' || !RATES.includes(msg.rate) || msg.samples !== msg.rate / 10 ||
      !Number.isInteger(msg.seq) || msg.seq < 0 || msg.seq > 0xffffffff || typeof msg.data !== 'string' ||
      msg.data.length > 2140) {
    throw new Error('invalid audio block');
  }
  const bytes = atob(msg.data);
  if (bytes.length !== 4 + Math.floor(msg.samples / 2) || bytes.charCodeAt(2) > 88 || bytes.charCodeAt(3) !== 0) {
    throw new Error('invalid ADPCM header');
  }
  let predictor = bytes.charCodeAt(0) | (bytes.charCodeAt(1) << 8);
  if (predictor >= 32768) predictor -= 65536;
  let index = bytes.charCodeAt(2);
  const pcm = new Float32Array(msg.samples);
  pcm[0] = predictor / 32768;
  for (let i = 1; i < msg.samples; i++) {
    const byte = bytes.charCodeAt(4 + ((i - 1) >> 1));
    const code = (byte >> (((i - 1) & 1) * 4)) & 15;
    const step = STEPS[index];
    let delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    predictor += (code & 8) ? -delta : delta;
    predictor = Math.max(-32768, Math.min(32767, predictor));
    index = Math.max(0, Math.min(88, index + INDEX_CHANGES[code & 7]));
    pcm[i] = predictor / 32768;
  }
  return pcm;
}

/** Called with the text for the OSD, and whether the browser needs a click or Enter before it plays sound. */
export type AudioStatusHandler = (status: string, needsGesture: boolean) => void;

// Adaptive jitter buffer. 'lead' is how much audio is kept queued ahead of playback: the smaller it is the lower the
// delay, the larger the better it rides out a late packet (a big picture ahead of it in the same TCP stream holds the
// audio back for ~100 ms). It starts small, grows by 30 ms after an underrun (up to 300 ms) and shrinks by 10 ms for
// every 2 quiet seconds (down to 60 ms), so a rough start does not keep the delay high for minutes.
const LEAD_START = 0.10;
const LEAD_MIN = 0.06;
const LEAD_MAX = 0.30;
const LEAD_STEP_UP = 0.03;
const LEAD_STEP_DOWN = 0.01;
const LEAD_QUIET_SECONDS = 2;
const UNDERRUN_MARGIN = 0.005;
const BURST_MARGIN = 0.10; // the queue may grow this far past the target before a packet is skipped
const GAP_WINDOW_SECONDS = 5;
const SILENCE_MS = 1000; // no packet for this long: stop and wait for the next ones

type AudioContextConstructor = typeof AudioContext;

export class AudioPlayer {
  enabled = false;
  private kbps = 64;
  private ctx: AudioContext | null = null;
  private resuming = false;
  private readonly sources = new Set<AudioBufferSourceNode>();
  private nextTime = 0;
  private lastSeq: number | null = null;
  private lastReceived = 0;
  // jitter buffer
  private lead = 0;
  private leadChanged = 0;
  private underruns = 0;
  private worstGap = 0;
  private shownGap = 0;
  private gapSince = 0;
  private lastArrival = 0;

  constructor(private readonly onStatus: AudioStatusHandler) {
    setInterval(() => {
      if (this.lastReceived && Date.now() - this.lastReceived > SILENCE_MS) {
        this.stop(this.enabled ? t('audio.waitingPackets') : t('audio.off'));
      }
    }, 250);
  }

  stop(status = t('audio.off')): void {
    for (const source of this.sources) {
      source.onended = null;
      try {
        source.stop();
      } catch (_) {
        /* Already ended. */
      }
      source.disconnect();
    }
    this.sources.clear();
    this.nextTime = 0;
    this.lastSeq = null;
    this.lastReceived = 0;
    this.onStatus(status, false);
  }

  configure(enabled: boolean, kbps: number): void {
    const rate = [32, 64, 128].includes(kbps) ? kbps : 64;
    if (this.enabled !== enabled || this.kbps !== rate) {
      this.stop(enabled ? t('audio.waiting', { kbps: rate }) : t('audio.off'));
      this.enabled = enabled;
      this.kbps = rate;
    }
  }

  /**
   * Starts (or resumes) the audio context. Sound starts by itself wherever the browser allows it (a TV always does).
   * Only a browser that blocks autoplay needs one click or Enter; the context then stays unlocked for as long as
   * the page lives.
   */
  resume(force = false): boolean {
    try {
      if (!this.ctx) {
        const Context: AudioContextConstructor | undefined =
          window.AudioContext || (window as unknown as { webkitAudioContext?: AudioContextConstructor }).webkitAudioContext;
        if (!Context) throw new Error('Web Audio unavailable');
        this.ctx = new Context();
      }
      // A blocked autoplay resume can remain pending. A real user gesture must be allowed to call resume again
      // rather than waiting on it.
      if (this.ctx.state !== 'running' && (!this.resuming || force)) {
        this.resuming = true;
        Promise.resolve(this.ctx.resume())
          .catch(() => {
            if (this.enabled) this.onStatus(t('audio.pressOk'), true);
          })
          .finally(() => {
            this.resuming = false;
          });
      }
      return this.ctx.state === 'running';
    } catch (err) {
      this.onStatus(`Audio: ${(err as Error).message}`, false);
      return false;
    }
  }

  push(msg: AudioMessage): void {
    if (!this.enabled) return;
    try {
      if (msg.rate !== this.kbps * 250) throw new Error('audio rate mismatch');
      const pcm = decodeAdpcm(msg);
      if (this.lastSeq !== null && msg.seq <= this.lastSeq) return;
      if (!this.resume()) {
        if (this.ctx) this.onStatus(t('audio.pressOk'), true);
        return; // Never accumulate audio while playback is suspended.
      }
      const ctx = this.ctx as AudioContext;
      const now = ctx.currentTime;
      if (!this.lead) {
        this.lead = LEAD_START;
        this.leadChanged = now;
        this.underruns = 0;
        this.worstGap = 0;
        this.gapSince = now;
      }
      const wall = performance.now();
      if (this.lastArrival) this.worstGap = Math.max(this.worstGap, wall - this.lastArrival);
      this.lastArrival = wall;
      if (now - this.gapSince > GAP_WINDOW_SECONDS) {
        this.shownGap = this.worstGap;
        this.worstGap = 0;
        this.gapSince = now;
      }
      const playing = this.nextTime > 0;
      const gap = this.lastSeq !== null && msg.seq !== this.lastSeq + 1;
      const underrun = playing && this.nextTime < now + UNDERRUN_MARGIN;
      if (underrun) {
        this.underruns++;
        this.lead = Math.min(LEAD_MAX, this.lead + LEAD_STEP_UP);
        this.leadChanged = now;
      } else if (playing && !gap && now - this.leadChanged > LEAD_QUIET_SECONDS && this.lead > LEAD_MIN) {
        this.lead = Math.max(LEAD_MIN, this.lead - LEAD_STEP_DOWN);
        this.leadChanged = now;
      }
      if (gap || underrun || !playing) {
        this.stop(t('audio.buffering'));
        this.nextTime = now + this.lead;
      } else if (this.nextTime > now + this.lead + BURST_MARGIN) {
        // Packets arrived in a burst and the queue grew past the target: skip this one instead of playing it late.
        // The queue then drains back to the target without any silence.
        this.lastSeq = msg.seq;
        this.lastReceived = Date.now();
        return;
      }
      const buffer = ctx.createBuffer(1, pcm.length, msg.rate);
      buffer.getChannelData(0).set(pcm);
      const source = ctx.createBufferSource();
      source.buffer = buffer;
      source.connect(ctx.destination);
      this.sources.add(source);
      source.onended = () => {
        this.sources.delete(source);
        source.disconnect();
      };
      const delay = this.nextTime - now; // how long until this packet is heard
      source.start(this.nextTime);
      this.nextTime += buffer.duration;
      this.lastSeq = msg.seq;
      this.lastReceived = Date.now();
      this.onStatus(
        `Audio: ${this.kbps} kbps | delay ${Math.round(delay * 1000)} ms (target ${Math.round(this.lead * 1000)})` +
          ` | underruns ${this.underruns} | packet gap max ${Math.round(this.shownGap || this.worstGap)} ms`,
        false,
      );
    } catch (err) {
      this.stop(`Audio: ${(err as Error).message}`);
    }
  }
}
