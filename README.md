# SMS Forwarding ESP-IDF 固件

<p align="center">
  <a href="https://github.com/MineSunshineone/sms_forwarding/actions/workflows/build.yml"><img alt="CI" src="https://github.com/MineSunshineone/sms_forwarding/actions/workflows/build.yml/badge.svg" /></a>
  <a href="LICENSE"><img alt="License: MIT" src="https://img.shields.io/badge/License-MIT-green.svg" /></a>
  <a href="https://linux.do"><img alt="LINUX DO" src="https://img.shields.io/badge/LINUX-DO-FFB003.svg?logo=data:image/svg%2bxml;base64,DQo8c3ZnIHhtbG5zPSJodHRwOi8vd3d3LnczLm9yZy8yMDAwL3N2ZyIgd2lkdGg9IjEwMCIgaGVpZ2h0PSIxMDAiPjxwYXRoIGQ9Ik00Ni44Mi0uMDU1aDYuMjVxMjMuOTY5IDIuMDYyIDM4IDIxLjQyNmM1LjI1OCA3LjY3NiA4LjIxNSAxNi4xNTYgOC44NzUgMjUuNDV2Ni4yNXEtMi4wNjQgMjMuOTY4LTIxLjQzIDM4LTExLjUxMiA3Ljg4NS0yNS40NDUgOC44NzRoLTYuMjVxLTIzLjk3LTIuMDY0LTM4LjAwNC0yMS40M1EuOTcxIDY3LjA1Ni0uMDU0IDUzLjE4di02LjQ3M0MxLjM2MiAzMC43ODEgOC41MDMgMTguMTQ4IDIxLjM3IDguODE3IDI5LjA0NyAzLjU2MiAzNy41MjcuNjA0IDQ2LjgyMS0uMDU2IiBzdHlsZT0ic3Ryb2tlOm5vbmU7ZmlsbC1ydWxlOmV2ZW5vZGQ7ZmlsbDojZWNlY2VjO2ZpbGwtb3BhY2l0eToxIi8+PHBhdGggZD0iTTQ3LjI2NiAyLjk1N3EyMi41My0uNjUgMzcuNzc3IDE1LjczOGE0OS43IDQ5LjcgMCAwIDEgNi44NjcgMTAuMTU3cS00MS45NjQuMjIyLTgzLjkzIDAgOS43NS0xOC42MTYgMzAuMDI0LTI0LjM4N2E2MSA2MSAwIDAgMSA5LjI2Mi0xLjUwOCIgc3R5bGU9InN0cm9rZTpub25lO2ZpbGwtcnVsZTpldmVub2RkO2ZpbGw6IzE5MTkxOTtmaWxsLW9wYWNpdHk6MSIvPjxwYXRoIGQ9Ik03Ljk4IDcwLjkyNmMyNy45NzctLjAzNSA1NS45NTQgMCA4My45My4xMTNRODMuNDI2IDg3LjQ3MyA2Ni4xMyA5NC4wODZxLTE4LjgxIDYuNTQ0LTM2LjgzMi0xLjg5OC0xNC4yMDMtNy4wOS0yMS4zMTctMjEuMjYyIiBzdHlsZT0ic3Ryb2tlOm5vbmU7ZmlsbC1ydWxlOmV2ZW5vZGQ7ZmlsbDojZjlhZjAwO2ZpbGwtb3BhY2l0eToxIi8+PC9zdmc+" /></a>
</p>

ESP32-C3 + ML307 系列 4G/LTE 模组的轻量短信转发固件。设备通过 UART/AT 接收 PDU 短信，在本地完成中文短信、长短信、去重和规则判断，然后经 WiFi 转发到邮件与最多 5 个推送通道；同时支持来电通知与 SIM 卡热插拔检测，并提供 Web UI 完成配置、诊断、日志、OTA、保号和定时任务。

本仓库现在只保留 **原生 ESP-IDF** 固件；Arduino fallback 已移除。项目目标不是做一个“能跑的 demo”，而是做一套适合长期插电、无人值守、可 OTA、可排障、资源占用可控的短信网关固件。

## 功能一览

