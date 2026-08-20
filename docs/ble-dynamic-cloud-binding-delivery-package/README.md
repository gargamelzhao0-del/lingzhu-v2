# ESP32-S3 BLE 动态云配置与设备绑定交付包

交付状态：`FIRMWARE_LOCAL_E2E_VERIFIED`。

本包面向 Android APP、云端和固件工程师，说明二维码识别、BLE Security 2、Wi-Fi 配网、动态 `cloudUrl`/`deviceSecret` 下发、逐设备云鉴权和账户绑定的完整边界。它是首轮“二维码 + BLE Wi-Fi 配网”交付包的升级说明，旧包保留，不应覆盖或删除。

当前固件、本地真实 Provider 模拟器和测试网关已经在同一次真机流程到达：

- `WIFI_READY`：14.3 秒，设备收到 `IP_EVENT_STA_GOT_IP`。
- `DEVICE_ONLINE`：33.6 秒，网关确认设备凭据提交。
- 最终结果：`SUCCESS`，网关 credential 为 active、pending 为空。

这证明本地三端协议可行，不代表 Android Provider、正式账户体系或量产安全已经验收。

## 阅读顺序

1. `00-scope-and-terminology.md`：交付范围和各类地址、密钥的区别。
2. `01-three-party-architecture.md`：本地测试服务、模拟器和设备的职责与完整时序。
3. `02-communication-protocol.md`：HTTP、BLE、WebSocket 的逐报文格式。
4. `03-android-app-change-guide.md`：Android 数据模型、接口和状态机改造。
5. `04-local-test-gateway.md`：本地测试服务的启动、注册表和限制。
6. `05-simulator-guide.md`：模拟器 Mock/Real 模式和真机操作。
7. `06-device-firmware-logic.md`：设备验签、NVS pending/active、鉴权和回滚。
8. `07-upgrade-from-first-package.md`：相对首轮交付包新增的功能及逻辑。
9. `08-security-and-production-boundaries.md`：开发凭据、生产密钥和量产阻塞项。
10. `09-test-and-acceptance.md`：测试矩阵、结果判断和交付检查表。
11. `10-file-manifest.md`：交付文件、来源和外部依赖边界。
12. `11-android-provider-reference.md`：冻结 Android 库版本、真实 API 和实现陷阱。
13. `12-final-audit-report.md`：本轮代码级审计结论、已修复问题和剩余边界。
14. `provisioning-contract.json`：APP/固件共用的机器可读冻结契约。

## 包内内容

```text
ble-dynamic-cloud-binding-delivery-package/
  README.md
  00..12 文档
  provisioning-contract.json
  simulator/
    网页模拟器
    Python BLE Bridge
    二维码解析器
    一键启动脚本
    Bridge/二维码测试
```

模拟器不自行仿造 Espressif GATT 协议；Real 模式直接复用工程锁定的 `esp_prov`、protobuf 和 protocomm Security 2。`simulator/start-full-stack.ps1` 会启动工程内现有测试网关，再启动只监听 loopback 的 BLE Bridge。交付 ZIP 也可脱离固件仓库启动 Mock 模式，Mock 只验证 APP 状态机，不宣称连接真实设备。

## 独立启动 Mock

只拿到本交付包时，可运行：

```powershell
cd ble-dynamic-cloud-binding-delivery-package\simulator
.\start-mock.ps1
```

打开 `http://127.0.0.1:8765/simulator/`，保持 `Mock` 模式。此路径读取包内 `provisioning-contract.json`，不需要 ESP-IDF、蓝牙设备、测试私钥或云网关。

## 启动 Real 三端联调

前提：本包仍位于完整固件工作区内；工程已有 ESP-IDF 构建信息、managed component、测试网关和匹配的测试签名服务；设备已经烧录本次验收固件，并通过设备 `Device > BLE` 按钮进入配网模式。

```powershell
cd docs\ble-dynamic-cloud-binding-delivery-package\simulator
.\start-full-stack.ps1
```

浏览器打开：

```text
http://127.0.0.1:8765/simulator/
```

脚本会选择设备可访问的电脑局域网 IPv4，在 `8799..8810` 中选择空闲网关端口。浏览器和 Bridge 只使用 `127.0.0.1:8765`；ESP32-S3 通过 Wi-Fi 访问脚本显示的局域网 WebSocket URL。

## 明确不随包交付

- 不包含真实 Wi-Fi 密码、生产逐设备 PoP、`deviceSecret`、APP Token、网关注册表或运行日志。契约有意包含测试固件共享的开发 PoP，正式 APP 不得将其作为 fallback。
- 不复制测试/生产签名私钥；一键脚本在当前工程中引用已有测试密钥。
- 不包含生产用户绑定、解绑、所有权转移、吊销和审计服务。
- 不包含固件 `.bin`、`sdkconfig`、NVS 镜像、Secure Boot 或 Flash Encryption 密钥。

最终验收固件 SHA-256：`46B85B6769639B7C8B6B5FFC3D66D97B4F5367D2DF0BFF5D3AEA5C6C940CE55A`。
# 归档警告

此目录保留的是已停用的签名 `cloud-config` 方案，包含
`credentialId`、`bundleVersion`、`signature` 和 pending/commit 语义，不能提交给
APP 工程师，也不能用于当前真机联调。当前冻结协议是
`main/nuwax_provision_new/esp32s3-idf6-provisioning-contract.json`
（`1.3-dynamic-vox-config`），当前模拟器位于 `docs/provision_simulator/`。
