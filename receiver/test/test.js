'use strict';
// End-to-end test of the PC receiver: starts the real server, plays the 3DS with FakeSender (CASTV2 over TLS)
// and a browser page with a WebSocket client, and checks what each side sees.
const { spawn } = require('child_process');
const dgram = require('dgram');
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const WebSocket = require('ws');
const { FakeSender, frameMessages } = require('./fake-sender');
const { NS } = require('../castv2');
const { jpegOfSize } = require('./sample-jpeg');

const CAST = 18009, HTTP = 18080, DIRECT = 18010, APP = '01B7A380';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
  const server = spawn(process.execPath, [path.join(__dirname, '..', 'server.js'), '--cast', String(CAST), '--http', String(HTTP), '--direct', String(DIRECT), '--name', 'Test Receiver'],
    { stdio: ['ignore', 'pipe', 'inherit'] });
  let output = '';
  server.stdout.on('data', (d) => { output += d; });
  try {
    for (let i = 0; i < 100 && !output.includes('mDNS: advertising'); i++) await sleep(100);
    assert(output.includes(':' + CAST + ' (TLS)'), 'server did not start:\n' + output);
    const lanIp = /-> (\d+\.\d+\.\d+\.\d+):/.exec(output)[1];

    // the browser page
    const page = new WebSocket(`ws://127.0.0.1:${HTTP}/dev`);
    const fromTv = [];
    page.on('message', (t) => fromTv.push(JSON.parse(t)));
    await new Promise((r) => page.once('open', r));
    const next = async (test, ms = 3000) => {
      for (let i = 0; i < ms / 20; i++) { const hit = fromTv.find(test); if (hit) return hit; await sleep(20); }
      throw new Error('page never received the expected message');
    };

    // The built page is served, with the script it loads; the script has the PC stand-in and the receiver's version.
    const html = await (await fetch(`http://127.0.0.1:${HTTP}/`)).text();
    const script = /<script type="module"[^>]*src="\.\/([^"]+)"/.exec(html);
    assert(script, 'the served page loads no script');
    const code = await (await fetch(`http://127.0.0.1:${HTTP}/${script[1]}`)).text();
    assert(code.includes('ws://'), 'the served page has no PC stand-in');
    assert(code.includes(require('../package.json').version), 'the served page does not carry the receiver version');

    // 1. launch like the 3DS does; a second launch reports the same running app
    const s = await new FakeSender().connect('127.0.0.1', CAST);
    await sleep(100);
    assert(/TLSv1\.2 ECDHE-RSA-CHACHA20-POLY1305/.test(output), 'the PC receiver should pick ChaCha20-Poly1305 (cheaper for a 3DS without AES instructions):\n' + output);
    s.send(NS.connection, 'receiver-0', { type: 'CONNECT' });
    s.send(NS.receiver, 'receiver-0', { type: 'GET_STATUS', requestId: 1 });
    const idle = await s.waitFor((m) => m.json && m.json.type === 'RECEIVER_STATUS');
    assert(!idle.json.status.applications, 'nothing should be running yet');
    const app = await s.launch(APP);
    assert.strictEqual(app.appId, APP);
    assert(/^web-\d+$/.test(app.transportId));
    await next((m) => m.ctl === 'launch');
    const s2 = await new FakeSender().connect('127.0.0.1', CAST);
    const again = await s2.launch(APP);
    assert.strictEqual(again.sessionId, app.sessionId, 'a running app must be reported, not restarted');
    s2.close();

    // 2. custom messages reach the page; the page answers CAPS with HELLO and the sender receives it
    const caps = await next((m) => m.ctl === 'cast' && m.data.type === 'CAPS');
    assert.strictEqual(caps.ns, NS.custom);
    page.send(JSON.stringify({ ctl: 'send', ns: NS.custom, senderId: caps.senderId, data: { type: 'HELLO', v: 3, ok: true } }));
    const hello = await s.waitFor((m) => m.json && m.json.type === 'HELLO');
    assert.strictEqual(hello.source, app.transportId);
    assert.strictEqual(hello.ns, NS.custom);
    assert.strictEqual(hello.json.direct, DIRECT, 'the HELLO passed on to the 3DS names the direct picture port');

    // 3. heartbeat
    s.send(NS.heartbeat, 'receiver-0', { type: 'PING' });
    await s.waitFor((m) => m.json && m.json.type === 'PONG');

    // 4. frames: small, two screens, and a 100 KB JPEG that must travel as FRAMEPART pieces
    const small = jpegOfSize(2000).toString('base64'), big = jpegOfSize(100000).toString('base64');
    fromTv.length = 0;
    s.sendFrame(small, small);
    const f1 = await next((m) => m.ctl === 'cast' && m.data.type === 'FRAME');
    assert.strictEqual(f1.data.top, small); assert.strictEqual(f1.data.bot, small);
    fromTv.length = 0;
    s.sendFrame(big, small);
    await next((m) => m.ctl === 'cast' && m.data.type === 'FRAME' && m.data.bot === small);
    const parts = fromTv.filter((m) => m.data.type === 'FRAMEPART' && m.data.w === 'top').map((m) => m.data);
    assert.strictEqual(parts.length, 3, 'a 100 KB JPEG is 3 pieces');
    assert.strictEqual(parts.map((p) => p.d).join(''), big, 'pieces must reassemble to the original');
    assert(parts.every((p, i) => p.i === i && p.n === 3 && p.cont === 1), 'every piece is followed by more messages');
    const expected = frameMessages({}, big, small).length;
    assert.strictEqual(fromTv.filter((m) => m.ctl === 'cast').length, expected);

    // 4b. partial updates: RECT messages (a small rectangle and a whole screen as three pieces) reach the page intact
    fromTv.length = 0;
    s.sendRects([{ screen: 'top', x: 48, y: 64, w: 16, h: 16, b64: small }, { screen: 'bot', x: 0, y: 0, w: 320, h: 240, b64: big }]);
    await next((m) => m.ctl === 'cast' && m.data.type === 'RECT' && m.data.i === 2);
    const rects = fromTv.filter((m) => m.ctl === 'cast' && m.data.type === 'RECT').map((m) => m.data);
    assert.strictEqual(rects.length, 4, 'one message for the small rectangle plus three pieces');
    assert.deepStrictEqual(rects.map((r) => r.cont), [1, 1, 1, undefined]);
    assert(rects[0].cfg && !rects[1].cfg, 'cfg only in the first message');
    assert.strictEqual(rects[0].d, small);
    assert.strictEqual(rects.slice(1).map((r) => r.d).join(''), big);

    // 4c. the direct port: length-prefixed JSON from the same address reaches the page, marked "via":"direct", with the
    // sender id of the Cast connection (the page answers to it); two messages in one write, and one in two pieces
    {
      const net = require('net');
      const direct = net.connect(DIRECT, '127.0.0.1');
      await new Promise((r) => direct.once('connect', r));
      const pack = (obj) => { const body = Buffer.from(JSON.stringify(obj)); const head = Buffer.alloc(4); head.writeUInt32BE(body.length); return Buffer.concat([head, body]); };
      fromTv.length = 0;
      const a = pack({ type: 'RECT', s: 'top', x: 0, y: 0, w: 400, h: 240, i: 0, n: 1, d: small });
      const b = pack({ type: 'RECT', s: 'bot', x: 0, y: 0, w: 320, h: 240, i: 0, n: 1, d: big });
      direct.write(Buffer.concat([a, b.subarray(0, 100)]));
      await sleep(50);
      direct.write(b.subarray(100));
      await next((m) => m.ctl === 'cast' && m.data.type === 'RECT' && m.data.s === 'bot');
      const viaDirect = fromTv.filter((m) => m.ctl === 'cast' && m.data.via === 'direct');
      assert.strictEqual(viaDirect.length, 2, 'both direct messages reach the page');
      assert.strictEqual(viaDirect[0].data.d, small); assert.strictEqual(viaDirect[1].data.d, big);
      assert(viaDirect[0].senderId.startsWith(caps.senderId.split(':')[0] + ':'), 'the page can answer to the Cast sender of that address');
      direct.destroy();
    }

    // 5. STOP (what the 3DS does before relaunching): status without the app, CLOSE for the transport, page reset
    fromTv.length = 0;
    const restarted = await s.launch(APP, { restart: true });
    assert.notStrictEqual(restarted.sessionId, app.sessionId, 'restart must give a fresh session');
    await next((m) => m.ctl === 'reset');
    await next((m) => m.ctl === 'launch');
    assert(s.inbox.some((m) => m.json && m.json.type === 'CLOSE' && m.source === app.transportId), 'CLOSE for the old transport');

    // 6. mDNS answers a legacy unicast question exactly like the 3DS asks (PTR, from an ephemeral port)
    const answer = await new Promise((resolve, reject) => {
      const sock = dgram.createSocket('udp4');
      sock.bind(0, lanIp, () => { sock.setMulticastInterface(lanIp); sock.setMulticastLoopback(true); go(); });
      const go = () => {
      const q = Buffer.from([0x4e, 0x33, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]);
      const name = Buffer.concat('_googlecast._tcp.local'.split('.').map((p) => Buffer.concat([Buffer.from([p.length]), Buffer.from(p)])).concat([Buffer.from([0, 0, 12, 0, 1])]));
      sock.on('message', (m) => { if (!(m[2] & 0x80) || !m.includes(Buffer.from('fn=Test Receiver'))) return; sock.close(); resolve(m); });
      sock.send(Buffer.concat([q, name]), 5353, '224.0.0.251');
      setTimeout(() => { try { sock.close(); } catch (e) { /* closed */ } reject(new Error('no mDNS answer (firewall? several network adapters? try --ip)')); }, 3000);
      };
    });
    fs.writeFileSync(path.join(require('os').tmpdir(), 'n3ds-mdns-reply.bin'), answer);
    assert(answer.readUInt16BE(0) === 0x4e33 && (answer[2] & 0x80), 'a legacy unicast answer repeats the question id and is a response');
    assert(answer.includes(Buffer.from('fn=Test Receiver')), 'TXT carries the friendly name');

    page.close(); s.close();
    console.log('PASS: PC receiver: launch/status/STOP, heartbeat, custom messages both ways, frames incl. 3-piece FRAMEPART, fresh session on restart, mDNS legacy-unicast answer');
  } finally {
    server.kill();
  }
}
main().then(() => process.exit(0)).catch((e) => { console.error(e); process.exit(1); });
