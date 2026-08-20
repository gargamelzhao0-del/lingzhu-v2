# 首轮 Android 真机联调操作单

## 准备

- Android 支持 BLE 扫描/连接所需运行时权限和系统蓝牙。
- 设备与 2.4 GHz AP 在同一测试区域；ESP32-S3 不支持 5 GHz。
- Provider 使用 `05-provisioning-contract.json` 的冻结值。
- 开发设备可使用契约中的开发 PoP；不得将其写入生产配置。

当前测试设备：`NX-3927-2693-9288` / `PROV_1467C4`。设备若已联网，先在 `Device` 详情页打开 `Device QR` 扫码，返回后点击 `Meeting` 下方带 Wi-Fi 图标的 `BLE` 按钮。设备会保存当前 STA 配置作为回滚记录，然后重启进入 300 秒窗口；新配网失败或超时时恢复原网络。

## 正常流程

1. APP 解析二维码，严格校验 v1、BLE、Wi-Fi、Security 2、`PROV_[0-9A-F]{6}` 和 username。
2. 扫描 UUID + 完整广播名；二维码设备未出现时提示用户点击设备 BLE 按钮，不得替换成附近其他设备。
3. 连接后先读 `proto-ver`，再建 Security 2。
4. 读 `device-info` 并校验 UID。
5. APP 使用登录态向云端 claim API 领取同一 UID 的签名云配置包，通过 `cloud-config` 下发并确认 `pending`。
6. 请求 `prov-scan`；显示 SSID、RSSI、信道、认证类型，不显示或保存 BSSID。
7. 用户选择 SSID、输入密码；点击开始后停止自动扫描，等待正在执行的扫描正常结束。
8. 调用 `prov-config` set/apply/status；35 秒内看到 `connected`，进入 `WIFI_READY`。
9. 断开 BLE，通过云端状态 API 等待同一 credential 的 `DEVICE_ONLINE`。`CLOUD_CONFIG_COMMIT_PENDING` 表示设备已经通过云鉴权但尚未回执 NVS 提交，必须继续等待；只有 `WIFI_READY + DEVICE_ONLINE` 才显示最终成功。

## 必测失败

| 用例 | 预期 |
| --- | --- |
| 错误 PoP | `SECURITY_AUTH_FAILED`，不发送 Wi-Fi 凭据 |
| 二维码 UID 与 `device-info` 不同 | `DEVICE_ID_MISMATCH`，不发送 Wi-Fi 凭据 |
| 云配置签名或版本错误 | `CLOUD_SIGNATURE_INVALID` / `CLOUD_CONFIG_ROLLBACK`，不发送 Wi-Fi 凭据 |
| 错误 Wi-Fi 密码 | `WIFI_AUTH_FAILED`，窗口内可重试 |
| 不存在 SSID | `NETWORK_NOT_FOUND`，窗口内可重试 |
| BLE 中断 | 重新发现同一二维码目标，重新建立 Security 2，不复用旧 session |
| 300 秒结束 | 停止自动重试，提示用户重新从设备菜单进入配网 |
| 扫描中点击开始 | 不并发；等待扫描释放后自动开始配网 |
| Wi-Fi 已连接但云端拒绝或未上线 | `CLOUD_CREDENTIAL_REJECTED` / `CLOUD_CONNECT_TIMEOUT` |

## 联调记录

每次记录：固件 SHA-256、APP commit、Android 机型/系统、设备 ID、广播名、AP 频段/认证方式、结果码、耗时、是否重连。不得记录 PoP、Wi-Fi 密码、salt、verifier 或 Security 2 原始报文。
