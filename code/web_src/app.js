// 主题固定为浅色(深色模式已移除)
    var WEB_ASSET_HASH = '{{ASSET_HASH}}';
    var webConfig = null, webConfigReq = null;
    var loadedPanels = {};
    var panelSeq = 0, currentPanel = '', panelFetchCtrl = null;

    function ensureConfig(force) {
      if (force) { webConfig = null; webConfigReq = null; }
      if (webConfig) return Promise.resolve(webConfig);
      if (!webConfigReq) {
        webConfigReq = fetch('/config.json?_=' + Date.now(), { cache: 'no-store' }).then(function(r){
          if (!r.ok) throw new Error('config ' + r.status);
          return r.json();
        }).then(function(d) {
          webConfig = d || {};
          return webConfig;
        }).catch(function(e) {
          webConfigReq = null;  // 临时网络/认证失败不能污染后续面板加载
          throw e;
        });
      }
      return webConfigReq;
    }
    function jsonOrThrow(r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    }
    function textOrThrow(r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.text();
    }
    function checked(v) { return v ? 'checked' : ''; }
    function buildTzOptions(sel) {
      var zones = [
        [480,'UTC+8 北京/香港'], [540,'UTC+9 东京'], [420,'UTC+7 曼谷'], [330,'UTC+5:30 印度'],
        [60,'UTC+1 中欧'], [0,'UTC/GMT'], [-300,'UTC-5 纽约'], [-360,'UTC-6 芝加哥'], [-480,'UTC-8 洛杉矶']
      ];
      var html = zones.map(function(z){ return '<option value="' + z[0] + '"' + (z[0] == sel ? ' selected' : '') + '>' + z[1] + '</option>'; }).join('');
      // 导入的配置可能带非预置时区值：不追加选中项的话浏览器会回落到第一项，
      // 下次"保存时间"就把时区悄悄改写成 UTC+8
      var known = zones.some(function(z){ return z[0] == sel; });
      if (!known && sel !== undefined && sel !== null && sel !== '') {
        html += '<option value="' + parseInt(sel, 10) + '" selected>UTC 偏移 ' + parseInt(sel, 10) + ' 分钟</option>';
      }
      return html;
    }
    function pushTypeOptions(type) {
      var opts = [
        [1,'POST JSON（通用格式）'], [2,'Bark（iOS推送）'], [3,'GET请求（参数在URL中）'], [4,'钉钉机器人'],
        [5,'PushPlus'], [6,'Server酱'], [7,'自定义模板'], [8,'飞书机器人'], [9,'Gotify'], [10,'Telegram Bot']
      ];
      return opts.map(function(o){ return '<option value="' + o[0] + '"' + (o[0] == type ? ' selected' : '') + '>' + o[1] + '</option>'; }).join('');
    }
    function pushKeyInputType(type, slot) {
      type = parseInt(type) || 1;
      if (slot === 1 && (type === 2 || type === 4 || type === 5 || type === 6 || type === 8 || type === 9)) return 'password';
      if (slot === 2 && type === 10) return 'password';
      return 'text';
    }
    function setPushKeyInputTypes(idx, type) {
      var key1 = document.getElementById('key1' + idx), key2 = document.getElementById('key2' + idx);
      try { if (key1) key1.type = pushKeyInputType(type, 1); } catch (e) {}
      try { if (key2) key2.type = pushKeyInputType(type, 2); } catch (e) {}
    }
    function syncPushKeyKeepState(idx, type) {
      for (var slot = 1; slot <= 2; slot++) {
        var keep = document.getElementById('key' + slot + 'Keep' + idx);
        if (!keep || keep.dataset.saved !== '1') continue;
        var match = String(type) === keep.dataset.redactedType;
        keep.value = match ? '1' : '0';
        var clear = document.getElementById('key' + slot + 'ClearWrap' + idx);
        var hint = document.getElementById('key' + slot + 'SavedHint' + idx);
        if (clear) clear.style.display = match ? '' : 'none';
        if (hint) hint.style.display = match ? '' : 'none';
        if (!match) {
          var cb = document.querySelector('[name="push' + idx + 'key' + slot + 'Clear"]');
          if (cb) cb.checked = false;
        }
      }
    }
    function pushKeyControl(idx, slot, type, value, ch) {
      var key = 'key' + slot;
      var redacted = !!ch[key + 'Redacted'], set = !!ch[key + 'Set'];
      var name = 'push' + idx + key;
      var html = '<input type="' + pushKeyInputType(type, slot) + '" name="' + name + '" id="' + key + idx + '" value="' + htmlEsc(value || '') + '">';
      html += '<input type="hidden" name="' + name + 'Keep" id="' + key + 'Keep' + idx + '" value="' + (redacted && set ? '1' : '0') + '" data-saved="' + (redacted && set ? '1' : '0') + '" data-redacted-type="' + type + '">';
      if (redacted && set) {
        html += '<label class="schedule-switch" id="' + key + 'ClearWrap' + idx + '" style="margin-top:6px;"><input type="checkbox" name="' + name + 'Clear"> 清除已保存参数</label>';
        html += '<p class="form-hint" id="' + key + 'SavedHint' + idx + '">已保存，留空不修改；填写新值会覆盖。</p>';
      }
      return html;
    }
    function buildPushChannels(chans) {
      chans = chans || [];
      var html = '';
      for (var i = 0; i < 5; i++) {
        var ch = chans[i] || {}, idx = String(i), en = !!ch.enabled;
        var pushType = ch.type || 1;
        var defName = '通道' + (i + 1);
        var configured = en || !!ch.url || !!ch.key1 || !!ch.key1Set || !!ch.key2 || !!ch.key2Set ||
          !!ch.customBody || (pushType && pushType != 1) || (ch.name && ch.name !== defName);
        var folded = !en;
        html += '<div class="push-channel' + (en ? ' enabled' : '') + (folded ? ' folded' : '') + (configured ? ' configured' : '') + '" id="channel' + idx + '" data-configured="' + (configured ? '1' : '0') + '">';
        html += '<div class="push-channel-header">';
        html += '<input type="checkbox" class="switch-input" name="push' + idx + 'en" id="push' + idx + 'en" onchange="toggleChannel(' + idx + ')"' + (en ? ' checked' : '') + '><span class="switch-slider"></span>';
        html += '<label for="push' + idx + 'en" class="label-inline">启用推送通道 ' + (i + 1) + '</label>';
        html += '<span class="ch-summary" id="chsum' + idx + '"></span>';
        html += '<button type="button" class="channel-fold" onclick="toggleChannelBody(' + idx + ')" id="foldBtn' + idx + '">展开</button>';
        html += '</div><div class="push-channel-body">';
        html += '<div class="form-group"><label>通道名称</label><input type="text" name="push' + idx + 'name" value="' + htmlEsc(ch.name || defName) + '" placeholder="自定义名称" oninput="updateChannelSummary(' + idx + ')"></div>';
        html += '<div class="form-group"><label>推送方式</label><select name="push' + idx + 'type" id="push' + idx + 'type" onchange="updateTypeHint(' + idx + ')">' + pushTypeOptions(pushType) + '</select><div class="push-type-hint" id="hint' + idx + '"></div></div>';
        html += '<div class="form-group"><label id="urllabel' + idx + '">推送URL/Webhook</label><input type="text" name="push' + idx + 'url" id="url' + idx + '" value="' + htmlEsc(ch.url || '') + '" placeholder="http://your-server.com/api 或 webhook地址"></div>';
        html += '<div id="extra' + idx + '" style="display:none;"><div class="form-group"><label id="key1label' + idx + '">参数1</label>' + pushKeyControl(idx, 1, pushType, ch.key1, ch) + '</div>';
        html += '<div class="form-group" id="key2group' + idx + '"><label id="key2label' + idx + '">参数2</label>' + pushKeyControl(idx, 2, pushType, ch.key2, ch) + '</div>';
        html += '<details class="bark-advanced" id="barkAdvanced' + idx + '" style="display:none;"><summary>Bark 高级参数 <span class="bark-summary" id="barkSummary' + idx + '"></span></summary><div class="bark-params" id="barkParams' + idx + '"><div class="bark-param-grid">';
        html += '<div class="form-group"><label>副标题 subtitle</label><input type="text" id="bark_subtitle' + idx + '" data-bark-key="subtitle" placeholder="可选"></div>';
        html += '<div class="form-group"><label>分组 group</label><input type="text" id="bark_group' + idx + '" data-bark-key="group" placeholder="SMS"></div>';
        html += '<div class="form-group"><label>中断级别 level</label><select id="bark_level' + idx + '" data-bark-key="level"><option value="">默认 active</option><option value="active">active</option><option value="timeSensitive">timeSensitive</option><option value="passive">passive</option><option value="critical">critical</option></select></div>';
        html += '<div class="form-group"><label>铃声 sound</label><input type="text" id="bark_sound' + idx + '" data-bark-key="sound" placeholder="bell / minuet"></div>';
        html += '<div class="form-group"><label>角标 badge</label><input type="number" id="bark_badge' + idx + '" data-bark-key="badge" placeholder="1"></div>';
        html += '<div class="form-group"><label>保存秒数 ttl</label><input type="number" id="bark_ttl' + idx + '" data-bark-key="ttl" placeholder="3600"></div>';
        html += '<div class="form-group"><label>图标 icon</label><input type="url" id="bark_icon' + idx + '" data-bark-key="icon" placeholder="https://.../icon.png"></div>';
        html += '<div class="form-group"><label>图片 image</label><input type="url" id="bark_image' + idx + '" data-bark-key="image" placeholder="https://.../image.jpg"></div>';
        html += '<div class="form-group"><label>点击 URL</label><input type="text" id="bark_url' + idx + '" data-bark-key="url" placeholder="https://... 或 URL Scheme"></div>';
        html += '<div class="form-group"><label>复制内容 copy</label><input type="text" id="bark_copy' + idx + '" data-bark-key="copy" placeholder="{message}"></div>';
        html += '<div class="form-group"><label>自动复制 autoCopy</label><select id="bark_autoCopy' + idx + '" data-bark-key="autoCopy"><option value="">跟随 App</option><option value="1">开启</option><option value="0">关闭</option></select></div>';
        html += '<div class="form-group"><label>保存历史 isArchive</label><select id="bark_isArchive' + idx + '" data-bark-key="isArchive"><option value="">跟随 App</option><option value="1">保存</option><option value="0">不保存</option></select></div>';
        html += '<div class="form-group"><label>持续响铃 call</label><select id="bark_call' + idx + '" data-bark-key="call"><option value="">关闭</option><option value="1">开启</option></select></div>';
        html += '<div class="form-group"><label>点击动作 action</label><select id="bark_action' + idx + '" data-bark-key="action"><option value="">默认</option><option value="alert">alert 弹窗</option></select></div>';
        html += '<div class="form-group"><label>通知 ID id</label><input type="text" id="bark_id' + idx + '" data-bark-key="id" placeholder="同 ID 会更新通知"></div>';
        html += '<div class="form-group"><label>其它参数</label><input type="text" id="bark_other' + idx + '" placeholder="markdown=...&ciphertext=..."></div>';
        html += '</div><p class="form-hint">保存时会自动合成为 Bark 参数；可用 {sender} {message} {timestamp} 占位符。</p></div></details></div>';
        html += '<div id="custom' + idx + '" style="display:none;"><div class="form-group"><label>请求体模板（使用 {sender} {message} {timestamp} 占位符）</label><textarea name="push' + idx + 'body" rows="4" style="width:100%;font-family:monospace;">' + htmlEsc(ch.customBody || '') + '</textarea></div></div>';
        html += '<button type="button" class="btn btn-secondary btn-sm" onclick="testPush(' + idx + ', this)">测试推送</button><div class="result-box" id="pushTestResult' + idx + '"></div>';
        html += '</div></div>';
      }
      return html;
    }
    function renderConfigTemplate(html) {
      var c = webConfig || {};
      var map = {
        WEB_USER: htmlEsc(c.webUser || ''), WEB_PASS: htmlEsc(c.webPass || ''),
        SMTP_SERVER: htmlEsc(c.smtpServer || ''), SMTP_PORT: String(c.smtpPort || 465),
        SMTP_USER: htmlEsc(c.smtpUser || ''), SMTP_PASS: htmlEsc(c.smtpPass || ''), SMTP_SEND_TO: htmlEsc(c.smtpSendTo || ''),
        ADMIN_PHONE: htmlEsc(c.adminPhone || ''), NUMBER_BLACK_LIST: htmlEsc(c.numberBlackList || ''), FORWARD_RULES: htmlEsc(c.forwardRules || ''),
        SMTP_CHECK: c.emailConfigured ? '已配置' : '未配置', EMAIL_EN: checked(c.emailEnabled), PUSH_EN: checked(c.pushEnabled),
        MODEM_CHECK: c.modemReady ? '已就绪' : '未就绪', PUSH_COUNT: String(c.pushEnabledCount || 0), INBOX_MAX: String(c.inboxMax || ''),
        NTP: htmlEsc(c.ntpServer || ''), RB_CHECKED: checked(c.rebootEnabled), RB_HOUR: String(c.rebootHour == null ? 3 : c.rebootHour),
        HB_CHECKED: checked(c.hbEnabled), HB_HOUR: String(c.hbHour == null ? 9 : c.hbHour), TZ_OPTIONS: buildTzOptions(c.tzOffsetMin),
        DATA_CHECKED: checked(c.dataEnabled), ROAMING_CHECKED: checked(c.roamingEnabled !== false),
        APN: htmlEsc(c.apn || ''), PHONE_NUMBER: htmlEsc(c.phoneNumber || ''),
        OPERATOR_PLMN: htmlEsc(c.operatorPlmn || ''), KA_PROFILE: htmlEsc(c.kaProfile || ''), PUSH_CHANNELS: buildPushChannels(c.pushChannels),
        NETLED_CHECKED: checked(c.netLedEnabled !== false), CALLNOTIFY_CHECKED: checked(c.callNotifyEnabled !== false), UPTIME: htmlEsc(c.uptimeText || '')
      };
      return html.replace(/%([A-Z0-9_]{2,})%/g, function(m, k){ return Object.prototype.hasOwnProperty.call(map, k) ? map[k] : m; });
    }
    function clearPanelTimers() {
      if (pingPollTimer) { clearTimeout(pingPollTimer); pingPollTimer = null; }
      if (kaRunTimer) { clearTimeout(kaRunTimer); kaRunTimer = null; }
      if (stTimer) { clearTimeout(stTimer); stTimer = null; }
      if (esimTimer) { clearTimeout(esimTimer); esimTimer = null; }
      if (latestOtpTimer) { clearTimeout(latestOtpTimer); latestOtpTimer = null; }
    }
    function cleanupPanelRuntime() {
      stopLogPoll();
      stopStatusPoll();
      if (statusAbort) { try { statusAbort.abort(); } catch (e) {} statusAbort = null; }
      clearPanelTimers();
      stBuilt = false;
      if (pushTestTimers) {
        for (var k in pushTestTimers) {
          if (Object.prototype.hasOwnProperty.call(pushTestTimers, k) && pushTestTimers[k]) {
            clearTimeout(pushTestTimers[k]);
            pushTestTimers[k] = null;
          }
        }
      }
      pushTestBtns = {};
    }
    function panelHook(name) {
      if (name === 'log') { initLogPanel(); } else { stopLogPoll(); }
      if (name === 'overview') { loadStatus(); loadLatestOtp(); startStatusPoll(); } else { stopStatusPoll(); }
      if (name === 'inbox') loadMessages();
      if (name === 'keepalive' || name === 'diagnose') kaLoadStatus();
      if (name === 'keepalive') stLoadStatus();
      if (name === 'sim') { wifiPrefill(); esimLoadStatus(); }
      if (name === 'push') { for (var i = 0; i < 5; i++) { toggleChannel(i); updateTypeHint(i); } setupChannels(); parseFwdRules(); renderRules(); }
      if (name === 'atterm') bindAtTerminal();
    }
    function activateNav(name) {
      document.querySelectorAll('.sidebar-nav a').forEach(function(a) { a.classList.remove('active'); });
      var a = document.querySelector('.sidebar-nav a[data-panel="' + name + '"]');
      if (a) a.classList.add('active');
    }
    function normalizePanelName(name) {
      name = String(name || '').replace(/^#/, '');
      return document.querySelector('.sidebar-nav a[data-panel="' + name + '"]') ? name : 'overview';
    }
    function panelActive(name) { return currentPanel === name; }
    function panelFromHash() {
      return normalizePanelName(decodeURIComponent((location.hash || '').slice(1)));
    }
    function switchPanel(name) {
      name = normalizePanelName(name);
      var seq = ++panelSeq;
      currentPanel = name;
      var host = document.getElementById('panelHost');
      cleanupPanelRuntime();
      if (panelFetchCtrl) { try { panelFetchCtrl.abort(); } catch (e) {} panelFetchCtrl = null; }
      function stale() { return seq !== panelSeq || currentPanel !== name; }
      activateNav(name);
      if (loadedPanels[name]) {
        ensureConfig(false).then(function() {
          if (stale()) return;
          host.innerHTML = renderConfigTemplate(loadedPanels[name]);
          var cachedPanel = document.getElementById('panel-' + name);
          if (cachedPanel) cachedPanel.classList.add('active');
          panelHook(name);
        }).catch(function(e) {
          if (!stale()) host.innerHTML = '<div class="msg-empty">页面加载失败: ' + htmlEsc(String(e)) + '</div>';
        });
        return;
      }
      host.innerHTML = '<div class="msg-empty">加载中...</div>';
      var ctrl = window.AbortController ? new AbortController() : null;
      panelFetchCtrl = ctrl;
      ensureConfig(false).then(function() {
        if (stale()) return null;
        var opt = { cache: 'no-store' };
        if (ctrl) opt.signal = ctrl.signal;
        return fetch('/ui?panel=' + encodeURIComponent(name) + '&v=' + encodeURIComponent(WEB_ASSET_HASH), opt).then(function(r){
          if (!r.ok) throw new Error('panel ' + r.status);
          return r.text();
        });
      }).then(function(html) {
        if (stale() || html == null) return;
        if (panelFetchCtrl === ctrl) panelFetchCtrl = null;
        loadedPanels[name] = html;
        host.innerHTML = renderConfigTemplate(html);
        var p = document.getElementById('panel-' + name);
        if (p) p.classList.add('active');
        panelHook(name);
      }).catch(function(e) {
        if (panelFetchCtrl === ctrl) panelFetchCtrl = null;
        if (stale() || (e && e.name === 'AbortError')) return;
        host.innerHTML = '<div class="msg-empty">页面加载失败: ' + htmlEsc(String(e)) + '</div>';
      });
    }
    document.querySelectorAll('.sidebar-nav a').forEach(function(a) {
      a.addEventListener('click', function() {
        var name = normalizePanelName(this.dataset.panel);
        if (location.hash === '#' + name) switchPanel(name);
        else location.hash = name;
      });
    });
    window.addEventListener('hashchange', function() { switchPanel(panelFromHash()); });

    // ---- Push Channel JS ----
    function toggleChannel(idx) {
      var ch = document.getElementById('channel' + idx);
      var cb = document.getElementById('push' + idx + 'en');
      if (!ch || !cb) return;
      if (cb.checked) {
        ch.classList.add('enabled');
        ch.classList.remove('folded');
        ch.dataset.configured = '1';
      } else {
        ch.classList.remove('enabled');
        ch.classList.add('folded');
      }
      updateChannelFoldText(idx);
    }
    function updateChannelFoldText(idx) {
      var ch = document.getElementById('channel' + idx);
      var btn = document.getElementById('foldBtn' + idx);
      if (!ch || !btn) return;
      btn.textContent = ch.classList.contains('folded') ? '展开' : '收起';
    }
    function toggleChannelBody(idx) {
      var ch = document.getElementById('channel' + idx);
      if (!ch) return;
      ch.classList.toggle('folded');
      updateChannelFoldText(idx);
    }
    var barkKeys = ['subtitle','group','level','sound','badge','ttl','icon','image','url','copy','autoCopy','isArchive','call','action','id'];
    function barkDecode(v) {
      try { return decodeURIComponent(String(v || '').replace(/\+/g, ' ')); } catch (e) { return String(v || ''); }
    }
    function barkEncode(v) { return encodeURIComponent(String(v || '')); }
    function parseParamString(raw) {
      var out = {}, extra = [];
      String(raw || '').replace(/^\?+/, '').split(/[&\n]/).forEach(function(part) {
        part = part.trim();
        if (!part) return;
        var p = part.indexOf('='), k = barkDecode(p < 0 ? part : part.slice(0, p)).trim();
        if (!k) return;
        var v = p < 0 ? '1' : barkDecode(part.slice(p + 1));
        if (barkKeys.indexOf(k) >= 0) out[k] = v;
        else if (['title','body','device_key','device_keys'].indexOf(k) < 0) extra.push(barkEncode(k) + '=' + barkEncode(v));
      });
      out._other = extra.join('&');
      return out;
    }
    function loadBarkParams(idx) {
      var key2 = document.getElementById('key2' + idx);
      var box = document.getElementById('barkParams' + idx);
      if (!key2 || !box) return;
      var params = parseParamString(key2.value);
      barkKeys.forEach(function(k) {
        var el = box.querySelector('[data-bark-key="' + k + '"]');
        if (el) el.value = params[k] || '';
      });
      var other = document.getElementById('bark_other' + idx);
      if (other) other.value = params._other || '';
      updateBarkSummary(idx);
    }
    function updateBarkSummary(idx) {
      var key2 = document.getElementById('key2' + idx);
      var summary = document.getElementById('barkSummary' + idx);
      if (!key2 || !summary) return;
      var params = parseParamString(key2.value);
      var items = [];
      ['group','level','sound','copy'].forEach(function(k) { if (params[k]) items.push(k + '=' + params[k]); });
      if (params._other) items.push('其它');
      summary.textContent = items.length ? items.slice(0, 4).join(' · ') : '未配置';
    }
    function serializeBarkParams(idx) {
      var key2 = document.getElementById('key2' + idx);
      var box = document.getElementById('barkParams' + idx);
      if (!key2 || !box) return;
      var parts = [];
      barkKeys.forEach(function(k) {
        var el = box.querySelector('[data-bark-key="' + k + '"]');
        var v = el ? String(el.value || '').trim() : '';
        if (v) parts.push(barkEncode(k) + '=' + barkEncode(v));
      });
      var other = document.getElementById('bark_other' + idx);
      if (other && other.value.trim()) parts.push(other.value.trim().replace(/^[?&]+/, ''));
      key2.value = parts.join('&');
      updateBarkSummary(idx);
    }
    function bindBarkParams(idx) {
      var box = document.getElementById('barkParams' + idx);
      if (!box || box.dataset.bound) return;
      box.dataset.bound = '1';
      box.querySelectorAll('input,select').forEach(function(el) {
        el.addEventListener('input', function(){ serializeBarkParams(idx); });
        el.addEventListener('change', function(){ serializeBarkParams(idx); });
      });
    }
    function serializeAllBarkParams() {
      for (var i = 0; i < 5; i++) {
        var sel = document.getElementById('push' + i + 'type');
        if (sel && parseInt(sel.value) === 2) serializeBarkParams(i);
      }
    }
    // 折叠状态下也能看出通道是什么：header 显示"名称 · 类型"摘要
    function pushTypeLabel(t) {
      var m = {1:'POST JSON',2:'Bark',3:'GET',4:'钉钉',5:'PushPlus',6:'Server酱',7:'自定义',8:'飞书',9:'Gotify',10:'Telegram'};
      return m[t] || '';
    }
    function updateChannelSummary(idx) {
      var el = document.getElementById('chsum' + idx);
      if (!el) return;
      var nameEl = document.querySelector('[name="push' + idx + 'name"]');
      var sel = document.getElementById('push' + idx + 'type');
      var name = nameEl ? nameEl.value.trim() : '';
      var t = sel ? pushTypeLabel(parseInt(sel.value)) : '';
      if (name === '通道' + (idx + 1)) name = '';  // 默认名不重复显示
      el.textContent = [name, t].filter(Boolean).join(' · ');
    }
    // 转发规则里按真实通道名显示（回退"通道N"）
    function chanName(n) {
      var arr = (webConfig && webConfig.pushChannels) || [];
      var ch = arr[n - 1] || {};
      return (ch.name || '').trim() || ('通道' + n);
    }
    function updateTypeHint(idx) {
      var sel = document.getElementById('push' + idx + 'type');
      var hint = document.getElementById('hint' + idx);
      var extra = document.getElementById('extra' + idx);
      var custom = document.getElementById('custom' + idx);
      if (!sel || !hint || !extra || !custom) return;
      var type = parseInt(sel.value);
      setPushKeyInputTypes(idx, type);
      syncPushKeyKeepState(idx, type);
      updateChannelSummary(idx);
      var bp = document.getElementById('barkParams' + idx);
      var ba = document.getElementById('barkAdvanced' + idx);
      extra.style.display = 'none'; custom.style.display = 'none';
      document.getElementById('key1label' + idx).innerText = '参数 1';
      document.getElementById('key2label' + idx).innerText = '参数 2';
      document.getElementById('urllabel' + idx).innerText = '推送URL/Webhook';
      document.getElementById('url' + idx).placeholder = 'http://your-server.com/api 或 webhook地址';
      document.getElementById('key1' + idx).placeholder = '';
      document.getElementById('key2' + idx).placeholder = '';
      var kg = document.getElementById('key2group' + idx);
      if (kg) kg.style.display = 'none';
      if (bp) bp.style.display = '';
      if (ba) ba.style.display = 'none';
      if (type == 1) hint.innerHTML = 'POST JSON<br>{"sender":"+447700900123","message":"...","timestamp":"2026-01-01 12:00:00"}';
      else if (type == 2) { hint.innerHTML = 'Bark (iOS)<br>URL 留空使用 https://api.day.app；自建填服务器根地址，设备端发送到 /push。'; extra.style.display='block'; if(kg)kg.style.display='none'; if(ba)ba.style.display='block'; document.getElementById('urllabel'+idx).innerText='Bark 服务器 URL'; document.getElementById('url'+idx).placeholder='留空=官方服务；自建=https://bark.example.com'; document.getElementById('key1label'+idx).innerText='Device Key'; document.getElementById('key1'+idx).placeholder='Bark App 中显示的 key'; bindBarkParams(idx); loadBarkParams(idx); serializeBarkParams(idx); }
      else if (type == 3) hint.innerHTML = 'GET 请求<br>URL?sender=xxx&message=xxx&timestamp=xxx';
      else if (type == 4) { hint.innerHTML = '钉钉机器人<br>填写 Webhook 地址，加签需填 Secret'; extra.style.display='block'; document.getElementById('key1label'+idx).innerText='Secret（加签密钥，可选）'; document.getElementById('key1'+idx).placeholder='SEC...'; }
      else if (type == 5) { hint.innerHTML = 'PushPlus<br>填写 Token，URL 留空使用默认'; extra.style.display='block'; document.getElementById('key1label'+idx).innerText='Token'; document.getElementById('key1'+idx).placeholder='pushplus token'; if(kg)kg.style.display='block'; document.getElementById('key2label'+idx).innerText='发送渠道'; document.getElementById('key2'+idx).placeholder='wechat / extension / app'; }
      else if (type == 6) { hint.innerHTML = 'Server酱<br>填写 SendKey，URL 留空使用默认'; extra.style.display='block'; document.getElementById('key1label'+idx).innerText='SendKey'; document.getElementById('key1'+idx).placeholder='SCT...'; }
      else if (type == 7) { hint.innerHTML = '自定义模板<br>使用 {sender} {message} {timestamp} 占位符'; custom.style.display='block'; }
      else if (type == 8) { hint.innerHTML = '飞书机器人<br>填写 Webhook 地址，签名验证需填 Secret'; extra.style.display='block'; document.getElementById('key1label'+idx).innerText='Secret（签名密钥，可选）'; document.getElementById('key1'+idx).placeholder='飞书签名密钥'; }
      else if (type == 9) { hint.innerHTML = 'Gotify<br>填写服务器地址 + 应用 Token'; extra.style.display='block'; document.getElementById('key1label'+idx).innerText='Token（应用 Token）'; document.getElementById('key1'+idx).placeholder='A...'; }
      else if (type == 10) { hint.innerHTML = 'Telegram Bot<br>Chat ID（参数1）+ Bot Token（参数2）'; extra.style.display='block'; document.getElementById('key1label'+idx).innerText='Chat ID'; document.getElementById('key1'+idx).placeholder='123456789'; if(kg)kg.style.display='block'; document.getElementById('key2label'+idx).innerText='Bot Token'; document.getElementById('key2'+idx).placeholder='12345678:ABC...'; }
    }
    // ---- 推送通道：只显示已启用，其余由"添加推送通道"逐个展开 ----
    function setupChannels() {
      for (var i = 0; i < 5; i++) {
        var ch = document.getElementById('channel' + i), en = document.getElementById('push' + i + 'en');
        if (ch && en && ch.dataset.configured !== '1' && !en.checked) ch.style.display = 'none';
        updateChannelFoldText(i);
      }
      var form = document.getElementById('mainForm3');
      if (form && !form.dataset.barkSubmitBound) {
        form.dataset.barkSubmitBound = '1';
        form.addEventListener('submit', serializeAllBarkParams);
      }
      updateAddBtn();
    }
    function updateAddBtn() {
      var btn = document.getElementById('addChannelBtn'); if (!btn) return;
      var hidden = 0;
      for (var i = 0; i < 5; i++) { var ch = document.getElementById('channel' + i); if (ch && ch.style.display === 'none') hidden++; }
      btn.style.display = hidden > 0 ? '' : 'none';
    }
    function addChannel() {
      for (var i = 0; i < 5; i++) {
        var ch = document.getElementById('channel' + i);
        if (ch && ch.style.display === 'none') {
          ch.style.display = '';
          ch.dataset.configured = '1';
          ch.classList.remove('folded');
          var en = document.getElementById('push' + i + 'en');
          if (en) { en.checked = true; toggleChannel(i); }
          updateTypeHint(i);
          updateChannelFoldText(i);
          updateAddBtn();
          return;
        }
      }
    }
    document.addEventListener('DOMContentLoaded', function() {
      for (var i = 0; i < 5; i++) { toggleChannel(i); updateTypeHint(i); }
      setupChannels();
    });

    // ---- Push Channel Test ----
    var pushTestTimers = {}, pushTestBtns = {};
    function pushTestDone(idx) {
      var b = pushTestBtns[idx];
      if (b) b.disabled = false;
    }
    function pollTestPush(idx) {
      if (pushTestTimers[idx]) clearTimeout(pushTestTimers[idx]);
      fetch('/testpush?action=status&ch=' + idx + '&_=' + Date.now(), { cache: 'no-store' }).then(jsonOrThrow).then(function(d) {
        var r = document.getElementById('pushTestResult' + idx);
        if (!r) return;
        if (d.queued || d.running) {
          r.className = 'result-box result-loading';
          r.textContent = d.message || '测试推送后台执行中...';
          pushTestTimers[idx] = setTimeout(function(){ pollTestPush(idx); }, 3000);
          return;
        }
        pushTestDone(idx);
        r.className = 'result-box ' + (d.success ? 'result-success' : 'result-error');
        r.textContent = d.message || (d.success ? '测试推送已发送' : '测试推送失败');
      }).catch(function(e){
        pushTestDone(idx);
        var r = document.getElementById('pushTestResult' + idx);
        if (!r) return;
        r.className = 'result-box result-error';
        r.textContent = '状态查询失败: ' + e;
      });
    }
    function testPush(idx, btn) {
      if (btn) { pushTestBtns[idx] = btn; btn.disabled = true; }  // 测试期间防重复点击
      var r = document.getElementById('pushTestResult' + idx);
      r.className = 'result-box result-loading'; r.textContent = '测试推送已提交（请先保存配置）...';
      fetch('/testpush?ch=' + idx, { method: 'POST', cache: 'no-store' }).then(jsonOrThrow).then(function(d) {
        if (d.success && d.queued) {
          r.textContent = d.message || '后台测试推送中...';
          pollTestPush(idx);
        } else {
          pushTestDone(idx);
          r.className = 'result-box result-error';
          r.textContent = d.message || '任务启动失败';
        }
      }).catch(function(e){ pushTestDone(idx); r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; });
    }

    // ---- USSD ----
    function sendUssd() {
      var code = document.getElementById('ussdCode').value.trim();
      if (!code) return;
      var r = document.getElementById('ussdResult');
      r.className = 'result-box result-loading'; r.textContent = '查询中（最长约 20 秒）...';
      fetch('/ussd?code=' + encodeURIComponent(code), { method: 'POST', cache: 'no-store' }).then(jsonOrThrow).then(function(d) {
        r.className = 'result-box ' + (d.success ? 'result-info' : 'result-error');
        r.textContent = d.message || '(无响应)';
      }).catch(function(e){ r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; });
    }

    // ---- Send SMS ----
    function updateCount(el) { var c = document.getElementById('charCount'); if (c && el) c.textContent = el.value.length; }
    // ---- 发短信弹窗 ----
    function openSmsModal() {
      var modal = document.getElementById('smsModal'), phone = document.getElementById('smsPhone');
      if (!modal || !phone) return;
      var r = document.getElementById('smsSendResult');
      if (r) { r.className = 'result-box'; r.textContent = ''; }
      modal.classList.add('show');
      phone.focus();
    }
    function closeSmsModal() {
      var modal = document.getElementById('smsModal');
      if (modal) modal.classList.remove('show');
    }
    function sendSmsModal() {
      var phoneEl = document.getElementById('smsPhone'), contentEl = document.getElementById('smsContent');
      var r = document.getElementById('smsSendResult');
      if (!phoneEl || !contentEl || !r) return;
      var phone = phoneEl.value.trim();
      var content = contentEl.value;
      if (!phone) { r.className = 'result-box result-error'; r.textContent = '请输入目标号码'; return; }
      if (!content.trim()) { r.className = 'result-box result-error'; r.textContent = '请输入短信内容'; return; }
      var btn = document.getElementById('smsSendBtn'); btn.disabled = true;
      r.className = 'result-box result-loading'; r.textContent = '提交中...';
      var body = 'phone=' + encodeURIComponent(phone) + '&content=' + encodeURIComponent(content);
      fetch('/sendsms', { method: 'POST', cache: 'no-store', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: body })
        .then(jsonOrThrow).then(function(d) {
          btn.disabled = false;
          r.className = 'result-box ' + (d.success ? 'result-success' : 'result-error'); r.textContent = d.message;
          if (d.success) {
            var cur = document.getElementById('smsContent');
            if (cur) { cur.value = ''; updateCount(cur); }
            loadStatus();
            if (smsBox === 'sent') setTimeout(loadMessages, 1200);
            setTimeout(closeSmsModal, 1000);
          }
        }).catch(function(e){ btn.disabled = false; r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; });
    }

    // ---- Cellular HTTP payload traffic ----
    var pingPollTimer = null;
    function doPing(){
      var b=document.getElementById('pingBtn'),r=document.getElementById('pingResult');
      var u=document.getElementById('pingUrl');
      if(!b||!r||!u)return;
      var url=(u.value||'').trim();
      b.disabled=true;b.textContent='...';
      r.className='result-box result-loading';r.textContent='提交 payload 下载任务...';
      fetch('/ping',{method:'POST',cache:'no-store',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'url='+encodeURIComponent(url)}).then(jsonOrThrow).then(function(d){
        if(d.success && d.running){ r.textContent='后台下载 payload 中（会短暂开启蜂窝数据）...'; pollPingStatus(); }
        else{ b.disabled=false;b.textContent='下载 payload'; r.className='result-box result-error'; r.textContent=d.message||'任务启动失败'; }
      }).catch(function(e){b.disabled=false;b.textContent='下载 payload';r.className='result-box result-error';r.textContent='请求失败: '+e;});
    }
    function pollPingStatus(){
      if(pingPollTimer) clearTimeout(pingPollTimer);
      fetch('/ping?action=status&_=' + Date.now(), {cache:'no-store'}).then(jsonOrThrow).then(function(d){
        var b=document.getElementById('pingBtn'),r=document.getElementById('pingResult');
        if(!b||!r)return;
        if(d.running){
          r.className='result-box result-loading';
          r.textContent='正在通过蜂窝网络下载 payload（后台执行，可刷新网页）...';
          pingPollTimer=setTimeout(pollPingStatus,1000);
          return;
        }
        b.disabled=false;b.textContent='下载 payload';
        if(d.success){r.className='result-box result-success';r.textContent='payload 下载完成 — '+(d.message||'完成');}
        else{r.className='result-box result-error';r.textContent='payload 下载失败 — '+(d.message||'无结果');}
      }).catch(function(e){
        var b=document.getElementById('pingBtn'),r=document.getElementById('pingResult');
        if(!b||!r)return;
        b.disabled=false;b.textContent='下载 payload';r.className='result-box result-error';r.textContent='状态查询失败: '+e;
      });
    }

    // ---- WiFi Control ----
    function wifiRestart(){
      if(!confirm('确定要重启WiFi吗？网页将暂时不可用。'))return;
      var r=document.getElementById('wifiResult');
      r.className='result-box result-loading';r.textContent='WiFi 重启中（约5秒）...';
      fetch('/wifi?action=restart',{method:'POST',cache:'no-store'}).then(jsonOrThrow).then(function(d){
        r.className=d.success?'result-box result-success':'result-box result-error';
        r.textContent=d.message;
      }).catch(function(e){r.className='result-box result-error';r.textContent='请求失败: '+e;});
    }

    // ---- Flight Mode ----
    function queryFlightMode(){
      var r=document.getElementById('flightResult');
      r.className='result-box result-loading';r.textContent='查询中...';
      fetch('/flight?action=query',{cache:'no-store'}).then(jsonOrThrow).then(function(d){
        if(d.success){r.className='result-box result-info';r.textContent=d.message;}
        else{r.className='result-box result-error';r.textContent='查询失败: '+d.message;}
      }).catch(function(e){r.className='result-box result-error';r.textContent='请求失败: '+e;});
    }
    function toggleFlightMode(){
      if(!confirm('确定要切换飞行模式吗？'))return;
      var b=document.getElementById('flightBtn'),r=document.getElementById('flightResult');
      b.disabled=true;r.className='result-box result-loading';r.textContent='切换中...';
      fetch('/flight?action=toggle',{method:'POST',cache:'no-store'}).then(jsonOrThrow).then(function(d){
        b.disabled=false;
        if(d.success){r.className='result-box result-success';r.textContent=d.message;}
        else{r.className='result-box result-error';r.textContent='切换失败: '+d.message;}
      }).catch(function(e){b.disabled=false;r.className='result-box result-error';r.textContent='请求失败: '+e;});
    }

    // ---- NTP 立即校时 ----
    function ntpSync(){
      var b=document.getElementById('ntpBtn'),r=document.getElementById('ntpResult');
      if(b){b.disabled=true;}
      r.className='result-box result-loading';r.textContent='正在向 NTP 服务器校时...';
      fetch('/ntp',{method:'POST',cache:'no-store'}).then(jsonOrThrow).then(function(d){
        r.className='result-box '+(d.success?'result-success':'result-error');
        r.textContent=(d.message||'')+(d.nowLocal?('　当前：'+d.nowLocal):'');
      }).catch(function(e){r.className='result-box result-error';r.textContent='请求失败: '+e;})
        .finally(function(){if(b)b.disabled=false;});
    }

    // ---- Modem Control ----
    function modemAction(action){
      var names={'restart':'软重启','hardreset':'硬重启'};
      var name=names[action]||action;
      var resultEl=document.getElementById('modemRstResult');   // 仅 restart/hardreset 调用本函数
      if(action==='hardreset'){
        if(!confirm('硬重启将断电重启模组，确定继续？'))return;
        resultEl.className='result-box result-loading';resultEl.textContent='硬重启中（约10秒）...';
        fetch('/modem?action=hardreset',{method:'POST',cache:'no-store'}).then(jsonOrThrow).then(function(d){
          resultEl.className='result-box result-success';resultEl.textContent=d.message+' — 稍后请手动查询信号确认恢复';
        }).catch(function(e){resultEl.className='result-box result-error';resultEl.textContent='请求失败: '+e;});
        return;
      }
      resultEl.className='result-box result-loading';resultEl.textContent=name+'中...';
      fetch('/modem?action='+action,{method:'POST',cache:'no-store'}).then(jsonOrThrow).then(function(d){
        if(d.success){
          resultEl.className='result-box result-success';resultEl.textContent=name+'成功: '+d.message;
        }
        else{resultEl.className='result-box result-error';resultEl.textContent=name+'失败: '+d.message;}
      }).catch(function(e){resultEl.className='result-box result-error';resultEl.textContent='请求失败: '+e;});
    }

    // ---- AT Terminal ----
    function atTime(){var d=new Date(),p=function(n){return('0'+n).slice(-2)};return p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds());}
    function addLog(msg,type){
      type=type||'resp';var log=document.getElementById('atLog'),div=document.createElement('div'),b=document.createElement('b');
      if(!log)return;
      var ts=document.createElement('span');ts.className='at-ts';ts.textContent='['+atTime()+'] ';
      if(type==='user'){b.style.color='#fff';b.textContent='> ';}
      else if(type==='error'){b.style.color='#f44336';b.textContent='! ';}
      else{b.style.color='#50e3c2';b.textContent='';}
      div.appendChild(ts);div.appendChild(b);div.appendChild(document.createTextNode(msg));
      log.appendChild(div);log.scrollTop=log.scrollHeight;
    }
    var atHistory=[],atHistPos=-1,atDraft='';
    function sendAT(){
      var inp=document.getElementById('atCmd');if(!inp)return;var cmd=inp.value.trim();if(!cmd)return;
      var btn=document.getElementById('atBtn');
      if(btn&&btn.disabled)return;  // 上一条还在执行(含快捷指令入口)，避免并发 /at 请求堆积拖垮 httpd
      if(atHistory[atHistory.length-1]!==cmd){atHistory.push(cmd);if(atHistory.length>30)atHistory.shift();}
      atHistPos=-1;atDraft='';
      btn.disabled=true;btn.textContent='...';
      addLog(cmd,'user');inp.value='';
      fetch('/at?cmd='+encodeURIComponent(cmd), { method: 'POST', cache: 'no-store' }).then(jsonOrThrow).then(function(d){
        addLog(d.message,d.success?'resp':'error');
      }).catch(function(e){addLog('网络错误: '+e,'error')}).finally(function(){btn.disabled=false;btn.textContent='发送';});
    }
    function atQuick(cmd){
      var inp=document.getElementById('atCmd');if(!inp)return;
      var btn=document.getElementById('atBtn');
      if(btn&&btn.disabled)return;  // 上一条在途：不覆盖用户草稿，sendAT 反正也会拒发
      inp.value=cmd;sendAT();
    }
    function clearATLog(){var l=document.getElementById('atLog');if(!l)return;l.innerHTML='';addLog('日志已清空','resp');}
    function bindAtTerminal(){
      var el = document.getElementById('atCmd');
      if (!el || el.dataset.bound) return;
      el.dataset.bound = '1';
      el.addEventListener('keydown',function(e){
        if(e.key==='Enter'){sendAT();return;}
        // ↑/↓ 翻阅历史命令(常用调试指令不用反复输入)；开始翻阅时暂存未发送的草稿，翻过头恢复
        if(e.key==='ArrowUp'||e.key==='ArrowDown'){
          if(!atHistory.length)return;
          e.preventDefault();
          if(e.key==='ArrowUp'){
            if(atHistPos<0){atDraft=el.value;atHistPos=atHistory.length-1;}
            else atHistPos=Math.max(0,atHistPos-1);
          }else{
            if(atHistPos<0)return;  // 没在翻历史，不动用户正在输入的内容
            atHistPos+=1;
          }
          if(atHistPos>=atHistory.length){atHistPos=-1;el.value=atDraft;return;}
          el.value=atHistory[atHistPos];
        }
      });
    }

    // ---- Log Viewer (进入页/手动刷新重放保留日志，自动刷新只追新增) ----
    var logTimer = null, logSince = 0, logLines = [], logLoading = false, logReqSeq = 0;
    function startLogPoll() {
      if (logTimer) return;
      logTimer = setInterval(refreshLog, 2000);
    }
    function stopLogPoll() {
      if (logTimer) { clearInterval(logTimer); logTimer = null; }
    }
    function toggleLogAuto() {
      var auto = document.getElementById('logAuto');
      if (auto && auto.checked) startLogPoll();
      else stopLogPoll();
    }
    function setLogStatus(text) {
      var s = document.getElementById('logStatus');
      if (s) s.textContent = text || '';
    }
    function logNearBottom(el) {
      return el.scrollHeight - el.scrollTop - el.clientHeight < 48;
    }
    function renderLogView(emptyText) {
      var el = document.getElementById('logView');
      if (!el) return;
      var stick = logNearBottom(el);  // 用户上翻查看历史时，自动刷新不把滚动条拖回底部
      if (!logLines.length) {
        el.textContent = emptyText == null ? '(暂无日志)' : emptyText;
      } else {
        // 先转义再拼接高亮 span，无注入风险
        el.innerHTML = logLines.map(function(l) {
          var cls = /失败|错误|超时|异常|error|fail/i.test(l) ? ' err'
                  : (/重试|警告|warn|降级|忽略/i.test(l) ? ' warn' : '');
          return '<span class="log-line' + cls + '">' + htmlEsc(l) + '</span>';
        }).join('\n');
      }
      if (stick) el.scrollTop = el.scrollHeight;  // 只有原本就在底部时才跟随
    }
    function copyLogAll(btn) { copyText(logLines.join('\n'), btn); }
    function loadPrevLog(btn) {
      var el = document.getElementById('prevLogView');
      if (!el) return;
      if (btn) btn.disabled = true;
      el.textContent = '加载中...';
      fetch('/prevlog?_=' + Date.now(), { cache: 'no-store' }).then(textOrThrow).then(function(t) {
        el.textContent = t || '（空）';
      }).catch(function() {
        el.textContent = '无法获取上次日志';
      }).finally(function() { if (btn) btn.disabled = false; });
    }
    function clearCoredump(btn) {
      if (!confirm('清除设备中保存的崩溃转储？已下载的文件不受影响。')) return;
      if (btn) btn.disabled = true;
      fetch('/coredump/clear', { method: 'POST', cache: 'no-store' }).then(jsonOrThrow).then(function(d) {
        showToast(d.message || (d.success ? '已清除' : '清除失败'), d.success ? 'ok' : 'err');
      }).catch(function(e) {
        showToast('清除崩溃转储失败: ' + e, 'err');
      }).finally(function() { if (btn) btn.disabled = false; });
    }
    function initLogPanel() {
      reloadLog();
      var auto = document.getElementById('logAuto');
      if (!auto || auto.checked) startLogPoll();
      else stopLogPoll();
    }
    function clearLogUI() {
      logLines = [];
      renderLogView('');
      setLogStatus('已清空，之后只显示新日志');
    }
    function reloadLog() {
      logLines = [];
      logSince = 0;
      renderLogView('加载中...');
      refreshLog(true);
    }
    function refreshLog(fullReload) {
      fullReload = !!fullReload;
      if (!panelActive('log')) return;
      if (document.hidden && !fullReload) return;  // 后台标签页不轮询，省带宽与设备负载
      if (logLoading) return;
      var el = document.getElementById('logView');
      if (!el) return;
      var reqSince = fullReload ? 0 : logSince;
      logLoading = true;
      if (fullReload) setLogStatus('正在加载设备端保留日志...');
      // 挂死的请求会永久卡住 logLoading，2s 轮询从此空转：8s 后主动中断/放行
      var myReq = ++logReqSeq;
      var ctrl = window.AbortController ? new AbortController() : null;
      var opt = { cache: 'no-store' };
      if (ctrl) opt.signal = ctrl.signal;
      var timeoutId = setTimeout(function() {
        if (myReq !== logReqSeq) return;
        if (ctrl) ctrl.abort();
        else logLoading = false;
      }, 8000);
      fetch('/log?since=' + reqSince + '&_=' + Date.now(), opt).then(jsonOrThrow).then(function(d) {
        if (!panelActive('log')) return;
        if (!d || !Array.isArray(d.lines)) return;
        if (fullReload || d.seq < logSince) logLines = [];  // 重载/设备重启 -> 重置
        logSince = d.seq;
        if (d.lines.length) {
          logLines = logLines.concat(d.lines);
          if (logLines.length > 500) logLines = logLines.slice(logLines.length - 500);
          renderLogView();
        } else if (fullReload || el.textContent === '加载中...') {
          renderLogView('(暂无日志)');
        }
        setLogStatus('共 ' + logLines.length + ' 条');
      }).catch(function() {
        if (!panelActive('log')) return;
        if (fullReload || el.textContent === '加载中...') {
          renderLogView('无法获取日志');
          setLogStatus('日志请求失败');
        }
      }).finally(function() {
        clearTimeout(timeoutId);
        if (myReq === logReqSeq) logLoading = false;
      });
    }
    // ---- Keep-Alive (保号) ----
    function normalizeKaUrlForDisplay(url) {
      url = url || '';
      if (url.indexOf('gg.incrafttime.top/api/payload') >= 0) url = url.replace('size=128684', 'size=64342');
      return url;
    }
    function kaLoadStatus() {
      fetch('/keepalive?action=status&_=' + Date.now(), {cache:'no-store'}).then(jsonOrThrow).then(function(d) {
        var el = document.getElementById('kaEnabled'); if (el) el.checked = !!d.enabled;
        el = document.getElementById('kaIntervalDays'); if (el) el.value = d.intervalDays;
        el = document.getElementById('kaAction'); if (el) el.value = d.action;
        el = document.getElementById('kaTarget'); if (el) el.value = d.target || '';
        profInit('kaProfile', d.profile || '');
        var kaUrl = normalizeKaUrlForDisplay(d.url);
        el = document.getElementById('kaUrl'); if (el) el.value = kaUrl;
        el = document.getElementById('pingUrl'); if (el) el.value = kaUrl;
        kaSyncHead();
        kaArmForm();  // 状态回填成功后才允许提交 kaForm，避免加载失败时点保存清掉已存保号配置
        var cd = document.getElementById('kaCountdown');
        if (!cd) return;
        if (!d.timeValid) cd.textContent = '时间未同步，倒计时暂不可用';
        else if (!d.lastTime) cd.textContent = '尚未建立基准日，启用后首次检查时建立';
        else cd.textContent = '上次 ' + (d.lastTimeLocal || '--') + ' · 约 ' + d.daysLeft + ' 天后';
      }).catch(function(){});
    }
    // 保号卡：启用态高亮 + kaForm 安全 arm（与自定义任务 st* 同一套保护）
    // 切换启用高亮：首次(面板切入后异步回填)时关掉过渡，避免卡片闪一下；之后的用户操作正常过渡
    function applyCardEnabled(card, enabled) {
      if (!card) return;
      if (!card.dataset.animReady) {
        card.classList.add('no-anim');
        card.classList.toggle('enabled', enabled);
        void card.offsetWidth;               // 强制重排，让无过渡样式立即落地
        card.classList.remove('no-anim');
        card.dataset.animReady = '1';
      } else {
        card.classList.toggle('enabled', enabled);
      }
    }
    function kaSyncHead() {
      var en = document.getElementById('kaEnabled');
      applyCardEnabled(document.getElementById('kaCard'), !!(en && en.checked));
    }
    function kaArmForm() {
      if (document.getElementById('kaFormFlag')) return;
      var form = document.getElementById('kaForm');
      if (!form || !document.getElementById('kaCard')) return;
      var flag = document.createElement('input');
      flag.type = 'hidden'; flag.name = 'kaForm'; flag.value = '1'; flag.id = 'kaFormFlag';
      form.appendChild(flag);
    }
    var kaRunTimer = null;
    function kaPollRunStatus() {
      if (kaRunTimer) clearTimeout(kaRunTimer);
      fetch('/keepalive?action=status&_=' + Date.now(), {cache:'no-store'}).then(jsonOrThrow).then(function(d) {
        // 切走面板后停止轮询：否则请求在途时的 .then 会重臂定时器，
        // 轮询在其他面板上一直弹 toast，还会覆盖诊断页正在输入的 URL
        if (!panelActive('keepalive')) return;
        if (d.jobQueued || d.jobRunning) {
          showToast(d.jobMessage || '保号动作后台执行中…', 'loading');
          kaRunTimer = setTimeout(kaPollRunStatus, 1500);
          return;
        }
        if (d.jobDone) {
          showToast(d.jobMessage || (d.jobSuccess ? '保号动作已完成' : '保号动作失败'), d.jobSuccess ? 'ok' : 'err');
          kaLoadStatus();
        }
      }).catch(function(e){ if (panelActive('keepalive')) showToast('状态查询失败: ' + e, 'err'); });
    }
    function kaRun() {
      showToast('正在保存并提交保号任务…', 'loading');
      kaSaveForm().then(function(saved) {
        if (!saved) { showToast('保存失败，未执行', 'err'); return; }
        fetch('/keepalive?action=run', {method:'POST', cache:'no-store'}).then(jsonOrThrow).then(function(d) {
          if (d.success && d.queued) { showToast(d.message || '保号动作已排队', 'loading'); kaPollRunStatus(); }
          else { showToast(d.message || '任务启动失败', 'err'); }
        }).catch(function(e){ showToast('请求失败: ' + e, 'err'); });
      }).catch(function(e){ showToast('保存失败: ' + e, 'err'); });
    }
    function kaReset() {
      if (!confirm('确定把保号基准日重置为今天？')) return;
      showToast('正在重置保号基准日…', 'loading');
      fetch('/keepalive?action=reset', {method:'POST', cache:'no-store'}).then(jsonOrThrow).then(function(d) {
        showToast(d.message || (d.success ? '保号基准日已重置为今天' : '重置失败'), d.success === false ? 'err' : 'ok');
        kaLoadStatus();
      }).catch(function(e){ showToast('请求失败: ' + e, 'err'); });
    }

    // ---- eSIM Profile 下拉选项（来自 /esim 状态缓存，不触发读卡） ----
    var esimOptions = null;
    function profileOptionsFromProfiles(profiles) {
      return (Array.isArray(profiles) ? profiles : []).map(function(p) {
        var id = p.iccid || p.isdpAid || '';
        var label = (p.nickname || p.serviceProvider || p.profileName || '未命名') +
          (id.length > 4 ? '（…' + id.slice(-4) + '）' : '') + (p.state === 'enabled' ? ' · 当前启用' : '');
        return {v: id, t: label};
      }).filter(function(o){ return o.v; });
    }
    var esimOptionsReq = null;  // 在途请求去重：面板初始化会同时发起 7 次调用
    function loadEsimOptions(cb) {
      if (esimOptions) { cb(esimOptions); return; }
      if (!esimOptionsReq) {
        esimOptionsReq = fetch('/esim?action=status&_=' + Date.now(), {cache:'no-store'})
          .then(jsonOrThrow).then(function(d) {
            esimOptions = profileOptionsFromProfiles(d.profiles);
            return esimOptions;
          }).catch(function(){ return []; })
          .then(function(opts){ esimOptionsReq = null; return opts; });
      }
      esimOptionsReq.then(cb);
    }
    // 三件套约定：hiddenId(实际提交) / hiddenId+'Sel'(下拉) / hiddenId+'Custom'(手动输入)
    function profSelChange(hiddenId) {
      var sel = document.getElementById(hiddenId + 'Sel');
      var custom = document.getElementById(hiddenId + 'Custom');
      var hidden = document.getElementById(hiddenId);
      if (!sel || !hidden) return;
      var isCustom = sel.value === '__custom';
      if (custom) custom.style.display = isCustom ? '' : 'none';
      hidden.value = isCustom ? (custom ? custom.value.trim() : '') : sel.value;
    }
    function profInit(hiddenId, value) {
      var sel = document.getElementById(hiddenId + 'Sel');
      var custom = document.getElementById(hiddenId + 'Custom');
      var hidden = document.getElementById(hiddenId);
      if (!sel || !hidden) return;
      hidden.value = value || '';
      loadEsimOptions(function(options) {
        sel.innerHTML = '';
        function add(v, t) { var o = document.createElement('option'); o.value = v; o.textContent = t; sel.appendChild(o); }
        add('', '不切换（使用当前卡）');
        var matched = !value;
        options.forEach(function(o){ add(o.v, o.t); if (o.v === value) matched = true; });
        add('__custom', '手动输入 ICCID/别名…');
        sel.value = matched ? (value || '') : '__custom';
        if (!matched && custom) custom.value = value || '';
        profSelChange(hiddenId);
      });
    }

    // ---- 自定义定时任务（复用推送通道卡片样式） ----
    var ST_MAX = 6;
    var stTimer = null, stBuilt = false;
    function stBuildCard(i) {
      var card = document.createElement('div');
      card.className = 'push-channel st-task';
      card.id = 'stTask' + i;
      card.style.display = 'none';
      card.innerHTML =
        '<div class="push-channel-header">' +
          '<input type="checkbox" class="switch-input" name="st' + i + 'En" id="st' + i + 'En" onchange="stSyncHead(' + i + ')"><span class="switch-slider"></span>' +
          '<label for="st' + i + 'En" class="label-inline">启用任务 ' + (i + 1) + '</label>' +
          '<span class="ch-summary" id="st' + i + 'Sum"></span>' +
          '<span class="schedule-tag" id="st' + i + 'Countdown" style="margin-left:auto;">--</span>' +
        '</div>' +
        '<div class="push-channel-body">' +
          '<div class="form-row">' +
            '<div class="form-group"><label>任务名称</label><input type="text" name="st' + i + 'Name" id="st' + i + 'Name" maxlength="32" placeholder="如 副卡月检" oninput="stSyncHead(' + i + ')"></div>' +
            '<div class="form-group"><label>周期（天）</label><input type="number" name="st' + i + 'Days" id="st' + i + 'Days" min="1" max="3650" value="30"></div>' +
          '</div>' +
          '<div class="form-row">' +
            '<div class="form-group"><label>动作</label><select name="st' + i + 'Act" id="st' + i + 'Act"><option value="0">推送自定义提醒</option><option value="1">蜂窝 HTTP 下载(ping)</option><option value="2">发送短信</option><option value="3">USSD 查询</option></select></div>' +
            '<div class="form-group"><label>目标 eSIM Profile</label><input type="hidden" name="st' + i + 'Prof" id="st' + i + 'Prof"><select id="st' + i + 'ProfSel" onchange="profSelChange(&quot;st' + i + 'Prof&quot;)"></select><input type="text" id="st' + i + 'ProfCustom" maxlength="64" placeholder="手动输入 ICCID/别名" style="display:none;margin-top:8px;" oninput="profSelChange(&quot;st' + i + 'Prof&quot;)"></div>' +
          '</div>' +
          '<div class="form-group"><label>目标（URL / 号码 / USSD 码；HTTP 留空用保号 URL）</label><input type="text" name="st' + i + 'Tgt" id="st' + i + 'Tgt" maxlength="128"></div>' +
          '<div class="form-group"><label>内容（推送 / 短信正文）</label><input type="text" name="st' + i + 'Pay" id="st' + i + 'Pay" maxlength="128" placeholder="如：记得给副卡充值"></div>' +
          '<div class="st-task-foot"><label><input type="checkbox" name="st' + i + 'Back" id="st' + i + 'Back" checked> 完成后切回原卡</label>' +
          '<span class="btn-row"><button type="button" class="btn btn-secondary btn-sm" onclick="stRun(' + i + ')">立即执行</button><button type="button" class="btn btn-secondary btn-sm" onclick="stResetBase(' + i + ')">基准日=今天</button><button type="button" class="btn btn-danger btn-sm" onclick="stRemove(' + i + ')">删除任务</button></span></div>' +
        '</div>';
      return card;
    }
    function stBuildCards() {
      var wrap = document.getElementById('stTasks');
      if (!wrap || stBuilt) return;
      stBuilt = true;
      for (var i = 0; i < ST_MAX; i++) wrap.appendChild(stBuildCard(i));
    }
    function stShow(i, show) {
      var el = document.getElementById('stTask' + i);
      if (el) el.style.display = show ? '' : 'none';
      var visible = 0;
      for (var k = 0; k < ST_MAX; k++) { var c = document.getElementById('stTask' + k); if (c && c.style.display !== 'none') visible++; }
      var btn = document.getElementById('stAddBtn');
      if (btn) btn.style.display = visible >= ST_MAX ? 'none' : '';
      var hint = document.getElementById('stEmptyHint');
      if (hint) hint.style.display = visible ? 'none' : '';
    }
    function stAddTask() {
      for (var i = 0; i < ST_MAX; i++) {
        var el = document.getElementById('stTask' + i);
        if (el && el.style.display === 'none') {
          var en = document.getElementById('st' + i + 'En'); if (en) en.checked = true;
          profInit('st' + i + 'Prof', '');
          stShow(i, true);
          stSyncHead(i);
          return;
        }
      }
    }
    function stRemove(i) {
      if (!confirm('删除任务 ' + (i + 1) + '？点击“保存自定义任务”后生效。')) return;
      ['Name', 'Tgt', 'Pay', 'Prof', 'ProfCustom'].forEach(function(sfx){ var el = document.getElementById('st' + i + sfx); if (el) el.value = ''; });
      var el = document.getElementById('st' + i + 'Days'); if (el) el.value = 30;
      el = document.getElementById('st' + i + 'Act'); if (el) el.value = 0;
      el = document.getElementById('st' + i + 'En'); if (el) el.checked = false;
      el = document.getElementById('st' + i + 'Back'); if (el) el.checked = true;
      el = document.getElementById('st' + i + 'ProfSel'); if (el) el.value = '';
      el = document.getElementById('st' + i + 'ProfCustom'); if (el) el.style.display = 'none';
      stShow(i, false);
    }
    function stSyncHead(i) {
      var name = document.getElementById('st' + i + 'Name');
      var sum = document.getElementById('st' + i + 'Sum');
      if (sum) sum.textContent = name && name.value ? name.value : '';
      var en = document.getElementById('st' + i + 'En');
      applyCardEnabled(document.getElementById('stTask' + i), !!(en && en.checked));
    }
    function stArmForm() {
      // stForm 标志只在状态成功回填后注入：加载失败/未完成时点保存
      // 不会带上 stForm，固件就不会用一屏默认值清掉已保存的任务
      if (document.getElementById('stFormFlag')) return;
      var form = document.getElementById('stForm');
      if (!form) return;
      var flag = document.createElement('input');
      flag.type = 'hidden'; flag.name = 'stForm'; flag.value = '1'; flag.id = 'stFormFlag';
      form.appendChild(flag);
    }
    function stSetVal(id, v) { var el = document.getElementById(id); if (el) el.value = (v == null ? '' : v); }
    function stCardUsed(t) { return !!(t.enabled || t.name || t.profile || t.target || t.payload); }
    function stLoadStatus(polling) {
      stBuildCards();
      fetch('/schedtask?action=status&_=' + Date.now(), {cache:'no-store'}).then(jsonOrThrow).then(function(d) {
        if (!panelActive('keepalive')) return;
        var tasks = Array.isArray(d.tasks) ? d.tasks : [];
        for (var i = 0; i < tasks.length && i < ST_MAX; i++) {
          var t = tasks[i];
          if (!polling) {  // 轮询期间不覆盖正在编辑的表单
            var el = document.getElementById('st' + i + 'En'); if (el) el.checked = !!t.enabled;
            el = document.getElementById('st' + i + 'Back'); if (el) el.checked = !!t.switchBack;
            stSetVal('st' + i + 'Name', t.name); stSetVal('st' + i + 'Days', t.intervalDays);
            stSetVal('st' + i + 'Act', t.action);
            stSetVal('st' + i + 'Tgt', t.target); stSetVal('st' + i + 'Pay', t.payload);
            profInit('st' + i + 'Prof', t.profile || '');
            stShow(i, stCardUsed(t));
            stSyncHead(i);
          }
          var cd = document.getElementById('st' + i + 'Countdown');
          if (cd) {
            if (!t.enabled) cd.textContent = '未启用';
            else if (!d.timeValid) cd.textContent = '时间未同步';
            else if (t.daysLeft < 0) cd.textContent = '待建立基准日';
            else if (t.daysLeft === 0) cd.textContent = '已到期，等待执行';
            else cd.textContent = '上次 ' + (t.lastLocal || '--').slice(0, 10) + ' · 约 ' + t.daysLeft + ' 天后';
          }
        }
        if (!polling) stArmForm();
        if (d.jobQueued || d.jobRunning) {
          showToast(d.jobMessage || '定时任务后台执行中…', 'loading');
        } else if (d.jobDone && polling) {
          showToast(d.jobMessage || (d.jobSuccess ? '定时任务已完成' : '定时任务失败'), d.jobSuccess ? 'ok' : 'err');
        }
        if (stTimer) clearTimeout(stTimer);
        if (d.jobQueued || d.jobRunning) stTimer = setTimeout(function(){ stLoadStatus(true); }, 2000);
      }).catch(function(e){
        if (!panelActive('keepalive')) return;
        if (polling) showToast('任务状态查询失败: ' + e, 'err');
      });
    }
    function saveScopedForm(formId, flagId) {
      if (flagId && !document.getElementById(flagId)) return Promise.resolve(false);
      var f = document.getElementById(formId);
      if (!f) return Promise.resolve(false);
      var fd = new FormData(f);
      var body = new URLSearchParams();
      fd.forEach(function(v, k){ body.append(k, v); });
      body.append('ajax', '1');
      return fetchSaveWithTimeout(body).then(function(r) {
        if (!r.ok) return false;
        return ensureConfig(true).catch(function(){ return null; }).then(function(){ return true; });
      });
    }
    // 立即执行前只保存对应任务域：避免一个按钮顺手重写其它定时配置
    function kaSaveForm() { return saveScopedForm('kaForm', 'kaFormFlag'); }
    function stSaveForm() { return saveScopedForm('stForm', 'stFormFlag'); }
    function stRun(i) {
      if (!confirm('立即执行任务 ' + (i + 1) + '？将先保存当前配置再执行。')) return;
      showToast('正在保存并提交任务…', 'loading');
      stSaveForm().then(function(saved) {
        if (!saved) { showToast('保存失败，未执行', 'err'); return; }
        fetch('/schedtask?action=run&index=' + i, {method:'POST', cache:'no-store'}).then(jsonOrThrow).then(function(d) {
          if (d.success) { showToast(d.message || '任务已排队', 'loading'); stLoadStatus(true); }
          else { showToast(d.message || '任务启动失败', 'err'); }
        }).catch(function(e){ showToast('请求失败: ' + e, 'err'); });
      }).catch(function(e){ showToast('保存失败: ' + e, 'err'); });
    }
    function stResetBase(i) {
      if (!confirm('把任务 ' + (i + 1) + ' 的基准日重置为今天？')) return;
      fetch('/schedtask?action=reset&index=' + i, {method:'POST', cache:'no-store'}).then(jsonOrThrow).then(function(d) {
        showToast(d.message || (d.success ? '基准日已重置' : '重置失败'), d.success ? 'ok' : 'err');
        stLoadStatus(true);
      }).catch(function(e){ showToast('请求失败: ' + e, 'err'); });
    }

    // ---- NET 指示灯临时开关 ----
    function netLedTemp(on) {
      showToast((on ? '正在开启' : '正在关闭') + ' NET 指示灯…', 'loading');
      fetch('/netled?action=' + (on ? 'on' : 'off'), {method:'POST', cache:'no-store'}).then(jsonOrThrow).then(function(d) {
        showToast(d.message || (d.success ? (on ? 'NET 灯已开启' : 'NET 灯已关闭') : '操作失败'), d.success ? 'ok' : 'err');
      }).catch(function(e){ showToast('请求失败: ' + e, 'err'); });
    }

    // ---- Overview dashboard (live /status) ----
    function ovSet(id, v) { var e = document.getElementById(id); if (e) e.textContent = v; }
    function fmtCsq(c) {
      if (c == null || c >= 99 || c < 0) return '无信号';
      var dbm = -113 + 2 * c;
      return dbm + ' dBm';
    }
    function modemPhaseText(d) {
      var p = d && d.modemInitPhase;
      if (p === 'powering') return '模组上电中';
      if (p === 'at_ready') return 'AT已就绪';
      if (p === 'registering') return '网络注册中';
      if (p === 'sampling') return '读取信息中';
      if (p === 'ready') return '已就绪';
      if (p === 'failed') return '注册超时';
      return d && d.modemReady ? '已就绪' : '启动中';
    }
    function signalPendingText(d) {
      if (!d || !d.signalFresh) {
        if (d && d.modemInitPhase === 'sampling') return modemPhaseText(d);
        if (Date.now() < modemSampleUntil) return '采样中';
        return d && d.modemReady ? '未刷新' : modemPhaseText(d);
      }
      if (!d.modemReady && d.modemInitPhase !== 'ready') return '注册中';
      return '无信号';
    }
    function fmtCsqStatus(d) {
      return (!d || d.csq == null || d.csq >= 99 || d.csq < 0) ? signalPendingText(d) : (d.csq + ' /31');
    }
    function fmtRssiStatus(d) {
      return (!d || d.csq == null || d.csq >= 99 || d.csq < 0) ? signalPendingText(d) : fmtCsq(d.csq);
    }
    function pendingValue(v, fresh, d) {
      return v || ((d && d.modemInitPhase === 'sampling') ? '读取中' : (fresh ? '--' : (Date.now() < modemSampleUntil ? '采样中' : '未刷新')));
    }
    function apnText(d) {
      if (d.apnSim) return d.apnSim;
      if (d.apn) return d.apn;
      if (d && d.modemInitPhase === 'sampling') return '读取中';
      if (Date.now() < modemSampleUntil && !d.identityFresh) return '采样中';
      if (!d.identityFresh && d.modemReady) return '未刷新';
      return '自动（运营商默认）';
    }
    var devTz = 480;  // 设备时区分钟偏移(从 /status 更新)，按此格式化时间，与查看者所在时区无关
    var apHandled = false;  // 配网模式只自动跳转一次
    function fmtEpoch(ep) {
      if (!ep) return '(无)';
      var d = new Date((ep + devTz * 60) * 1000), p = function(n){ return ('0' + n).slice(-2); };
      return d.getUTCFullYear() + '-' + p(d.getUTCMonth() + 1) + '-' + p(d.getUTCDate()) + ' ' + p(d.getUTCHours()) + ':' + p(d.getUTCMinutes());
    }
    // 详情抽屉里用到秒的完整时间
    function fmtEpochFull(ep) {
      if (!ep) return '(无)';
      var d = new Date((ep + devTz * 60) * 1000), p = function(n){ return ('0' + n).slice(-2); };
      return d.getUTCFullYear() + '-' + p(d.getUTCMonth() + 1) + '-' + p(d.getUTCDate()) + ' ' +
             p(d.getUTCHours()) + ':' + p(d.getUTCMinutes()) + ':' + p(d.getUTCSeconds());
    }
    function fmtClockEpoch(ep) {
      if (!ep) return '--';
      var d = new Date((ep + devTz * 60) * 1000), p = function(n){ return ('0' + n).slice(-2); };
      return p(d.getUTCHours()) + ':' + p(d.getUTCMinutes()) + ':' + p(d.getUTCSeconds());
    }
    function fmtRsrp(r) { if (r == null || r >= 0 || r < -200) return '--'; return r + ' dBm'; }
    function fmtBer(b) { return (b == null || b >= 99) ? '99 (未知)' : String(b); }
    function fmtDb(v, unit) { if (v == null || v === 999 || v < -200) return '--'; return v + (unit || ' dB'); }
    function fmtUptime(s) { var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60; return h + ':' + ('0' + m).slice(-2) + ':' + ('0' + x).slice(-2); }
    // ESP-IDF esp_reset_reason_t 显示名；11=USB 外设复位，常见于 USB 串口/下载/主机重新枚举。
    function resetReasonText(v) {
      var m = {0:'未知',1:'上电/复位键',2:'外部引脚',3:'软件重启',4:'异常崩溃',5:'中断看门狗',6:'任务看门狗',7:'其他看门狗',8:'深睡眠唤醒',9:'棕断/欠压',10:'SDIO复位',11:'USB复位',12:'JTAG复位',13:'eFuse错误',14:'电源毛刺',15:'CPU锁死'};
      return (m[v] || '原因码') + ' (' + v + ')';
    }
    function kb(b) { return Math.round(b / 1024) + ' KB'; }
    // 信号量表填充：raw 映射到 lo..hi 百分比，按 warnAt/okAt 染色(越大越好)
    function setGauge(id, raw, lo, hi, label, warnAt, okAt) {
      var fill = document.getElementById(id), vEl = document.getElementById(id + 'V');
      if (vEl) vEl.textContent = label;
      if (!fill) return;
      var bad = (raw == null || raw === 999 || raw < -200 || (lo === 0 && hi === 31 && raw >= 99));
      if (bad) { fill.style.width = '0'; fill.className = 'fill'; return; }
      var pct = Math.max(2, Math.min(100, (raw - lo) / (hi - lo) * 100));
      fill.style.width = pct + '%';
      fill.className = 'fill ' + (raw >= okAt ? 'ok' : raw >= warnAt ? 'warn' : 'bad');
    }
    var statusTimer = null, statusPolling = false, statusSeq = 0, statusAbort = null, statusLoading = false, statusFailCount = 0, devEpochBase = 0, devEpochBaseMs = 0, latestStatusKey = '', statusFastUntil = 0, modemSampleUntil = 0;
    function deviceEpochNow() {
      if (!devEpochBase) return 0;
      return devEpochBase + Math.floor((Date.now() - devEpochBaseMs) / 1000);
    }
    function updateDeviceClockLabel() {
      var ep = deviceEpochNow();
      if (ep) ovSet('ovRefresh', '设备 ' + fmtClockEpoch(ep));
    }
    function loadStatus(sample) {
      if (statusLoading) {
        if (sample) setTimeout(function(){ loadStatus(true); }, 300);
        return;  // 上一次 /status 还没回来时不叠加请求，避免 ESP32 被轮询拖慢
      }
      statusLoading = true;
      var seq = ++statusSeq, timedOut = false, finished = false;
      var ctrl = window.AbortController ? new AbortController() : null;
      statusAbort = ctrl;
      var opt = { cache: 'no-store' };
      if (ctrl) opt.signal = ctrl.signal;
      var timeoutId = setTimeout(function() {
        if (finished || seq !== statusSeq) return;
        timedOut = true;
        if (ctrl) ctrl.abort();
        else {
          // 无 AbortController 时不能真中断：同步放开 statusLoading 闸门，
          // 否则一个挂死的 /status 会永久冻结概览刷新
          ovSet('ovRefresh', '刷新超时');
          finished = true;
          statusLoading = false;
        }
      }, 7000);
      var url = '/status?' + (sample ? 'sample=1&' : '') + '_=' + Date.now();
      fetch(url, opt).then(jsonOrThrow).then(function(d) {
        if (seq !== statusSeq || timedOut) return;  // 旧响应直接丢弃，只显示最新状态
        statusFailCount = 0;
        if (typeof d.tz === 'number') devTz = d.tz;
        if (typeof d.nowEpoch === 'number' && d.nowEpoch > 100000) {
          devEpochBase = d.nowEpoch;
          devEpochBaseMs = Date.now();
          updateDeviceClockLabel();
        }
        // KPI 指标条
        setGauge('gCsq', d.csq, 0, 31, fmtCsqStatus(d), 8, 14);  // CSQ 原始值
        setGauge('gRssi', (d.csq == null || d.csq >= 99) ? null : (-113 + 2 * d.csq), -110, -50, fmtRssiStatus(d), -95, -75);  // RSSI=CSQ 换算 dBm(与诊断"信号查询"一致)
        var samplingNow = Date.now() < modemSampleUntil;
        setGauge('gRsrp', d.rsrp, -120, -70, (samplingNow && (d.rsrp == null || d.rsrp >= 0 || d.rsrp === 999)) ? '采样中' : ((!d.modemReady && (d.rsrp == null || d.rsrp >= 0)) ? '注册后采样' : fmtRsrp(d.rsrp)), -110, -100);
        setGauge('gRsrq', d.rsrq, -20, -3, (samplingNow && (d.rsrq == null || d.rsrq === 999)) ? '采样中' : ((!d.modemReady && (d.rsrq == null || d.rsrq === 999)) ? '注册后采样' : fmtDb(d.rsrq)), -15, -10);
        setGauge('gSinr', d.sinr, 0, 30, (samplingNow && (d.sinr == null || d.sinr === 999)) ? '采样中' : ((!d.modemReady && (d.sinr == null || d.sinr === 999)) ? '注册后采样' : fmtDb(d.sinr)), 0, 13);
        setGauge('gWifi', (d.rssi == null || d.rssi >= 0) ? null : d.rssi, -90, -40, (d.rssi == null || d.rssi >= 0) ? '--' : (d.rssi + ' dBm'), -75, -65);  // WiFi RSSI(质量见下方卡片)
        ovSet('ovData', d.dataEnabled ? '已启用' : '已禁用');
        ovSet('ovDataSub', d.dataEnabled ? (d.cellIp || '获取中') : '零流量');
        ovSet('ovSms', d.smsTotal); ovSet('ovLastSms', '最近 ' + fmtEpoch(d.lastSmsEpoch));
        ovSet('ovInbox', d.inboxCount + ' 条');
        var busyQueues = !!(d.slowBusy || d.fwdQueueDepth || d.queueDepth || d.outSmsQueueDepth || d.emailQueueDepth);
        if (busyQueues) statusFastUntil = Date.now() + 12000;
        var lk = [d.smsTotal, d.lastSmsEpoch, d.inboxCount, d.fwdQueueDepth, d.queueDepth, d.slowBusy ? 1 : 0].join(':');
        if (lk !== latestStatusKey) {
          latestStatusKey = lk;
          loadLatestOtp();
        }
        // 设备 / 固件信息卡片
        ovSet('dvHeap', kb(d.freeHeap) + ' · 最低 ' + kb(d.minFreeHeap));
        window.__upt = d.uptime; ovSet('dvUptime', fmtUptime(d.uptime)); ovSet('dvEspVer', d.version);
        ovSet('dvTemp', (d.chipTemp != null ? d.chipTemp + ' ℃' : '--'));
        ovSet('dvMfr', pendingValue(d.mfr, d.identityFresh, d)); ovSet('dvModel', pendingValue(d.model, d.identityFresh, d)); ovSet('dvFw', pendingValue(d.fwver, d.identityFresh, d));
        // SIM 卡信息
        ovSet('tOp', d.operator || (d.modemInitPhase === 'sampling' ? '读取中' : (d.modemReady ? '未刷新' : modemPhaseText(d)))); ovSet('tModem', modemPhaseText(d));
        ovSet('tCellIp', d.dataEnabled ? (d.cellIp || '获取中') : '— (未启用)');
        ovSet('tPhone', d.phone || '--'); ovSet('tImei', pendingValue(d.imei, d.identityFresh, d)); ovSet('tIccid', pendingValue(d.iccid, d.identityFresh, d));
        ovSet('tImsi', pendingValue(d.imsi, d.identityFresh, d)); ovSet('tApn', apnText(d));
        // WiFi 详细信息卡片(独立卡，原"网络与SIM"里的 WiFi 行已移出)
        ovSet('wfSsid', d.ssid || '--');
        ovSet('wfIp', d.ip || '--'); ovSet('wfGw', d.gw || '--'); ovSet('wfMask', d.mask || '--');
        ovSet('wfDns', d.dns || '--'); ovSet('wfMac', d.mac || '--'); ovSet('wfBssid', d.bssid || '--');
        ovSet('wfChan', (d.chan != null && d.chan > 0) ? d.chan : '--');
        // 转发与系统
        var qText = d.fwdQueueDepth + ' / ' + d.queueDepth + ' / ' + (d.outSmsQueueDepth || 0) + ' / ' + (d.emailQueueDepth || 0);
        if (d.slowBusy) qText += ' · 推送中';
        ovSet('tQueue', qText);
        ovSet('tMaxBlock', kb(d.maxAllocHeap));
        ovSet('tReset', resetReasonText(d.resetReason));
        ovSet('tTime', (d.timeSynced ? '已同步 ' : '未同步 ') + (d.nowEpoch > 100000 ? fmtEpoch(d.nowEpoch) : '--'));
        ovSet('cfgEmail', d.emailEnabled ? (d.emailConfigured ? '已配置' : '未配置') : '已关闭');
        ovSet('cfgPush', d.pushEnabled ? ((d.pushEnabledCount || 0) + ' 个已启用') : '已关闭');
        ovSet('cfgAdmin', d.adminPhone || '--');
        if (d.apMode) {
          var lv = document.getElementById('ovLive');
          if (lv) { lv.textContent = '● 配网模式（请到 WiFi 设置配置网络）'; lv.style.color = 'var(--error)'; }
          if (!apHandled) { apHandled = true; switchPanel('sim'); }  // WiFi 配网表单在 SIM/网络页
        }
      }).catch(function() {
        if (seq !== statusSeq) return;
        statusFailCount++;
        if (timedOut) { ovSet('ovRefresh', '刷新超时'); return; }
        ovSet('ovRefresh', '刷新失败');
        if (statusFailCount >= 3) {
          var e = document.getElementById('ovLive'); if (e) { e.textContent = '● 离线'; e.style.color = 'var(--error)'; }
        }
      }).then(function() {
        if (seq !== statusSeq) return;
        finished = true;
        statusLoading = false;
        clearTimeout(timeoutId);
        if (statusAbort === ctrl) statusAbort = null;
      });
    }
    function scheduleStatusPoll(delay) {
      if (!statusPolling || statusTimer) return;
      statusTimer = setTimeout(function() {
        statusTimer = null;
        if (!statusPolling) return;
        if (!document.hidden) loadStatus();
        scheduleStatusPoll(Date.now() < statusFastUntil ? 800 : 2000);
      }, delay);
    }
    function startStatusPoll() {
      if (statusPolling) return;
      statusPolling = true;
      scheduleStatusPoll(500);
      // 运行时长本地秒表：每秒自增显示(不靠轮询)，由 loadStatus 拉到的 uptime 校准
      if (!window.__uptTimer) window.__uptTimer = setInterval(function(){
        if (window.__upt != null && !document.hidden) { window.__upt++; var e = document.getElementById('dvUptime'); if (e) e.textContent = fmtUptime(window.__upt); }
        if (!document.hidden) updateDeviceClockLabel();
      }, 1000);
    }
    function stopStatusPoll() { statusPolling = false; if (statusTimer) { clearTimeout(statusTimer); statusTimer = null; } }
    function refreshModemInfo(btn) {
      var old = btn ? btn.textContent : '';
      if (btn) { btn.disabled = true; btn.textContent = '采样中...'; }
      ovSet('ovRefresh', '正在采样...');
      modemSampleUntil = Date.now() + 15000;
      statusFastUntil = Date.now() + 15000;
      loadStatus(true);
      [700, 1500, 2500, 4000].forEach(function(delay) {
        setTimeout(function() { if (panelActive('overview')) loadStatus(); }, delay);
      });
      setTimeout(function() {
        if (btn) { btn.disabled = false; btn.textContent = old || '刷新模组信息'; }
      }, 3000);
    }

    // ---- SIM / 网络 ----
    // ---- SMS (收 / 发) ----
    var smsBox = 'recv';
    function smsTab(b) {
      smsBox = b;
      document.getElementById('tabRecv').classList.toggle('active', b === 'recv');
      document.getElementById('tabSent').classList.toggle('active', b === 'sent');
      loadMessages();
    }
    // 验证码提取：短信须含验证码类关键词才提取(营销/通知类不出芯片)，
    // 并跳过日期/时间片段(15/07/2026、2026-07-06、12:30)与更长数字串(订单号/手机号)，
    // 优先取关键词之后最近的 4-8 位独立数字串(G-123456 类前置码退回首个候选)
    function otpExtract(t) {
      t = String(t);
      var kw = t.search(/[验驗][证證][码碼]|[校检檢][验驗][码碼]|[动動][态態]密?[码碼]|[确確][认認][码碼]|激活[码碼]|安全[码碼]|取件[码碼]|[授][权權][码碼]|[随隨][机機][码碼]|短信[码碼]|登[录錄入][码碼]|一次性|verif|one[- ]?time|otp|passcode|auth code|security code|\b2fa\b|\bcode\b|\bpin\b/i);
      if (kw < 0) return '';
      var re = /\d{4,8}/g, m, first = '', after = '';
      while ((m = re.exec(t))) {
        var s = m[0], i = m.index, j = i + s.length;
        var prev = i > 0 ? t.charAt(i - 1) : '', next = j < t.length ? t.charAt(j) : '';
        if (/\d/.test(prev) || /\d/.test(next)) continue;                    // 更长数字串的一段
        if (/[\/:.\-]/.test(prev) && /\d/.test(t.charAt(i - 2) || '')) continue;  // 日期/时间后半段
        if (/[\/:.\-]/.test(next) && /\d/.test(t.charAt(j + 1) || '')) continue;  // 日期/时间前半段
        if (!first) first = s;
        if (i > kw) { after = s; break; }
      }
      return after || first;
    }
    function htmlEsc(s) { return String(s).replace(/[&<>"]/g, function(c){ return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]; }); }
    function copyText(txt, el) {
      try { if (navigator.clipboard) navigator.clipboard.writeText(txt); } catch (e) {}
      if (el) { var o = el.textContent; el.textContent = '已复制'; setTimeout(function(){ el.textContent = o; }, 800); }
    }
    function loadMessages() {
      var box = document.getElementById('inboxList');
      if (!box) return;
      var reqBox = smsBox;
      var sent = (smsBox === 'sent');
      // 有同类缓存先即时渲染(避免每次进入都空白"加载中")，再后台拉取刷新
      if (window.__msgFull && window.__msgBox === (sent ? 'sent' : 'recv')) renderMessages();
      else box.textContent = '加载中...';
      fetch('/messages' + (sent ? '?box=sent&' : '?') + '_=' + Date.now(), {cache:'no-store'}).then(jsonOrThrow).then(function(arr) {
        if (!panelActive('inbox') || reqBox !== smsBox) return;
        if (!Array.isArray(arr)) arr = [];
        window.__msgFull = arr;                 // 全量缓存：搜索本地过滤，不再每按键请求
        window.__msgBox = sent ? 'sent' : 'recv';
        var cnt = document.getElementById(sent ? 'cntSent' : 'cntRecv');
        if (cnt) cnt.textContent = arr.length ? String(arr.length) : '';
        renderMessages();
      }).catch(function() { if (document.getElementById('inboxList')) box.textContent = '无法获取短信'; });
    }
    // 仅用缓存过滤+渲染(搜索框输入调用，零网络请求，瞬时响应)
    function renderMessages() {
      var box = document.getElementById('inboxList');
      if (!box) return;
      var sent = (window.__msgBox === 'sent');
      var qEl = document.getElementById('smsSearch');
      var q = (qEl && qEl.value || '').trim().toLowerCase();
      var arr = (window.__msgFull || []).slice();
      if (q) arr = arr.filter(function(m){ var who = sent ? (m.target||'') : (m.sender||''); return (who + ' ' + (m.text||'')).toLowerCase().indexOf(q) >= 0; });
      box.innerHTML = '';
      if (!arr.length) {
        var em = document.createElement('div'); em.className = 'msg-empty';
        em.textContent = q ? ('未匹配 “' + q + '”') : (sent ? '(暂无已发送短信)' : '(暂无短信)'); box.appendChild(em); return;
      }
      window.__msgCache = arr;
      arr.forEach(function(m) {
          var d = document.createElement('div'); d.className = 'msg'; d.style.cursor = 'pointer'; d.onclick = function(){ openMsgDrawer(m.id); };
          var h = document.createElement('div'); h.className = 'msg-head';
          var s = document.createElement('span'); s.className = 'msg-sender';
          var right = document.createElement('span'); right.style.display = 'flex'; right.style.gap = '6px'; right.style.alignItems = 'center';
          var otp = sent ? '' : otpExtract(m.text || '');
          if (otp) {
            var oc = document.createElement('span'); oc.className = 'msg-chip otp'; oc.textContent = '⎘ ' + otp; oc.title = '点击复制验证码';
            oc.onclick = function(ev){ ev.stopPropagation(); copyText(otp, oc); };
            right.appendChild(oc);
          }
          var chip = document.createElement('span'); chip.className = 'msg-chip';
          if (sent) {
            s.textContent = m.target || '(未知)';
            chip.textContent = m.ok ? '发送成功' : '发送失败'; chip.className += m.ok ? ' ok' : ' bad';
          } else {
            s.textContent = m.sender || '(未知)';
            chip.textContent = m.fwd ? '已处理' : '待处理'; chip.className += m.fwd ? ' ok' : ' wait';
          }
          right.appendChild(chip);
          h.appendChild(s); h.appendChild(right);
          var t = document.createElement('div'); t.className = 'msg-time';
          t.textContent = fmtEpoch(sent ? m.sent : m.recv);
          // 列表里长正文折叠到 4 行(点开抽屉看全文)，避免一条长短信占满整屏
          var b = document.createElement('div'); b.className = 'msg-body clamp';
          var body = htmlEsc(m.text || '');
          if (otp) body = body.replace(otp, '<mark>' + otp + '</mark>');  // otp 为纯数字，先转义再高亮，无注入风险
          b.innerHTML = body;
          d.appendChild(h); d.appendChild(t); d.appendChild(b); box.appendChild(d);
        });
    }

    // ---- System maintenance ----
    function sysReboot() {
      if (!confirm('确定重启设备？约 15 秒后恢复。')) return;
      var r = document.getElementById('sysResult');
      r.className = 'result-box result-loading'; r.textContent = '正在重启...';
      fetch('/reboot', {method:'POST', cache:'no-store'}).then(jsonOrThrow).then(function(d){
        r.className = 'result-box result-success'; r.textContent = d.message;
      }).catch(function(){ r.className = 'result-box result-info'; r.textContent = '设备正在重启，请稍后刷新页面'; });
    }
    function sysFactory() {
      if (!confirm('确定恢复出厂设置？将清空所有配置并重启，不可撤销！')) return;
      if (!confirm('再次确认：账号/邮件/推送/保号等全部配置都会被清除！')) return;
      var r = document.getElementById('sysResult');
      r.className = 'result-box result-loading'; r.textContent = '正在清除配置并重启...';
      fetch('/factory', {method:'POST', cache:'no-store'}).then(jsonOrThrow).then(function(d){
        r.className = 'result-box result-success'; r.textContent = d.message;
      }).catch(function(){ r.className = 'result-box result-info'; r.textContent = '设备正在重启，请稍后用默认账号登录'; });
    }

    // ---- eSIM 本地管理 ----
    var esimTimer = null;
    function esimStateText(s) {
      if (s === 'enabled') return '启用';
      if (s === 'disabled') return '禁用';
      return s || '未知';
    }
    function esimMaskedId(id) {
      id = String(id || '');
      return id.length > 8 ? id.slice(0, 4) + '****' + id.slice(-4) : id;
    }
    function esimActionButton(label, cls, action, id, nick) {
      var btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'btn btn-sm ' + cls;
      btn.textContent = label;
      btn.onclick = function(){ esimRun(action, id, nick); };
      return btn;
    }
    function esimRender(d) {
      var el = document.getElementById('esimEid'); if (el) el.textContent = d.eid || '未读取';
      el = document.getElementById('esimCount'); if (el) el.textContent = '共 ' + String(d.profileCount == null ? 0 : d.profileCount) + ' 个';
      el = document.getElementById('esimUpdated'); if (el) el.textContent = d.updatedLocal || '--';
      var tbody = document.getElementById('esimProfiles');
      if (!tbody) return;
      tbody.innerHTML = '';
      var profiles = Array.isArray(d.profiles) ? d.profiles : [];
      esimOptions = profileOptionsFromProfiles(profiles);
      if (!profiles.length) {
        var empty = document.createElement('tr');
        var cell = document.createElement('td');
        cell.colSpan = 4;
        cell.className = 'msg-empty';
        cell.textContent = '尚未读取 eSIM Profile';
        empty.appendChild(cell);
        tbody.appendChild(empty);
        return;
      }
      profiles.forEach(function(p) {
        var id = p.iccid || p.isdpAid || '';
        var row = document.createElement('tr');
        var iccid = document.createElement('td');
        iccid.textContent = p.iccid || p.isdpAid || '--';
        var nick = document.createElement('td');
        nick.className = 'nick';
        nick.textContent = p.nickname || p.profileName || p.serviceProvider || '--';
        var state = document.createElement('td');
        state.className = p.state === 'enabled' ? 'state-enabled' : 'state-disabled';
        state.textContent = esimStateText(p.state);
        var ops = document.createElement('td');
        var wrap = document.createElement('div');
        wrap.className = 'ops';
        if (p.state === 'enabled') {
          wrap.appendChild(esimActionButton('禁用', 'btn-danger', 'disable', id, p.nickname || ''));
          wrap.appendChild(esimActionButton('昵称', 'btn-secondary', 'nickname', id, p.nickname || ''));
        } else {
          wrap.appendChild(esimActionButton('切换/启用', 'btn-secondary', 'switch', id, p.nickname || ''));
          wrap.appendChild(esimActionButton('昵称', 'btn-secondary', 'nickname', id, p.nickname || ''));
          wrap.appendChild(esimActionButton('删除', 'btn-danger', 'delete', id, p.nickname || ''));
        }
        ops.appendChild(wrap);
        row.appendChild(iccid); row.appendChild(nick); row.appendChild(state); row.appendChild(ops);
        tbody.appendChild(row);
      });
      if (d.jobQueued || d.jobRunning) {  // 任务执行中禁用操作按钮，避免误触重复提交
        var btns = tbody.getElementsByTagName('button');
        for (var bi = 0; bi < btns.length; bi++) btns[bi].disabled = true;
      }
    }
    function esimLoadStatus(polling) {
      fetch('/esim?action=status&_=' + Date.now(), {cache:'no-store'}).then(jsonOrThrow).then(function(d) {
        if (!panelActive('sim')) return;
        d = d || {};
        esimRender(d);
        // 仅在主动跟踪任务(polling)时弹 toast，被动打开面板不因历史已完成任务误弹旧消息
        if (polling) {
          if (d.jobQueued || d.jobRunning) {
            showToast(d.jobMessage || 'eSIM 操作后台执行中…', 'loading');
          } else if (d.jobDone) {
            showToast(d.message || (d.jobSuccess ? 'eSIM 操作已完成' : 'eSIM 操作失败'), d.jobSuccess ? 'ok' : 'err');
          }
        }
        if (esimTimer) clearTimeout(esimTimer);
        if (d.jobQueued || d.jobRunning) esimTimer = setTimeout(function(){ esimLoadStatus(true); }, 1500);
      }).catch(function(e) {
        if (!panelActive('sim')) return;
        if (polling) showToast('eSIM 状态查询失败: ' + e, 'err');
      });
    }
    function esimStart(action, body) {
      showToast('正在提交 eSIM 任务…', 'loading');
      fetch('/esim?action=' + encodeURIComponent(action), {method:'POST', cache:'no-store', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:body || ''}).then(jsonOrThrow).then(function(d) {
        if (d.success && d.queued) { showToast(d.message || 'eSIM 任务已排队', 'loading'); esimLoadStatus(true); }
        else { showToast(d.message || 'eSIM 任务启动失败', 'err'); }
      }).catch(function(e){ showToast('请求失败: ' + e, 'err'); });
    }
    function esimInfo() { esimStart('info'); }
    function esimRefresh() { esimStart('refresh'); }
    function esimRun(action, id, nick) {
      id = String(id || '').trim(); nick = String(nick || '').trim();
      if (!id) { alert('请先选择 Profile'); return; }
      if (action === 'disable' && !confirm('确定禁用该 eSIM Profile？')) return;
      if (action === 'switch' && !confirm('确定切换/启用该 eSIM Profile？模组可能短暂掉线。')) return;
      if (action === 'delete' && !confirm('确定删除该 eSIM Profile？\n\n' + esimMaskedId(id) + '\n\n删除后需要重新下载安装配置文件。')) return;
      if (action === 'nickname') {
        var nv = prompt('设置该 Profile 昵称（留空清除）', nick);
        if (nv === null) return;  // 用户取消
        esimStart('nickname', 'id=' + encodeURIComponent(id) + '&nickname=' + encodeURIComponent(nv.trim()));
        return;
      }
      var body = 'id=' + encodeURIComponent(id) + '&nickname=' + encodeURIComponent(nick);
      esimStart(action, body);
    }

    // ---- WiFi settings ----
    var wifiPickTarget = 1;
    function wifiSetPickTarget(slot) {
      wifiPickTarget = slot === 2 ? 2 : 1;
    }
    function wifiScan() {
      var sel = document.getElementById('wifiScanSel');
      sel.innerHTML = '<option value="">扫描中（约 3 秒）...</option>';
      fetch('/wifiscan?_=' + Date.now(), {cache:'no-store'}).then(jsonOrThrow).then(function(arr) {
        if (!Array.isArray(arr) || !arr.length) { sel.innerHTML = '<option value="">未发现 WiFi</option>'; return; }
        arr.sort(function(a, b){ return b.rssi - a.rssi; });
        sel.innerHTML = '<option value="">选择网络（共 ' + arr.length + ' 个）</option>';
        arr.forEach(function(w) {
          var o = document.createElement('option');
          o.value = w.ssid;
          o.textContent = (w.ssid || '(隐藏网络)') + '  ·  ' + w.rssi + ' dBm  ·  ' + (w.enc ? '加密' : '开放');
          sel.appendChild(o);
        });
      }).catch(function(){ sel.innerHTML = '<option value="">扫描失败</option>'; });
    }
    function wifiPick() {
      var v = document.getElementById('wifiScanSel').value;
      if (!v) return;
      var ssidIn = document.getElementById(wifiPickTarget === 2 ? 'wifiSsid2In' : 'wifiSsidIn');
      var passIn = document.getElementById(wifiPickTarget === 2 ? 'wifiPass2In' : 'wifiPassIn');
      if (ssidIn) ssidIn.value = v;
      if (passIn) passIn.focus();
    }
    function wifiSave() {
      var s = document.getElementById('wifiSsidIn').value.trim();
      var s2 = document.getElementById('wifiSsid2In').value.trim();
      if (!s && !s2) { alert('请至少输入一组 WiFi 名称'); return; }
      var names = s && s2 ? (s + ' / ' + s2) : (s || s2);
      if (!confirm('保存并重启接入 “' + names + '”？设备将重启约 15-20 秒。')) return;
      var r = document.getElementById('wifiCfgResult');
      r.className = 'result-box result-loading'; r.textContent = '保存中，设备即将重启...';
      var body = 'ssid=' + encodeURIComponent(s) +
                 '&pass=' + encodeURIComponent(document.getElementById('wifiPassIn').value) +
                 '&ssid2=' + encodeURIComponent(s2) +
                 '&pass2=' + encodeURIComponent(document.getElementById('wifiPass2In').value);
      fetch('/wificonfig', {method:'POST', cache:'no-store', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:body}).then(jsonOrThrow).then(function(d) {
        r.className = 'result-box ' + (d.success ? 'result-success' : 'result-error'); r.textContent = d.message;
      }).catch(function(){ r.className = 'result-box result-info'; r.textContent = '设备正在重启，请连接目标 WiFi 后重新访问设备'; });
    }
    function wifiPrefill() {
      ensureConfig(false).then(function(cfg) {
        var ssidIn = document.getElementById('wifiSsidIn');
        var ssid2In = document.getElementById('wifiSsid2In');
        if (ssidIn && cfg.wifiSsid && !ssidIn.value) ssidIn.value = cfg.wifiSsid;
        if (ssid2In && cfg.wifiSsid2 && !ssid2In.value) ssid2In.value = cfg.wifiSsid2;
      }).catch(function(){});
      fetch('/status?_=' + Date.now(), {cache:'no-store'}).then(jsonOrThrow).then(function(d) {
        var ssidIn = document.getElementById('wifiSsidIn');
        if (ssidIn && d.ssid && !d.apMode && !ssidIn.value) ssidIn.value = d.ssid;
      }).catch(function(){});
    }

    // ---- Config backup / OTA ----
    function doExport() { location.href = '/export'; }
    function doImport() {
      var t = document.getElementById('importBox').value;
      if (!t.trim()) { return; }
      if (!confirm('确定导入配置？将覆盖当前设置。')) return;
      var r = document.getElementById('importResult');
      r.className = 'result-box result-loading'; r.textContent = '导入中...';
      fetch('/import', {method:'POST', cache:'no-store', headers:{'Content-Type':'text/plain'}, body:t}).then(jsonOrThrow).then(function(d){
        r.className = 'result-box ' + (d.success ? 'result-success' : 'result-error'); r.textContent = d.message;
      }).catch(function(e){ r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; });
    }
    function doOta() {
      var f = document.getElementById('otaFile').files[0];
      if (!f) { alert('请先选择 .bin 固件文件'); return; }
      if (!confirm('确定升级固件？升级过程中请勿断电。')) return;
      var r = document.getElementById('otaResult');
      r.className = 'result-box result-loading'; r.textContent = '上传中，请勿断电...';
      var fd = new FormData(); fd.append('update', f, f.name);
      fetch('/update', {method:'POST', cache:'no-store', body:fd}).then(jsonOrThrow).then(function(d){
        r.className = 'result-box ' + (d.success ? 'result-success' : 'result-error'); r.textContent = d.message;
      }).catch(function(){ r.className = 'result-box result-info'; r.textContent = '设备可能正在重启，请稍后刷新'; });
    }

    // ---- 转发规则 可视化搭建器 ----
    var fwdRules = [];  // {type,pattern,chans{},email,drop,enabled}
    function parseFwdRules() {
      var raw = (document.getElementById('forwardRulesRaw') || {}).value || '';
      fwdRules = [];
      raw.split('\n').forEach(function(line) {
        if (!line.trim()) return;
        var p = line.split('\t'); if (p.length < 3) return;
        var r = { type: p[0], pattern: p[1], chans: {}, email: false, drop: false, enabled: (p.length > 3 ? p[3] : '1') !== '0' };
        (p[2] || '').split(',').forEach(function(t) { t = t.trim();
          if (t === 'drop') r.drop = true; else if (t === 'email') r.email = true; else if (/^[1-5]$/.test(t)) r.chans[t] = true; });
        fwdRules.push(r);
      });
    }
    function serializeRules() {
      var lines = fwdRules.map(function(r) {
        var a = []; if (r.drop) a.push('drop'); else { if (r.email) a.push('email'); for (var c = 1; c <= 5; c++) if (r.chans[c]) a.push(String(c)); }
        return r.type + '\t' + r.pattern + '\t' + a.join(',') + '\t' + (r.enabled ? '1' : '0');
      });
      var el = document.getElementById('forwardRulesRaw'); if (el) el.value = lines.join('\n');
    }
    function addRule() { fwdRules.push({ type: 'kw', pattern: '', chans: {}, email: true, drop: false, enabled: true }); renderRules(); }
    function delRule(i) { fwdRules.splice(i, 1); renderRules(); }
    function moveRule(i, dlt) { var j = i + dlt; if (j < 0 || j >= fwdRules.length) return; var t = fwdRules[i]; fwdRules[i] = fwdRules[j]; fwdRules[j] = t; renderRules(); }
    function renderRules() {
      serializeRules();
      var box = document.getElementById('rulesList'); if (!box) return;
      if (!fwdRules.length) { box.innerHTML = '<div class="msg-empty">暂无规则。点“添加规则”新建；不加规则时短信转发到全部启用通道。</div>'; return; }
      box.innerHTML = '';
      fwdRules.forEach(function(r, i) {
        var d = document.createElement('div'); d.className = 'push-channel enabled'; d.style.marginBottom = '10px';
        var typeOpts = [['kw','关键词(子串)'],['re','正文正则'],['from','发件人正则']].map(function(o){return '<option value="'+o[0]+'"'+(r.type===o[0]?' selected':'')+'>'+o[1]+'</option>';}).join('');
        var chans = ''; for (var c = 1; c <= 5; c++) chans += '<label style="margin-right:12px;font-size:12px;white-space:nowrap;"><input type="checkbox" data-ch="'+c+'"'+(r.chans[c]?' checked':'')+(r.drop?' disabled':'')+'> '+htmlEsc(chanName(c))+'</label>';
        d.innerHTML =
          '<div class="push-channel-header"><b style="font-family:var(--mono);color:var(--amber);">'+(i+1<10?'0':'')+(i+1)+'</b>'+
          '<label style="font-size:12px;"><input type="checkbox" class="rEn"'+(r.enabled?' checked':'')+'> 启用</label>'+
          '<span style="margin-left:auto;display:flex;gap:6px;">'+
          '<button type="button" class="btn btn-secondary btn-sm" data-a="up">↑</button>'+
          '<button type="button" class="btn btn-secondary btn-sm" data-a="dn">↓</button>'+
          '<button type="button" class="btn btn-danger btn-sm" data-a="del">删除</button></span></div>'+
          '<div class="form-row"><div class="form-group" style="flex:0 0 132px;"><label class="form-label">匹配方式</label><select class="form-select rType">'+typeOpts+'</select></div>'+
          '<div class="form-group"><label class="form-label">模式（关键词 / 正则）</label><input class="form-input rPat" placeholder="如  验证码   或   ^(10086|10010)"></div></div>'+
          '<div class="form-group"><label class="form-label">命中后</label>'+
          '<label style="margin-right:14px;font-size:12px;color:var(--error);"><input type="checkbox" class="rDrop"'+(r.drop?' checked':'')+'> 丢弃(不转发)</label>'+
          '<label style="margin-right:12px;font-size:12px;"><input type="checkbox" class="rEmail"'+(r.email?' checked':'')+(r.drop?' disabled':'')+'> 邮件</label>'+chans+'</div>';
        d.querySelector('.rType').onchange = function(){ r.type = this.value; serializeRules(); };
        var pat = d.querySelector('.rPat'); pat.value = r.pattern; pat.oninput = function(){ r.pattern = this.value; serializeRules(); };
        d.querySelector('.rEn').onchange = function(){ r.enabled = this.checked; serializeRules(); };
        d.querySelector('.rDrop').onchange = function(){ r.drop = this.checked; renderRules(); };
        d.querySelector('.rEmail').onchange = function(){ r.email = this.checked; serializeRules(); };
        [].forEach.call(d.querySelectorAll('input[data-ch]'), function(cb){ cb.onchange = function(){ r.chans[this.getAttribute('data-ch')] = this.checked; serializeRules(); }; });
        d.querySelector('[data-a=up]').onclick = function(){ moveRule(i, -1); };
        d.querySelector('[data-a=dn]').onclick = function(){ moveRule(i, 1); };
        d.querySelector('[data-a=del]').onclick = function(){ delRule(i); };
        box.appendChild(d);
      });
    }
    // ---- 规则本地测试：镜像固件 eval_forward_rules + 派发门控（自上而下首条命中即止）----
    // 浏览器用 JS 正则预览，与设备端 POSIX ERE 在个别语法上可能有差异
    // 按固件派发逻辑折算实际会发出的目标：推送总开关、通道启用、邮件启用+配置齐全
    function deliverTargets(useEmail, chanMask) {
      var c = webConfig || {}, arr = c.pushChannels || [];
      var out = [], skipped = [];
      for (var n = 1; n <= 5; n++) {
        if (!chanMask[n]) continue;
        if (!c.pushEnabled) skipped.push(chanName(n) + '(推送总开关已关)');
        else if (!(arr[n - 1] || {}).enabled) skipped.push(chanName(n) + '(通道未启用)');
        else out.push(chanName(n));
      }
      if (useEmail) {
        if (!c.emailEnabled) skipped.push('邮件(邮件转发已关)');
        else if (!c.emailConfigured) skipped.push('邮件(SMTP 未配置齐全)');
        else out.push('邮件');
      }
      return { out: out, skipped: skipped };
    }
    function testRules() {
      serializeRules();
      var from = (document.getElementById('rtFrom').value || '').trim();
      var text = document.getElementById('rtText').value || '';
      var r = document.getElementById('rtResult');
      if (!text && !from) { r.className = 'result-box result-error'; r.textContent = '请先填写测试发件人或正文'; return; }
      for (var i = 0; i < fwdRules.length; i++) {
        var rule = fwdRules[i];
        if (!rule.enabled || !rule.pattern) continue;
        var hit = false;
        if (rule.type === 'kw') {
          hit = text.indexOf(rule.pattern) >= 0;
        } else {
          try { hit = new RegExp(rule.pattern, 'i').test(rule.type === 'from' ? from : text); }
          catch (e) { r.className = 'result-box result-error'; r.textContent = '规则 ' + (i + 1) + ' 的正则浏览器无法解析，请检查语法：' + e.message; return; }
        }
        if (!hit) continue;
        if (rule.drop) {
          r.className = 'result-box result-error';
          r.textContent = '命中规则 ' + (i + 1) + ' → 丢弃(不转发)';
          return;
        }
        var d = deliverTargets(rule.email, rule.chans);
        var msg = '命中规则 ' + (i + 1) + ' → ' + (d.out.length ? '实际转发到：' + d.out.join('、') : '没有可用转发目标(该短信不会被转发)');
        if (d.skipped.length) msg += '；跳过：' + d.skipped.join('、');
        r.className = 'result-box ' + (d.out.length ? 'result-success' : 'result-error');
        r.textContent = msg;
        return;
      }
      // 未命中：固件默认转发到全部启用通道 + 邮件(各自仍受开关/配置门控)
      var all = {}; for (var n = 1; n <= 5; n++) all[n] = true;
      var dd = deliverTargets(true, all);
      r.className = 'result-box ' + (dd.out.length ? 'result-info' : 'result-error');
      r.textContent = '未命中任何规则 → ' + (dd.out.length ? '按默认策略实际转发到：' + dd.out.join('、') : '没有可用转发目标(该短信不会被转发)');
    }

    // ---- 短信详情抽屉(点击展开) ----
    function openMsgDrawer(id) {
      var arr = window.__msgCache || [], box = window.__msgBox || 'recv', m = null;
      for (var i = 0; i < arr.length; i++) if (arr[i].id === id) { m = arr[i]; break; }
      if (!m) return;
      var drawerBody = document.getElementById('drawerBody');
      var drawer = document.getElementById('msgDrawer');
      var bg = document.getElementById('drawerBg');
      if (!drawerBody || !drawer || !bg) return;
      var sent = (box === 'sent');
      var who = sent ? (m.target || '(未知)') : (m.sender || '(未知)');
      var otp = sent ? '' : otpExtract(m.text || '');
      var h = '<div class="dk">' + (sent ? '目标' : '发件人') + '</div>' +
              '<div class="dv">' + htmlEsc(who) + ' <span class="msg-chip" id="drawerCopyWho" style="cursor:pointer;">复制号码</span></div>';
      h += '<div class="dk">时间</div><div class="dv">' + fmtEpochFull(sent ? m.sent : m.recv) + '</div>';
      h += '<div class="dk">状态</div><div class="dv">' + (sent ? (m.ok ? '发送成功' : '发送失败') : (m.fwd ? '已处理' : '待处理')) + '</div>';
      h += '<div class="dk">正文长度</div><div class="dv">' + String((m.text || '').length) + ' 字符</div>';
      if (otp) h += '<div class="dk">验证码</div><div class="dv"><span class="otpcode" onclick="copyText(\'' + otp + '\', this)">' + otp + '</span></div>';
      var bh = htmlEsc(m.text || ''); if (otp) bh = bh.replace(otp, '<mark>' + otp + '</mark>');
      h += '<div class="dfull">' + bh + '</div><div class="btn-row">';
      if (!sent) h += '<button class="btn btn-primary btn-sm" onclick="resendMsg(' + id + ')">重发转发</button>';
      if (!sent) h += '<button class="btn btn-danger btn-sm" onclick="deleteMsg(' + id + ')">删除</button>';
      if (sent) h += '<button class="btn btn-primary btn-sm" id="drawerSendAgain">再发一次</button>';
      h += '<button class="btn btn-secondary btn-sm" id="drawerCopyFull">复制全文</button></div><div class="result-box" id="drawerRes"></div>';
      drawerBody.innerHTML = h;
      var cf = document.getElementById('drawerCopyFull'); if (cf) cf.onclick = function(){ copyText(m.text || '', cf); };
      var cw = document.getElementById('drawerCopyWho'); if (cw) cw.onclick = function(){ copyText(who, cw); };
      // 已发送短信一键回填到发送弹窗，改两个字就能再发
      var sa = document.getElementById('drawerSendAgain');
      if (sa) sa.onclick = function() {
        closeMsgDrawer();
        openSmsModal();
        var p = document.getElementById('smsPhone');
        var c = document.getElementById('smsContent');
        if (p) p.value = m.target || '';
        if (c) { c.value = m.text || ''; updateCount(c); }
      };
      drawer.classList.add('show');
      bg.classList.add('show');
    }
    function closeMsgDrawer() {
      var drawer = document.getElementById('msgDrawer');
      var bg = document.getElementById('drawerBg');
      if (drawer) drawer.classList.remove('show');
      if (bg) bg.classList.remove('show');
    }
    // Esc 关闭抽屉/发短信弹窗
    document.addEventListener('keydown', function(e) {
      if (e.key === 'Escape') { closeMsgDrawer(); closeSmsModal(); }
    });
    function resendMsg(id) {
      var r = document.getElementById('drawerRes'); if (!r) return;
      r.className = 'result-box result-loading'; r.textContent = '重发中...';
      fetch('/resend?id=' + id, { method: 'POST', cache:'no-store' }).then(jsonOrThrow).then(function(d) {
        r.className = 'result-box ' + (d.success ? 'result-success' : 'result-error'); r.textContent = d.message;
      }).catch(function(e){ r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; });
    }
    function deleteMsg(id) {
      if (!confirm('确定删除这条短信？')) return;
      fetch('/delete?id=' + id, { method: 'POST', cache:'no-store' }).then(jsonOrThrow).then(function(d) {
        if (d.success) { closeMsgDrawer(); loadMessages(); }
        else { var r = document.getElementById('drawerRes'); if (r) { r.className = 'result-box result-error'; r.textContent = d.message; } }
      }).catch(function(e){ var r = document.getElementById('drawerRes'); if (r) { r.className = 'result-box result-error'; r.textContent = '请求失败: ' + e; } });
    }
    // ---- 概览：最新接收 / 验证码 hero ----
    var latestOtpLoading = false, latestOtpTimer = null;
    function loadLatestOtp() {
      if (!panelActive('overview')) return;
      if (latestOtpLoading) return;
      latestOtpLoading = true;
      // 同 refreshLog：挂死请求不能永久卡住轮询闸门
      var ctrl = window.AbortController ? new AbortController() : null;
      var opt = {cache:'no-store'};
      if (ctrl) opt.signal = ctrl.signal;
      var timeoutId = setTimeout(function() {
        if (ctrl) ctrl.abort();
        else latestOtpLoading = false;
      }, 8000);
      fetch('/messages?limit=1&_=' + Date.now(), opt).then(jsonOrThrow).then(function(arr) {
        if (!panelActive('overview')) return;
        var card = document.getElementById('otpHeroCard');
        if (!card) return;
        if (!Array.isArray(arr) || !arr.length) { card.style.display = 'none'; return; }
        var m = arr[0], otp = otpExtract(m.text || '');
        var from = document.getElementById('ohFrom');
        var text = document.getElementById('ohText');
        var when = document.getElementById('ohWhen');
        var code = document.getElementById('ohCode');
        if (!from || !text || !when || !code) return;
        from.textContent = m.sender || '(未知)';
        text.textContent = m.text || '';
        when.textContent = fmtEpoch(m.recv) + (m.fwd ? '　已处理' : '　待处理');
        if (otp) { code.textContent = otp; code.onclick = function(){ copyText(otp, code); }; code.style.display = ''; } else { code.style.display = 'none'; }
        card.style.display = '';
        if (!m.fwd && panelActive('overview')) latestOtpTimer = setTimeout(loadLatestOtp, 1500);  // 刚入库时转发拆分可能下一帧才标记完成
      }).catch(function(){}).then(function(){ clearTimeout(timeoutId); latestOtpLoading = false; });
    }

    document.addEventListener('DOMContentLoaded', function() {
      switchPanel(panelFromHash());
    });
    // ---- 右上角 toast：统一承载"会撑大卡片"的临时提示（下发/入队/执行结果等）----
    // kind: 'ok'(绿) | 'err'(红) | 'info'(中性) | 'loading'(琥珀，常驻直到被下一条替换)
    function showToast(msg, kind) {
      kind = kind || 'ok';
      var t = document.getElementById('saveToast');
      if (!t) { t = document.createElement('div'); t.id = 'saveToast'; document.body.appendChild(t); }
      // loading 态连续刷新时只换文案、不重放入场动画，避免每次轮询都闪一下
      var keepAnim = (kind === 'loading' && t.classList.contains('show') && t.classList.contains('loading'));
      t.textContent = msg || '';
      t.className = 'save-toast ' + kind + ' show';
      if (!keepAnim) { t.classList.remove('show'); void t.offsetWidth; t.classList.add('show'); }
      clearTimeout(window.__toastT);
      // loading 不自动消失（等成功/失败结果来替换）；其余 2.6s 后淡出
      if (kind !== 'loading') window.__toastT = setTimeout(function() { t.classList.remove('show'); }, 2600);
    }
    // ---- 设置表单 AJAX 原地保存：拦截所有 /save 表单，不再整页跳转 ----
    function showSaved(ok, msg) {
      showToast(msg || (ok ? '✓ 已保存' : '✗ 保存失败'), ok ? 'ok' : 'err');
    }
    function fetchSaveWithTimeout(body) {
      var ctrl = window.AbortController ? new AbortController() : null;
      var opt = { method: 'POST', cache: 'no-store', body: body };
      if (ctrl) opt.signal = ctrl.signal;
      var timer = setTimeout(function() { if (ctrl) ctrl.abort(); }, 12000);
      return fetch('/save', opt).then(function(r) {
        clearTimeout(timer);
        return r;
      }, function(e) {
        clearTimeout(timer);
        throw e;
      });
    }
    document.addEventListener('submit', function(e) {
      var f = e.target;
      if (!f || f.getAttribute('action') !== '/save') return;
      e.preventDefault();             // 阻止整页跳转；规则表单在此处再同步一次，避免事件顺序差异
      if (f.dataset.saving === '1') return;
      if ((f.id === 'kaForm' && !document.getElementById('kaFormFlag')) ||
          (f.id === 'stForm' && !document.getElementById('stFormFlag'))) {
        showSaved(false, '✗ 状态未加载完成，请刷新后再保存');
        return;
      }
      f.dataset.saving = '1';
      if (f.querySelector('#forwardRulesRaw')) serializeRules();
      var fd = new FormData(f);
      var body = new URLSearchParams();                  // 关键：用 urlencoded 而非 FormData(multipart)——
      fd.forEach(function(v, k){ body.append(k, v); });  // ESP32 WebServer 对 multipart 大表单会卡/超时致"网络错误"
      body.append('ajax', '1');
      var passChanged = (fd.get('webPass') || '') !== '';
      var btn = f.querySelector('button[type="submit"]'), old = btn ? btn.textContent : '';
      if (btn) { btn.disabled = true; btn.textContent = '保存中…'; }
      var attempt = function(n) {
        return fetchSaveWithTimeout(body).then(function(r) {
          if (!r.ok) {
            showSaved(false, '✗ 保存失败 (' + r.status + ')');
            return;
          }
          return ensureConfig(true).catch(function(){ return null; }).then(function() {
            showSaved(true, passChanged ? '✓ 已保存，账号密码已更新' : '✓ 已保存');
          });
        }).catch(function() {
          if (n > 0) return new Promise(function(res){ setTimeout(res, 700); }).then(function(){ return attempt(n - 1); });  // 连接抖动自动重试
          showSaved(false, '✗ 网络错误（已重试，请稍后再点保存）');
        });
      };
      var done = function() { f.dataset.saving = '0'; if (btn) { btn.disabled = false; btn.textContent = old; } };
      attempt(1).then(done, done);
    });
