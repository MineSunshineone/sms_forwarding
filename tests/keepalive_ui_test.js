// 执行实际网页脚本，验证状态回填、表单保存和重新加载不会改写 URL。
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const settle = () => new Promise(resolve => setImmediate(resolve));
function app(initialUrl) {
  let savedUrl = initialUrl;
  const posts = [], listeners = {};
  const elements = {};
  const input = () => ({ value: '', checked: false, dataset: {}, classList: { add() {}, remove() {}, toggle() {} } });
  for (const id of ['kaUrl', 'pingUrl', 'kaEnabled', 'kaIntervalDays', 'kaAction', 'kaTarget', 'kaCard']) elements[id] = input();
  elements.kaForm = {
    id: 'kaForm', dataset: {}, getAttribute: name => name === 'action' ? '/save' : null,
    querySelector: () => null,
    appendChild: child => { elements[child.id] = child; }
  };
  const context = {
    console, Date, Promise, URLSearchParams,
    document: {
      getElementById: id => elements[id] || null, querySelectorAll: () => [],
      createElement: () => input(), addEventListener: (name, callback) => { listeners[name] = callback; }
    },
    FormData: class {
      constructor(form) {
        assert.equal(form, elements.kaForm);
        this.values = new URLSearchParams({ kaForm: elements.kaFormFlag.value, kaUrl: elements.kaUrl.value });
      }
      forEach(callback) { this.values.forEach(callback); }
      get(key) { return this.values.get(key); }
    },
    addEventListener() {}, setTimeout: () => 1, clearTimeout() {}, setInterval() {}, clearInterval() {},
    fetch: async (url, options) => {
      if (url === '/save') {
        assert.equal(options.method, 'POST');
        const fields = new URLSearchParams(options.body.toString());
        assert.equal(fields.get('kaForm'), '1');
        savedUrl = fields.get('kaUrl');
        posts.push(savedUrl);
      }
      return { ok: true, json: async () => url.startsWith('/keepalive') ? { url: savedUrl } : {} };
    }
  };
  context.window = context;
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../code/web_src/app.js'), 'utf8'), context);
  return { context, elements, posts, listeners };
}
(async () => {
  const urls = [
    'http://gg.incrafttime.top/api/payload?size=128684',
    'https://gg.incrafttime.top/api/payload?size=64342',
    'http://gg.incrafttime.top/api/payload?size=1286840',
    'http://gg.incrafttime.top/api/payload?x=a%2Fb&size=100000&x=z',
    'http://example.com/api/payload?size=128684',
    'http://example.com/gg.incrafttime.top/api/payload?size=128684',
    'http://gg.incrafttime.top.evil.test/api/payload?size=128684',
    'http://gg.incrafttime.top/'
  ];
  for (const url of urls) {
    const { context: c, elements: e, posts, listeners } = app(url);
    c.kaLoadStatus(); await settle();
    assert.equal(e.kaUrl.value, url);
    assert.equal(e.pingUrl.value, url);
    assert.ok(e.kaFormFlag);
    // 真实保存按钮事件，以及立即执行前的局部保存，均应提交用户输入。
    e.kaUrl.value = url;
    listeners.submit({ target: e.kaForm, preventDefault() {} });
    await settle();
    assert.equal(posts[0], url);
    e.kaUrl.value = ''; e.pingUrl.value = '';
    c.kaLoadStatus(); await settle();
    assert.equal(e.kaUrl.value, url);
    assert.equal(e.pingUrl.value, url);
    assert.equal(await c.kaSaveForm(), true);
    assert.equal(posts[1], url);
  }
  console.log('keepalive URL save/reload UI regressions passed');
})().catch(error => { console.error(error); process.exit(1); });
