/*
 * Listen-together relay helper
 *
 * Floral starts this script with stdin and stdout connected to pipes and exchanges one JSON object per line.
 * The script connects to the MQTT broker, compresses and encrypts, and passes on every message of the room in
 * the order it arrives, the member's own included. Logs go to stderr. The script exits when stdin closes.
 *
 * Commands:
 *   {"cmd":"join","id":1,"code":"...","host":"...","port":8883,"user":"...","pass":"..."}  empty code: new room
 *   {"cmd":"leave"}
 *   {"cmd":"send","text":"..."}
 * Events, each with the id of the join it belongs to:
 *   {"ev":"joined","id":1,"text":"<room code>"}   {"ev":"failed","id":1,"text":"<reason>"}
 *   {"ev":"down","id":1}   {"ev":"up","id":1}      {"ev":"message","id":1,"text":"..."}
 * A failure after joined means the connection could not be restored and has been closed.
 */
'use strict';
const crypto = require('node:crypto');
const readline = require('node:readline');
const zlib = require('node:zlib');
const mqtt = require('mqtt');

const CODE_ALPHABET = 'abcdefghjkmnpqrstuvwxyz23456789';
const CODE_LENGTH = 12;
const JOIN_TIMEOUT_MS = 15000;

// A member receives its own messages back from the broker, so those not back yet are still being uploaded.
// New messages are dropped while they exceed BACKLOG_BYTES, which bounds the delay at BACKLOG_BYTES / bandwidth
const BACKLOG_BYTES = 128 * 1024;
const ECHO_LOST_MS = 30000;   // an own message not back after this long counts as lost

let room = null;

const log = (...args) => console.error('[together]', ...args);
const emit = (event) => process.stdout.write(JSON.stringify(event) + '\n');
const sha256 = (text) => crypto.createHash('sha256').update(text).digest();

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

function join(cmd) {
  leave();
  const code = String(cmd.code).trim().toLowerCase() || newCode();
  const conn = {
    id: cmd.id,
    topic: 'floral/' + sha256('floral-topic|' + code).toString('hex').slice(0, 32),
    key: sha256('floral-key|' + code),
    joined: false,
    subscribed: false,   // the current connection holds the subscription
    unconfirmed: [],     // { iv, bytes, at } of own messages not back yet, oldest first
  };
  conn.client = mqtt.connect({
    host: cmd.host, port: cmd.port, protocol: 'mqtts',
    username: cmd.user, password: cmd.pass,
    clientId: 'floral_' + crypto.randomBytes(8).toString('hex'),
    clean: true, keepalive: 15, reconnectPeriod: 2000,
    queueQoSZero: false, resubscribe: false,
  });
  room = conn;

  const report = (ev, text) => { if (room === conn) emit({ ev, id: conn.id, text }); };
  const fail = (reason) => {
    if (room !== conn) return;
    log('failed:', reason);
    report('failed', reason);
    room = null;
    conn.client.end(true);
  };
  setTimeout(() => { if (!conn.joined) fail('timed out'); }, JOIN_TIMEOUT_MS).unref();

  conn.client.on('connect', () => {
    conn.client.subscribe(conn.topic, { qos: 0 }, (err) => {
      if (err) return fail(err.message);
      conn.subscribed = true;
      if (conn.joined) return report('up');
      conn.joined = true;
      report('joined', code);
    });
  });
  conn.client.on('close', () => {
    conn.unconfirmed = [];
    if (conn.subscribed) {
      conn.subscribed = false;
      report('down');
    } else if (!conn.joined) {
      fail('connection closed');
    }
  });
  conn.client.on('error', (err) => {
    log('error:', err.message);
    if (!conn.joined) fail(err.message);
  });
  conn.client.on('message', (_topic, payload) => {
    if (room !== conn) return;
    // Messages come back in order, so the ones before this were delivered or lost
    const own = conn.unconfirmed.findIndex((m) => m.iv.equals(payload.subarray(0, 12)));
    if (own >= 0) conn.unconfirmed.splice(0, own + 1);
    const text = decrypt(conn.key, payload);
    if (text !== null) report('message', text);
  });
}

function leave() {
  if (!room) return;
  // A connection that is up closes after the messages already handed to it
  room.client.end(!room.client.connected);
  room = null;
}

function send(text) {
  if (!room || !room.subscribed) return;
  const now = Date.now();
  const unconfirmed = room.unconfirmed;
  while (unconfirmed.length && now - unconfirmed[0].at > ECHO_LOST_MS) unconfirmed.shift();
  if (unconfirmed.reduce((sum, m) => sum + m.bytes, 0) > BACKLOG_BYTES) {
    log('upload backed up, message dropped');
    return;
  }
  const payload = encrypt(room.key, text);
  unconfirmed.push({ iv: payload.subarray(0, 12), bytes: payload.length, at: now });
  room.client.publish(room.topic, payload, { qos: 0 });
}

const input = readline.createInterface({ input: process.stdin });
input.on('line', (line) => {
  const cmd = JSON.parse(line);
  if (cmd.cmd === 'join') join(cmd);
  else if (cmd.cmd === 'leave') leave();
  else if (cmd.cmd === 'send') send(cmd.text);
});
input.on('close', leave);
