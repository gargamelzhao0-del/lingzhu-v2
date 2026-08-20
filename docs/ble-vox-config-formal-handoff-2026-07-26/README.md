# ESP32-S3 BLE 配网与动态云端绑定交接包

版本：2026-07-26，协议 `1.3-dynamic-vox-config`。

本包是当前可用于 Android 首轮真机联调的唯一交接基线。旧包中的
`cloud-config`、`credentialId`、`bundleVersion`、`signature`、ECDSA 签名
bundle 和 claim/status 提交流程均已废弃，不得继续实现。

## 当前冻结结论

- ESP32-S3，ESP-IDF `v6.0`，`espressif/network_provisioning` `1.2.4`。
- BLE Host 为 NimBLE，只支持 BLE，不支持 SoftAP fallback。
- Security 2，禁止降级 Security 1/0。
- 产品扩展 endpoint 为 `device-info`、`vox-config`。
- APP 通过 `vox-config` 下发动态 `{deviceId,gatewayUrl,deviceSecret}`。
- `deviceSecret` 来自现有 `bind`；`gatewayUrl` 来自 APP 环境或后端配置，
  当前已观察的 `bind` 响应不包含 `gatewayUrl`。
- Wi-Fi 使用官方 `prov-scan`、`prov-config set/apply/status`。
- `WIFI_READY` 是配网成功；正式云端在线是独立结果。
- 正式服务 `wss://testagent.xspaceagi.com/api/device/ws` 已由真机连接并收到
  多次应用层 `pong`。
- 设备密钥没有写入本包、源码或示例，必须使用 bind 返回的最新值。

## APP 工程师阅读顺序

1. `01-冻结协议与APP流程.md`
2. `02-动态云端配置与正式服务.md`
3. `03-远程进入配网模式.md`
4. `04-真机验收与已知边界.md`
5. `provisioning-contract.json`
6. `simulator/README.md`
7. `06-最终代码审计结论.md`

## 模拟器主机前置条件

- Windows 主机和可用 BLE 适配器。
- ESP-IDF `v6.0`，解压独立包后先设置
  `$env:IDF_PATH = "C:\esp\v6.0\esp-idf"`（按实际安装位置修改）。
- Python 3.10+、Node.js 20+。
- 正常在线状态不持续广播。设备可通过本地菜单进入 BLE 配网；联调固件还
  支持模拟器经物理 USB Serial/JTAG 触发，生产发布必须关闭该 USB 入口。

## 目录说明

- `provisioning-contract.json`：机器可读冻结契约。
- `simulator/`：当前真机模拟器完整源码，不是旧版签名模拟器。
- `test-gateway/`：仅供局域网三端联调和远程配网按钮使用的测试网关。
- `firmware-reference/`：与 APP 协议直接相关的固件参考源码，包括联调
  USB 控制模块。
- `05-文件清单与复核命令.md`：交付文件及复核方式。
- `06-最终代码审计结论.md`：最终可交付判断、修正项和剩余边界。

## APP 当前必须实现

- QR/手动身份输入、BLE 定向扫描与 `PROV_` 名称校验。
- Security 2 会话和 `device-info.serialNumber` 二次身份核对。
- 使用已验证的 `deviceId` 调用 `bind`，取得最新 `deviceSecret`，并与
  当前环境 `gatewayUrl` 组合；任一来源失败时不得下发 Wi-Fi。
- 解析 `proto-ver.prov` 与 `proto-ver.nuwax` 两个 capability 位置。
- 通过 endpoint 名称发送 `vox-config`，不得硬编码 characteristic UUID。
- 设备 Wi-Fi 扫描列表、扫描与配网互斥、开始配网前停止自动扫描。
- Wi-Fi 官方状态与云端在线状态分开呈现。
- 敏感数据不得记录到日志、埋点、崩溃报告或导出记录。

## 当前不属于已完成范围

- Android Provider 真机联合验收和多机型兼容。
- 逐设备量产 PoP、NVS Encryption、Flash Encryption、Secure Boot。
- 正式服务端“远程进入配网模式”的用户鉴权/设备所有权 API。
- 20 次以上重复配网、长时间稳定性和故障注入。
