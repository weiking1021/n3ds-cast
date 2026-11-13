#!/usr/bin/env node
'use strict';
// A stand-in for the 3DS, speaking the same CASTV2 + n3ds-cast messages (see 3ds/shared/cast_client.c), so the PC
// receiver and the receiver page can be tested without a 3DS. Library use: see test.js.
//   node fake-sender.js [--host 127.0.0.1] [--port 8009] [--app 01B7A380] [--kb 40] [--fps 10] [--seconds 20]
const tls = require('tls');
const { encode, Framer, NS } = require('../castv2');
const { jpegOfSize } = require('./sample-jpeg');

// Same limits as 3ds/shared/cast_client.c: one message may carry at most 56000 Base64 characters of images,
// bigger screens are cut into 48000-character FRAMEPART pieces; "cont":1 marks all but the last message.
const MSG_IMG_MAX = 56000, PART_CHARS = 48000;
const DEFAULT_CFG = { layout: 4, filter: 0, stretch: 0, show_osd: 1, audio_enabled: 0, audio_kbps: 64 };

function screenMessages(cfg, which, b64, cont) {
  if (b64.length <= MSG_IMG_MAX) return [{ type: 'FRAME', cfg, ...(cont ? { cont: 1 } : {}), [which]: b64 }];
  const n = Math.ceil(b64.length / PART_CHARS), out = [];
  for (let i = 0; i < n; i++) {
    const last = i + 1 === n;
    out.push({ type: 'FRAMEPART', w: which, i, n, cfg, ...(cont || !last ? { cont: 1 } : {}), d: b64.slice(i * PART_CHARS, (i + 1) * PART_CHARS) });
  }
  return out;
}
// RECT messages the 3DS sends for a list of rectangles {screen:'top'|'bot', x, y, w, h, b64} (cast_client_send_rects):
// one per rectangle, numbered pieces of 48000 characters for big ones, cfg in the first, cont on all but the last.
function rectMessages(cfg, rects) {
  const total = rects.reduce((n, r) => n + Math.ceil(r.b64.length / PART_CHARS), 0);
  const out = [];
  for (const r of rects) {
    const parts = Math.ceil(r.b64.length / PART_CHARS);
    for (let i = 0; i < parts; i++) {
      out.push({ type: 'RECT', s: r.screen, x: r.x, y: r.y, w: r.w, h: r.h, i, n: parts,
        ...(out.length === 0 ? { cfg } : {}), ...(out.length + 1 < total ? { cont: 1 } : {}),
        d: r.b64.slice(i * PART_CHARS, (i + 1) * PART_CHARS) });
    }
  }
  return out;
}
// The list of messages the 3DS would send for one frame (cast_client_send_frame).
function frameMessages(cfg, topB64, botB64) {
  const topLen = topB64 ? topB64.length : 0, botLen = botB64 ? botB64.length : 0;
  if (topLen + botLen <= MSG_IMG_MAX) {
    return [{ type: 'FRAME', cfg, ...(topLen ? { top: topB64 } : {}), ...(botLen ? { bot: botB64 } : {}) }];
  }
  return [...(topLen ? screenMessages(cfg, 'top', topB64, botLen > 0) : []), ...(botLen ? screenMessages(cfg, 'bot', botB64, false) : [])];
}

