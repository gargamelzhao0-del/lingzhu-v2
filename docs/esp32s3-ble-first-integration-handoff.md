# ESP32-S3 BLE Wi-Fi 配网首轮联调交接单

> 2026-07-25 覆盖说明：最终镜像已烧录并通过设备、本地真实 Provider 模拟器和测试网关的单次 `WIFI_READY + DEVICE_ONLINE` 验收。对外提交仍以 `docs/ble-provisioning-delivery-package/` 为权威入口，本文件保留早期首轮联调过程。

## 设备扫描 Wi-Fi 列表补充

首轮联调模拟器现已覆盖 `wifi_scan` capability：锁定目标设备后，通过 Security 2 调用官方 `prov-scan`，在 SSID/密码输入框下显示 ESP32-S3 实际扫描到的网络。首次扫描自动开始，每次完整扫描结束 15 秒后串行刷新，手动刷新仍可用。列表按 SSID 去重、按最强 RSSI 排序，显示信道和认证模式；自动刷新保留当前输入，点击条目才填写 SSID、清空旧密码并聚焦密码框。

真机已验证 `xspaceagi` 为设备可见的 2.4 GHz 信道 4 网络。APP Android Provider 应采用相同的“串行请求 `prov-scan` → 展示设备扫描结果 → 用户选择 SSID → 用户输入密码并暂停自动刷新 → 完成当前扫描/释放 BLE → `prov-config`”流程，不应使用手机或电脑本机扫描结果替代设备 `prov-scan` 的事实来源，也不应期待设备通过私有通知主动推送列表。扫描与配网必须互斥；配网成功或失败后都保持扫描停止，需要时由用户手动刷新。固件最多保留 32 条扫描结果。

## 2026-07-23 QR/内存回归补充

1. 完整 UI、Wi-Fi 和 BLE 同时启动时曾在 NimBLE 广播数据设置阶段出现 `BLE_ERR_MEM_CAPACITY (519)`。固件已裁剪未使用的 Central、Observer、GATT Client、GAP SMP、BLE 5 扩展和 DTM 配置，并重新构建、烧录到 `COM8`。
2. 修复后 `PROV_1467C4` 与冻结 Service UUID 可被稳定发现；二维码定向锁定、Security 2、`wifi_prov` 和 `device-info.serialNumber` 校验通过。
3. 模拟器已直接解析官方 `prov-config` protobuf 的 `wifi_fail_reason`，确保认证失败为 `WIFI_AUTH_FAILED`、SSID 不存在为 `NETWORK_NOT_FOUND`，不再被失败后的辅助读取异常覆盖成 `INTERNAL_ERROR`。
4. `SUCCESS` 只以官方 `connected`/`IP_EVENT_STA_GOT_IP` 为准；最终 `device-info` 仍可用于诊断，但不是成功条件。
5. 后续已确认该 SSID 是 ESP32-S3 可见的 WPA2/WPA3 transition 网络。固件在 BLE 配网阶段关闭 WPA3 compatibility override 后，两次正确凭据测试分别在 `14.051 s`、`13.232 s` 返回 `SUCCESS`。
6. ESP-IDF 6.0 PSA 硬件 SHA/AES 路径在 SRP6a 阶段曾因内部 DMA 内存不足失败；切换为相同算法的软件实现后 Security 2 稳定，协议和安全等级不变。
7. `GOT_IP` 后保留 15 秒查询窗口，transport 清理延时由误用的 30 秒改为 1 秒，避免 APP 已成功后设备 UI/业务仍额外等待约 30 秒。

本文可作为 Android Provider 首轮联调输入。固件和本地模拟器预验收已经通过；仍需补跑 Android Provider、BLE 断线恢复、300 秒边界和连续 20 次稳定性测试，再将契约状态升级为 `INTEGRATION_VERIFIED`。

## 1. 交付状态

交付状态：`FIRMWARE_IMPLEMENTED_FIRST_INTEGRATION`。

本版本用于 Android Provider 与 ESP32-S3 首轮真机联调。固件已经完成编译、烧录和本地模拟器真机预验收，但尚未完成 Android + 实机联合验收，因此不能标记为 `INTEGRATION_VERIFIED` 或量产版本。

本期只支持 BLE 配网，不支持 SoftAP fallback。

## 2. 冻结技术基线

