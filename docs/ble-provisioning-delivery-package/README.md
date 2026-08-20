# ESP32-S3 BLE Wi-Fi 配网首轮联调交付包

交付状态：`FIRMWARE_LOCAL_E2E_VERIFIED`。

本包可以提交给 Android APP 工程师开展 Security 2、Wi-Fi 和逐设备云端鉴权的首轮联调。契约 1.2 已增加设备 NVS 提交回执、原始 Wi-Fi 断线诊断、损坏槽位恢复和可回滚的重新配网。最终源码已完成 ESP-IDF 6.0 构建、应用分区烧录、主机回归，以及 ESP32-S3 + 本地真实 Provider 模拟器 + 测试网关三端真机验收：同一次流程已到达 `WIFI_READY + DEVICE_ONLINE`。这表示固件接口和本地参考 Provider 已具备 Android 首轮联调条件，但不代表 Android 联调完成或量产就绪；逐设备 PoP、factory NVS 注入、正式 APP 登录/claim API、Secure Boot、Flash Encryption 和 NVS Encryption 仍是量产阻塞项。

## 阅读顺序

1. `01-app-firmware-handoff.md`：双方必须按此实现的冻结协议。
2. `05-provisioning-contract.json`：机器可读契约，Provider 常量应以它为准。
3. `02-first-integration-runbook.md`：首轮真机操作和结果判定。
4. `06-simulator-guide.md`：Mock/Real 模拟器使用说明。
5. `00-audit-report.md`：已修复问题、遗留风险和可交付结论。
6. `03-architecture.md`、`08-acceptance-matrix.md`：固件结构和验收矩阵。
7. `09-cloud-onboarding.md`：云配置包、签名、NVS 双槽、逐设备 WebSocket 鉴权和 APP 状态机。
8. `10-final-device-acceptance.md`：2026-07-25 本地三端真机验收结果和证据边界。

## 冻结值

| 项目 | 值 |
| --- | --- |
| 芯片 / SDK | ESP32-S3 / ESP-IDF `v6.0` |
| ESP-IDF commit | `662a3be354759d9487bf4b1a629fadb766cb1800` |
| 配网组件 | `espressif/network_provisioning` `1.2.4` |
| 组件 commit | `2de4980640bbe3d2d69473d7251640039e185b92` |
| BLE Host | NimBLE |
| Service UUID | `0000ffff-0000-1000-8000-00805f9b34fb` |
| 广播名 | `PROV_` + STA MAC 后三字节大写十六进制 |
| 安全 | protocomm Security 2，SRP6a + AES-GCM，不降级 |
| capabilities | `wifi_prov`、`wifi_scan`、`cloud_prov` |
| 自定义 endpoint | `device-info`、`cloud-config` |
| Wi-Fi 里程碑 | `IP_EVENT_STA_GOT_IP` / `prov-config=connected` |
| 最终成功标准 | `WIFI_READY` + `DEVICE_ONLINE` |
| 云配置提交中 | `CLOUD_CONFIG_COMMIT_PENDING`，APP 继续等待，不得显示成功 |
| 重新配网失败 | 固件恢复进入配网前保存的 STA 配置；已有 active 云配置继续使用 |
| 云端鉴权 | `X-Device-ID` + `X-Credential-ID` + Bearer `device_secret`，101 前逐设备校验 |
| 传输 | 仅 BLE，不支持 SoftAP fallback |
| 已烧录并验收的应用镜像 | `3,302,976` bytes；SHA-256 `46B85B6769639B7C8B6B5FFC3D66D97B4F5367D2DF0BFF5D3AEA5C6C940CE55A` |
| 本地三端验收 | `WIFI_READY` 14.3 s；`DEVICE_ONLINE` 33.6 s；最终 `SUCCESS` |

当前开发设备：设备 ID `NX-3927-2693-9288`，广播名 `PROV_1467C4`。开发联调 PoP 写在机器契约中，仅用于首轮联调，禁止用于生产。

## 模拟器

`simulator/` 包含网页、BLE Bridge、测试网关启动脚本、二维码解析、离线 jsQR、依赖文件和测试。Mock 模式验证 APP 状态机；Real 模式复用工程锁定的 Espressif `esp_prov`、protobuf 和 Security 2，并从测试网关领取签名云配置。

从本目录启动：

```powershell
cd simulator
.\start-full-stack.ps1
```

打开 `http://127.0.0.1:8765/simulator/`。脚本自动选择可供 ESP32-S3 访问的电脑局域网 IPv4，启动测试网关和回环 BLE Bridge。Bridge 不记录 PoP、Wi-Fi 密码或 `device_secret`，并串行执行 BLE 发现、设备 Wi-Fi 扫描和完整 onboarding。

## 不包含的内容

- 不包含 `sdkconfig`、运行日志、缓存、`__pycache__` 或本地 Wi-Fi/云端凭据。
- 不包含固件 `.bin`；固件镜像可能包含本地开发配置，不属于 APP 对接交付物。
- 不包含量产密钥和每设备 PoP 数据库。
- 最终应用镜像已通过 `app-flash` 写入开发设备 `COM8` 的应用分区 `0x420000`；未擦除或改写 bootloader、分区表、NVS、模型及 storage。
