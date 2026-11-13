'use strict';
// Minimal mDNS responder that makes this PC show up as a Google Cast device ("_googlecast._tcp") so the 3DS
// discovery (3ds/app/discovery.c) lists it. It answers PTR/SRV/TXT/A questions with the full record set and
// announces itself regularly. Questions from a legacy unicast resolver (source port != 5353, which is what the
// 3DS uses) are answered by unicast to the asker, as RFC 6762 section 6.7 requires.
const dgram = require('dgram');

const MDNS_ADDR = '224.0.0.251', MDNS_PORT = 5353;
const SERVICE = '_googlecast._tcp.local';

function nameBytes(name) {
  const parts = name.split('.');
  const out = [];
  for (const p of parts) { const b = Buffer.from(p); out.push(Buffer.from([b.length]), b); }
  out.push(Buffer.from([0]));
  return Buffer.concat(out);
}
function u16(n) { const b = Buffer.alloc(2); b.writeUInt16BE(n); return b; }
function u32(n) { const b = Buffer.alloc(4); b.writeUInt32BE(n); return b; }
function record(name, type, ttl, data, flush) {
  return Buffer.concat([nameBytes(name), u16(type), u16(flush ? 0x8001 : 0x0001), u32(ttl), u16(data.length), data]);
}

// Reads a (possibly compressed) name; returns [name, nextOffset] or null.
function readName(buf, at) {
  const labels = []; let next = -1, hops = 0;
  for (;;) {
    if (at >= buf.length || hops > 20) return null;
    const n = buf[at];
    if ((n & 0xc0) === 0xc0) { if (next < 0) next = at + 2; at = ((n & 0x3f) << 8) | buf[at + 1]; hops++; continue; }
    if (n === 0) { if (next < 0) next = at + 1; break; }
    labels.push(buf.toString('latin1', at + 1, at + 1 + n).toLowerCase()); at += 1 + n;
  }
  return [labels.join('.'), next];
}

function startMdns({ ip, friendlyName, port, id, log }) {
  const instance = `${friendlyName.replace(/[^A-Za-z0-9]+/g, '-')}-${id.slice(0, 8)}._googlecast._tcp.local`.toLowerCase();
  const host = `${friendlyName.replace(/[^A-Za-z0-9]+/g, '-')}-${id.slice(0, 8)}.local`.toLowerCase();
  const txt = [
    `id=${id}`, `cd=${id}`, 'rm=', 've=05', 'md=3DS Cast PC Receiver', 'ic=/setup/icon.png', `fn=${friendlyName}`,
    'ca=201221', 'st=0', 'bs=000000000000', 'nf=1', 'rs=',
  ].map((s) => Buffer.concat([Buffer.from([Buffer.byteLength(s)]), Buffer.from(s)]));

  function answerPacket(ttl) {
    const recs = [
      record(SERVICE, 12, ttl, nameBytes(instance), false),
      record(instance, 33, ttl, Buffer.concat([u16(0), u16(0), u16(port), nameBytes(host)]), true),
      record(instance, 16, ttl, Buffer.concat(txt), true),
      record(host, 1, ttl, Buffer.from(ip.split('.').map(Number)), true),
    ];
    const head = Buffer.alloc(12);
    head.writeUInt16BE(0x8400, 2); head.writeUInt16BE(recs.length, 6);
    return Buffer.concat([head, ...recs]);
  }

  const socket = dgram.createSocket({ type: 'udp4', reuseAddr: true });
  const send = (buf, addr, p) => socket.send(buf, p, addr, (e) => { if (e) log(`mDNS send failed: ${e.message}`); });

  socket.on('message', (msg, rinfo) => {
    if (msg.length < 12 || (msg[2] & 0x80)) return;           // only questions
    const questions = msg.readUInt16BE(4);
    let at = 12, wanted = false;
    for (let i = 0; i < questions; i++) {
      const r = readName(msg, at); if (!r) return;
      const [name, next] = r; at = next + 4;
      if (name === SERVICE || name === instance || name === host) wanted = true;
    }
    if (!wanted) return;
    const reply = answerPacket(120);
    if (rinfo.port !== MDNS_PORT) { reply.writeUInt16BE(msg.readUInt16BE(0), 0); send(reply, rinfo.address, rinfo.port); }
    else send(reply, MDNS_ADDR, MDNS_PORT);
  });
  socket.on('error', (e) => log(`mDNS socket error: ${e.message}`));

  return new Promise((resolve) => {
    socket.bind(MDNS_PORT, '0.0.0.0', () => {
      try {
        socket.addMembership(MDNS_ADDR, ip);
        socket.setMulticastInterface(ip);
        socket.setMulticastTTL(255);
        socket.setMulticastLoopback(true);
      } catch (e) { log(`mDNS multicast setup failed: ${e.message}`); }
      const announce = () => send(answerPacket(120), MDNS_ADDR, MDNS_PORT);
      announce(); setTimeout(announce, 1000); setTimeout(announce, 3000);
      const timer = setInterval(announce, 10000);
      log(`mDNS: advertising "${friendlyName}" as ${instance} -> ${ip}:${port}`);
      resolve({
        stop() {
          clearInterval(timer);
          try { send(answerPacket(0), MDNS_ADDR, MDNS_PORT); } catch (_) { /* goodbye is best effort */ }
          setTimeout(() => { try { socket.close(); } catch (_) { /* already closed */ } }, 100);
        },
      });
    });
  });
}

module.exports = { startMdns };