这不是单纯的“收到短信转发一下”。固件已经把短信网关常用的配置、诊断、保号、定时和 eSIM 管理都放进 Web UI，普通用户不接串口也能完成大部分维护。

### 首页概览

首页会集中显示设备是否真的启动完成，以及当前短信网关是否健康：

- 模组状态：启动阶段、AT 状态、网络注册状态、蜂窝数据是否启用。
- 信号信息：CSQ/RSSI、RSRP、RSRQ、SINR，以及 WiFi RSSI。
- SIM 信息：运营商、本机号码、IMEI、ICCID、IMSI、APN、蜂窝 IP。
- 设备信息：模组厂家、模组型号、模组固件、主控固件版本、空闲堆、最低堆、最大可分配块、ESP32-C3 片内温度。
- 转发状态：待转发队列、推送队列、待发短信队列、邮件队列。
- 系统状态：运行时长、复位原因、NTP 时间同步、邮件通知状态、已启用推送通道数、管理员号码。
- 收件箱摘要：最近短信、验证码提取结果、本地留存数量。

启动时不会只因为模组注册上网络就显示“已就绪”。首页需要的基础身份、SIM、固件和信号信息采样完成后，才会从“读取信息中”切换到“已就绪”。

### 短信收发

- 接收短信：使用 PDU 模式，支持中文短信、Unicode 内容和长短信合并。
- 双路径补收：同时处理 `+CMT` / `+CMTI` 通知和 `AT+CMGL` 存储轮询，降低漏收概率。
- 本地收件箱：保存最近短信，页面可查看处理状态和验证码摘要。
- 发送短信：Web UI 可手动发送短信，也支持管理员短信命令触发发送。
- 管理员命令：可配置管理员号码，通过短信执行重启、代发短信等远程操作。
- 黑名单：可配置号码黑名单，忽略指定来源短信。
- 来电通知：有来电时把主叫号码经与短信相同的邮件/推送通道转发（可在转发设置里开关，默认开启）；一通电话只通知一次，运营商隐藏号码时显示为未知号码，设备只通知、不接听也不回拨。
- SIM 卡热插拔：运行中插入 SIM 卡自动重新初始化并注册网络，拔出后自动标记未就绪并清空卡身份，无需手动重启设备。

### 邮件与推送

转发支持邮件和最多 **5 个推送通道** 同时启用，每个通道可以独立命名、启用、保存和测试。

邮件通道：

- SMTP 邮件通知。
- 支持服务器、端口、账号、授权码、收件人配置。
- 授权码留空保存时会保留旧值，避免误清空。

推送通道类型：

- 通用 `POST JSON`。
- `GET` 请求，适合把参数直接放在 URL 里的服务。
- 自定义请求体模板，可使用 `{sender}`、`{message}`、`{timestamp}` 等占位符。
- Bark 官方服务或自建 Bark 服务。
- 钉钉机器人。
- 飞书机器人。
- PushPlus。
- Server 酱。
- Gotify。
- Telegram Bot。

转发规则：

- 可按发件人、关键词、正则匹配短信。
- 可把短信分流到指定推送通道，或直接丢弃。
- 支持页面内规则测试，本地预览命中结果，不会真的发送。

### 定时任务与保号

Web UI 提供独立的定时任务页，既能做 SIM 保号，也能做系统维护：

- SIM 保号任务：按间隔天数执行短信、USSD、蜂窝 HTTP 等动作，用于降低停机/回收风险。
- 每日心跳：定时发送状态通知，确认设备还在线。
- 每日重启：指定小时自动重启，适合无人值守场景做周期性自恢复。
- 自定义定时任务：最多可配置 6 个任务卡片，每个任务可设置名称、间隔天数、目标 Profile、是否切回、动作和 payload。
- 任务保存是局部保存，保号、系统定时、自定义任务互不影响。

### eSIM 管理

固件包含 eSIM / eUICC 基础管理能力，适合需要多 Profile 轮换或保号的场景：

- 可读取 eSIM Profile 列表。
- 可查看 Profile 标识、昵称、状态等信息。
- 可启用指定 Profile，实现切卡/换卡。
- 支持任务里选择目标 Profile，并在任务后按配置切回原 Profile。
- eSIM 操作走模组 AT / APDU 能力，具体可用性取决于模组固件、卡片和运营商 Profile 支持情况。

