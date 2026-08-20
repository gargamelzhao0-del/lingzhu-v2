# BLE 配网改造变更清单

> 当前覆盖说明（2026-07-25）：本文记录历次实现过程，包含旧镜像大小、旧测试数量和中间失败结果，不应直接作为当前交付结论。权威状态见 `docs/ble-provisioning-delivery-package/`：最终已烧录镜像 SHA-256 为 `46B85B6769639B7C8B6B5FFC3D66D97B4F5367D2DF0BFF5D3AEA5C6C940CE55A`；本地真实 Provider、设备和测试网关已在同一次流程到达 `WIFI_READY + DEVICE_ONLINE`。

## 2026-07-23 实机回归新增修改

### 最终 Wi-Fi 关联、Security 2 内存和停止延时修复

位置：`main/nuwax_provision_new/nuwax_provision_main.c`、`sdkconfig`、`sdkconfig.defaults`。

修改内容：

- 在 `NETWORK_PROV_SET_WIFI_STA_CONFIG` 回调中将配网 STA 设置为 `WIFI_FAST_SCAN`，显式配置 `WPA3_SAE_PWE_BOTH`，并在 BLE 配网共存期间设置 `disable_wpa3_compatible_mode=1`；现场 WPA2/WPA3 transition AP 因此稳定选择 WPA2 关联路径；
- 关闭 IDF 6.0 的 `CONFIG_MBEDTLS_HARDWARE_SHA` 和 `CONFIG_MBEDTLS_HARDWARE_AES`。PSA 硬件驱动在 SRP6a-3072 阶段申请内部 DMA context 失败；软件 SHA/AES-GCM 使用相同算法和协议报文，不降低 Security 2；
- 保留独立的 30 秒单次凭据观察期和 `network_prov_mgr_reset_wifi_sm_state_on_failure()` 重试逻辑；
- 将 `network_prov_mgr_disable_auto_stop()` 的参数从误用的 `30000` 改为 `1000`。该参数只是显式 stop 后的 transport cleanup delay，不是单次 Wi-Fi 观察期；
- 保留 `GOT_IP` 后 15 秒 APP 状态查询窗口，随后约 1 秒释放 provisioning/BLE 资源，避免成功后再额外阻塞 UI 和业务启动约 30 秒；
- 删除所有一次性 `BENCH-ONLY` 强制进 BLE/直连对照代码，正式逻辑恢复为仅在 ESP-IDF 未保存 Wi-Fi 配置时自动配网。

作用：解决真机 `WIFI_REASON_AUTH_FAIL (202)`、Security 2 内部 DMA 内存失败和 connected 后屏幕状态迟到的问题，同时不改变冻结 UUID、Security 2、endpoint、成功标准或错误码。

### 设备侧 Wi-Fi 扫描列表与自动刷新

位置：`docs/provision_simulator/bridge.py`、`index.html`、`app.js`、`style.css`、`bridge_logic_test.py`。

修改内容：

- Bridge 新增 loopback-only `POST /api/wifi-scan`，仅接收当前 BLE serviceName、Security 2 username 和 PoP；
- 扫描前重新验证 `proto-ver` 的 Security 2 与 `wifi_scan` capability，并通过加密 `device-info.serialNumber` 校验设备身份；
- 直接调用锁定组件 1.2.4 自带的 `esp_prov.scan_wifi_APs("ble", ...)` 和官方 `prov-scan` protobuf，不实现私有扫描协议；
- 扫描结果按 SSID 去重并保留最强 RSSI，返回 SSID、RSSI、信道和认证模式，不向浏览器返回 BSSID；
- 修复 Espressif Python helper 以 latin-1 解码 UTF-8 SSID 导致的中文名称乱码；
- SSID/密码输入框下方新增“设备扫描到的 Wi-Fi”列表和“扫描设备 Wi-Fi”按钮；
- 点击网络条目只填写 SSID、清空旧密码、标记选中项并聚焦密码框，不保存或自动填写密码；
- BLE 配网和设备 Wi-Fi 扫描共用单操作锁，防止两个 Security 2 会话并发控制同一设备；
- 修复扫描与配网的状态竞争：即使当前扫描已发出，“开始真实配网”也接受一次点击，立即取消后续扫描，等待当前官方 `prov-scan` 完成并释放 BLE 后自动开始配网；
- 目标设备和 Security 2 身份就绪 1.2 秒后自动执行首次扫描；每次完整扫描结束 15 秒后串行刷新，不使用可能重叠的 `setInterval`；
- 用户输入 Wi-Fi 密码后暂停下一次自动刷新，避免按钮即将点击时又开始扫描；配网成功或失败后均保持扫描停止，手动刷新或重置后才恢复；设备、二维码、身份、模式改变时停止旧扫描计划；
- 自动刷新保留现有列表、已选择 SSID、已输入密码和焦点，只有用户主动点击网络条目才清空旧密码；
- 将 `CONFIG_NETWORK_PROV_SCAN_MAX_ENTRIES` 从 16 提高到 32，并写入 `sdkconfig.defaults`，降低现场 AP 较多时结果被截断的概率；
- PoP 仅保留在单次内存请求中，请求结束后清空，不写入事件记录和导出数据。

