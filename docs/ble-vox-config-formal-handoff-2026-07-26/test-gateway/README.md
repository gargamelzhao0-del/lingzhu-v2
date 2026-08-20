# 最小测试网关

此网关只用于局域网三端验收，不是正式云端，也不提供 APP 绑定接口。

它实现当前 `vox-config` 协议需要验证的四项能力：

- `ws://<局域网IP>:<端口>/api/device/ws` 设备 WebSocket。
- 使用运行时 `deviceSecret` 校验 `X-Bridge-Token` 和 `hello.device_secret`。
- `GET /api/status` 返回当前在线设备，不返回密钥。
- `POST /api/device/command` 转发 `start_ble_provisioning`。

不包含 `cloud-config`、签名 bundle、claim/onboarding、音频服务或正式业务逻辑。
网关零 npm 依赖，使用 Node.js 20 或更高版本直接运行。通常应从
`../simulator/start-full-stack.ps1` 启动，由脚本安全读取测试密钥并设置环境变量。

单独运行：

```powershell
$env:CLOUD_GATEWAY_HOST = "0.0.0.0"
$env:CLOUD_GATEWAY_PORT = "8800"
$env:CLOUD_GATEWAY_TOKEN = Read-Host "Device Secret"
node cloud-gateway.js
```

单元测试：

```powershell
npm test
```

不要使用真实生产密钥启动共享测试网关，也不要把密钥写入脚本、日志或文档。