class FakeSender {
  constructor() { this.waiters = []; this.inbox = []; this.request = 1; }
  connect(host, port) {
    return new Promise((resolve, reject) => {
      this.socket = tls.connect({ host, port, rejectUnauthorized: false, minVersion: 'TLSv1.2' }, () => resolve(this));
      this.socket.on('error', reject);
      const framer = new Framer((m) => this.onMessage(m));
      this.socket.on('data', (d) => framer.push(d));
    });
  }
  onMessage(m) {
    let json = null; try { json = JSON.parse(m.payload); } catch (_) { /* not JSON */ }
    const entry = { ...m, json };
    this.inbox.push(entry);
    this.waiters = this.waiters.filter((w) => { if (w.test(entry)) { clearTimeout(w.timer); w.resolve(entry); return false; } return true; });
  }
  send(ns, dest, obj) { this.socket.write(encode({ source: 'sender-0', dest, ns, payload: JSON.stringify(obj) })); }
  waitFor(test, ms = 3000) {
    const hit = this.inbox.find(test);
    if (hit) return Promise.resolve(hit);
    return new Promise((resolve, reject) => {
      const w = { test, resolve };
      w.timer = setTimeout(() => { this.waiters = this.waiters.filter((x) => x !== w); reject(new Error('timeout waiting for a message')); }, ms);
      this.waiters.push(w);
    });
  }
  // The same sequence as cast_client_connect_and_launch(): CONNECT, GET_STATUS (stop a running app when
  // restart is set), LAUNCH, wait for the transportId, CONNECT to it, CONFIG and the CAPS question.
  async launch(appId, { restart = false, cfg = DEFAULT_CFG } = {}) {
    this.send(NS.connection, 'receiver-0', { type: 'CONNECT' });
    const mark = () => this.inbox.length;
    if (restart) {
      let from = mark();
      this.send(NS.receiver, 'receiver-0', { type: 'GET_STATUS', requestId: this.request++ });
      let st = await this.waitFor((m, i = this.inbox.indexOf(m)) => i >= from && m.json && m.json.type === 'RECEIVER_STATUS');
      const running = st.json.status.applications && st.json.status.applications.find((a) => a.appId === appId);
      if (running) {
        from = mark();
        this.send(NS.receiver, 'receiver-0', { type: 'STOP', sessionId: running.sessionId, requestId: this.request++ });
        await this.waitFor((m) => this.inbox.indexOf(m) >= from && m.json && m.json.type === 'RECEIVER_STATUS' && !(m.json.status.applications || []).length);
      }
    }
    const from = mark();
    this.send(NS.receiver, 'receiver-0', { type: 'LAUNCH', appId, requestId: this.request++ });
    const st = await this.waitFor((m) => this.inbox.indexOf(m) >= from && m.json && m.json.type === 'RECEIVER_STATUS' &&
      (m.json.status.applications || []).some((a) => a.appId === appId));
    this.transportId = st.json.status.applications[0].transportId;
    this.send(NS.connection, this.transportId, { type: 'CONNECT' });
    this.send(NS.custom, this.transportId, { type: 'CONFIG', ...cfg });
    this.send(NS.custom, this.transportId, { type: 'CAPS', rects: 1 });
    return st.json.status.applications[0];
  }
  sendFrame(topB64, botB64, cfg = DEFAULT_CFG) {
    for (const m of frameMessages(cfg, topB64, botB64)) this.send(NS.custom, this.transportId, m);
  }
  sendRects(rects, cfg = DEFAULT_CFG) {
    for (const m of rectMessages(cfg, rects)) this.send(NS.custom, this.transportId, m);
  }
  close() { this.socket.destroy(); }
}

module.exports = { FakeSender, frameMessages, rectMessages, DEFAULT_CFG };

if (require.main === module) {
  const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i < 0 ? d : process.argv[i + 1]; };
  (async () => {
    const s = await new FakeSender().connect(arg('host', '127.0.0.1'), Number(arg('port', 8009)));
    const app = await s.launch(arg('app', '01B7A380'), { restart: process.argv.includes('--restart') });
    console.log('launched', app.appId, 'transport', app.transportId);
    const kb = Number(arg('kb', 40)), fps = Number(arg('fps', 10)), seconds = Number(arg('seconds', 20));
    const top = jpegOfSize(kb * 1024).toString('base64'), bot = jpegOfSize(8 * 1024).toString('base64');
    let n = 0;
    const timer = setInterval(() => {
      s.sendFrame(top, bot);
      if (++n >= fps * seconds) { clearInterval(timer); s.close(); }
    }, 1000 / fps);
    setInterval(() => s.send(NS.heartbeat, 'receiver-0', { type: 'PING' }), 5000).unref();
    console.log(`sending a ${kb} KB top + 8 KB bottom frame ${fps}x per second for ${seconds} s`);
  })().catch((e) => { console.error(e.message); process.exit(1); });
}