### Web 管理

- WiFi 配网：支持首次 SoftAP 配网，正常连接后可通过 IP 或 `sms.local` 访问（mDNS 主机名可在系统设置中自定义，多设备可各用一个 `xxx.local`）。
- 历史 WiFi：最多保存 5 组网络，成功连接过的网络自动加入列表（同手机）；连接前先扫描并自动接入在场且信号最好的一组（多地点部署免等逐组超时），列表可在“网络设置”中增删、SSID 支持从扫描结果下拉选择。
- 账号密码：Web 管理账号可单独保存。
- 时间设置：NTP 服务器和时区可配置。
- 蜂窝设置：蜂窝数据开关、APN、运营商 PLMN 锁定可配置。
- NET 指示灯：可关闭模组 NET 灯，配置会持久保存。
- AT 终端：Web 上可发送 AT 命令辅助诊断。
- 系统日志：Web 可查看最近 120 行运行日志。
- OTA 升级：支持通过 Web 上传新固件。
- 配置导入导出：便于备份和迁移设备。


## 项目特点

### 原生 ESP-IDF 固件

- 基于 ESP-IDF 5.x，目标芯片为 ESP32-C3。
- 使用组件化结构拆分配置、WiFi、Web、模组、短信、推送、日志和收件箱。
- 通过 CI 持续构建，减少“本地能编过、换环境不行”的问题。
- Web UI 使用 `code/web_src/` 维护源码，再打包为 gzip 静态资源链接进固件。

### 稳定的短信接收链路

- 支持 PDU 模式短信接收，覆盖中文短信和长短信合并。
- 同时使用 `+CMT`/`+CMTI` URC 与周期性 `AT+CMGL` 存储轮询，单一路径丢事件时仍可补收。
- 接收侧做短信去重、黑名单、管理员命令和收件箱缓存。
- 默认短信转发走 WiFi，蜂窝网络主要用于可选的保号 HTTP、短信或 USSD 动作。
- 运行时周期检测 SIM 卡热插拔，插卡自动重初始化并重新注册，换卡不必手动重启设备。

### 异步队列设计

- `idf_modem` 独占 UART1，所有 AT 命令串行化，避免多个任务同时抢模组。
- 短信接收、Web 请求、推送、邮件、保号、定时任务分层处理，慢操作尽量进入后台 worker。
- 推送/邮件使用队列、失败重试和通道冷却，降低 TLS、SMTP 或外部服务抖动对短信接收的影响。
- Web 表单保存按页面和配置域拆分，点击哪里的保存只影响对应配置，减少全量保存带来的连带风险。
- 定时保号、每日心跳、每日重启、低堆保护由 Web scheduler 统一调度，耗时动作再交给后台执行。

### 多通道转发

- 邮件 SMTP。
- POST JSON、自定义 GET、自定义模板。
- Bark 官方/自建、钉钉、PushPlus、Server 酱、飞书、Gotify、Telegram。
- 最多 5 个推送通道，可按规则决定是否转发到指定通道。

### Web 管理与维护

- Web UI 覆盖系统概览、转发配置、短信收发、定时任务、诊断控制、日志查看和 OTA。
- 支持 SoftAP 配网、mDNS 域名（默认 `sms.local`，主机名可自定义）、NTP 时间同步、AT 终端和运行状态查看。
- 日志进入 120 行环形缓冲，便于无串口时从 Web 排障。
- OTA 双分区升级，配合崩溃转储分区，方便远程升级和异常分析。

## 项目架构

固件入口在 `main/app_main.cpp`，启动后按“配置 -> 网络 -> 模组 -> 短信 -> 推送/Web”的顺序初始化各组件。运行时尽量保持单一职责：模组 AT 只由 `idf_modem` 访问，短信接收只负责解码和入队，推送和邮件交给后台 worker，Web handler 只做参数解析和轻量响应。

