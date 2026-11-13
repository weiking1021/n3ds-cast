import './style.css';
import { version } from '../package.json';
import { AudioPlayer } from './audio';
import { createCastContext, createDevContext, type ReceiverContext } from './context';
import { Hud } from './hud';
import { NAMESPACE, type DisplayConfig, type HelloMessage, type Message } from './protocol';
import { Video } from './video';

const WAITING_FOR_3DS = 'PC 測試接收端：等待 3DS 連線...';

const hud = new Hud(version);
const audio = new AudioPlayer((status, needsGesture) => hud.setAudioStatus(status, needsGesture));
hud.onAudioEnable(() => audio.resume(true));
document.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && audio.enabled) audio.resume(true);
});

/** What the 3DS asked for: layout, filter, aspect, OSD, and whether (and how fast) the sound is sent. */
function applyConfig(cfg: DisplayConfig): void {
  hud.applyLayout(cfg);
  if (cfg.audio_enabled !== undefined) audio.configure(cfg.audio_enabled === 1, Number(cfg.audio_kbps));
}

const video = new Video(hud, (cfg) => {
  hud.hideSplash();
  hud.setState('LIVE');
  if (cfg) applyConfig(cfg);
});

// ---- Where are we running? On a Cast device the real Cast SDK drives this page. Anywhere else (a desktop browser
// opened from receiver/server.js, http://localhost:8080) there is no Cast SDK, so a small stand-in with the same
// three calls is connected by WebSocket to the PC receiver. Override with ?dev=1 (stand-in) or ?dev=0 (real SDK).
const params = new URLSearchParams(location.search);
const dev = params.get('dev');
const local = location.hostname === 'localhost' || location.hostname === '127.0.0.1';
const devMode = dev === '1' || (dev !== '0' && (typeof cast === 'undefined' || !cast.framework || local));

// The sender stopped the receiver app (the 3DS does this before every new stream): back to the waiting screen without
// reloading, so the browser keeps the audio unlocked and nobody is asked again.
function resetPage(): void {
  video.reset();
  hud.showSplash();
  hud.setSplashStatus(WAITING_FOR_3DS);
  hud.setState('READY');
  hud.lastQuality = '';
  hud.showVersions('');
  audio.stop('Audio: OFF');
}

const context: ReceiverContext = devMode
  ? createDevContext(params.get('ws') || `ws://${location.hostname || 'localhost'}:${location.port || 8080}/dev`, {
      onOpen: () => hud.setSplashStatus(WAITING_FOR_3DS),
      onReset: resetPage,
    })
  : createCastContext();

function handleMessage(raw: unknown, senderId: string): void {
  let message: Message;
  if (typeof raw === 'string') {
    try {
      message = JSON.parse(raw) as Message;
    } catch (err) {
      console.error('Invalid JSON from 3DS:', err);
      return;
    }
  } else {
    message = raw as Message;
  }

  switch (message.type) {
    case 'CAPS': {
      // The sender asks what this page understands: "rects" = partial screen updates (RECT messages). An older page
      // never answers and the sender keeps to whole Base64 frames.
      hud.showVersions(typeof message.ver === 'string' ? message.ver.slice(0, 16) : '');
      const hello: HelloMessage = { type: 'HELLO', v: 4, ok: true, rects: true, ver: version };
      context.sendCustomMessage(NAMESPACE, senderId, hello);
      break;
    }
    case 'RECT':
      video.handleRect(message);
      break;
    case 'CONFIG':
      // A fresh sender session can restart audio sequence numbers at zero.
      audio.stop(message.audio_enabled === 1 ? 'Audio: waiting for packets' : 'Audio: OFF');
      applyConfig(message);
      hud.setState('CONFIGURED');
      hud.setSplashStatus('已連線至 New 3DS，等待串流開始...');
      break;
    case 'STATS':
      try {
        hud.showStats(message);
      } catch (err) {
        console.error('Bad STATS', err);
      }
      break;
    case 'AUDIO':
      audio.push(message); // Audio packets must not inflate video FPS or frame-size stats.
      break;
    case 'AUDIO_STOP':
      audio.stop();
      break;
    case 'FRAMEPART':
      video.handleFramePart(message);
      break;
    case 'FRAME':
      video.handleFrame(message);
      break;
  }
}

context.addCustomMessageListener(NAMESPACE, (event) => handleMessage(event.data, event.senderId));
context.start();
