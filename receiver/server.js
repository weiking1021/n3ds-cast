#!/usr/bin/env node
'use strict';
// PC stand-in for the Google TV: the 3DS finds it by mDNS (or by typing this PC's IP), talks CASTV2 over TLS to
// port 8009 exactly as it does to a Chromecast, and the frames appear in a browser tab running the real
// page built into dist/ (npm run build; opened from http://localhost:8080 it switches to its WebSocket stand-in for
// the Cast SDK).
//
//   node server.js [--name "PC Receiver"] [--ip 192.168.1.20] [--http 8080] [--cast 8009] [--direct 8010] [--aes] [--no-mdns] [--page <folder>]
//
// Besides the Cast port there is a plain TCP "direct" port (--direct 0 turns it off): the page's HELLO is passed on to the
// 3DS with that port added, and the 3DS then sends its pictures there, 4-byte length + the same JSON, without TLS (the
// encryption costs the 3DS more CPU time than the network does). Audio and everything else stays on the Cast port.
const http = require('http');
const net = require('net');
const tls = require('tls');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { WebSocketServer } = require('ws');
const selfsigned = require('selfsigned');
const { encode, Framer, NS } = require('./castv2');
const { startMdns } = require('./mdns');

function arg(name, fallback) {
  const i = process.argv.indexOf('--' + name);
  if (i < 0) return fallback;
  const v = process.argv[i + 1];
  return v === undefined || v.startsWith('--') ? true : v;
}
const opts = {
  name: String(arg('name', 'PC Receiver (dev)')),
  ip: arg('ip', null),
  http: Number(arg('http', 8080)),
  cast: Number(arg('cast', 8009)),
  direct: Number(arg('direct', 8010)),
  aes: !!arg('aes', false), // prefer AES-GCM (what a server picks by default) instead of ChaCha20-Poly1305
  mdns: !arg('no-mdns', false),
  page: path.resolve(String(arg('page', path.join(__dirname, 'dist')))),
};
const { version } = require('./package.json');
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.map': 'application/json' };
const log = (msg) => console.log(`[${new Date().toLocaleTimeString()}] ${msg}`);

// Which of this PC's addresses should the 3DS use? Best: the one in the same /24 as `--peer <3DS or TV ip>`.
// Otherwise a guess that prefers a physical LAN adapter (Ethernet / Wi-Fi) with a 192.168.x address over
// VPNs, virtual switches and VirtualBox/WSL/Tailscale ones. The list is always printed; --ip overrides.
function pickIp() {
  if (typeof opts.ip === 'string') return opts.ip;
  const peer = typeof arg('peer', null) === 'string' ? arg('peer', null).split('.').slice(0, 3).join('.') : null;
  const found = [];
  for (const [name, list] of Object.entries(os.networkInterfaces())) {
    for (const a of list || []) {
      if (a.family !== 'IPv4' || a.internal) continue;
      const virtual = /vEthernet|VirtualBox|VMware|WSL|Docker|Hyper-V|Loopback|Tailscale|ZeroTier|VPN|TAP|TUN|Bluetooth/i.test(name) ||
        /^192\.168\.56\./.test(a.address) || /^100\.(6[4-9]|[7-9]\d|1[01]\d|12[0-7])\./.test(a.address);
      const physical = /Ethernet|乙太網路|Wi-?Fi|WLAN|無線|Wireless|^en\d|^eth\d|^wlan/i.test(name);
      const priv = /^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)/.test(a.address);
      let score = (physical ? 4 : 0) + (a.address.startsWith('192.168.') ? 2 : priv ? 1 : 0) - (virtual ? 8 : 0);
      if (peer && a.address.startsWith(peer + '.')) score += 100;
      found.push({ name, ip: a.address, score });
    }
  }
  found.sort((a, b) => b.score - a.score);      // stable: ties keep the system's order
  if (!found.length) return '127.0.0.1';
  if (found.length > 1) log(`network adapters: ${found.map((f) => `${f.name}=${f.ip}`).join(', ')}`);
  log(`using ${found[0].ip} (change with --ip <address>, or --peer <3DS or TV address> to match its subnet)`);
  return found[0].ip;
}

// ---- the Cast "device": one app slot, any number of sender connections ----
const senders = new Map();            // socket id -> { socket, nextRequest }
let nextSocket = 1, nextWeb = 1;
let app = null;                       // { appId, sessionId, transportId } while a receiver app is "running"
const pages = new Set();              // WebSocket connections of browser tabs showing the receiver page
const stats = { frames: 0, bytes: 0, msgs: 0 };

