// 直接执行实际网页脚本，覆盖任务切换、删除和重新添加时的字段状态。
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const elements = {};
function element() { return {style: {}, classList: {toggle() {}}, value: '', checked: false}; }
const document = {
  getElementById: id => elements[id] || null,
  querySelectorAll: () => [], querySelector: () => null, addEventListener() {},
  createElement: () => element()
};
const context = {document, window: {addEventListener() {}}, location: {hash: ''},
  localStorage: {getItem: () => null}, setTimeout: () => 0, clearTimeout() {},
  setInterval: () => 0, clearInterval() {}, confirm: () => true, console};
vm.createContext(context);
vm.runInContext(fs.readFileSync(path.join(__dirname, '../../code/web_src/app.js'), 'utf8'), context);
for (let i = 0; i < 6; i++) {
  const card = context.stBuildCard(i);
  elements['stTask' + i] = card;
  const html = card.innerHTML;
  for (const suffix of ['Time', 'Sys', 'Reply', 'Sender', 'Body', 'Timeout']) {
    assert(html.includes('name="st' + i + suffix + '"'));
    assert(html.includes('id="st' + i + suffix + '"'));
  }
  for (const match of html.matchAll(/id="([^"]+)"/g)) elements[match[1]] = element();
  elements['st' + i + 'Act'].value = '2';
  elements['st' + i + 'Sys'].checked = true;
  elements['st' + i + 'Reply'].checked = true;
  context.stSyncAction(i);
  assert.equal(elements['st' + i + 'Health'].style.display, '');
  assert.equal(elements['st' + i + 'Reply'].checked, true);
  elements['st' + i + 'Act'].value = '0';
  context.stSyncAction(i);
  assert.equal(elements['st' + i + 'Health'].style.display, 'none');
  assert.equal(elements['st' + i + 'Sys'].checked, false);
  assert.equal(elements['st' + i + 'Reply'].checked, false);
  elements['st' + i + 'Sender'].value = '^10086$';
  elements['st' + i + 'Body'].value = '余额为.*元';
  elements['st' + i + 'Time'].value = '08:00';
  elements['st' + i + 'Timeout'].value = 60;
  context.stRemove(i);
  for (const suffix of ['Sender', 'Body', 'Time']) assert.equal(elements['st' + i + suffix].value, '');
  assert.equal(elements['st' + i + 'Timeout'].value, 300);
  assert.equal(elements['st' + i + 'Reply'].checked, false);
  assert.equal(card.style.display, 'none');
}
console.log('scheduled SMS UI regressions passed (6 task slots)');
