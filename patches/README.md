# ESP-IDF 本地补丁

项目固定使用 ESP-IDF v6.0.2。在运行 `idf.py` 之前执行：

```bash
python tools/apply_idf_patches.py --idf-path "$IDF_PATH"
```

`tools/idf.ps1` 和固件 CI 已自动执行这一步。`--check-only` 只检查，不修改 SDK。
脚本先验证全部补丁，再分别在 ESP-IDF 根目录和 Mbed TLS 子模块应用；重复运行不会重复修改文件。
上下文不匹配时直接失败，不静默跳过。升级 SDK 后应重新核对补丁是否已被上游包含。

- `mbedtls/0001-*`：解析所有合法的 certificate policy OID，兼容 GSMA RSP 证书。
- `esp-idf/0001-*`：回移 [ESP-IDF 737e973](https://github.com/espressif/esp-idf/commit/737e97340b7ce15598ecca2a611fa6870904be05) 和 [78bf36d](https://github.com/espressif/esp-idf/commit/78bf36db3ea64cddae8e73b1ad7c8ff41e1ab0c6) 的交叉签名 CA 回调内存修复。`subject_raw` 引用持久证书包，subject OID/value 引用在验证期间仍存活的子证书 issuer；只分配 Mbed TLS 会释放的证书结构和链表节点，且使用配对的 `mbedtls_calloc` / `mbedtls_free`。保留交叉签名、根证书信任和签名验证，不通过关闭校验规避问题。

## 验证

```bash
python -m unittest discover -s tools/tests -v
python tools/build_web_assets.py --check
```

有 `IDF_PATH` 时，请先应用补丁再运行测试；测试会额外检查真实 SDK 补丁状态，
并提取实际 CA 回调编译主机测试（6,000 次创建/释放及分配、密钥解析失败路径）。
固件 CI 使用固定 v6.0.2 运行上述测试并编译。单独运行回调测试：

```bash
python tools/tests/test_crt_bundle_callback.py --idf-path "$IDF_PATH" -v
```
主机测试不能代替 ESP32-C3 长时间运行测试。对于 issue #33 的衍生 HTTPS 轮询程序，仍需在相同轮询频率、证书链和网络条件下检查空闲堆、最低堆与重启情况；不要将主机测试或构建成功描述为已完成设备复现。
