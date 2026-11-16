import type { DisplayConfig, FrameMessage, FramePartMessage, RectMessage, Screen } from './protocol';
import type { Hud } from './hud';

const MAX_PIECES = 16; // a screen or rectangle that did not fit in one message arrives in at most this many pieces
const STATS_INTERVAL_MS = 1000;

/** The pieces of one screen (or rectangle) collected so far. */
interface Pieces {
  total: number;
  next: number;
  parts: string[];
}

interface Surface {
  canvas: HTMLCanvasElement;
  context: CanvasRenderingContext2D;
  image: HTMLImageElement; // for whole-screen frames
  pieces: Pieces | null; // FRAMEPART pieces
  rectPieces: Pieces | null; // RECT pieces
  drawChain: Promise<unknown>; // rectangles are drawn in the order they arrived
}

function surface(id: string): Surface {
  const canvas = document.getElementById(id) as HTMLCanvasElement;
  const context = canvas.getContext('2d', { alpha: false }) as CanvasRenderingContext2D;
  const image = new Image();
  image.onload = () => context.drawImage(image, 0, 0, canvas.width, canvas.height);
  return { canvas, context, image, pieces: null, rectPieces: null, drawChain: Promise.resolve() };
}

function isPieceOf(total: number, index: number): boolean {
  return total >= 1 && total <= MAX_PIECES && index >= 0 && index < total;
}

/** Takes the next piece of `slot`. Returns the whole text once the last piece has arrived, '' while more follows. */
function collect(slot: Pieces | null, total: number, index: number, data: string): { slot: Pieces | null; done: string | null } | null {
  let pieces = slot;
  if (index === 0) pieces = { total, next: 0, parts: [] };
  else if (!pieces || pieces.total !== total || pieces.next !== index) return null;
  pieces.parts.push(data);
  pieces.next++;
  if (pieces.next === total) return { slot: null, done: pieces.parts.join('') };
  return { slot: pieces, done: null };
}

/**
 * The two screens: shows whole frames (FRAME, FRAMEPART) and partial updates (RECT), counts them for the OSD and
 * measures the stutter of this page (gap between messages, time to handle one, longest pause of the render loop).
 */
export class Video {
  private readonly screens: Record<Screen, Surface> = { top: surface('canvas-top'), bot: surface('canvas-bottom') };
  private frameCount = 0;
  private lastFpsTime = performance.now();
  private lastFrameBytes = 0;
  private pendingBytes = 0;
  private jankGap = 0;
  private jankProc = 0;
  private jankRaf = 0;
  private lastArrive = 0;
  private rafLast = performance.now();
  private lastKeyAsk = -Infinity;
  private sawPicture = false;

  /** `begin` is called with the config of every message that starts to show a picture. */
  constructor(
    private readonly hud: Hud,
    private readonly begin: (cfg: DisplayConfig | undefined) => void,
    private readonly needKey: () => void = () => {},
  ) {
    const loop = () => {
      requestAnimationFrame((t) => {
        this.jankRaf = Math.max(this.jankRaf, t - this.rafLast);
        this.rafLast = t;
        loop();
      });
    };
    loop();
  }

  handleFrame(msg: FrameMessage): void {
    this.measure(() => {
      const wire = (msg.top ? msg.top.length : 0) + (msg.bot ? msg.bot.length : 0);
      this.showFrame(msg.cfg, msg.top || null, msg.bot || null, wire, msg.via === 'direct' ? 'base64 direct' : 'base64', msg.cont === 1);
    });
  }

  // One screen of a frame that did not fit in a single 64 KB Cast message: numbered pieces, glued back together here.
  // TCP keeps them in order; anything unexpected drops the partial screen (the next frame replaces it).
  handleFramePart(msg: FramePartMessage): void {
    const which: Screen = msg.w === 'bot' ? 'bot' : 'top';
    const screen = this.screens[which];
    const index = Number(msg.i);
    const total = Number(msg.n);
    if (!isPieceOf(total, index) || typeof msg.d !== 'string') {
      screen.pieces = null;
      return;
    }
    const result = collect(screen.pieces, total, index, msg.d);
    if (!result) {
      screen.pieces = null;
      return;
    }
    screen.pieces = result.slot;
    if (result.done !== null) {
      const full = result.done;
      this.showFrame(msg.cfg, which === 'top' ? full : null, which === 'bot' ? full : null, full.length, msg.via === 'direct' ? 'base64 direct' : 'base64', msg.cont === 1);
    }
  }

