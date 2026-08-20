# BLE 配网模拟器说明

## 组成

- `index.html`、`style.css`、`app.js`：Mock/Real 页面和状态机。
- `qr-codec.js`：Nuwax v1 二维码严格解析。
- `vendor/jsQR.js`：离线摄像头/图片二维码解析备用实现。
- `bridge.py`：浏览器到 Windows BLE 的本机桥接。
- `start-full-stack.ps1`：真实全流程入口，同时启动测试网关和 BLE Bridge。
- `start-bridge.ps1`、`requirements.txt`：仅启动 Bridge 和已验证的精确依赖版本；未配置网关时 Real 全流程不可用。
- `bridge_logic_test.py`、`qr-codec.test.cjs`：协议、输入、安全和二维码测试。

## 运行条件

Real 模式必须位于完整固件仓库内，且工程至少构建过一次。Bridge 会从 `build/project_description.json` 取得 ESP-IDF 路径，并直接加载该工程 `managed_components/espressif__network_provisioning` 1.2.4 的 `esp_prov`/protobuf；这样不会用自制协议模拟官方 GATT。

```powershell
cd docs\ble-provisioning-delivery-package\simulator
.\start-full-stack.ps1
```

打开 `http://127.0.0.1:8765/simulator/`。若端口被占用，可手动执行：

```powershell
python bridge.py --host 127.0.0.1 --port 8766
```

## Mock 模式

覆盖 `SUCCESS`、`SECURITY_AUTH_FAILED`、`WIFI_AUTH_FAILED`、`NETWORK_NOT_FOUND`、`TIMEOUT`、`INTERNAL_ERROR`、BLE 断线重连和 300 秒窗口结束。它验证页面状态机和错误展示，不证明真实 BLE 可用。

## Real 模式

推荐顺序：扫描设备二维码、等待精确广播名、Security 2、`device-info` 身份校验、设备 Wi-Fi 扫描、选择 SSID、输入密码、开始配网。Bridge 随后模拟 APP 调用测试云 claim API、通过 `cloud-config` 下发签名配置，再执行官方 `prov-config`。页面只有在 `WIFI_READY` 和 `DEVICE_ONLINE` 都完成后才显示成功；`CLOUD_CONFIG_COMMIT_PENDING` 表示已鉴权但设备持久化回执尚未到达，页面继续等待。二维码不含 PoP 时允许从受控凭据补录；没有 PoP 仍不能开始 Security 2。

设备 Wi-Fi 列表来自 ESP32-S3 官方 `prov-scan`，不是电脑扫描结果。前端在锁定设备后自动请求，完成后约 15 秒再刷新；输入密码或开始配网后停止自动扫描。扫描和配网共用一把 BLE 操作锁。

## 本机安全边界

- Bridge 只允许 `127.0.0.1`/`localhost`，校验 Host、Origin 和 JSON Content-Type。
- 静态服务只暴露 `simulator/`；契约通过 `/api/contract` 提供。
- `/api/devices` 使用同源 POST，避免其他网页用 GET 触发 BLE 扫描。
- PoP、Wi-Fi 密码和云端明文 `device_secret` 仅存在于单次操作内存，任务响应和导出记录不包含它们；Python 不保证字符串可被物理内存安全擦除，因此测试机仍应按敏感环境管理。
- BLE 地址不返回给前端；永久身份只使用 `device-info.serialNumber`。
- 任务最多保留 64 个终态记录；Wi-Fi BLE 阶段和云在线等待的总保护时间为 240 秒。
- 测试网关的签名私钥和 `ws://` 明文链路仅用于局域网首轮联调，禁止部署到生产环境。

## 测试

```powershell
python -m py_compile bridge.py bridge_logic_test.py
python -m unittest bridge_logic_test.py
node --check app.js
node --test qr-codec.test.cjs
```

模拟器不是 Android 发布验收替代品；Android Provider 真机结果才是 APP 发布依据。
