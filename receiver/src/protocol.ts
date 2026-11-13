// The messages of the n3ds-cast protocol (custom Cast namespace, JSON), as the 3DS program sends them.

export const NAMESPACE = 'urn:x-cast:com.n3ds.cast';

/** How the 3DS wants the picture shown. Sent with CONFIG, and with the first message of a frame. */
export interface DisplayConfig {
  layout?: number; // 0 top, 1 bottom, 2 vertical, 3 side by side, 4 picture in picture
  filter?: number; // 0 pixelated, 1 smooth
  stretch?: number; // 0 keep the aspect ratio, 1 fill
  show_osd?: number; // 0 hide, 1 show
  audio_enabled?: number;
  audio_kbps?: number;
}

export type Screen = 'top' | 'bot';

/** Asks what the page understands ("rects": partial updates), and tells the page the 3DS program's version. */
export interface CapsMessage {
  type: 'CAPS';
  rects?: number;
  ver?: string;
}

/** The page's answer to CAPS. */
export interface HelloMessage {
  type: 'HELLO';
  v: number;
  ok: boolean;
  rects: boolean;
  ver: string;
}

export interface ConfigMessage extends DisplayConfig {
  type: 'CONFIG';
}

/** A whole screen (or both) as Base64 JPEG. `cont` = another message of the same frame follows. */
export interface FrameMessage {
  type: 'FRAME';
  cfg?: DisplayConfig;
  top?: string;
  bot?: string;
  cont?: number;
}

/** One numbered piece of a screen that did not fit in a single 64 KB message. */
export interface FramePartMessage {
  type: 'FRAMEPART';
  cfg?: DisplayConfig;
  w: Screen;
  i: number;
  n: number;
  d: string;
  cont?: number;
}

/** A rectangle of a screen (partial update), or a numbered piece of one. */
export interface RectMessage {
  type: 'RECT';
  cfg?: DisplayConfig;
  s: Screen;
  x: number;
  y: number;
  i: number;
  n: number;
  d: string;
  cont?: number;
}

export interface AudioMessage {
  type: 'AUDIO';
  codec: string;
  rate: number;
  samples: number;
  seq: number;
  data: string;
}

export interface AudioStopMessage {
  type: 'AUDIO_STOP';
}

/** Once a second: what the 3DS program measures, for the debug panel. All fields may be missing. */
export interface StatsMessage {
  type: 'STATS';
  fps?: number;
  r?: number;
  j?: number;
  t?: number;
  core?: number;
  fb?: number;
  title?: string;
  rej?: number;
  as?: number;
  ak?: number;
  ap?: number;
  ar?: number;
  an?: number;
  sent?: number;
  drop?: number;
  c?: number[];
  nz?: number[];
  h?: number[];
  tn?: number;
  jc?: number;
  jj?: number;
  jb?: number;
  sl?: number;
  kb?: number;
  q?: number;
  qn?: number;
  qx?: number;
  rc?: number;
}

export type Message =
  | CapsMessage
  | ConfigMessage
  | FrameMessage
  | FramePartMessage
  | RectMessage
  | AudioMessage
  | AudioStopMessage
  | StatsMessage;