  // Partial updates ("RECT" messages): the sender re-encodes only the 16x16 tiles that changed, as a few small JPEGs.
  // Each is decoded (off the main thread) and drawn at its place on the screen's canvas; what did not change simply
  // stays. Once the sender knows we do this (HELLO "rects"), every picture arrives this way, a whole screen being
  // one rectangle, so the order of drawing is the order of arrival (a chain per screen). A big rectangle may come in
  // numbered pieces (i of n), like FRAMEPART.
  handleRect(msg: RectMessage): void {
    this.measure(() => {
      const which: Screen = msg.s === 'bot' ? 'bot' : 'top';
      const screen = this.screens[which];
      const index = Number(msg.i);
      const total = Number(msg.n);
      const x = Number(msg.x);
      const y = Number(msg.y);
      if (!(isPieceOf(total, index) && x >= 0 && y >= 0) || typeof msg.d !== 'string') {
        screen.rectPieces = null;
        return;
      }
      const result = collect(screen.rectPieces, total, index, msg.d);
      if (!result) {
        screen.rectPieces = null;
        return;
      }
      this.begin(msg.cfg);
      if (!this.sawPicture) {
        // the sender does not repeat whole screens by itself: the first picture message a page gets may be a part of a
        // picture it never had, so it asks once (a few extra KB when it was the first whole screen anyway)
        this.sawPicture = true;
        this.askKey();
      }
      screen.rectPieces = result.slot;
      if (result.done !== null) {
        this.queueDraw(screen, x, y, result.done);
        this.accountFrame(result.done.length, msg.via === 'direct' ? 'rects direct' : 'rects', msg.cont === 1);
      }
    });
  }

  /** Back to an empty picture (the sender stopped the receiver app). */
  reset(): void {
    this.pendingBytes = 0;
    for (const screen of Object.values(this.screens)) {
      screen.pieces = null;
      screen.rectPieces = null;
      // after any picture of the old stream that is still being decoded
      screen.drawChain = screen.drawChain.then(() => {
        screen.context.fillStyle = '#000';
        screen.context.fillRect(0, 0, screen.canvas.width, screen.canvas.height);
      });
    }
  }

  private showFrame(cfg: DisplayConfig | undefined, top: string | null, bot: string | null, wireBytes: number, format: string, cont: boolean): void {
    this.begin(cfg);
    if (top) this.screens.top.image.src = `data:image/jpeg;base64,${top}`;
    if (bot) this.screens.bot.image.src = `data:image/jpeg;base64,${bot}`;
    this.accountFrame(wireBytes, format, cont);
  }

  // Asks the sender for a whole picture (at most once a second): there is nothing here to build the rectangles on, e.g.
  // this page was just opened while the 3DS was already streaming.
  private askKey(): void {
    const now = performance.now();
    if (now - this.lastKeyAsk > 1000) {
      this.lastKeyAsk = now;
      this.needKey();
    }
  }

  private queueDraw(screen: Surface, x: number, y: number, base64: string): void {
    const image = new Image();
    image.src = `data:image/jpeg;base64,${base64}`;
    const ready = image.decode
      ? image.decode().then(() => true, () => false)
      : new Promise<boolean>((resolve) => {
          image.onload = () => resolve(true);
          image.onerror = () => resolve(false);
        });
    screen.drawChain = screen.drawChain
      .then(() => ready)
      .then((ok) => {
        if (ok) screen.context.drawImage(image, x, y);
      });
  }

  // A frame too big for one Cast message (64 KB) arrives as several: only the last one counts as a frame, and the KB
  // figure adds up the pieces.
  private accountFrame(wireBytes: number, format: string, cont: boolean): void {
    this.pendingBytes += wireBytes;
    if (cont) return;
    this.lastFrameBytes = this.pendingBytes;
    this.pendingBytes = 0;
    this.frameCount++;
    const now = performance.now();
    if (now - this.lastFpsTime >= STATS_INTERVAL_MS) {
      const fps = Math.round((this.frameCount * 1000) / (now - this.lastFpsTime));
      this.frameCount = 0;
      this.lastFpsTime = now;
      this.hud.setFrameStats(
        `FPS: ${fps} | Frame: ${(this.lastFrameBytes / 1024).toFixed(1)} KB | ${this.hud.lastQuality || 'Q -'} | ${format || 'base64'}`,
      );
      this.hud.setJank(
        `gap ${Math.round(this.jankGap)} | decode ${Math.round(this.jankProc)} | render ${Math.round(this.jankRaf)} ms (max)`,
      );
      this.jankGap = this.jankProc = this.jankRaf = 0;
    }
  }

  // Times the handling of one message and the gap since the previous one (the worst of each is shown per second).
  private measure(handle: () => void): void {
    const start = performance.now();
    if (this.lastArrive) this.jankGap = Math.max(this.jankGap, start - this.lastArrive);
    this.lastArrive = start;
    handle();
    this.jankProc = Math.max(this.jankProc, performance.now() - start);
  }
}
