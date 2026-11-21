// Drives the real helper exactly as Floral does: stdio line protocol -> control channel -> server
const { spawn } = require('node:child_process');
const path = require('node:path');

const helperDir = process.argv[2] || path.join(process.env.TEMP, 'sc-chk3', 'together');
const HOST = '47.108.201.112';

const child = spawn(process.env.NODE_BIN || 'node', ['together-server.js'], {
  cwd: helperDir, stdio: ['pipe', 'pipe', 'pipe'],
});

let buf = '';
const waiters = [];
child.stdout.on('data', (d) => {
  buf += d.toString();
  let nl;
  while ((nl = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, nl);
    buf = buf.slice(nl + 1);
    if (!line.trim()) continue;
    let ev;
    try { ev = JSON.parse(line); } catch { continue; }
    const w = waiters.shift();
    if (w) w(ev); else console.log('  [push]', line.slice(0, 120));
  }
});
child.stderr.on('data', (d) => process.stderr.write('  [helper] ' + d.toString()));

const next = () => new Promise((r) => waiters.push(r));
const send = (o) => child.stdin.write(JSON.stringify(o) + '\n');

const fails = { n: 0 };
function check(label, cond, extra) {
  console.log((cond ? 'PASS ' : 'FAIL ') + label + (extra ? '  ' + extra : ''));
  if (!cond) fails.n++;
}

(async () => {
  send({ cmd: 'ctl-open', id: 1, relays: [{ host: HOST, port: 443 }] });
  const open = await next();
  check('helper opens the control channel', open.ev === 'ctl-open', JSON.stringify(open).slice(0, 60));

  send({ cmd: 'ctl-send', text: JSON.stringify({ t: 'pubkey' }) });
  const pk = await next();
  check('server answers pubkey', pk.ev === 'ctl-message' && pk.text.includes('"pubkey"'), pk.text.slice(0, 40));

  send({ cmd: 'ctl-send', text: JSON.stringify({ t: 'hello', id: 'ZZZZZZZZ', secret: 'x' }) });
  const auth = await next();
  check('bad identity rejected', auth.text.includes('"auth"'), auth.text.slice(0, 40));

  child.stdin.end();
  await new Promise((r) => setTimeout(r, 400));
  console.log(fails.n === 0 ? '\nALL PASS' : `\n${fails.n} FAILED`);
  process.exit(fails.n === 0 ? 0 : 1);
})().catch((e) => { console.error('ERROR', e.message); process.exit(1); });