作用：模拟器展示 ESP32-S3 实际可见的 2.4 GHz 网络，而不是电脑无线网卡的扫描结果，交互与 Android Provider 的 `wifi_scan` capability 保持一致。官方 `prov-scan` 是 APP 请求/设备响应，因此“自动刷新”由模拟器或 Android Provider 串行发起，不增加设备私有推送协议。

真机验证：修改前设备返回 16 个可用网络；`xspaceagi` 被设备识别为信道 4、约 -40 dBm、WPA2/WPA3 混合认证。点击该条目后 SSID 输入框为 `xspaceagi`，密码框获得焦点。32 条上限的新固件已完成编译并烧录至 `COM8`。扫描与配网互斥、密码输入停止自动刷新及点击 Start 等待当前扫描释放 BLE 的流程已完成真机验证。当前模拟器审计新增设备发现互斥锁、输入长度校验和静态目录隔离；Bridge 单元测试 10 个、JavaScript 语法检查和二维码测试均通过。

### `sdkconfig.defaults`、`sdkconfig`

完整 UI、Wi-Fi 和 BLE 同时启动时，设备内部 RAM 仅剩约 43 KB，NimBLE 在设置广播数据时返回 `BLE_ERR_MEM_CAPACITY (519)`。为保留 BLE Peripheral 配网能力并降低未使用功能占用，修改为：

- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`；
- 仅保留 NimBLE Peripheral、Broadcaster 和 GATT Server；
- 关闭 Central、Observer、GATT Client、GAP SMP/bonding、BLE 5 扩展、DTM 和未使用示例服务；
- `CONFIG_BT_NIMBLE_ATT_MAX_PREP_ENTRIES=4`；
- `CONFIG_BT_CTRL_BLE_MAX_ACT=2`；
- 保留单连接、ATT MTU 517、NimBLE 外部内存分配和 protocomm Security 2。

作用：修复 `Error setting advertisement data`，同时不改变冻结 Service UUID、广播名、Security 2、protocomm endpoint 或 BLE-only 传输契约。`idf.py reconfigure`、完整构建和 `COM8` 烧录均已通过；最终 binary 大小为 `0x322d30`。

### `docs/provision_simulator/bridge.py`

- 继续使用组件自带 `prov-config` 请求、Security 2 解密和官方 protobuf，但保留 Espressif `esp_prov.get_wifi_config()` 返回字符串时丢弃的 `wifi_fail_reason`；
- `wifi_fail_reason=0` 映射为 `WIFI_AUTH_FAILED`，`wifi_fail_reason=1` 映射为 `NETWORK_NOT_FOUND`，未知值才映射为 `INTERNAL_ERROR`；
- 失败后 `device-info` 读取超时不再覆盖已经取得的官方 Wi-Fi 失败原因；
- 官方状态到达 `connected` 后立即以 `IP_EVENT_STA_GOT_IP` 语义返回 `SUCCESS`，最终 `device-info` 不再是成功前置条件。

作用：模拟器与冻结契约的成功标准和错误映射保持一致，避免 BLE 会话在边界关闭时产生假失败。

### `docs/provision_simulator/bridge_logic_test.py`

新增官方 Wi-Fi 状态 protobuf、输入边界、设备名格式、静态目录隔离的回归测试。Bridge 单元测试总数 10 个，全部通过；`bridge.py` 和测试文件的 Python 语法检查通过。

### 2026-07-23 真机结果

- 二维码图片被本地解析并定向锁定 `PROV_1467C4`；扫描同时满足冻结 Service UUID；
- BLE GATT、`proto-ver`、Security 2、`wifi_prov` capability 和 `device-info.serialNumber` 校验通过；
- 身份一致后才发送 Wi-Fi 凭据；
- 错误密码由官方状态准确返回 `WIFI_AUTH_FAILED`，错误 PoP 返回 `SECURITY_AUTH_FAILED`；
- WPA2/WPA3 transition 关联修复后，正确凭据两次分别在 `14.051 s`、`13.232 s` 返回 `SUCCESS`，设备取得 `192.168.31.3`；
- 固件日志确认 `wifi: init -> auth -> assoc -> run`、`STA Got IP`、主凭据保存和 Wi-Fi 历史保存；
- 保存凭据的正式固件重启后 `192.168.31.3` 可达，模拟器 `/api/devices` 返回空列表，证明正常已配网启动不广播 `PROV_1467C4`。

本文只记录本次 BLE 配网改造实际产生的修改、删除和新增文件。工作区中原先已经存在的音频、会议模式、云端和 UI 未提交修改不属于本次清单，也未在本文重复描述。

## 1. `main/nuwax_provision_new`

### 1.1 删除旧自定义 BLE 协议

删除以下文件：

- `nuwax_ble.c/.h`
- `nuwax_ble_gap.c/.h`
- `nuwax_ble_gatts.c/.h`
- `nuwax_provision.c/.h`
- `nuwax_device_name.c/.h`

删除原因：这些文件实现的是自定义 `FE40~FE47` GATT 服务、手写 JSON/CSV Wi-Fi 协议和独立状态机，与 APP 已冻结的 `network_provisioning + protocomm` 契约不一致。继续保留会让后续开发误用旧服务 UUID、旧特征和旧安全模型。

### 1.2 `nuwax_device_uid.c/.h`

位置：`main/nuwax_provision_new/nuwax_device_uid.c`、`.h`

修改内容：

- 保留 UID 的 NVS 缓存、格式化和二维码前缀能力。
- 适配 ESP-IDF 6.0：移除不存在的 `esp_efuse_get_unique_id()`，改用 `esp_efuse_mac_get_default()` / `esp_read_mac()` 获取稳定 STA MAC。
- 使用 PSA Crypto 的 `psa_hash_compute(PSA_ALG_SHA_256)`，替代 IDF 6.0 工程中不可直接包含的 `mbedtls/sha256.h`。
- 对 MAC 加产品域字符串 `NUWAX7-UID` 后计算 SHA-256，仅使用哈希前部生成可读 UID，不暴露原始 MAC。
- 修正 UID 字符串缓冲区长度，支持 `NX-AABB-CCDD-EEFF` 加结尾 NUL。

作用：为 BLE 用户名、设备信息、云端 `hello.device_id` 提供稳定设备标识；IDF 6.0 下可直接编译。

### 1.3 `nuwax_provision_main.h/.c`

位置：`main/nuwax_provision_new/nuwax_provision_main.h`、`.c`

这是新的官方组件适配层，替代旧的 BLE/GATT 状态机。

#### 初始化和服务配置

- `nuwax_provision_main_init()` 在现有 Wi-Fi driver 初始化后调用官方 `network_prov_mgr_init()`。
- 使用 `network_prov_scheme_ble`，不启用 SoftAP。
- 显式固定 Service UUID：`0000ffff-0000-1000-8000-00805f9b34fb`。
- 设置 APP 信息和官方 capabilities：`wifi_prov`、`wifi_scan`。
- 创建自定义 `device-info` protocomm endpoint。
- 监听 `NETWORK_PROV_EVENT`、`PROTOCOMM_SECURITY_SESSION_EVENT` 和 `IP_EVENT_STA_GOT_IP`。

#### Security 2

- 使用 `NETWORK_PROV_SECURITY_2`。
- 开发模式下使用 Kconfig PoP，并通过 `esp_srp_gen_salt_verifier()` 动态生成 salt/verifier。
- 生产模式下从 `nuwax_factory` NVS 读取 `sec2_salt` 和 `sec2_verifier`，不把 PoP 明文放入固件。
- 生成二维码 payload 的开发接口为 `nuwax_provision_main_qr_payload()`；生产二维码由产线系统生成。

#### `device-info` endpoint

响应为 UTF-8 JSON，包含：

- `protocolVersion`
- `serialNumber`
- `productModel`
- `hardwareVersion`
- `firmwareVersion`
- `capabilities`
- `lastProvisioningOutcome`
- `lastProvisioningReason`

作用：让 APP 在安全会话建立后获取设备信息，并在 BLE 重连后读取最近一次配网结果。

#### 配网生命周期

- `nuwax_provision_main_should_auto_enter()`：查询官方 Wi-Fi provisioned 标志。
- `nuwax_provision_main_start()`：启动 BLE 广播和 Security 2 provisioning。
- `nuwax_provision_main_run(300000)`：最多等待 300 秒的首次广播/配网流程。
- 收到 Wi-Fi 凭据后，30 秒内未获得终态则标记 `TIMEOUT`，并允许重新尝试。
- 每次收到新凭据时清空上一轮结果并重新开始 30 秒观察，避免重试期间继续暴露旧错误。
- 300 秒窗口截止前已经接收的凭据可补足本次 30 秒观察期；失败完成后清除 active-attempt 标记，避免截止瞬间提前拆除正在连接的正确请求，也不会允许无限续期。
- 收到 `IP_EVENT_STA_GOT_IP` 后标记成功，额外保留 BLE 15 秒供 APP 读取状态。
- `nuwax_provision_main_stop()`：停止 provisioning manager 并释放 Security 2 动态内存。

#### 错误映射

- Security 2 凭据不匹配：`AUTH_FAILED`。
- Wi-Fi `AuthError(0)`：`AUTH_FAILED`。
- Wi-Fi `WifiNetworkNotFound(1)`：`NETWORK_NOT_FOUND`。
- 官方失败事件缺少 reason 时：`INTERNAL_ERROR`，不能误报 `NETWORK_NOT_FOUND`。
- 30 秒未完成：`TIMEOUT`。
- 组件、NVS 或安全参数错误：`INTERNAL_ERROR`。
- 官方组件没有原生 DHCP 失败码，`DHCP_FAILED` 保留给后续扩展状态。

## 2. `main/CMakeLists.txt`

位置：`main/CMakeLists.txt`

修改内容：

- 将 `nuwax_provision_new/nuwax_device_uid.c` 加入 `SRCS_COMMON`。
- 将 `nuwax_provision_new/nuwax_provision_main.c` 加入 `SRCS_COMMON`。
- 将 `audio_modes/meeting` 和 `nuwax_provision_new` 加入头文件搜索路径；前者是工作区原有并行修改，本次 BLE 相关的是后者。
- 在 `PRIV_REQUIRES` 增加 `efuse`、`network_provisioning`、`protocomm`。

作用：让新的 UID 和官方 provisioning 适配层真正参与固件构建，并提供所需 ESP-IDF 组件头文件和链接库。

## 3. `main/idf_component.yml`

位置：`main/idf_component.yml`

新增：

```yaml
espressif/network_provisioning: 1.2.4
```

作用：锁定 APP/固件契约要求的官方组件主版本，不允许使用浮动版本导致 protocomm、安全或 BLE UUID 行为漂移。

## 4. `dependencies.lock`

位置：工程根目录 `dependencies.lock`

修改内容：

- 新增 `espressif/network_provisioning` 1.2.4。
- 锁定组件 hash `72d27784e3daf807418a34fb00be136ec50c6db49d989ce981d22e031fc0e7f8`。
- 记录其对 `espressif/cjson` 和 IDF `>=5.1` 的依赖。
- 更新 direct dependencies 和 manifest hash。

组件解析同时将 `espressif/cjson` 从原锁定修订更新到 registry 解析出的兼容修订；这是依赖求解的伴随变化，不是业务代码修改。

作用：保证其他开发机和 CI 使用相同的 network provisioning 源码。

## 5. `main/Kconfig.projbuild`

位置：`main/Kconfig.projbuild` 的 `BLE Wi-Fi provisioning` 菜单。

新增配置：

- `CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE`：默认开启开发 Security 2 PoP 模式。
- `CONFIG_NUWAX_PROVISION_SEC2_DEV_POP`：开发 PoP，默认值为 `Nuwax7-Dev-PoP`。

作用：开发固件可直接和 APP Mock/Provider 联调；生产必须关闭开发模式并改为产线注入 salt/verifier。

## 6. `sdkconfig.defaults`

位置：`sdkconfig.defaults` 末尾 BLE 配置段。

新增配置：

- `CONFIG_BT_ENABLED=y`
- `CONFIG_BT_NIMBLE_ENABLED=y`
- `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`
- `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y`
- `CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y`
- `CONFIG_BT_NIMBLE_GATT_SERVER=y`
- `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`
- `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517`
- Security 2 的首个 SRP6a 公钥 protobuf 报文约 415 字节；517 MTU 避免 Windows BLE 栈因单次 GATT 写入超过 256 MTU 而拒绝请求。
- `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`
- `CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2=y`
- `CONFIG_NETWORK_PROV_BLE_NOTIFY=y`

作用：启用桌搭作为 BLE Peripheral 的最小 NimBLE 能力和 protocomm Security 2。NimBLE Host 动态内存和 mbedTLS/PSA 的 Security 2 临时密码学内存放入设备已有的 8 MB PSRAM，避免完整 UI、Wi-Fi 和音频栈启动后内部 RAM 不足导致 HCI 初始化或 SRP6a SHA-512 会话计算失败；控制器所需内部/DMA内存仍由 ESP-IDF 管理。未启用 BLE bonding 和强制链路加密，避免 Android 系统配对弹窗干扰应用层 Security 2。

当前工作区的生成配置 `sdkconfig` 也已同步出这些符号；它属于本机生成配置，后续应以 `sdkconfig.defaults` 和 `idf.py reconfigure` 结果为准，不应手工复制到其他构建环境。

## 7. `main/wifi_manager.h/.c`

### 7.1 Wi-Fi 状态同步

位置：`main/wifi_manager.c` 的 `IP_EVENT_STA_GOT_IP` 分支。

修改：收到 GOT_IP 时调用 `esp_wifi_get_config()`，同步当前 STA SSID 到应用状态缓存。

作用：官方 provisioning manager 建立连接后，原应用 UI、状态上报和 WebSocket 层能看到正确 SSID。

### 7.2 配置优先级

位置：`main/wifi_manager.c` 的 `nuwax_wifi_connect()`。

修改：启动连接时先读取 ESP-IDF 当前 STA 配置；只要存在已配网 SSID，就优先使用该配置，之后才回退到原有 Kconfig 主/备 SSID。

作用：防止 BLE 配网成功后的凭据在重启时被固定 Kconfig SSID 覆盖。

### 7.3 凭据导入

位置：`main/wifi_manager.h/.c` 的 `nuwax_wifi_import_driver_credentials()`。

修改：新增公开 API，读取官方 provisioning 写入的 STA 配置，并同步保存到应用自身的 Wi-Fi NVS 和历史网络列表。

作用：兼容现有 UI 的已保存网络列表、云端 Wi-Fi 状态和后续手动重连逻辑。

### 7.4 Wi-Fi 历史 NVS 键长度

位置：`main/wifi_manager.c` 的 `WIFI_NVS_KEY_SAVED_PASSWORD_SUFFIX`。

修改：密码键后缀从 `_password` 改为 `_pwd`。原索引键 `saved_<n>_password` 超过 ESP-IDF NVS 15 字符上限，会在配网已成功后返回 `ESP_ERR_NVS_KEY_TOO_LONG`。

作用：配网成功后应用主凭据和 Wi-Fi 历史可以同时持久化；真机日志已确认 `Saved Wi-Fi history SSID: xspaceagi slot=0`。

## 8. `main/nuwax_ai_partner_main.c`

位置：`app_main()` Wi-Fi 初始化之后、fallback watcher 之前。

修改流程：

1. 调用 `nuwax_provision_main_init()`。
2. 未配网时调用 `nuwax_provision_main_start("first_boot")`。
3. 阻塞等待最多 300 秒完成 BLE 配网。
4. 成功后调用 `nuwax_wifi_import_driver_credentials()`。
5. 停止 provisioning manager。
6. 只有在 BLE 配网流程结束后，才启动原有 Wi-Fi fallback watcher。
7. 如果 Wi-Fi 已由 provisioning manager 建立，则跳过重复的 `nuwax_wifi_connect()`。

作用：避免官方 provisioning manager 和原 `wifi_manager` 同时控制 STA、重试和 fallback。

新增头文件：

```c
#include "nuwax_provision_new/nuwax_provision_main.h"
```

## 9. `main/session_protocol.c`

位置：`nuwax_session_send_hello()`。

修改：`hello.device_id` 优先使用 `nuwax_device_uid_string()`；UID 尚未初始化时才回退到 `CONFIG_NUWAX_DEVICE_ID`。

作用：云端设备身份与 BLE 配网/设备信息使用同一设备标识，避免 BLE UID、Wi-Fi 配置和 WebSocket 设备 ID 不一致。

## 10. 本地重新配网 UI 入口

### `main/nuwax_ui_v28/nuwax_focus_menu.c`

- 在 `Device` 详情页的 `Meeting` 开关下方新增 Wi-Fi 图标加 `BLE` 的配网按钮（内部动作名为 `BLE Provision`）。
- 按钮使用独立、受限的触摸命中区域，点击后产生既有 `NUWAX_MENU_ACTION_REPROVISION_WIFI` 动作。
- 缩小 `Meeting` 行的命中范围，避免与新增按钮重叠。

### `main/app_shell.c`

- 移除 Wi-Fi 详情页静止长按 5 秒的旧重新配网入口。
- 保留菜单动作到应用动作的既有映射，由 `Device` 页按钮触发。

### `main/conversation/touch_gesture.c/.h`

- 为 `NUWAX_APP_SHELL_ACTION_REPROVISION_WIFI` 增加独立本地回调，不再等待 WebSocket 主循环消费。
- 其他 UI 动作仍使用原有 Shell 队列，BLE 重新配网动作不会重复入队。

### `main/nuwax_ai_partner_main.c`、`main/nuwax_provision_new/nuwax_provision_main.c`

- 复用既有重新配网执行链：显示状态、调用 `esp_wifi_restore()`、延时 500 ms 后重启。
- UI 和触摸任务在网络连接前启动，因此已有 Wi-Fi 失效或云端断开时，本地按钮仍可执行。
- 重启后官方 provisioned 标志缺失，设备自动启动 BLE + Security 2 配网。
- 日志改为描述 `Device` 菜单入口，不再误报为 Wi-Fi 页长按。

作用：已配网设备可以通过明确的本地物理按钮切换 Wi-Fi；不增加远程清除凭据接口，也不改变 BLE 协议、安全方式和 APP 状态机。

## 11. 新增契约和交付文档

### `main/nuwax_provision_new/README.md`

说明官方组件、Service UUID、广播命名、Security 2、APP 流程、错误映射、生产切换条件和构建验证方式。

### `main/nuwax_provision_new/esp32s3-idf6-provisioning-contract.json`

保存可供 APP/固件共同使用的冻结契约：IDF/组件版本、BLE 参数、Security 2、capabilities、`device-info`、成功标准、错误码和生命周期策略。

### `docs/ble-provisioning-delivery.md`

提供 Android Provider 真机联调顺序、开发 PoP、生产切换要求、产线 factory NVS 数据和当前验证边界。

### `docs/ble-provisioning-architecture.md`

说明固件模块边界、官方 endpoint 时序、Security 2 生命周期、错误映射、BLE 成功/失败策略、重新配网入口和生产检查项。

### `docs/esp32s3-ble-app-firmware-handoff.md`

作为 APP/固件完整对接单，冻结软件工程师要求的全部协议参数，并记录 2026-07-22 固件侧真机预验收结论和 Android 待验收边界。

### `docs/esp32s3-ble-first-integration-handoff.md`

作为可直接提交给 Android 工程师的首轮联调简版，给出 Provider 输入、调用顺序、错误映射、生命周期、构建产物和验收矩阵。

### `docs/ble-provisioning-change-inventory.md`

即本文，逐文件说明 BLE/配网相关修改位置、内容、作用、验证结果和剩余事项。

## 12. `docs/provision_simulator`

### `index.html`、`style.css`

- 提供同一页面内的 Mock 协议模拟和真实 ESP32-S3 两种模式。
- 展示冻结契约、Provider 执行链、事件记录、断言和脱敏联调输入。

### `app.js`

- Mock 模式覆盖成功、Security 2 失败、Wi-Fi 认证失败、SSID 不存在、超时、内部错误、断线重连和 300 秒窗口结束。
- 真实模式通过本机 bridge 扫描、提交一次性凭据并轮询任务；导出记录不包含 PoP 和 Wi-Fi 密码。
- 设备扫描、设备侧 Wi-Fi 扫描和配网在前端统一互斥；扫描期间配网按钮不可用，配网期间不能发起新的设备发现。
- 生产身份二维码允许不携带 PoP；此时锁定设备名和 Username，但保留 PoP 输入框，供受控产线凭据补录后继续 Security 2。

### `bridge.py`

- 直接复用工程锁定的 `network_provisioning` 1.2.4 `esp_prov`、protobuf 和 protocomm Security 2 实现，不自行仿造 GATT 协议。
- 仅绑定 loopback，不创建 BLE bond；敏感字段只保存在单次任务内存并在结束时清空。
- 静态 HTTP 根目录隔离到 `docs/provision_simulator`，通过 `/api/contract` 提供契约；不会从 Bridge 读取或暴露仓库 `sdkconfig`、源码、构建产物和运行日志。
- 设备发现、设备 Wi-Fi 扫描和配网共用后端 BLE 操作锁；发现接口不再向浏览器返回 BLE address。
- 对 serviceName、Username、PoP、SSID 和密码执行协议长度/格式校验，并限制已完成任务保留数量，避免本地误操作造成资源无限增长。
- 扫描同时匹配 `PROV_` 前缀与冻结 Service UUID。
- 将 Security 2 建链异常脱敏映射为 `SECURITY_AUTH_FAILED`，避免泄漏握手材料或误报 `INTERNAL_ERROR`。

### `README.md`、`requirements.txt`、`start-bridge.ps1`

说明启动、依赖、真实设备准备步骤和安全边界，并提供本机 bridge 启动脚本。

## 13. `.gitignore`

- 只对白名单中的 BLE 对接文档和 `docs/provision_simulator` 源文件解除 `docs/*` 忽略规则。
- 作用：代码提交时携带交付文档和模拟器，同时继续排除其他历史工作记录与生成文件。

## 14. 验证结果和未完成项

已完成：

- `idf.py reconfigure` 成功。
- ESP-IDF 6.0/NimBLE/network provisioning 完整编译成功。
- `build/NuwaxAIPartner.bin` 生成成功。
- 当前固件已烧录至 ESP32-S3，并完成启动验证。
- 契约 JSON 可解析。
- 未发现 Wi-Fi 密码、PoP、salt、verifier 的日志输出。
- 2026-07-22 真机确认广播名 `PROV_1467C4`、冻结 Service UUID、实际 ATT MTU 517。
- 本地模拟器与真机完成 Security 2、`proto-ver`、capabilities 和 `device-info` 验证。
- 不存在 SSID 返回 `NETWORK_NOT_FOUND`，BLE 在 300 秒总窗口内保持可重试。
- 正确 2.4 GHz Wi-Fi 两次分别在 `14.051 s`、`13.232 s` 返回官方 `SUCCESS`。
- `GOT_IP` 后 BLE 查询窗口实测为 15 秒，随后停止广播。
- 网页 Mock 模式的 8 个状态机场景全部通过：`SUCCESS`、`SECURITY_AUTH_FAILED`、`WIFI_AUTH_FAILED`、`NETWORK_NOT_FOUND`、`TIMEOUT`、`INTERNAL_ERROR`、断线重连成功和 300 秒窗口结束。
- 网页真实模式仅列出同时匹配 `PROV_` 广播名前缀和冻结 Service UUID 的设备，扫描条件与 Android Provider 契约一致。
- 2026-07-22 重试状态和未知 reason 映射修正后，ESP-IDF 6.0 增量编译成功并重新烧录至 `COM8`。
- 错误 PoP 真机返回 `SECURITY_AUTH_FAILED`；错误 Wi-Fi 密码真机返回 `WIFI_AUTH_FAILED`。
- `WIFI_AUTH_FAILED` 后设备继续广播并允许在同一窗口重新提交。
- `Device > BLE` 按钮已真机验证触摸、清除 Wi-Fi、重启和恢复 `PROV_1467C4` 广播。
- 正确凭据最终返回 `SUCCESS`；成功保留窗口内可发现 BLE，15 秒后停止广播。
- 串口诊断发现 300 秒截止与正在连接的凭据存在竞争；active-attempt grace 修复已编译并烧录。
- IDF 6.0 PSA 硬件 SHA/AES DMA 内存问题、transition AP 关联失败、NVS 历史键超长和成功后额外 30 秒 cleanup 阻塞均已修复。
- 最终应用大小 `0x322d30`，应用分区剩余约 34%；证据日志为 `runtime-logs/nuwax7-ble-final-credential-test-20260723-172305.log` 和 `runtime-logs/nuwax7-ble-history-persist-retry-20260723-173653.log`。

尚未完成：

- Android Provider Security 2 真机握手。
- 真实 BLE 断连重连和 300 秒边界 active-attempt grace 专项注入。
- 连续 20 次配网稳定性测试。
- 生产 factory NVS 写入工具和 Secure Boot/Flash Encryption/NVS Encryption 配置。

当前开发固件的 `CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE` 必须视为联调配置，不得直接作为生产版本发布。

## 15. Flash/NVS 操作移入内部 RAM 任务

位置：`main/nuwax_ai_partner_main.c`。

最终真机调试确认两个调用路径可能从 PSRAM 栈任务直接进入 Flash/NVS：

- Device 菜单触摸任务保存重新配网回滚记录。
- WebSocket RX 任务在鉴权 hello 后把云配置从 pending 提升为 active。

ESP-IDF 写 Flash 时会暂时关闭 cache，位于 PSRAM 的任务栈不能在该窗口继续使用，因此这两条路径可能触发 cache assertion。修复内容：

- 新增单个 4 KB 内部 RAM 栈任务 `nuwax_flash_ops`，优先级 6。
- 使用 `NUWAX_FLASH_OP_REPROVISION` 和 `NUWAX_FLASH_OP_CLOUD_COMMIT` notification bit 串行化两类持久化动作。
- Device 菜单回调仅排队重新配网，不再在触摸任务内写 NVS；重复点击做去抖。
- WebSocket RX 任务仅排队 active 提交，不再在 RX 任务内写 NVS。
- 主任务只在 active 已成功持久化后发送 `cloud_config_committed`，保证网关不会提前把 onboarding 判定为成功。

作用：消除 PSRAM 任务栈在 Flash cache 关闭期间访问外部内存的断言风险，同时保持云配置 pending/active 事务语义和重新配网回滚语义。

## 16. 2026-07-25 最终本地三端验收

最终镜像：

- 大小：`3,302,976` bytes（`0x326640`）。
- SHA-256：`46B85B6769639B7C8B6B5FFC3D66D97B4F5367D2DF0BFF5D3AEA5C6C940CE55A`。
- 仅写入 `COM8` 应用分区 `0x420000`；未擦除 NVS、bootloader、分区表、模型或 storage。

同一次真实流程结果：

- `PROV_1467C4` 扫描、GATT、Security 2、`device-info` 和签名 `cloud-config` 均通过。
- `prov-config` set/apply 后 14.3 秒到达 `IP_EVENT_STA_GOT_IP`，设备 IP 为 `192.168.31.3`。
- 33.6 秒到达 `DEVICE_ONLINE`，模拟器最终结果为 `SUCCESS`。
- 测试网关确认设备 `NX-3927-2693-9288` 的新 credential 为 active，pending 为空。
- 串口确认 `nuwax_flash_ops` 在内部 RAM 栈完成云配置提升，`cloud_config_committed_ack` 返回，后续 WebSocket ping/pong 正常，无新 assertion 或 panic。

权威记录：`docs/ble-provisioning-delivery-package/10-final-device-acceptance.md`。仍未完成 Android Provider 联合验收、连续 20 次稳定性、300 秒边界故障注入和量产安全项，因此当前状态为 `FIRMWARE_LOCAL_E2E_VERIFIED`，不是 `INTEGRATION_VERIFIED` 或生产就绪。
