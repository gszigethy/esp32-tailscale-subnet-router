// Exercise the actual DDNS page code with a small DOM and deterministic timers.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../main/index.html'), 'utf8');
for (const script of html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)) new vm.Script(script[1]);
const start = html.indexOf('    var s_ddns_clear =');
const end = html.indexOf('    // =====================================================================', start);
const elements = new Map();
const listeners = new Map();
const timers = [];
const element = id => {
  if (!elements.has(id)) elements.set(id, {
    value: '', checked: false, disabled: false, textContent: '', type: 'password',
    style: {display: 'none'}, classList: {contains: () => true},
    addEventListener: (event, fn) => listeners.set(`${id}:${event}`, fn)
  });
  return elements.get(id);
};
let requests = 0;
const context = {
  document: {getElementById: element}, console,
  formatUptime: x => `${x} s`, showToast: () => {},
  setInterval: (fn, ms) => { timers.push({fn, ms, interval: true}); return timers.length; },
  clearInterval: () => {},
  setTimeout: (fn, ms) => { timers.push({fn, ms, interval: false}); return timers.length; },
  apiFetch: async () => {
    requests++;
    return {ok: true, json: async () => ({queued: true, url: 'stored URL', status: {state: 'ok', last_http: 200}})};
  }
};
vm.createContext(context);
vm.runInContext(html.slice(start, end), context);
const settle = async () => { for (let i = 0; i < 12; i++) await Promise.resolve(); };
(async () => {
  const poll = timers.find(x => x.interval && x.ms === 5000);
  assert.ok(poll, 'DDNS polls every five seconds');
  const force = listeners.get('ddns-force:click');
  assert.ok(force);
  force.call(element('ddns-force'));
  await settle();
  element('ddns-url').value = 'unsaved URL';
  element('ddns-pass').value = 'new password';
  element('ddns-token').value = 'new token';
  element('ddns-save').disabled = false;
  context.s_ddns_clear.pass = true;
  const refresh = timers.find(x => !x.interval && x.ms === 5000);
  assert.ok(refresh);
  refresh.fn();
  await settle();
  assert.equal(element('ddns-st-state').textContent, 'ok');
  assert.equal(element('ddns-url').value, 'unsaved URL');
  assert.equal(element('ddns-pass').value, 'new password');
  assert.equal(element('ddns-token').value, 'new token');
  assert.equal(context.s_ddns_clear.pass, true);
  assert.equal(element('ddns-save').disabled, false);
  poll.fn(); poll.fn(); // No overlapping requests.
  await settle();
  assert.equal(requests, 3);
  element('page-ddns').classList.contains = () => false;
  poll.fn(); await settle(); assert.equal(requests, 3);
  element('page-ddns').classList.contains = () => true;
  element('authLogin').style.display = '';
  poll.fn(); await settle(); assert.equal(requests, 3);
  console.log('PASS: DDNS Force/polling preserves edits and secrets, updates status, avoids overlap, and skips hidden/unauthenticated pages');
})().catch(e => { console.error(e); process.exitCode = 1; });
