import './style.css';
import { version } from '../package.json';
import { AudioPlayer } from './audio';
import { createCastContext, createDevContext, type ReceiverContext } from './context';
import { Hud } from './hud';
import { resolveLocale, setLocale, t } from './i18n';
import { NAMESPACE, type DisplayConfig, type HelloMessage, type Message } from './protocol';
import { Video } from './video';

// ---- The language: the address's ?lang= first (for trying), then the language the 3DS program is set to (its CAPS
// message), then what the browser or the TV prefers; a candidate that is missing or not supported is skipped, and
// without any match it is English.
const params = new URLSearchParams(location.search);
let senderLanguage: string | undefined;
function chooseLanguage(): void {
  setLocale(resolveLocale([params.get('lang'), senderLanguage, ...(navigator.languages || [navigator.language])]));
}
chooseLanguage();

const hud = new Hud(version);
const audio = new AudioPlayer((status, needsGesture) => hud.setAudioStatus(status, needsGesture));
hud.setAudioStatus(t('audio.off'), false);
hud.onAudioEnable(() => audio.resume(true));
document.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && audio.enabled) audio.resume(true);
});

/** What the 3DS asked for: layout, filter, aspect, OSD, and whether (and how fast) the sound is sent. */
function applyConfig(cfg: DisplayConfig): void {
  hud.applyLayout(cfg);
  if (cfg.audio_enabled !== undefined) audio.configure(cfg.audio_enabled === 1, Number(cfg.audio_kbps));
}

// The sender that talks to this page: a page that was just opened while the 3DS is already streaming has no picture to
// build the rectangles on, so it asks that sender for a whole one.
let lastSenderId = '';
const video = new Video(
  hud,
  (cfg) => {
    hud.hideSplash();
    hud.setState('state.live');
    if (cfg) applyConfig(cfg);
  },
  () => {
    if (lastSenderId) context.sendCustomMessage(NAMESPACE, lastSenderId, { type: 'NEED_KEY' });
  },
);

// ---- Where are we running? On a Cast device the real Cast SDK drives this page. Anywhere else (a desktop browser
// opened from receiver/server.js, http://localhost:8080) there is no Cast SDK, so a small stand-in with the same
// three calls is connected by WebSocket to the PC receiver. Override with ?dev=1 (stand-in) or ?dev=0 (real SDK).
const dev = params.get('dev');
const local = location.hostname === 'localhost' || location.hostname === '127.0.0.1';
const devMode = dev === '1' || (dev !== '0' && (typeof cast === 'undefined' || !cast.framework || local));

// The sender stopped the receiver app (the 3DS does this before every new stream): back to the waiting screen without
// reloading, so the browser keeps the audio unlocked and nobody is asked again.
function resetPage(): void {
  video.reset();
  hud.showSplash();
  hud.setSplashStatus('splash.waitingPc');
  hud.setState('state.ready');
  hud.lastQuality = '';
  hud.showVersions('');
  audio.stop(t('audio.off'));
  senderLanguage = undefined;
  chooseLanguage();
}

const context: ReceiverContext = devMode
  ? createDevContext(params.get('ws') || `ws://${location.hostname || 'localhost'}:${location.port || 8080}/dev`, {
      onOpen: () => hud.setSplashStatus('splash.waitingPc'),
      onReset: resetPage,
    })
  : createCastContext();

function handleMessage(raw: unknown, senderId: string): void {
  if (senderId) lastSenderId = senderId;
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
      hud.showVersions(
        typeof message.ver === 'string' ? message.ver.slice(0, 16) : '',
        typeof message.build === 'string' ? message.build.slice(0, 24) : '',
      );
      if (typeof message.lang === 'string') {
        senderLanguage = message.lang;
        chooseLanguage();
      }
      const hello: HelloMessage = { type: 'HELLO', v: 4, ok: true, rects: true, ver: version };
      context.sendCustomMessage(NAMESPACE, senderId, hello);
      break;
    }
    case 'RECT':
      video.handleRect(message);
      break;
    case 'CONFIG':
      // A fresh sender session can restart audio sequence numbers at zero.
      audio.stop(message.audio_enabled === 1 ? t('audio.waitingPackets') : t('audio.off'));
      applyConfig(message);
      hud.setState('state.configured');
      hud.setSplashStatus('splash.connected');
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