```text
main/app_main.cpp
  ├─ idf_config      NVS 配置、默认值、表单保存、导入导出
  ├─ idf_wifi        STA/SoftAP 配网、DNS 门户、mDNS、SNTP、重连 watchdog
  ├─ idf_modem       UART1 单 owner、AT 串行化、蜂窝 HTTP/SMS/USSD
  ├─ idf_sms         PDU 收发、长短信合并、去重、黑名单、管理员命令
  ├─ idf_push        邮件/推送 worker、失败重试、通道冷却、规则判断
  ├─ idf_web         HTTP 路由、Web UI、OTA、状态、日志、定时任务
  ├─ idf_esim        eUICC APDU/ES10c 基础能力与配置入口
  ├─ idf_inbox       收件箱/已发送记录的本地环形缓存
  ├─ idf_logbuf      Web 可见日志环形缓冲
  ├─ idf_pdu         PDU 编解码组件
  └─ web_assets      gzip Web 静态资源
```

核心数据流：

```text
ML307 UART URC / AT+CMGL 轮询
  -> idf_modem 串行 AT 收发
  -> idf_sms PDU 解码、长短信合并、去重、规则预处理
  -> idf_inbox 本地留存
  -> idf_push 转发队列
  -> SMTP / Bark / Telegram / Gotify / 自定义 HTTP 等通道
```

Web 侧数据流：

```text
浏览器 Web UI
  -> idf_web HTTP 路由
  -> idf_config 局部保存配置
  -> scheduler / worker queue 执行慢任务
  -> idf_logbuf / status JSON 返回运行状态
```

这种结构的重点是把“容易慢”的动作从请求处理和短信接收路径上移开。外部推送服务慢、SMTP 握手慢、保号 HTTP 超时、浏览器多标签页刷新，都不应该直接卡住 UART 收短信。

## 固件分区

当前默认面向 4MB Flash，使用自定义分区表 `partitions_ota_1m6.csv`：

| 分区 | 大小 | 用途 |
| --- | ---: | --- |
| `nvs` | 20KB | 系统配置与基础参数 |
| `otadata` | 8KB | OTA 启动选择信息 |
| `app0` | 1.625MB | OTA 固件槽位 0 |
| `app1` | 1.625MB | OTA 固件槽位 1 |
| `smsdata` | 640KB | 短信、收件箱等本地数据，使用 NVS 磨损均衡与掉电原子写入 |
| `coredump` | 64KB | 崩溃转储，便于定位无人值守设备的异常 |

`app0`/`app1` 保留双 OTA 槽位，每个槽位 1.625MB。当前固件大小约 1.24MB，单槽仍有约 24% 余量，可继续容纳后续 eSIM、诊断和 UI 功能。`smsdata` 单独放大，避免短信留存和普通配置共用很小的 NVS 空间。

## 体积与内存优化

- 编译使用 `-Os` 尺寸优化，并减少断言字符串带来的 rodata 占用。
- 关闭项目不使用的 IPv6、termios、WiFi NVS 等能力，减少 flash 和 RAM 占用。
- WiFi 收发路径、FreeRTOS 与 ringbuf 部分函数放回 flash，释放 ESP32-C3 紧张的 IRAM。
- TLS 使用动态缓冲和会话后释放策略，空闲期减少堆占用。
- socket 上限提高到 20，适配多浏览器标签页、mDNS、SNTP、推送和 SMTP 并存的场景。
- SNTP 配置主服务器和兜底服务器，单个 NTP 不可达时不影响时间同步。
- 启用 flash coredump，异常重启后仍能保留定位信息。

## 稳定性验证

近期在真实设备上做过一轮针对 Web 与后台队列的压力验证：

- 设备通过 COM5 烧录后运行，Web 访问地址为局域网 IP。
- 版本号验证到 `1.0.5`，模组状态、队列状态、时间同步、推送通道等状态页字段正常。
- Web 路由 4 并发压力测试通过，`2223/2223` 请求成功，错误数为 0，未观察到异常日志。
- 10 分钟轮询压力测试通过，`2484/2484` 请求成功。
- 待转发、推送队列、待发短信、邮件队列在空闲状态下保持 `0 / 0 / 0 / 0`。
- 对 NET LED 保存、页面局部保存、APN 显示、保号任务布局等近期问题做过回归验证。

