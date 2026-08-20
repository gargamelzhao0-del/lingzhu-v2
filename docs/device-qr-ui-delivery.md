# 设备二维码 UI 与 APP 对接交付说明

更新时间：2026-07-22

状态：代码已实现、通过 ESP-IDF 6.0 完整编译并烧录至 `COM8`；真机 UI 视觉和手机扫码结果待人工点开确认。

## 1. 交付目标

在圆屏 UI 的 `Device` 详情页增加 `Device QR` 入口。用户点击后，设备全屏显示与现有 BLE 配网契约一致的二维码，供 APP 扫描设备身份和首轮联调配网参数。原 `BLE Provision` 操作保留，并缩写为同一行的 `BLE` 按钮。

操作路径：

1. 主界面右滑进入菜单。
2. 切换到 `Device`，点击进入详情。
3. 点击 `Device QR` 显示二维码。
4. APP 扫码并解析 Nuwax v1 provisioning payload。
5. 在二维码页右滑返回 `Device` 详情。
6. 已配网设备需要更换 Wi-Fi 时，点击 `BLE`，设备清除 Wi-Fi 配置并重启进入 300 秒 BLE 配网窗口。

## 2. 二维码内容

屏幕二维码直接使用 `nuwax_provision_main_qr_payload()`。开发联调格式为：

```text
https://nuwax.com/lz.html?n=PROV_A1B2C3&u=NX-A1B2-C3D4-E5F6&p=Nuwax7-Dev-PoP
```

查询参数 `n` 是 BLE 广播名，`u` 是设备 username，`p` 是开发模式 Security 2 PoP。三个值均使用 URL 百分号编码。量产固件不保存明文 PoP，因此屏幕 URL 只包含 `n` 和 `u`。

APP 扫码后仍必须同时使用冻结 Service UUID 和参数 `n` 发现设备，并在 Security 2 建立后读取 `device-info`，确认 `serialNumber` 与参数 `u` 一致。BLE address 不能作为设备 ID。

## 3. UI 行为

- 二维码视图尺寸为 `232 x 232`，启用四模块 quiet zone，使用黑白高对比配色。
- 二维码页显示设备 UID，便于人工核对。
- 二维码页右滑先返回 `Device` 详情，不会直接退出整个菜单。
- 点击 `BLE` 的原有重配网行为不变：清除 ESP-IDF Wi-Fi 配置、重启并恢复 `PROV_` 广播。
- 二维码生成失败或 provisioning manager 尚未准备好时显示不可用状态，不输出二维码原文到日志。

## 4. 修改位置、内容与作用

### `main/nuwax_ui_v28/menu/nuwax_device_qr_view.c/.h`

新增全屏二维码视图，负责 LVGL QRCode 创建、payload 更新、设备 ID 显示、显示/隐藏和返回状态。作用是把二维码绘制和 Focus Menu 导航解耦，避免继续扩大菜单核心文件的绘制职责。

### `main/nuwax_ui_v28/nuwax_focus_menu.c`

- 在 `Device` 详情页新增 `Device QR` 按钮。
- 将原全宽 `BLE Provision` 按钮调整为并排 `BLE` 按钮，业务动作保持不变。
- 接入二维码视图的打开、关闭、右滑返回、菜单关闭和状态刷新保护。
- 二维码内容从 provisioning 模块和设备 UID 模块读取，不复制配置常量。

### `main/nuwax_provision_new/nuwax_provision_main.c`

- 新增统一的 `build_qr_payload()`。
- 在 provisioning manager 初始化完成、Security 2 参数准备时生成同一份 payload。
- 修复已配网正常启动时未进入 `prepare_security2()` 导致二维码 payload 为空的问题。
- 开发模式包含开发 PoP；生产模式明确不包含 PoP。

### `main/CMakeLists.txt`

将 `nuwax_device_qr_view.c` 加入 LVGL UI 源文件列表，确保进入固件构建。

### `sdkconfig.defaults` 与 `sdkconfig`

启用 `CONFIG_LV_USE_QRCODE=y`，使用工程内 LVGL 9.5 自带 QRCode/qrcodegen，不增加第三方协议栈或 APP 私有编码器。

### `main/nuwax_provision_new/esp32s3-idf6-provisioning-contract.json`

记录屏幕二维码入口、开发与生产模式差异、APP 扫码后的身份核对要求和返回行为。

## 5. 生产安全边界

当前开发固件使用共享开发 PoP，所以首轮联调 APP 可以只扫描屏幕二维码完成 Security 2。该行为不能直接用于生产。

生产固件只保存每设备 SRP6a salt/verifier，不保存明文 PoP，因此屏幕二维码不会包含生产 PoP。正式产品必须在以下方案中冻结一种后才能宣称“生产 APP 只扫设备屏幕即可绑定和配网”：

1. 保持当前每设备 PoP 方案，由受控产线标签提供 PoP；屏幕二维码用于设备识别。
2. 设计短期、一次性临时 PoP，在用户物理打开二维码页时生成，并同步更新本次 Security 2 会话参数；该方案需要 APP/固件协议共同升级和专项安全评审。

不得把生产 PoP、salt、verifier、Wi-Fi 密码、设备私钥或云端令牌写入串口日志、`device-info` 或普通云端接口。

## 6. 验证记录

- ESP-IDF：`v6.0 / 662a3be354759d9487bf4b1a629fadb766cb1800`
- LVGL：`9.5.0`
- `CONFIG_LV_USE_QRCODE=y`：已进入构建配置。
- 完整构建：通过。
- `lv_qrcode_update`、`nuwax_device_qr_view_show`：已进入最终 ELF。
- 应用大小：`0x332eb0`。
- 最小应用分区剩余：`0x18d150`，约 33%。
- `COM8` 烧录：成功，所有写入区段通过哈希校验，未擦除 NVS 分区。
- 启动后串口捕获到约 61 秒的业务 keepalive 和云端 `pong`，未发生 QRCode/UI 初始化崩溃。

真机仍需确认：按钮命中区域、圆屏无遮挡、手机相机/Android APP 解码、扫码后 Service UUID + `PROV_` 双条件发现、Security 2 和 `device-info.serialNumber` 一致性。