function sendTo(entry, source, dest, ns, obj) {
  if (entry.socket.destroyed) return;
  entry.socket.write(encode({ source, dest, ns, payload: typeof obj === 'string' ? obj : JSON.stringify(obj) }));
}
function receiverStatus(requestId) {
  const status = {
    isActiveInput: true,
    volume: { controlType: 'attenuation', level: 1, muted: false, stepInterval: 0.05 },
  };
  if (app) {
    status.applications = [{
      appId: app.appId, appType: 'WEB', displayName: '3DS Cast (PC dev)', iconUrl: '', isIdleScreen: false,
      launchedFromCloud: false, namespaces: [{ name: NS.custom }], sessionId: app.sessionId, statusText: '',
      transportId: app.transportId, universalAppId: app.appId,
    }];
  }
  return { requestId, status, type: 'RECEIVER_STATUS' };
}
function toPages(obj) { const text = JSON.stringify(obj); for (const ws of pages) if (ws.readyState === 1) ws.send(text); }

function onSenderMessage(entry, id, m, wire) {
  entry.lastSource = m.source;
  stats.msgs++;
  let json = null;
  try { json = JSON.parse(m.payload); } catch (_) { /* custom payloads are JSON too, but be tolerant */ }
  const type = json && json.type;
  if (m.ns === NS.connection) {
    if (type === 'CONNECT') log(`sender ${id}: CONNECT ${m.source} -> ${m.dest}`);
    else if (type === 'CLOSE') log(`sender ${id}: CLOSE ${m.source} -> ${m.dest}`);
    return;
  }
  if (m.ns === NS.heartbeat) {
    if (type === 'PING') sendTo(entry, m.dest, m.source, NS.heartbeat, { type: 'PONG' });
    return;
  }
  if (m.ns === NS.receiver) {
    const requestId = json && json.requestId;
    if (type === 'GET_STATUS') {
      sendTo(entry, 'receiver-0', m.source, NS.receiver, receiverStatus(requestId));
    } else if (type === 'LAUNCH') {
      if (!app) {
        app = { appId: json.appId, sessionId: crypto.randomUUID(), transportId: `web-${nextWeb++}` };
        log(`LAUNCH ${app.appId}: session ${app.sessionId.slice(0, 8)}, transport ${app.transportId}`);
        toPages({ ctl: 'launch' });
      } else log(`LAUNCH ${json.appId}: already running, reporting it (like a real TV)`);
      sendTo(entry, 'receiver-0', m.source, NS.receiver, receiverStatus(requestId));
    } else if (type === 'STOP') {
      log('STOP: receiver app closed');
      const old = app; app = null;
      toPages({ ctl: 'reset' });                       // the page reloads: a fresh receiver, as after a real STOP
      for (const e of senders.values()) {
        if (old) sendTo(e, old.transportId, 'sender-0', NS.connection, { type: 'CLOSE' });
        sendTo(e, 'receiver-0', 'sender-0', NS.receiver, receiverStatus(requestId));
      }
    } else log(`receiver namespace: unhandled ${type}`);
    return;
  }
  if (m.ns === NS.custom) {
    if (!app || m.dest !== app.transportId) return;
    if (type === 'FRAME' || type === 'FRAMEPART' || type === 'RECT') { stats.frames++; stats.bytes += wire; }
    toPages({ ctl: 'cast', ns: m.ns, senderId: `${id}:${m.source}`, data: json !== null ? json : m.payload });
  }
}

// The direct port: length-prefixed JSON messages (the pictures) from a 3DS that is also connected on the Cast port.
// They are passed to the pages like the ones from the Cast port, marked with "via":"direct" for the page's panel.
const MAX_DIRECT_MESSAGE = 4 * 1024 * 1024;
function startDirectServer() {
  const server = net.createServer((socket) => {
    socket.setNoDelay(true);
    let pending = Buffer.alloc(0);
    log(`direct connection from ${socket.remoteAddress}`);
    socket.on('data', (chunk) => {
      pending = pending.length ? Buffer.concat([pending, chunk]) : chunk;
      while (pending.length >= 4) {
        const length = pending.readUInt32BE(0);
        if (length === 0 || length > MAX_DIRECT_MESSAGE) { socket.destroy(); return; }
        if (pending.length < 4 + length) break;
        const text = pending.subarray(4, 4 + length).toString('utf8');
        pending = pending.subarray(4 + length);
        let json; try { json = JSON.parse(text); } catch (_) { socket.destroy(); return; }
        stats.msgs++;
        if (json.type === 'FRAME' || json.type === 'FRAMEPART' || json.type === 'RECT') { stats.frames++; stats.bytes += length; }
        json.via = 'direct';
        // the page answers to the sender it knows: the one on the Cast port with the same address
        let senderId = '';
        for (const [sid, entry] of senders) {
          if (entry.socket.remoteAddress === socket.remoteAddress) senderId = `${sid}:${entry.lastSource || 'sender-0'}`;
        }
        toPages({ ctl: 'cast', ns: NS.custom, senderId, data: json });
      }
    });
    socket.on('error', (e) => log(`direct ${socket.remoteAddress}: ${e.message}`));
    socket.on('close', () => log(`direct connection from ${socket.remoteAddress} closed`));
  });
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(opts.direct, '0.0.0.0', () => resolve(server));
  });
}