这些验证不能替代长期老化测试，但已经覆盖了多页面轮询、配置保存、Web 刷新、后台任务和模组状态读取并发时的主要风险点。

## Web UI 预览

以下截图由 `preview/build_preview.py` 基于当前 `code/web_src/` 生成，所有号码、邮箱、Device Key、IMEI/ICCID/IMSI 均为 mock 示例数据，不包含真实设备信息。

```powershell
python preview\build_preview.py
start preview\index.html
```

| 系统概览 | 转发设置 |
| --- | --- |
| ![系统概览](assets/ui-overview.png) | ![转发设置](assets/ui-push.png) |

| 收发短信 | 定时任务 |
| --- | --- |
| ![收发短信](assets/ui-inbox.png) | ![定时任务](assets/ui-keepalive.png) |

| 诊断与控制 | 系统日志 |
| --- | --- |
| ![诊断与控制](assets/ui-diagnose.png) | ![系统日志](assets/ui-log.png) |

## 固件下载与刷写

普通用户不需要下载源码，也不需要安装 Python 或 ESP-IDF。到 GitHub Release 下载对应版本的 `.bin` 文件即可：

| 文件 | 用途 | 使用场景 |
| --- | --- | --- |
| `sms_forwarder_full_v*.bin` | 整机刷写包，烧录地址 `0x0` | 首次刷机、救砖、分区表变化后重刷 |
| `sms_forwarder_ota_v*.bin` | OTA 升级包，只包含 app 分区镜像 | 设备已经能打开 Web UI 时，在网页“固件升级”上传 |

注意：`full` 包和 `ota` 包不能混用。首次刷机请把 `sms_forwarder_full_v*.bin` 写到 `0x0`；网页 OTA 只上传 `sms_forwarder_ota_v*.bin`。

### 首次刷机（推荐 GUI）

推荐用乐鑫官方图形工具，普通用户不用接触命令行：

