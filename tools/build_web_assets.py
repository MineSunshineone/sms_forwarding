#!/usr/bin/env python3
"""一次性 CI staging 引导器：应用 #18/#30 已验证改动，随后恢复原脚本。"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BRANCH = "codex/stage-issues-18-30"


def replace_once(rel: str, old: str, new: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, got {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


def replace_span(rel: str, start: str, end: str, replacement: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    a = text.find(start)
    if a < 0:
        raise RuntimeError(f"{rel}: start marker not found")
    b = text.find(end, a)
    if b < 0:
        raise RuntimeError(f"{rel}: end marker not found")
    path.write_text(text[:a] + replacement + text[b:], encoding="utf-8", newline="\n")


def run(*args: str) -> None:
    subprocess.run(args, cwd=ROOT, check=True)


def apply_changes() -> None:
    replace_once(
        "components/idf_push/include/idf_push.h",
        '#include "esp_err.h"\n\nesp_err_t idf_push_start(void);',
        '''#include "esp_err.h"\n\nstruct IdfForwardRuleDecision {\n    bool matched = false;\n    bool drop = false;\n    uint32_t chMask = 0;\n    bool email = false;\n    int ruleIndex = 0;\n};\n\nesp_err_t idf_push_start(void);''',
    )
    replace_once(
        "components/idf_push/include/idf_push.h",
        '''bool idf_push_busy(void);\n\nbool idf_push_enqueue_test(uint8_t channel, std::string& message);''',
        '''bool idf_push_busy(void);\n\n// 与实际短信转发共用同一规则引擎，供网页规则测试使用，避免浏览器正则语义与固件不一致。\nIdfForwardRuleDecision idf_push_eval_forward_rules(const std::string& rules,\n                                                   const std::string& sender,\n                                                   const std::string& body);\n\nbool idf_push_enqueue_test(uint8_t channel, std::string& message);''',
    )

    replace_once(
        "components/idf_push/idf_push.cpp",
        '''struct ForwardDecision {\n    bool matched = false;\n    bool drop = false;\n    uint32_t chMask = 0;\n    bool email = false;\n};\n\n''',
        "",
    )
    replace_once(
        "components/idf_push/idf_push.cpp",
        '''static ForwardDecision eval_forward_rules(const std::string& rules, const std::string& sender, const std::string& body)\n{\n    ForwardDecision d;\n    size_t pos = 0;''',
        '''IdfForwardRuleDecision idf_push_eval_forward_rules(const std::string& rules,\n                                                   const std::string& sender,\n                                                   const std::string& body)\n{\n    IdfForwardRuleDecision d;\n    size_t pos = 0;\n    int rule_index = 0;''',
    )
    replace_once(
        "components/idf_push/idf_push.cpp",
        '''        if (t1 == std::string::npos || t2 == std::string::npos) continue;\n        size_t t3 = line.find('\\t', t2 + 1);''',
        '''        if (t1 == std::string::npos || t2 == std::string::npos) continue;\n        ++rule_index;\n        size_t t3 = line.find('\\t', t2 + 1);''',
    )
    replace_once(
        "components/idf_push/idf_push.cpp",
        '''        d.matched = true;\n        size_t ap = 0;''',
        '''        d.matched = true;\n        d.ruleIndex = rule_index;\n        size_t ap = 0;''',
    )
    replace_once(
        "components/idf_push/idf_push.cpp",
        '''    ForwardDecision fd = eval_forward_rules(cfg.forwardRules, job.sender, job.text);\n    if (fd.matched && fd.drop) {\n        idf_logf("转发规则命中：丢弃短信 id=%u", static_cast<unsigned>(job.inboxId));''',
        '''    IdfForwardRuleDecision fd = idf_push_eval_forward_rules(cfg.forwardRules, job.sender, job.text);\n    if (fd.matched && fd.drop) {\n        idf_logf("转发规则 %d 命中：丢弃短信 id=%u", fd.ruleIndex,\n                 static_cast<unsigned>(job.inboxId));''',
    )

    replace_once(
        "components/idf_modem/idf_modem.cpp",
        '''    if (state == "ready") {\n        set_sim_status("ready", false, "SIM 已就绪");\n        return true;\n    }''',
        '''    if (state == "ready") {\n        // ICCID 是 PIN 凭据的主键，不应依赖注册完成后的概览采样；部分 CMCC 卡在\n        // 启动早期采样窗口拿不到 ICCID，但 CPIN READY 后厂商命令已经可稳定读取。\n        IdfModemStatus status = idf_modem_get_status();\n        std::string iccid = is_iccid_text(status.iccid) ? status.iccid : query_current_iccid();\n        set_sim_status("ready", false, "SIM 已就绪", iccid);\n        if (!iccid.empty()) save_identity_cache(std::string(), iccid);\n        return true;\n    }''',
    )

    handler = '''static esp_err_t handle_test_rule(httpd_req_t* req)\n{\n    if (!check_auth(req)) return ESP_OK;\n    if (!check_csrf(req)) return ESP_OK;\n\n    std::string raw;\n    if (read_body(req, raw, 16384) != ESP_OK) return ESP_OK;\n    IdfFormFields fields = parse_urlencoded(raw);\n    std::string rules = field_text(fields, "rules");\n    std::string sender = field_text(fields, "sender");\n    std::string text = field_text(fields, "text");\n    set_json_no_cache(req);\n\n    if (rules.size() > 2048 || sender.size() > 128 || text.size() > 4096) {\n        httpd_resp_set_status(req, "400 Bad Request");\n        return httpd_resp_sendstr(req, "{\\\"success\\\":false,\\\"message\\\":\\\"规则或测试内容过长\\\"}");\n    }\n\n    std::string validation_error;\n    if (idf_config_validate_forward_rules(rules, &validation_error) != ESP_OK) {\n        httpd_resp_set_status(req, "400 Bad Request");\n        std::string body = "{\\\"success\\\":false,";\n        json_prop(body, "message", validation_error.empty() ? "转发规则格式无效" : validation_error);\n        body += "}";\n        return httpd_resp_send(req, body.data(), body.size());\n    }\n\n    IdfForwardRuleDecision decision = idf_push_eval_forward_rules(rules, sender, text);\n    char body[192];\n    snprintf(body, sizeof(body),\n             "{\\\"success\\\":true,\\\"matched\\\":%s,\\\"drop\\\":%s,\\\"email\\\":%s,\\\"chMask\\\":%u,\\\"ruleIndex\\\":%d}",\n             decision.matched ? "true" : "false",\n             decision.drop ? "true" : "false",\n             decision.email ? "true" : "false",\n             static_cast<unsigned>(decision.chMask), decision.ruleIndex);\n    return httpd_resp_sendstr(req, body);\n}\n\n'''
    replace_once(
        "components/idf_web/idf_web.cpp",
        '''    return httpd_resp_send(req, body.c_str(), body.size());\n}\n\nstatic bool keepalive_url_valid''',
        '''    return httpd_resp_send(req, body.c_str(), body.size());\n}\n\n''' + handler + '''static bool keepalive_url_valid''',
    )
    replace_once(
        "components/idf_web/idf_web.cpp",
        '''    IDF_WEB_TRY_REGISTER("/testpush", register_handler(s_server, "/testpush", HTTP_ANY, handle_test_push));\n    IDF_WEB_TRY_REGISTER("/ussd", register_handler(s_server, "/ussd", HTTP_ANY, handle_ussd));''',
        '''    IDF_WEB_TRY_REGISTER("/testpush", register_handler(s_server, "/testpush", HTTP_ANY, handle_test_push));\n    IDF_WEB_TRY_REGISTER("/testrule", register_handler(s_server, "/testrule", HTTP_POST, handle_test_rule));\n    IDF_WEB_TRY_REGISTER("/ussd", register_handler(s_server, "/ussd", HTTP_ANY, handle_ussd));''',
    )

    replace_once(
        "code/web_src/app.js",
        '''    // ---- 规则本地测试：镜像固件 eval_forward_rules + 派发门控（自上而下首条命中即止）----\n    // 浏览器用 JS 正则预览，与设备端 POSIX ERE 在个别语法上可能有差异\n    // 按固件派发逻辑折算实际会发出的目标：推送总开关、通道启用、邮件启用+配置齐全''',
        '''    // ---- 规则测试：匹配交给固件真实规则引擎，前端只折算当前可投递目标 ----''',
    )
    new_test_rules = '''    function testRules() {\n      serializeRules();\n      var from = (document.getElementById('rtFrom').value || '').trim();\n      var text = document.getElementById('rtText').value || '';\n      var r = document.getElementById('rtResult');\n      if (!text && !from) { r.className = 'result-box result-error'; r.textContent = '请先填写测试发件人或正文'; return; }\n      r.className = 'result-box result-loading'; r.textContent = '按设备实际规则测试中...';\n      var raw = (document.getElementById('forwardRulesRaw') || {}).value || '';\n      var body = new URLSearchParams();\n      body.append('rules', raw); body.append('sender', from); body.append('text', text);\n      csrfFetch('/testrule', {method:'POST', cache:'no-store', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:body})\n        .then(jsonOrThrow).then(function(result) {\n        var index = result.ruleIndex || 0;\n        if (result.matched && result.drop) {\n          r.className = 'result-box result-error';\n          r.textContent = '命中规则 ' + index + ' → 丢弃(不转发)';\n          return;\n        }\n        var selected = {};\n        if (result.matched) {\n          for (var c = 1; c <= 5; c++) selected[c] = !!(result.chMask & (1 << (c - 1)));\n        } else {\n          for (var n = 1; n <= 5; n++) selected[n] = true;\n        }\n        var d = deliverTargets(result.matched ? !!result.email : true, selected);\n        var prefix = result.matched ? ('命中规则 ' + index + ' → ') : '未命中任何规则 → ';\n        var msg = prefix + (d.out.length ? (result.matched ? '实际转发到：' : '按默认策略实际转发到：') + d.out.join('、') : '没有可用转发目标(该短信不会被转发)');\n        if (d.skipped.length) msg += '；跳过：' + d.skipped.join('、');\n        r.className = 'result-box ' + (d.out.length ? (result.matched ? 'result-success' : 'result-info') : 'result-error');\n        r.textContent = msg;\n      }).catch(function(e) {\n        r.className = 'result-box result-error';\n        r.textContent = '规则测试失败：' + (e && e.message ? e.message : '设备未返回有效结果');\n      });\n    }\n\n'''
    replace_span(
        "code/web_src/app.js",
        "    function testRules() {\n",
        "    // ---- 短信详情抽屉(点击展开) ----\n",
        new_test_rules,
    )

    replace_once("CMakeLists.txt", 'set(PROJECT_VER "1.1.5")', 'set(PROJECT_VER "1.1.6")')
    replace_once(
        "components/idf_config/include/idf_config.h",
        'static constexpr const char* IDF_FW_VERSION = "1.1.5";',
        'static constexpr const char* IDF_FW_VERSION = "1.1.6";',
    )


def main() -> int:
    if os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("GITHUB_REF_NAME") != BRANCH:
        raise RuntimeError("此 staging 引导器只允许在指定 GitHub Actions 分支运行")

    apply_changes()

    # 恢复仓库原始生成器；最终提交不保留本引导器。
    run("git", "checkout", "HEAD^", "--", "tools/build_web_assets.py")
    run(sys.executable, "tools/build_web_assets.py")
    run(sys.executable, "tools/build_web_assets.py", "--check")
    run("git", "diff", "--check")
    run("node", "--check", "code/web_src/app.js")

    run("git", "config", "user.name", "github-actions[bot]")
    run("git", "config", "user.email", "41898282+github-actions[bot]@users.noreply.github.com")
    run("git", "add", "-A")
    run("git", "commit", "-m", "fix: resolve remaining SMS and SIM issues (#18 #30)")
    run("git", "push", "origin", f"HEAD:{BRANCH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
