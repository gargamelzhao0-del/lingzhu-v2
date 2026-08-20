# 最终代码级审计报告

审计日期：2026-07-25。结论：修正本报告列出的问题后，本交付包可以作为 Android Provider 首轮开发和联合调试的权威输入；协议字段、调用顺序、endpoint 发现、动态云配置事务和成功标准均可从固件、模拟器或测试网关代码落实。它仍不是 Android 联调通过或生产绑定系统交付。

## 审计依据

- 固件：`main/nuwax_provision_new`、`main/nuwax_ai_partner_main.c`、`main/ws_transport.c`、`main/session_protocol.c`、`main/wifi_manager.c`。
- 模拟器：本包 `simulator/bridge.py`、`app.js`、二维码代码和测试。
- 测试网关：`cloud-gateway/device-onboarding.js`、`cloud-gateway/cloud-gateway.js`、onboarding tests。
- Android 参考：Espressif `esp-idf-provisioning-android` tag `lib-2.4.4`、commit `99719ae2d89b9a209cc83ca88bc6f3f3f4318028`。
- 真机证据：最终固件一次完整 `WIFI_READY + DEVICE_ONLINE` 三端流程。

原 APP GitHub 分支文档在本次环境中返回 404/不可访问，因此未把远端 APP 当前实现作为已验证事实。APP 团队合并时仍需用其实际源码逐项对照 `11-android-provider-reference.md`。

## 本轮发现与修复

| 严重度 | 问题 | 修复 |
| --- | --- | --- |
| P0 对接 | `cloud_prov` 来源未写清，APP 可能从 `getDeviceCapabilities()/prov.cap` 检查并误判 | 契约明确 `prov.cap` 与 `nuwax.cap`；Android 文档要求解析 `getVersionInfo()` |
| P0 对接 | 自定义 endpoint 只写名称，未说明 characteristic UUID 是否固定 | 明确通过 `0x2901 User Description` 动态发现，禁止硬编码 UUID |
| P0 对接 | Android 库版本和真实 API 未冻结 | 冻结参考 `lib-2.4.4`/commit，补充创建、连接、session、自定义 endpoint、scan/provision 调用 |
| P0 报文 | 契约的 bundle 必填字段漏列固件强制校验的 `version` | 三份契约补齐 `version`，文档明确必须是 JSON 整数 `1` |
| P0 模拟器 | Real Provider 只检查 cloud-config `ok=true`，未核对回执身份 | 增加 `state/credentialId/bundleVersion` 与 claim 的严格匹配并补测试 |
| P1 状态机 | Wi-Fi 35 秒与云端 90 秒可能被理解成同一计时器 | 契约明确 35 秒仅 Wi-Fi，WIFI_READY 后另起云上线 90 秒 |
| P1 业务 | “设备绑定”可能被误解为正式账户所有权已经实现 | 明确测试网关只验证逐设备 credential 绑定，正式账户 claim/解绑/转移仍未实现 |
| P1 安全 | 官方 Android 参考库源码包含原始二维码和 PoP `Log.d` | 标记生产阻塞，要求 patched/forked 依赖或验证 R8/release 产物；普通 wrapper 不视为修复 |
| P1 恢复 | 当前测试 claim 每次生成新 pending，APP 重连可能重复 claim | 文档要求同一流程复用 bundle，并把生产幂等 attempt 列为云端必做 |
| P1 可靠性 | 设备本地提交后只发送一次 `cloud_config_committed`，未持久化服务端 `_ack` | 明确当前边界和 APP 超时语义；把生产幂等补发列为发布阻塞项 |
| P2 交付 | 机器契约未记录 Android Provider 基线、endpoint 发现和 claim 语义 | 已补入三份同步契约，保持源/交付副本一致 |
| P2 交付 | ZIP 中的模拟器依赖完整固件仓库，APP 单独拿包无法运行 Mock | 新增独立 Mock 启动脚本和包内契约回退；Real 依赖边界保持明确 |

## 已逐代码确认

- Service UUID 字节序生成正确，广播名来自 STA MAC 后三字节。
- 两个自定义 endpoint 在 provisioning start 前 create，start 后 register。
- `device-info` 请求必须是 JSON 对象，响应字段与契约一致。
- `cloud-config` 使用 UTF-8 JSON，校验 deviceId、URL、版本和 ECDSA 签名后才写 pending。
- 固件没有编译期 URL/secret fallback，应用循环从 NVS effective config 建立 WebSocket。
- WebSocket Upgrade 在 101 前发送/校验三项鉴权 header。
- pending 只有在 authenticated hello 匹配并成功写 active 后才发送 commit。
- 网关收到匹配 commit 才把 status 变为 DEVICE_ONLINE。
- 模拟器 Real 模式严格按 claim -> cloud-config -> prov-config -> online 顺序。
- BLE 发现、Wi-Fi 扫描、配网由 Bridge 串行锁保护。

## 最终自动验证

- 三套模拟器各通过 Bridge 18 项、二维码 16 项、JavaScript 语法和 Python 编译检查。
- 测试网关 claim、错误 secret 401、hello、commit 和 status 集成测试通过。
- 独立 ZIP Mock 页面在浏览器中完成 `SUCCESS` 与 `WIFI_AUTH_FAILED` 场景，控制台无 warning/error。
- 三份 JSON 契约可解析且字节一致，SHA-256 为 `34B28EC9EE6B5A3747C9B2F6D75A7082423F4E26E164F256442CE454B00D29AC`。
- 清理后交付目录为 29 个文件，不含日志、缓存、私钥、注册表或已知真实 Wi-Fi/设备密钥。

## 仍需双方联合验证

- Android 12+ 权限、各手机 BLE 栈、MTU 协商、GATT descriptor 发现。
- Android Security 2 正确/错误 PoP、BLE 中断、前后台和 Activity/进程恢复。
- Android 库去敏日志构建的实际产物扫描。
- 生产用户 claim、所有权、防抢绑、幂等、解绑/转移/吊销。
- 生产逐设备 PoP 来源、factory NVS 和签名公钥轮换。
- `cloud_config_committed` 丢失后的持久化/幂等补发；当前固件只发送一次，APP 应超时失败而不能误报成功。
- 连续 20 次、300 秒边界、断电/NVS 写失败和 401 故障注入。

## 可提交结论

APP 工程师现在可以依次使用：

1. `provisioning-contract.json` 生成常量/模型。
2. `11-android-provider-reference.md` 实现 Provider。
3. `02-communication-protocol.md` 实现 claim/custom endpoint/status 报文。
4. `09-test-and-acceptance.md` 做首轮联合验收。

在 Android 真机通过前，状态保持 `FIRMWARE_LOCAL_E2E_VERIFIED`，不得升级为 `INTEGRATION_VERIFIED`。
