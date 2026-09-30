const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function app() {
  const elements = {
    modemRefreshButton: { disabled: false, textContent: '刷新模组信息' },
    modemSampleResult: { textContent: '' },
    ovRefresh: { textContent: '' }
  };
  const timers = [], urls = [];
  let reply = () => Promise.resolve({ modemSampleRequested: 1, modemSampleCompleted: 0 });
  const context = {
    console, Date, Promise, URLSearchParams,
    document: { hidden: false, querySelectorAll: () => [], addEventListener() {},
      getElementById: id => elements[id] || null },
    addEventListener() {},
    setTimeout: (fn, delay) => { timers.push({ fn, delay }); return timers.length; },
    clearTimeout() {}, setInterval() {}, clearInterval() {},
    fetch: url => { urls.push(url); return reply().then(data => ({ ok: true, json: () => Promise.resolve(data) })); }
  };
  context.window = context;
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../code/web_src/app.js'), 'utf8'), context);
  return { context, elements, timers, urls, setReply(fn) { reply = fn; } };
}
const settle = () => new Promise(resolve => setImmediate(resolve));

async function testQueuedAndComplete() {
  const { context: c, elements: e, timers, urls } = app();
  c.refreshModemInfo(e.modemRefreshButton);
  await settle();
  assert.match(urls[0], /sample=1/);
  assert.equal(e.modemRefreshButton.disabled, true);
  assert.match(e.modemSampleResult.textContent, /排队/);
  assert.equal(timers.some(timer => timer.delay === 3000), false);
  c.renderModemSampleState({ modemSampleRequested: 1, modemSampleCompleted: 0, modemSampleRunning: true });
  assert.match(e.modemRefreshButton.textContent, /采样中/);
  c.renderModemSampleState({ modemSampleRequested: 2, modemSampleCompleted: 1, modemSampleRunning: false });
  assert.equal(e.modemRefreshButton.disabled, true);
  c.renderModemSampleState({ modemSampleRequested: 2, modemSampleCompleted: 2, iccid: '8986001234567890123' });
  assert.equal(e.modemRefreshButton.disabled, false);
  assert.match(e.modemSampleResult.textContent, /运营商仍未读到/);
  c.renderModemSampleState({ modemSampleRequested: 2, modemSampleCompleted: 2, iccid: '8986001234567890123', operator: 'CMCC' });
  assert.equal(e.modemSampleResult.textContent, '模组信息刷新完成');
}
async function testInflightPollAndErrors() {
  const a = app(), c = a.context, e = a.elements;
  c.statusLoading = true;
  c.refreshModemInfo(e.modemRefreshButton);
  c.renderModemSampleState({ modemSampleRequested: 0, modemSampleCompleted: 0 });
  assert.equal(e.modemRefreshButton.disabled, true);
  c.statusLoading = false;
  a.timers.find(timer => timer.delay === 300).fn();
  await settle();
  assert.match(a.urls[0], /sample=1/);
  a.setReply(() => Promise.reject(new Error('offline')));
  c.loadStatus(true);
  await settle();
  assert.equal(e.modemRefreshButton.disabled, false);
  assert.match(e.modemSampleResult.textContent, /未确认/);

  // 无 AbortController 且请求永不返回时，也要允许用户重试。
  const b = app();
  b.setReply(() => new Promise(() => {}));
  b.context.refreshModemInfo(b.elements.modemRefreshButton);
  b.timers.find(timer => timer.delay === 7000).fn();
  assert.equal(b.elements.modemRefreshButton.disabled, false);
  assert.equal(b.context.modemSampleAwaitingResponse, false);
  assert.equal(b.context.statusLoading, false);
}
(async () => {
  await testQueuedAndComplete();
  await testInflightPollAndErrors();
  console.log('modem refresh UI regression tests passed');
})().catch(error => { console.error(error); process.exit(1); });
