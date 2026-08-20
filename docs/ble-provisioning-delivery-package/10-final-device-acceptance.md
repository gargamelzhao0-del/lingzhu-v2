# 本地三端真机验收记录

验收日期：2026-07-25。结论：ESP32-S3 固件、本地真实 Provider 模拟器和测试网关已完成一次端到端成功验收，可以交给 Android 工程师开始首轮 Provider 联调。本记录不等同于 Android 联调通过或量产验收。

## 验收基线

| 项目 | 结果 |
| --- | --- |
| ESP-IDF | `v6.0`，commit `662a3be354759d9487bf4b1a629fadb766cb1800` |
| network_provisioning | `1.2.4`，commit `2de4980640bbe3d2d69473d7251640039e185b92` |
| 最终应用镜像 | `3,302,976` bytes |
| SHA-256 | `46B85B6769639B7C8B6B5FFC3D66D97B4F5367D2DF0BFF5D3AEA5C6C940CE55A` |
| 烧录范围 | `COM8` 应用分区 `0x420000`；未擦除 NVS、bootloader、分区表、模型或 storage |
| 设备 | `NX-3927-2693-9288` / `PROV_1467C4` |
| BLE | NimBLE；Service UUID `0000ffff-0000-1000-8000-00805f9b34fb` |
| 安全 | protocomm Security 2，SRP6a + AES-GCM，无 Security 1 降级 |
| 测试 Wi-Fi | `xspaceagi`，2.4 GHz；密码不记录 |
| 设备 IP | `192.168.31.3` |
| 测试网关 | `http://192.168.31.180:8800` / `ws://192.168.31.180:8800/api/device/ws` |

## 同一次操作的时序

| 时间 | 阶段 | 结果 |
| --- | --- | --- |
| 0.0 s | BLE 扫描 | 找到 `PROV_1467C4`，RSSI -57 dBm |
| 7.1 s | GATT / proto-ver | 已连接，确认 Security 2、`wifi_prov`、`wifi_scan`、`cloud_prov` |
| 7.7 s | Security 2 | SRP6a + AES-GCM 会话建立 |
| 7.9 s | `device-info` | 返回并匹配 `NX-3927-2693-9288` |
| 8.5 s | `cloud-config` | 签名云配置写入 pending |
| 8.7 s | `prov-config` set | Wi-Fi 凭据被官方 endpoint 接受 |
| 9.0 s | `prov-config` apply | 开始连接 Wi-Fi |
| 14.3 s | `WIFI_READY` | 官方状态 `connected`，设备收到 `IP_EVENT_STA_GOT_IP` |
| 33.6 s | `DEVICE_ONLINE` | 测试网关确认逐设备鉴权和配置提交，模拟器最终 `SUCCESS` |

## 固件和网关证据

- 设备将新 Wi-Fi 保存至主凭据和历史槽位，并清除重新配网回滚记录。
- BLE 在成功查询保留期后停止广播并释放 BLE 内存。
- WebSocket 连接使用 APP 动态下发并经签名校验的 URL、credential ID 和 `device_secret`；固件未使用写死的云端密钥。
- 鉴权 hello 后，内部 RAM 的 `nuwax_flash_ops` 任务把云配置从 pending 提升为 active；主任务随后发送 `cloud_config_committed`，网关返回确认。
- 测试网关注册表最终为 active，pending 为空；串口后续持续出现 ping/pong，未发现新的 assertion、panic 或重启。
- 原始验收日志位于工作区 `runtime-logs/nuwax7-final-e2e-20260725-123804.log`，不包含在对外交付包中。

## 仍未通过的发布门槛

- Android Provider 的 BLE 权限、Security 2、断连恢复、前后台切换和完整状态机尚未联合验收。
- 尚未完成连续 20 次成功/失败/重配网稳定性测试及 300 秒窗口边界故障注入。
- pending/active 的断电、NVS 写入失败、错误签名、旧版本包和 401 拒绝路径仍需补充真机故障测试。
- 生产逐设备 PoP、factory NVS、正式账户绑定/解绑/吊销、Secure Boot、Flash Encryption 和 NVS Encryption 尚未完成。

因此当前准确状态是 `FIRMWARE_LOCAL_E2E_VERIFIED`：可提交 Android 首轮联调，但不得宣称 `INTEGRATION_VERIFIED` 或生产就绪。