| 项目 | 值 |
| --- | --- |
| 芯片/模组 | ESP32-S3 / ESP32-S3-N16R8 |
| ESP-IDF | `v6.0` |
| ESP-IDF commit | `662a3be354759d9487bf4b1a629fadb766cb1800` |
| 配网组件 | `espressif/network_provisioning` `1.2.4` |
| 组件仓库 commit | `2de4980640bbe3d2d69473d7251640039e185b92` |
| 协议 | ESP-IDF `protocomm` |
| BLE Host | NimBLE |
| 密码学动态内存 | 外部 PSRAM（`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`） |
| SHA/AES实现 | mbedTLS软件路径；硬件加速关闭，不改变Security 2算法和报文 |
| 安全 | Security 2，禁止降级 |
| 成功标准 | `IP_EVENT_STA_GOT_IP` |

## 3. Android Provider 输入

```text
Transport: ble
Primary Service UUID: 0000ffff-0000-1000-8000-00805f9b34fb
Service name prefix: PROV_
Service name format: PROV_ + final 3 STA MAC bytes, uppercase hexadecimal
Example: PROV_A1B2C3
Security: 2
Development username: device UID from serial log
Development PoP: Nuwax7-Dev-PoP
Required capability: wifi_prov
Optional capability: wifi_scan
Custom endpoint: device-info
Firmware credential timeout: 30000 ms
Recommended APP status timeout: 35000 ms
Total BLE provisioning window: 300000 ms
Active attempt grace: up to 30000 ms when credentials were accepted before window expiry
Post-success BLE query window: 15000 ms
```

APP 不得把 BLE address 当作永久设备 ID。Security 2 建立后，以 `device-info.serialNumber` 作为稳定设备身份。

## 4. Provider 调用顺序

```text
initialize
  -> scan(Service UUID + PROV_ prefix)
  -> connect
  -> read proto-ver
  -> verify prov.sec_ver == 2
  -> verify prov.cap contains wifi_prov
  -> establish Security 2(username + PoP)
  -> read device-info
  -> optional prov-scan
  -> prov-config set SSID/passphrase
  -> prov-config apply
  -> query official provisioning status through prov-config
  -> success or mapped failure
  -> optional final device-info read
  -> disconnect/dispose
```

`prov-config`是写入、apply和配网结果查询的权威endpoint；`prov-ctrl`仅用于reset/reprovision控制。`device-info`用于设备身份和辅助诊断。APP收到官方success后应立即进入成功页面，不应把再次读取`device-info`作为成功的必要条件。

## 5. 状态与错误映射

| APP状态 | 固件/协议来源 |
| --- | --- |
| `SECURITY_AUTH_FAILED` | Security 2 username/PoP不匹配 |
| `WIFI_AUTH_FAILED` | `NETWORK_PROV_WIFI_STA_AUTH_ERROR` |
| `NETWORK_NOT_FOUND` | `NETWORK_PROV_WIFI_STA_AP_NOT_FOUND` |
| `TIMEOUT` | 固件30秒观察超时或APP 35秒保护超时 |
| `INTERNAL_ERROR` | 未知Wi-Fi reason、protocomm错误或本地`ESP_ERR` |
| `SUCCESS` | `IP_EVENT_STA_GOT_IP` |

当前没有独立`DHCP_FAILED`原生状态。未获得IP且达到超时条件时映射为`TIMEOUT`。

`device-info.lastProvisioningOutcome`支持：

```text
none
success
auth_failed
network_not_found
timeout
internal_error
stopped
```

## 6. BLE生命周期

### 首次配网

- ESP-IDF Wi-Fi provisioned标志不存在时自动广播。
- 失败后在300秒主窗口内保持BLE服务，可以在同一连接重试。
- BLE意外断开后，只要主窗口或已接纳尝试的grace未结束，设备继续广播并允许重连查询状态。
- 300秒截止时没有进行中的尝试则停止BLE；截止前已接收的凭据先补足最多30秒观察期。之后用户需要在设备上重新进入配网模式。

### 配网成功

- `GOT_IP`后返回success。
- BLE继续保留15秒供APP读取最终状态。
- 15秒后停止provisioning，最多约1秒完成transport清理，固件继续正常业务启动。

### 已配网设备重新配网

物理操作：

1. 在设备上打开`Device`详情页。
2. 如需二维码定向，先点击`Device QR`完成APP扫描并返回详情页。
3. 点击`Meeting`开关下方实际显示为Wi-Fi图标加`BLE`的按钮。
4. 设备显示`BLE provisioning restart`状态。
5. 固件只清除ESP-IDF Wi-Fi配置并重启。
6. 重启后自动进入BLE provisioning。

不提供远程无条件清除Wi-Fi的endpoint或云端命令。

## 7. 二维码与生产密钥

首轮联调使用串口取得username，并使用固定开发PoP。生产版本必须改为每设备独立PoP、salt和verifier；本次联调固件不代表生产密钥链路已经完成。

