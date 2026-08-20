# 固件协议参考源码

本目录用于 APP 工程师核对设备端实际行为，不是可独立编译的 ESP-IDF
工程。可执行固件的唯一源码仍位于主工程 `main/`，本目录是打包时的
同步快照。

| 文件 | 作用 |
| --- | --- |
| `nuwax_provision_main.c/.h` | BLE provisioning manager、Security 2、`device-info`、`vox-config`、Wi-Fi 状态和可回滚重新配网 |
| `nuwax_cloud_config.c/.h` | 校验并原子保存 `{deviceId,gatewayUrl,deviceSecret}` |
| `nuwax_device_uid.c/.h` | 生成稳定硬件 UID，作为 `device-info.serialNumber` 和云端 `device_id` |
| `nuwax_provision_usb_control.c/.h` | 联调版物理 USB Serial/JTAG 入口；只接受冻结命令，不实现另一套配网状态机 |
| `ws_transport.c/.h` | 向动态 URL 连接，并在 WebSocket Upgrade 中发送 `X-Bridge-Token` |
| `session_protocol.c/.h` | 发送 hello v1，包含硬件 `device_id`、当前 `device_secret` 和 `remote_ble_reprovision` capability |
| `session_dispatch.c/.h` | 验证并分发 `start_ble_provisioning` 设备命令 |

主工程粘合点：

- `main/CMakeLists.txt` 已编译 `nuwax_provision_usb_control.c`。
- `main/Kconfig.projbuild` 使用 `CONFIG_NUWAX_PROVISION_USB_CONTROL_ENABLE`
  控制联调 USB 入口。
- `main/nuwax_ai_partner_main.c` 把设备菜单、鉴权后的云端命令和物理 USB
  命令全部转发到同一个可回滚重新配网 worker。
- 联调镜像启用 USB 控制；量产构建必须关闭。生产 APP 远程入口必须
  经正式后端的用户鉴权、设备所有权校验、限频和审计。

APP 工程师不应根据这些 C 文件推导 characteristic UUID；自定义 endpoint
的 characteristic 由 provisioning manager 动态分配，APP 必须从 `0x2901`
descriptor 按 `device-info` / `vox-config` 名称发现。
