# 测试与验收

## 准备

- ESP32-S3 烧录最终测试固件。
- Windows 蓝牙可用，Python 能导入 `bleak`、`protobuf`、`cryptography`。
- 电脑和设备连接同一可互通的 2.4 GHz LAN。
- 工程包含已验收测试网关和与固件测试公钥匹配的测试签名私钥。
- 不在截图、工单或日志记录 Wi-Fi 密码、PoP、APP Token、`deviceSecret`。

## 正常真机验收

1. 运行 `simulator/start-full-stack.ps1`，记录脚本显示的 LAN WebSocket URL和模拟器 URL。
2. 已配网设备在 `Device` 菜单点击 BLE 按钮；确认重启后广播 `PROV_`。
3. 模拟器 Real 模式扫描二维码或二维码图片。
4. 扫描 BLE，确认名称与二维码完全相同、Service UUID 匹配。
5. 完成 Security 2 并读取 `device-info`；设备 ID 必须一致。
6. 获取设备 Wi-Fi 扫描列表，选择 2.4 GHz SSID。
7. 点击开始后确认自动扫描停止，Bridge claim 签名包并收到 cloud-config pending。
8. 确认 `prov-config` 到达 connected / `WIFI_READY`。
9. 确认设备用动态 URL/secret 完成 WebSocket 鉴权。
10. 确认出现 `cloud_config_committed_ack`，status 返回同一 credential 的 `DEVICE_ONLINE`。
11. 模拟器最终必须显示 `SUCCESS` 和 `WIFI_READY + DEVICE_ONLINE`。

## 当前已完成证据

| 项目 | 结果 |
| --- | --- |
| 设备 | `NX-3927-2693-9288` / `PROV_1467C4` |
| GATT | 7.1 秒连接 |
| Security 2 | 7.7 秒，SRP6a + AES-GCM |
| `device-info` | 7.9 秒身份一致 |
| `cloud-config` | 8.5 秒签名配置 pending |
| Wi-Fi set/apply | 8.7 / 9.0 秒 |
| `WIFI_READY` | 14.3 秒，IP `192.168.31.3` |
| `DEVICE_ONLINE` | 33.6 秒 |
| 网关注册表 | credential active，pending 为空 |
| 运行稳定性 | 后续 WebSocket ping/pong 正常，无新 assertion/panic |
| 固件 SHA-256 | `46B85B6769639B7C8B6B5FFC3D66D97B4F5367D2DF0BFF5D3AEA5C6C940CE55A` |

## 必测失败用例

| 用例 | 预期 |
| --- | --- |
| 错误 PoP | `SECURITY_AUTH_FAILED`，不 claim/不发 Wi-Fi |
| QR 与 device-info 不同 | `DEVICE_ID_MISMATCH`，不 claim/不发配置 |
| claim 401 | 显示账户/绑定权限失败，不发 BLE cloud-config |
| 修改 bundle 任一值 | `CLOUD_SIGNATURE_INVALID` |
| 缺少 `version` 或 `version != 1` | `CLOUD_CONFIG_INVALID`，不发送 Wi-Fi |
| cloud-config 回执 credential/version 与 claim 不同 | `PROTOCOL_ERROR`，不发送 Wi-Fi |
| 下发旧版本/同版本不同内容 | `CLOUD_CONFIG_ROLLBACK` |
| 缺少 cloud-config | `CLOUD_CONFIG_REQUIRED` |
| 错误 Wi-Fi 密码 | `WIFI_AUTH_FAILED`，窗口内允许重试 |
| SSID 不存在 | `NETWORK_NOT_FOUND` |
| 正确 Wi-Fi、错误 deviceSecret | 101 前 401，`CLOUD_CREDENTIAL_REJECTED` |
| 丢弃首次 `cloud_config_committed` | 当前基线会进入 `CLOUD_CONNECT_TIMEOUT`；生产版本必须重连补发并收敛 |
| Wi-Fi connected、网关不可达 | `CLOUD_CONNECT_TIMEOUT`，不能显示成功 |
| BLE 中断后重连 | 重建 Security 2，重发同一个 bundle，不重复 claim |
| 重新配网失败 | 恢复旧 Wi-Fi，旧 active 云配置保持 |
| 300 秒边界 active attempt | 已接收尝试最多宽限 30 秒，不无限广播 |

## 自动测试

在新包模拟器目录运行：

```powershell
python bridge_logic_test.py
node --test qr-codec.test.cjs
python -m py_compile bridge.py bridge_logic_test.py
```

在工程测试网关目录运行：

```powershell
npm run test:onboarding
```

机器契约必须能解析：

```powershell
Get-Content ..\provisioning-contract.json -Raw -Encoding utf8 | ConvertFrom-Json | Out-Null
```

## Android 首轮验收记录项

每次记录：APP commit、固件 SHA-256、Android 机型/系统、deviceId、广播名、AP 频段/认证方式、claim HTTP 结果、cloud-config 结果、Wi-Fi 结果、online 状态、各阶段耗时和最终 APP 结果码。

禁止记录：二维码 PoP、Wi-Fi 密码、APP Token、`deviceSecret`、salt/verifier、Security 2 原始报文、测试签名私钥。

## 发布门槛

当前状态为 `FIRMWARE_LOCAL_E2E_VERIFIED`。升级为双方 `INTEGRATION_VERIFIED` 至少需要：

- Android Provider 正常流程和全部 P1 失败路径通过。
- Android Provider 已验证从 `getVersionInfo()` 的 `nuwax.cap` 读取 `cloud_prov`，并能通过 0x2901 descriptor 发现两个自定义 endpoint。
- Android 构建已证明不输出二维码、PoP、Wi-Fi 密码或 `deviceSecret` 日志。
- BLE 断连、APP 前后台/进程恢复通过。
- 连续至少 20 次配网/重配网稳定性通过。
- 300 秒窗口和断电/NVS 写失败故障注入通过。
- commit 消息丢失后幂等补发并最终达到同一 credential 的 `DEVICE_ONLINE`。
- 正式 claim/status/账户绑定 API 冻结并验收。

量产发布还必须完成 `08-security-and-production-boundaries.md` 中的所有生产安全项。
