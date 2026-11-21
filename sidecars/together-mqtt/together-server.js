/*
 * Listen-together relay client
 *
 * Floral starts this script with stdin and stdout connected to pipes and exchanges one JSON object per line.
 * The script opens a TCP connection to the first reachable relay server, compresses and encrypts, and passes
 * on every message of the room in the order it arrives. Logs go to stderr. The script exits when stdin closes.
 *
 * Server protocol (see tools/relay-server.cpp):
 *   client -> server, once:   {"room":"<hex>"}\n
 *   server -> client, once:   {"ok":true}\n   or {"ok":false,"err":"..."}\n
 *   both directions after:    uint32 big-endian length, then that many payload bytes
 *
 * Commands from Floral:
 *   {"cmd":"join","id":1,"code":"...","relays":[{"host":"1.2.3.4","port":443}, ...]}   empty code: new room
 *   {"cmd":"leave"}
 *   {"cmd":"send","text":"..."}
 * Events, each with the id of the join it belongs to:
 *   {"ev":"joined","id":1,"text":"<room code>"}   {"ev":"failed","id":1,"text":"<reason>"}
 *   {"ev":"down","id":1}   {"ev":"up","id":1}      {"ev":"message","id":1,"text":"..."}
 * A failure after joined means the connection could not be restored and has been closed.
 */
'use strict';
const crypto = require('node:crypto');
const net = require('node:net');
const readline = require('node:readline');
const zlib = require('node:zlib');

const CODE_ALPHABET = 'abcdefghjkmnpqrstuvwxyz23456789';
const CODE_LENGTH = 12;
const JOIN_TIMEOUT_MS = 15000;   // for the whole relay list
const RELAY_TIMEOUT_MS = 5000;   // per relay before moving on to the next
const BACKLOG_BYTES = 256 * 1024;
const MAX_FRAME = 512 * 1024;

let room = null;

const log = (...args) => console.error('[together]', ...args);
const emit = (event) => process.stdout.write(JSON.stringify(event) + '\n');
const sha256Hex = (text) => crypto.createHash('sha256').update(text).digest('hex');

function newCode() {
  let code = '';
  for (let i = 0; i < CODE_LENGTH; i++) code += CODE_ALPHABET[crypto.randomInt(CODE_ALPHABET.length)];
  return code;
}

// Payload: 12-byte IV, 16-byte GCM tag, ciphertext of the deflated text
function encrypt(key, text) {
  const iv = crypto.randomBytes(12);
  const cipher = crypto.createCipheriv('aes-256-gcm', key, iv);
  const body = Buffer.concat([cipher.update(zlib.deflateRawSync(text)), cipher.final()]);
  return Buffer.concat([iv, cipher.getAuthTag(), body]);
}

function decrypt(key, payload) {
  try {
    const decipher = crypto.createDecipheriv('aes-256-gcm', key, payload.subarray(0, 12), { authTagLength: 16 });
    decipher.setAuthTag(payload.subarray(12, 28));
    return zlib.inflateRawSync(Buffer.concat([decipher.update(payload.subarray(28)), decipher.final()])).toString('utf8');
  } catch {
    return null;
  }
}

function frame(payload) {
  const head = Buffer.alloc(4);
  head.writeUInt32BE(payload.length, 0);
  return Buffer.concat([head, payload]);
}

function relayList(cmd) {
  if (Array.isArray(cmd.relays) && cmd.relays.length) return cmd.relays;
  return [];
}

