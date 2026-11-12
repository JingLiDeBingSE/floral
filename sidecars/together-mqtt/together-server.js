/*
 * Floral 一起听：EMQX 中继（本地辅助进程）
 *
 * 为什么不直接在 C++ 里讲 MQTT：
 *   TLS + MQTT 帧解析在 Win32 下要自己写 SChannel 握手，几百行且容易错。
 *   而 exe 里本来就带着 Node 运行时（网易云/QQ 的 sidecar 在用），
 *   直接让 Node 讲 MQTT，C++ 只跟本地 127.0.0.1 说 HTTP，简单得多。
 *
 * 用法：node together-server.js
 *   env: FLORAL_MQTT_HOST / FLORAL_MQTT_PORT / FLORAL_MQTT_USER / FLORAL_MQTT_PASS
 *
 * HTTP：
 *   POST /open                 开房 → { code }
 *   POST /join?code=XXXX       加入 → { ok }
 *   GET  /state                取对方状态 → { peer, song, src, ti, ar, pos, play, t }
 *   POST /push?data=<json>     推送自己的状态
 *   POST /close                断开
 *   GET  /health               存活探测
 */
const http = require('node:http');
const crypto = require('node:crypto');
const mqtt = require('mqtt');

const HOST = process.env.FLORAL_MQTT_HOST || 'k9bf6e3a.ala.cn-shenzhen.emqxsl.cn';
const PORT = Number(process.env.FLORAL_MQTT_PORT || 8883);
const USER = process.env.FLORAL_MQTT_USER || '';
const PASS = process.env.FLORAL_MQTT_PASS || '';
const LISTEN = Number(process.env.FLORAL_LISTEN_PORT || 4100);

let client = null;
let roomCode = '';
let peer = null;            // { nick, song, src, ti, ar, pos, play, t }
let peerNick = '';
let skipFlag = false;      // 对方请求跳过


/* 房间码 → 密钥：AES-256-GCM，broker 只看得到密文 */
const keyOf = (code) => crypto.createHash('sha256').update('floral-together|' + code).digest();

function encrypt(code, text) {
  const iv = crypto.randomBytes(12);
  const c = crypto.createCipheriv('aes-256-gcm', keyOf(code), iv);
  const enc = Buffer.concat([c.update(text, 'utf8'), c.final()]);
  return iv.toString('base64') + '.' + c.getAuthTag().toString('base64') + '.' + enc.toString('base64');
}

function decrypt(code, blob) {
  try {
    const [a, b, c2] = blob.split('.');
    if (!a || !b || !c2) return null;
    const d = crypto.createDecipheriv('aes-256-gcm', keyOf(code), Buffer.from(a, 'base64'));
    d.setAuthTag(Buffer.from(b, 'base64'));
    return Buffer.concat([d.update(Buffer.from(c2, 'base64')), d.final()]).toString('utf8');
  } catch { return null; }       // 拿错房间码 / 别人灌的垃圾 → 直接丢
}

const topicOf = (code) => 'floral/' + code;

function newCode() {
  const abc = 'abcdefghjkmnpqrstuvwxyz23456789';
  let s = '';
  for (const b of crypto.randomBytes(12)) s += abc[b % abc.length];
  return s;
}

function connectBroker(code, nick) {
  return new Promise((resolve, reject) => {
    if (client) { try { client.end(true); } catch {} client = null; }
    peer = null;
    skipFlag = false;
    roomCode = code;
    const c = mqtt.connect(`mqtts://${HOST}:${PORT}`, {
      username: USER, password: PASS,
      clientId: 'floral_' + crypto.randomBytes(6).toString('hex'),
      connectTimeout: 8000, reconnectPeriod: 3000, clean: true,
    });
    let done = false;
    c.on('connect', () => {
      c.subscribe(topicOf(code), { qos: 0 }, (err) => {
        if (err) { if (!done) { done = true; reject(err); } return; }
        client = c;
        if (!done) { done = true; resolve(); }
      });
    });
    c.on('message', (_t, buf) => {
      const obj = decrypt(code, buf.toString());
      if (!obj) return;
      try {
        const m = JSON.parse(obj);
        if (m.n === nick) return;              // 自己发的不要
        if (m.skip) { skipFlag = true; return; }  // 跳过请求
        peer = m;
        peerNick = m.n || '朋友';
      } catch {}
    });
    c.on('error', (e) => { if (!done) { done = true; reject(e); } });
    setTimeout(() => { if (!done) { done = true; reject(new Error('连接 broker 超时')); } }, 12000);
  });
}

function json(res, obj, code = 200) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8' });
  res.end(body);
}

const server = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://127.0.0.1');
  const q = u.searchParams;
  let body = '';
  req.on('data', (d) => (body += d));
  req.on('end', async () => {
    try {
      if (u.pathname === '/health') return json(res, { ok: true });
      if (u.pathname === '/open') {
        const nick = q.get('nick') || '房主';
        const code = newCode();
        await connectBroker(code, nick);
        console.log('[open] code=' + code);
        return json(res, { code, ok: true });
      }
      if (u.pathname === '/join') {
        const code = (q.get('code') || '').trim().toLowerCase();
        const nick = q.get('nick') || '朋友';
        if (!code || code.length < 6) return json(res, { ok: false, err: '房间码不对' }, 400);
        await connectBroker(code, nick);
        console.log('[join] code=' + code);
        return json(res, { ok: true });
      }
      if (u.pathname === '/state') {
        const skip = skipFlag;
        skipFlag = false;
        return json(res, { peer: peer ? peerNick : '', skip, data: peer });
      }
      if (u.pathname === '/push') {
        if (!client) return json(res, { ok: false }, 400);
        const payload = q.get('data') || '{}';
        client.publish(topicOf(roomCode), encrypt(roomCode, payload), { qos: 0 });
        return json(res, { ok: true });
      }
      if (u.pathname === '/close') {
        if (client) { try { client.end(true); } catch {} client = null; }
        roomCode = ''; peer = null; peerNick = '';
        return json(res, { ok: true });
      }
      json(res, { ok: false, err: 'unknown' }, 404);
    } catch (e) {
      console.log('[err]', e.message);
      json(res, { ok: false, err: e.message }, 500);
    }
  });
});

server.listen(LISTEN, '127.0.0.1', () => {
  console.log('together-server ready on 127.0.0.1:' + LISTEN + ' broker=' + HOST + ':' + PORT);
});