```json
{
  "ver": "v1",
  "name": "PROV_A1B2C3",
  "transport": "ble",
  "network": "wifi",
  "security": 2,
  "username": "NX-A1B2-C3D4-E5F6",
  "pop": "PER_DEVICE_RANDOM_POP"
}
```

开发固定PoP不得进入公开日志、截图、生产标签或正式发布固件。

## 8. 固件构建交付

本地构建验证：`PASS`。

| 产物 | 路径 |
| --- | --- |
| APP binary | `build/NuwaxAIPartner.bin` |
| ELF | `build/NuwaxAIPartner.elf` |
| MAP | `build/NuwaxAIPartner.map` |
| Bootloader | `build/bootloader/bootloader.bin` |
| Partition table | `build/partition_table/partition-table.bin` |
| OTA data | `build/ota_data_initial.bin` |
| 完整配置 | `sdkconfig` |
| 依赖锁 | `dependencies.lock` |
| 机器契约 | `main/nuwax_provision_new/esp32s3-idf6-provisioning-contract.json` |

烧录命令：

```powershell
idf.py -p <PORT> flash monitor
```

串口波特率：`115200`。日志禁止输出Wi-Fi密码、PoP、salt、verifier和Security 2会话材料。

## 9. 首轮验收矩阵

1. 正确username、PoP和2.4GHz Wi-Fi成功取得IP。
2. 错误PoP在Security 2阶段失败。
3. 错误Wi-Fi密码返回`WIFI_AUTH_FAILED`。
4. 不存在的SSID返回`NETWORK_NOT_FOUND`。
5. 单次超时返回`TIMEOUT`，并可在300秒窗口内重试。
6. BLE断开后重新扫描、连接并查询最近状态。
7. APP在15秒成功窗口内稳定读取官方success。
8. 已配网设备通过`Device`页实际显示为Wi-Fi图标加`BLE`的按钮重新开放BLE配网。
9. 连续配网20次，记录成功率、总耗时、失败阶段和BLE断连原因。

## 10. 联调记录

联调时双方填写：

```text
Firmware git commit:
APP git commit:
Test date:
Device serialNumber:
Chip revision:
Android device/model:
Android version:
Tester:
Result:
Log location:
```

全部验收项通过后，双方将契约状态更新为`INTEGRATION_VERIFIED`。

## 11. 固件侧真机预验收（2026-07-23）

已使用本地模拟器作为 Provider 对烧录后的 ESP32-S3 完成以下验证：

- `PROV_1467C4` 广播、冻结 Service UUID 和实际 ATT MTU 517。
- Security 2、`proto-ver`、`wifi_prov,wifi_scan` capabilities 和加密 `device-info`。
- 不存在 SSID 返回 `NETWORK_NOT_FOUND`，并可在 300 秒窗口内继续重试。
- 正确 2.4 GHz Wi-Fi 两次完整流程分别在 `14.051 s`、`13.232 s` 返回官方 `SUCCESS`，设备取得 `192.168.31.3`。
- `GOT_IP` 后 BLE 保留 15 秒，随后停止广播。
- 错误 PoP 返回 `SECURITY_AUTH_FAILED`，错误 Wi-Fi 密码返回 `WIFI_AUTH_FAILED`。
- Wi-Fi 失败后设备保持可发现并允许重试；随后正确凭据返回 `SUCCESS`。
- `Device > BLE` 的触摸、清除 Wi-Fi、重启和恢复广播全链路通过。
- ESP-IDF STA配置、应用主凭据和Wi-Fi历史均保存成功；历史密码NVS键后缀改为`_pwd`，不再触发`ESP_ERR_NVS_KEY_TOO_LONG`。
- 正式自动判断固件重启后使用保存凭据联网，且模拟器扫描不到`PROV_1467C4`，符合“已配网正常启动不广播BLE”的策略。
- 最终应用大小为`0x322d30`，应用分区剩余约34%。证据日志为`runtime-logs/nuwax7-ble-final-credential-test-20260723-172305.log`和`runtime-logs/nuwax7-ble-history-persist-retry-20260723-173653.log`。

尚未验收：Android Provider、真实 BLE 断连恢复、300 秒边界 active-attempt grace、纯 WPA3-only AP 和连续 20 次稳定性测试。因此当前状态仍为 `FIRMWARE_IMPLEMENTED_FIRST_INTEGRATION`。
> **归档说明（2026-07-24）**：本文是 Wi-Fi-only 首轮联调历史记录。当前可提交版本为 `docs/ble-provisioning-delivery-package/` 契约 1.1；完整成功已经从单独 `GOT_IP` 调整为 `WIFI_READY + DEVICE_ONLINE`。
