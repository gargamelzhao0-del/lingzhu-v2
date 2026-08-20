# 配网模拟器说明

## 定位

模拟器用于在 Android Provider 接入前验证同一业务状态机。Real 模式中的 Python BLE Bridge 相当于桌面版手机 Provider：它调用 Espressif 官方 `esp_prov`，真实连接 ESP32-S3，并调用测试网关 claim/status API。

模拟器证明协议和参考实现可行，但不能证明 Android 蓝牙权限、系统后台限制、具体手机 BLE 栈或 APP 断连恢复已经通过。

## 组成

| 文件 | 作用 |
| --- | --- |
| `index.html` / `style.css` | 页面、状态、Wi-Fi 列表和事件轨迹 |
| `app.js` | Mock/Real 状态机、表单、任务轮询、脱敏导出 |
| `bridge.py` | loopback HTTP、BLE Provider、claim/status 调用 |
| `qr-codec.js` | Nuwax v1 二维码校验和设备锁定 |
| `vendor/jsQR.js` | 离线二维码图片识别 |
| `start-mock.ps1` | 仅使用包内契约启动可携带 Mock 模式 |
| `start-full-stack.ps1` | 测试网关 + Bridge 一键启动 |
| `bridge_logic_test.py` | Bridge 输入、错误和状态逻辑测试 |
| `qr-codec.test.cjs` | 二维码和目标设备筛选测试 |

## Browser 到 Bridge 的本地接口

Bridge 只绑定 `127.0.0.1`，并校验 Host、Origin 和 `application/json`。

| 接口 | 作用 |
| --- | --- |
| `GET /api/health` | BLE/网关准备状态 |
| `GET /api/contract` | 返回机器契约 |
| `POST /api/devices` | 扫描 UUID + `PROV_` 设备 |
| `POST /api/wifi-scan` | 建立 Security 2 后调用设备 `prov-scan` |
| `POST /api/provision` | 启动完整 cloud-config + Wi-Fi + online 任务 |
| `GET /api/jobs/<id>` | 查询事件、里程碑和最终结果 |

Real 配网提交给 Bridge 的本地 JSON：

```json
{
  "serviceName": "PROV_1467C4",
  "username": "NX-3927-2693-9288",
  "pop": "<development-or-per-device-pop>",
  "ssid": "<2.4GHz-ssid>",
  "password": "<wifi-password>"
}
```

该 JSON 只是浏览器到本机 Bridge 的模拟器接口，不是 Android 与固件的 BLE 报文格式。真实 BLE 报文由 `esp_prov` 和 `02-communication-protocol.md` 定义。

## Mock 模式

不连接设备，用于验证页面状态机：成功、Security 2 失败、Wi-Fi 认证失败、SSID 不存在、超时、内部错误、BLE 重连和 300 秒窗口结束。

交付 ZIP 可独立运行 Mock：在 `simulator/` 执行 `.\start-mock.ps1`，打开 `http://127.0.0.1:8765/simulator/`。页面先尝试 Bridge 契约 API；API 不存在时只回退读取包内 `provisioning-contract.json`，Real 控件保持不可用。

Mock 成功不能作为 BLE 或云端真机证据。

## Real 模式

1. 设备通过 `Device > BLE` 进入 300 秒配网窗口。
2. 扫描设备二维码或加载二维码图片，锁定完整广播名和 username。
3. 生产二维码不含 PoP 时，从受控来源输入；开发版使用冻结开发 PoP。
4. 扫描 BLE，只列出同时匹配 Service UUID 和名称的设备。
5. 请求设备 Wi-Fi 扫描，列表来源是 ESP32-S3 `prov-scan`，不是电脑网卡。
6. 选择 SSID、输入密码并开始配网；自动 Wi-Fi 扫描立即停止，等待当前扫描正常结束。
7. Bridge 建立新 Security 2 session，校验 `device-info`，claim 并发送 `cloud-config`。
8. Bridge 调用 `prov-config`，到达 `WIFI_READY` 后断开 BLE。
9. Bridge 查询测试网关；只有 `DEVICE_ONLINE` 才显示 `SUCCESS`。

## 并发规则

设备发现、设备 Wi-Fi 扫描和完整配网共享同一把后端 BLE 操作锁。正在执行的 `prov-scan` 不被强行取消；点击开始后停止新的扫描，当前扫描释放连接后再开始配网，防止两个客户端争用同一个 NimBLE Peripheral。

## 敏感数据处理

- PoP、Wi-Fi 密码和 `deviceSecret` 不写入事件记录或导出记录。
- Bridge 任务结束后清空提交 payload 和云 bundle 对象。
- Bridge 不向页面返回 BLE address/BSSID 或 `deviceSecret`。
- 静态 HTTP 根目录限制为模拟器目录，不能下载工程源码、构建产物或 `sdkconfig`。

## 回归命令

```powershell
python bridge_logic_test.py
node --test qr-codec.test.cjs
python -m py_compile bridge.py bridge_logic_test.py
```

当前基线：Bridge 18 项、二维码 16 项全部通过。