function startCastServer(cert) {
  // ChaCha20-Poly1305 first, as a Google TV prefers it for a client that lists it first: the 3DS has no AES instructions, so
  // software AES-GCM costs it much more CPU time. (A server picks by its own order, so it has to be set here; --aes brings
  // the default order back to compare.)
  const cipherOrder = opts.aes ? {} : { ciphers: 'ECDHE-RSA-CHACHA20-POLY1305:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384', honorCipherOrder: true };
  const server = tls.createServer({ key: cert.private, cert: cert.cert, minVersion: 'TLSv1.2', ...cipherOrder }, (socket) => {
    const id = nextSocket++;
    const entry = { socket };
    senders.set(id, entry);
    socket.setNoDelay(true);
    log(`sender ${id} connected from ${socket.remoteAddress} (${socket.getProtocol()} ${socket.getCipher().name})`);
    const framer = new Framer((m, len) => onSenderMessage(entry, id, m, len));
    socket.on('data', (chunk) => { try { framer.push(chunk); } catch (e) { log(`sender ${id}: ${e.message}`); socket.destroy(); } });
    socket.on('error', (e) => log(`sender ${id}: ${e.message}`));
    socket.on('close', () => { senders.delete(id); log(`sender ${id} disconnected`); });
  });
  server.on('tlsClientError', (e) => log(`TLS error: ${e.message}`));
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(opts.cast, '0.0.0.0', () => resolve(server));
  });
}

function startHttp() {
  const server = http.createServer((req, res) => {
    const url = req.url.split('?')[0];
    if (url === '/status') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ app, senders: senders.size, pages: pages.size, ...stats }));
      return;
    }
    // The built page: dist/index.html and the files it refers to (nothing outside dist/).
    const file = path.join(opts.page, url === '/' ? 'index.html' : url);
    if (!file.startsWith(opts.page + path.sep)) { res.writeHead(403); res.end('forbidden'); return; }
    fs.readFile(file, (err, data) => {
      if (err) { res.writeHead(404); res.end(`not found (is the page built? npm run build): ${url}`); return; }
      res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-store' });
      res.end(data);
    });
  });
  const wss = new WebSocketServer({ server, path: '/dev' });
  wss.on('connection', (ws) => {
    pages.add(ws);
    log(`browser page connected (${pages.size})`);
    ws.on('message', (text) => {                       // page -> sender: sendCustomMessage()
      let m; try { m = JSON.parse(text); } catch (_) { return; }
      if (m.ctl !== 'send') return;
      const [sid, source] = String(m.senderId).split(':');
      const entry = senders.get(Number(sid));
      // the page's HELLO tells the 3DS where to send the pictures without TLS
      if (m.data && m.data.type === 'HELLO' && opts.direct) m.data = { ...m.data, direct: opts.direct };
      if (entry && app) sendTo(entry, app.transportId, source || 'sender-0', m.ns, m.data);
    });
    ws.on('close', () => { pages.delete(ws); log(`browser page closed (${pages.size})`); });
  });
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(opts.http, '0.0.0.0', () => resolve(server));
  });
}

async function main() {
  const ip = pickIp();
  const id = crypto.createHash('md5').update(os.hostname() + ip).digest('hex');
  const pems = await selfsigned.generate([{ name: 'commonName', value: opts.name }],
    { keySize: 2048, algorithm: 'sha256', days: 365, extensions: [{ name: 'basicConstraints', cA: true }] });
  const castServer = await startCastServer(pems);
  const httpServer = await startHttp();
  let directServer = null;
  if (opts.direct) {
    try { directServer = await startDirectServer(); } catch (e) { log(`direct port ${opts.direct}: ${e.message} (going on without it)`); }
  }
  log(`n3ds-cast receiver v${version}`);
  if (directServer) log(`direct picture port ${opts.direct} (plain TCP; if the 3DS cannot reach it, allow it in the firewall, or it falls back to the Cast port)`);
  log(`Cast device "${opts.name}" listening on ${ip}:${opts.cast} (TLS), page at http://localhost:${opts.http}`);
  let mdns = null;
  if (opts.mdns) mdns = await startMdns({ ip, friendlyName: opts.name, port: opts.cast, id, log });
  log('On the 3DS: open the app, pick this device (or type ' + ip + ') and start streaming. Open the page in a browser first.');
  const bye = () => { if (mdns) mdns.stop(); castServer.close(); httpServer.close(); if (directServer) directServer.close(); setTimeout(() => process.exit(0), 300); };
  process.on('SIGINT', bye); process.on('SIGTERM', bye);
  setInterval(() => {
    if (stats.msgs) log(`stats: ${stats.frames} frame messages, ${(stats.bytes / 1024).toFixed(0)} KB, ${senders.size} sender(s), ${pages.size} page(s)`);
  }, 30000).unref();
}
main().catch((e) => { console.error(e); process.exit(1); });
