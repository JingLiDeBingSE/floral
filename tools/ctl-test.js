// End-to-end test of the Floral control protocol (accounts + friends)
const net = require('node:net');
const crypto = require('node:crypto');
const fs = require('node:fs');

const PORT = Number(process.env.CTL_PORT || 18831);
const JWK_PATH = process.env.CTL_JWK || (process.env.TEMP + '/relay-srv/pub.jwk');
const jwk = JSON.parse(fs.readFileSync(JWK_PATH, 'utf8'));
const pub = crypto.createPublicKey({ key: jwk, format: 'jwk' });

function conn() {
  const s = net.connect({ host: '127.0.0.1', port: PORT });
  let buf = Buffer.alloc(0);
  const waiters = [];
  const pushes = [];
  s.on('data', (c) => {
    buf = Buffer.concat([buf, c]);
    let nl;
    while ((nl = buf.indexOf(0x0a)) >= 0) {
      const line = buf.subarray(0, nl).toString();
      buf = buf.subarray(nl + 1);
      const obj = JSON.parse(line);
      const w = waiters.shift();
      if (w) w(obj); else pushes.push(obj);
    }
  });
  s.on('error', () => {});
  return {
    send: (o) => s.write(JSON.stringify(o) + '\n'),
    next: () => new Promise((r) => waiters.push(r)),
    takePush: () => pushes.shift() || null,
    close: () => s.destroy(),
  };
}

function blob(platform, uid, token, name) {
  const msg = Buffer.from(JSON.stringify({ platform, uid, token, name }), 'utf8');
  return crypto.publicEncrypt(
    { key: pub, padding: crypto.constants.RSA_PKCS1_OAEP_PADDING, oaepHash: 'sha1' }, msg).toString('base64');
}

const stamp = Date.now().toString().slice(-6);
const uidA = 'a' + stamp, uidB = 'b' + stamp;
let fails = 0;
function check(label, cond, extra) {
  console.log((cond ? 'PASS ' : 'FAIL ') + label + (extra ? '  ' + extra : ''));
  if (!cond) fails++;
}

(async () => {
  const A = conn(), B = conn();

  A.send({ t: 'bind', blob: blob('kugou', uidA, 'tokA', 'User A') });
  const a = await A.next();
  check('bind creates identity', a.t === 'bound' && !!a.id && !!a.secret, a.id);

  B.send({ t: 'bind', blob: blob('kugou', uidB, 'tokB', 'User B') });
  const b = await B.next();
  check('second identity is different', b.t === 'bound' && b.id !== a.id, b.id);

  const C = conn();
  C.send({ t: 'bind', blob: blob('kugou', uidA, 'tokA', 'User A') });
  const c = await C.next();
  check('same platform account returns same identity', c.id === a.id && c.secret === a.secret);
  C.close();

  A.send({ t: 'hello', id: a.id, secret: a.secret });
  const ha = await A.next();
  check('hello ok', ha.t === 'ok' && ha.id === a.id);

  B.send({ t: 'hello', id: b.id, secret: b.secret });
  await B.next();

  A.send({ t: 'add', code: b.id });
  const add = await A.next();
  check('add sends a request', add.t === 'add' && add.ok === true);

  const req = await B.next();
  check('target receives the request', req.t === 'request' && req.from === a.id, req.name);

  B.send({ t: 'accept', uid: a.id });
  const acc = await B.next();
  check('accepter gets an ack', acc.t === 'accepted' && acc.ok === true);

  const friendOnA = await A.next();
  check('requester learns the new friend', friendOnA.t === 'friend' && friendOnA.id === b.id);

  A.send({ t: 'invite', uid: b.id, room: 'roomXYZ' });
  const inv = await A.next();
  check('invite accepted by server', inv.t === 'invited' && inv.ok === true);

  const push = await B.next();
  check('invite delivered to friend', push.t === 'invite' && push.room === 'roomXYZ', push.name);

  // Wrong secret must be refused
  const D = conn();
  D.send({ t: 'hello', id: a.id, secret: 'deadbeef' });
  const bad = await D.next();
  check('bad secret rejected', bad.t === 'err' && bad.code === 'auth');
  D.close();

  A.close(); B.close();
  await new Promise((r) => setTimeout(r, 300));
  console.log(fails === 0 ? '\nALL PASS' : `\n${fails} FAILED`);
  process.exit(fails === 0 ? 0 : 1);
})().catch((e) => { console.error('ERROR', e.message); process.exit(1); });