- Windows：使用 [Flash Download Tool](https://docs.espressif.com/projects/esp-test-tools/en/latest/esp32c3/production_stage/tools/flash_download_tool.html)。选择 `ESP32-C3`，模式选 `Develop`，下载方式选 `UART`，把 `sms_forwarder_full_v*.bin` 填到地址 `0x0`，选择实际 `COM` 口和波特率 `460800` 后点击 `START`。
- Chrome / Edge：使用 [ESP Launchpad](https://espressif.github.io/esp-launchpad/) 的自定义固件刷写功能。连接设备后选择本地 `sms_forwarder_full_v*.bin`，地址填 `0x0`，再开始刷写。

如果自动进入下载模式失败，可以按住开发板 `BOOT`，轻点 `RST/EN`，开始烧录后松开 `BOOT`。不同 ESP32-C3 开发板按钮名称可能不同，以板子丝印为准。

### 首次刷机（开发者命令行）

已经安装 Python/esptool 的用户，也可以直接用命令行刷完整包：

```powershell
esptool.py --chip esp32c3 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 sms_forwarder_full_v*.bin
```

其中 `COM5` 和文件名按实际情况替换。`esptool.py` 是电脑端刷写工具，不在固件里；没有 Python 环境时请优先使用上面的 GUI 工具。

### 后续 OTA 升级

设备已经能进入 Web UI 时，不需要数据线：

1. 打开设备网页。
2. 进入“系统设置 / 固件升级 (OTA)”。
3. 选择 `sms_forwarder_ota_v*.bin`。
4. 可选填入 Release 页面提供的 OTA 包 SHA-256。
5. 点击升级，等待设备自动重启。

不要把 `sms_forwarder_full_v*.bin` 上传到网页 OTA。完整包包含 bootloader 和分区表，只适合从 `0x0` 串口整机刷写。

### 首次访问 Web UI

首次启动时设备会优先连接已保存 WiFi；如果没有配置或连接失败，会开启开放热点 `SMS-Forwarder-XXXX`。连接热点后访问 `http://192.168.1.1` 配网；设备连上路由器后，可通过串口日志里的 IP 地址或 `http://sms.local` 打开 Web UI（主机名可在“系统设置 → 局域网域名”中修改，多台设备请设置不同主机名）。

默认账号密码为 `admin` / `admin123`，首次使用请立即修改。

## 本地开发构建

开发者需要本地编译固件时，再准备源码和 ESP-IDF：

- Windows + PowerShell。
- ESP32-C3 开发板或 ESP32-C3 Super Mini。
- ML307 系列 4G/LTE 模组。
- USB 数据线和对应串口驱动。
- ESP-IDF 5.5.4。仓库脚本默认查找 `E:\Espressif\esp-idf-v5.5.4`，也可以通过 `IDF_PATH` 和 `IDF_TOOLS_PATH` 环境变量指定自己的安装位置。

确认串口号：

```powershell
Get-CimInstance Win32_SerialPort | Select-Object DeviceID,Description
```

拉取并进入项目：

```powershell
git clone https://github.com/MineSunshineone/sms_forwarding.git
cd sms_forwarding
```

仓库提供了 ESP-IDF 封装脚本，会自动加载 ESP-IDF 环境，并把构建产物放在 `build/idf`，把 `sdkconfig` 放在 `build/sdkconfig`：

```powershell
powershell -ExecutionPolicy Bypass -File tools\idf.ps1 build
```

如果只改了 Web UI 源码，构建前先重新生成静态资源：

```powershell
python tools\build_web_assets.py
python tools\build_web_assets.py --check
```

本地编译后烧录到 ESP32-C3：

```powershell
powershell -ExecutionPolicy Bypass -File tools\idf.ps1 flash -Port COM5
```

打开串口日志：

```powershell
powershell -ExecutionPolicy Bypass -File tools\idf.ps1 monitor -Port COM5
```

`monitor` 中可以看到 WiFi 连接、模组初始化、短信接收、推送、Web 访问和崩溃原因等日志。退出 monitor 通常使用 `Ctrl+]`。

CI 使用 `.github/workflows/build.yml` 构建 ESP-IDF 固件，日常本地开发建议仍使用 `tools\idf.ps1`，这样构建目录和配置文件位置保持一致。

## Bark 推送

推送方式选择 `Bark（iOS推送）` 后：

- `Bark 服务器 URL` 留空使用 `https://api.day.app`；自建服务填服务器根地址，例如 `https://bark.example.com` 或 `http://192.168.1.10:8080`，固件会发送到 `/push`。
- `Device Key` 填 Bark App 中显示的 key；兼容旧配置里直接填写 `https://api.day.app/<key>` 的方式。
- `可选参数` 使用 URL query 风格，例如 `group=SMS&sound=bell&level=timeSensitive&icon=https%3A%2F%2Fexample.com%2Ficon.png`；支持 Bark 的 `subtitle`、`level`、`badge`、`sound`、`group`、`icon`、`url`、`copy`、`isArchive` 等参数。

## 出厂 WiFi 配置

可选：本地创建 `code/wifi_config.h` 作为出厂 WiFi seed（该文件被 `.gitignore` 忽略）：

```cpp
#define WIFI_SSID "your-wifi"
#define WIFI_PASS "your-password"
```

也可以留空，完全走 Web 配网。

## 硬件接线

ESP32-C3 Super Mini 与 ML307x-DC UART 连接：

```text
ESP32-C3 GPIO5  -> ML307 EN
ESP32-C3 GPIO3  -> ML307 RX
ESP32-C3 GPIO4  -> ML307 TX
ESP32-C3 GND    -> ML307 GND
ESP32-C3 5V     -> ML307 VCC
```

默认引脚定义在 ESP-IDF 模组组件中：TXD=GPIO3，RXD=GPIO4，MODEM_EN=GPIO5，LED=GPIO8。

## 开发入口

- `main/`：ESP-IDF app entry。
- `components/idf_*`：配置、WiFi、Web、模组、短信、推送、日志、收件箱。
- `components/web_assets`：打包后的 Web 静态资源组件。
- `code/web_src/`：可编辑 Web UI 源。
- `code/web_assets.*`：生成文件，不手改。
- `dev_doc/`：架构和迁移说明。

更多工程约定见 `AGENTS.md`。

## 致谢

感谢 [LINUX DO](https://linux.do) 社区提供交流与灵感。