function join(cmd) {
  leave();
  const code = String(cmd.code || '').trim().toLowerCase() || newCode();
  const relays = relayList(cmd);
  const conn = {
    id: cmd.id,
    code,
    room: sha256Hex('floral-room|' + code).slice(0, 40),   // the server only sees this
    key: crypto.createHash('sha256').update('floral-key|' + code).digest(),
    joined: false,
    attempt: -1,
    timer: null,
    failed: [],
    socket: null,
    buffered: Buffer.alloc(0),
  };
  room = conn;

  const report = (ev, text) => { if (room === conn) emit({ ev, id: conn.id, text }); };
  const fail = (reason) => {
    if (room !== conn) return;
    log('failed:', reason);
    report('failed', reason);
    room = null;
    clearTimeout(conn.timer);
    if (conn.socket) conn.socket.destroy();
  };

  setTimeout(() => {
    if (!conn.joined && room === conn) fail('timed out');
  }, JOIN_TIMEOUT_MS).unref();

  const nextRelay = () => {
    if (room !== conn || conn.joined) return;
    conn.attempt++;
    if (conn.attempt >= relays.length) {
      fail(conn.failed.length ? conn.failed.join('; ') : 'no relay reachable');
      return;
    }
    const r = relays[conn.attempt];
    log(`trying ${r.host}:${r.port}`);

    const socket = net.connect({ host: r.host, port: r.port });
    conn.socket = socket;
    conn.buffered = Buffer.alloc(0);
    let greeted = false;

    clearTimeout(conn.timer);
    conn.timer = setTimeout(() => {
      if (room !== conn || conn.socket !== socket) return;
      conn.failed.push(`${r.host}: no answer`);
      socket.destroy();
      nextRelay();
    }, RELAY_TIMEOUT_MS);
    conn.timer.unref();

    socket.setNoDelay(true);
    socket.on('connect', () => {
      socket.write(JSON.stringify({ room: conn.room }) + '\n');
    });

    socket.on('data', (chunk) => {
      if (room !== conn || conn.socket !== socket) return;
      conn.buffered = Buffer.concat([conn.buffered, chunk]);

      if (!greeted) {
        const nl = conn.buffered.indexOf(0x0a);
        if (nl < 0) return;
        const line = conn.buffered.subarray(0, nl).toString('utf8');
        conn.buffered = conn.buffered.subarray(nl + 1);
        greeted = true;
        clearTimeout(conn.timer);

        let reply = null;
        try { reply = JSON.parse(line); } catch { reply = null; }
        if (!reply || reply.ok !== true) {
          conn.failed.push(`${r.host}: ${(reply && reply.err) || 'refused'}`);
          socket.destroy();
          nextRelay();
          return;
        }
        if (!conn.joined) {
          conn.joined = true;
          log(`joined via ${r.host}:${r.port}`);
          report('joined', code);
        } else {
          report('up');
        }
      }

      for (;;) {
        if (conn.buffered.length < 4) return;
        const len = conn.buffered.readUInt32BE(0);
        if (len > MAX_FRAME) { socket.destroy(); return; }
        if (conn.buffered.length < 4 + len) return;
        const payload = conn.buffered.subarray(4, 4 + len);
        conn.buffered = conn.buffered.subarray(4 + len);
        const text = decrypt(conn.key, payload);
        if (text !== null) report('message', text);
      }
    });

    socket.on('close', () => {
      if (room !== conn || conn.socket !== socket) return;
      conn.socket = null;
      if (conn.joined) {
        conn.joined = false;
        report('down');
        // The room may have been dropped by the server: reconnect from the first relay
        conn.attempt = -1;
        setTimeout(() => { if (room === conn) nextRelay(); }, 1000).unref();
      } else {
        nextRelay();
      }
    });

    socket.on('error', (err) => {
      if (room !== conn || conn.socket !== socket) return;
      conn.failed.push(`${r.host}: ${err.message}`);
    });
  };

  nextRelay();
}

function leave() {
  if (!room) return;
  clearTimeout(room.timer);
  if (room.socket) room.socket.destroy();
  room = null;
}

function send(text) {
  if (!room || !room.joined || !room.socket) return;
  if (room.socket.writableLength > BACKLOG_BYTES) {
    log('upload backed up, message dropped');
    return;
  }
  room.socket.write(frame(encrypt(room.key, text)));
}

/* ── control channel (accounts / friends) ─────────────────────────────
   A second, independent TCP connection to the same relay server. Its first line chooses the
   protocol on the server side, so the app decides what to send first. */

let ctl = null;

function ctlClose() {
  if (!ctl) return;
  ctl.closed = true;
  clearTimeout(ctl.retry);
  if (ctl.sock) ctl.sock.destroy();
  ctl = null;
}

function ctlOpen(cmd) {
  ctlClose();
  const relays = Array.isArray(cmd.relays) ? cmd.relays : [];
  const st = { id: cmd.id, attempt: -1, sock: null, connected: false, closed: false,
               retry: null, buffered: Buffer.alloc(0) };
  ctl = st;
  if (!relays.length) { emit({ ev: 'ctl-down', id: st.id, text: 'no relay' }); return; }

  const report = (ev, text) => { if (ctl === st) emit({ ev, id: st.id, text }); };

  const attempt = () => {
    if (ctl !== st || st.closed) return;
    st.attempt = (st.attempt + 1) % relays.length;
    const r = relays[st.attempt];
    const sock = net.connect({ host: r.host, port: r.port });
    st.sock = sock;
    st.buffered = Buffer.alloc(0);
    st.sock.setNoDelay(true);

    sock.on('connect', () => {
      if (ctl !== st) return;
      st.connected = true;
      log(`control connected via ${r.host}:${r.port}`);
      report('ctl-open', '');
    });

    sock.on('data', (chunk) => {
      if (ctl !== st) return;
      st.buffered = Buffer.concat([st.buffered, chunk]);
      for (;;) {
        const nl = st.buffered.indexOf(0x0a);
        if (nl < 0) break;
        const line = st.buffered.subarray(0, nl).toString('utf8');
        st.buffered = st.buffered.subarray(nl + 1);
        if (line.length) report('ctl-message', line);
      }
    });

    sock.on('error', () => {});
    sock.on('close', () => {
      if (ctl !== st) return;
      st.connected = false;
      st.sock = null;
      report('ctl-down', '');
      if (st.closed) return;
      st.retry = setTimeout(attempt, 2000).unref();
    });
  };

  attempt();
}

function ctlSend(text) {
  if (!ctl || !ctl.connected || !ctl.sock) return;
  if (ctl.sock.writableLength > BACKLOG_BYTES) return;
  ctl.sock.write(text + '\n');
}

const input = readline.createInterface({ input: process.stdin });
input.on('line', (line) => {
  let cmd;
  try { cmd = JSON.parse(line); } catch { return; }
  if (cmd.cmd === 'join') join(cmd);
  else if (cmd.cmd === 'leave') leave();
  else if (cmd.cmd === 'send') send(cmd.text);
  else if (cmd.cmd === 'ctl-open') ctlOpen(cmd);
  else if (cmd.cmd === 'ctl-send') ctlSend(cmd.text);
  else if (cmd.cmd === 'ctl-close') ctlClose();
});
input.on('close', () => { leave(); ctlClose(); });
