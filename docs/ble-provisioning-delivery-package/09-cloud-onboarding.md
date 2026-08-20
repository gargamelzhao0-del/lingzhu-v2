# 云配置下发与逐设备鉴权

## 目标和边界

固件不再编译云端 WebSocket URL、共享网关 Token 或 `device_secret`。APP 在 BLE Security 2 会话内下发云端签名配置，设备获得 Wi-Fi 后使用逐设备密钥连接网关。PoP 不能删除：PoP 认证 APP 到近端设备，`device_secret` 认证设备到云端，APP 登录 token/claim 权限认证用户账户，三个安全域互不替代。

## APP 必须执行的顺序

1. 扫描二维码并锁定完整 BLE 广播名。
2. 建立 Security 2，读取 `device-info`，校验 `serialNumber`。
3. APP 携带用户登录态向正式云端 claim API 请求该 `deviceId` 的签名配置包。测试模拟器使用 `POST /api/onboarding/claim`。
4. APP 原样发送 `cloud-config {op:"set",bundle:{...}}`，禁止用户编辑生产 URL、密钥、版本或签名。
5. 收到 `ok=true,state=pending` 后才发送官方 `prov-config set/apply`。
6. `prov-config=connected` 时记录 `WIFI_READY` 并结束 BLE。
7. APP 通过 HTTPS 查询设备状态，只有同一 `deviceId + credentialId` 返回 `DEVICE_ONLINE` 才完成页面流程。

## 签名配置包

必填字段为 `version=1`、`deviceId`、`cloudUrl`、`credentialId`、`deviceSecret`、`bundleVersion` 和 `signature`。`bundleVersion` 必须是 `1..9007199254740991` 范围内的十进制整数，不接受小数、零、负数或超过 JSON 安全整数上限的值。签名算法为 ECDSA P-256/SHA-256，签名值是 DER 编码后再 base64。规范化原文必须逐字节构造为：

```text
NUWAX-CLOUD-CONFIG-V1
version=<decimal>
deviceId=<value>
cloudUrl=<value>
credentialId=<value>
deviceSecret=<value>
bundleVersion=<decimal>

```

固件校验 `deviceId` 等于硬件派生 UID；拒绝带用户信息、空主机、空白字符或不支持 scheme 的 URL；拒绝低于最新 NVS 版本的包，同一版本只有内容完全相同时允许幂等重发。测试构建允许签名 `ws://` 局域网地址，生产构建必须关闭 `CONFIG_NUWAX_CLOUD_CONFIG_ALLOW_INSECURE_WS`，只接受 `wss://`。

## 固件存储和回退

NVS namespace 为 `nuwax_cloud`，包含完整原子 blob `pending` 和 `active`。BLE 下发只写 `pending`；设备使用 pending 进行 WebSocket 鉴权，收到匹配 `credentialId` 的 authenticated hello 后把 pending 提升为 active，成功后发送 `cloud_config_committed`。网关收到匹配回执后才把服务端 pending 提升为 active。损坏槽位会被单独隔离，不再连带阻断另一个有效槽位。新配置失败时旧 active 不被覆盖；明确 401 拒绝或连续失败三次且存在旧 active 时，固件删除 pending 并回退 active。切换 Wi-Fi 会保留旧 STA 配置用于失败回滚。

当前首轮构建尚未启用 NVS Encryption、Flash Encryption 和 Secure Boot，因此只适用于受控联调设备。量产烧录必须同时启用三项保护，并把开发签名公钥替换为正式云配置签名公钥。

## 网关鉴权

设备 WebSocket Upgrade 发送：

```http
X-Device-ID: NX-XXXX-XXXX-XXXX
X-Credential-ID: cred_xxx
Authorization: Bearer <device_secret>
```

网关按 `deviceId` 查找 pending/active 凭据，只持久化 `device_secret` 的 SHA-256 摘要，并用常量时间比较。无效、吊销或不匹配的凭据必须在返回 `101 Switching Protocols` 前以 401 拒绝。成功 Upgrade 后 hello v2 只携带 `device_id`、`credential_id` 和能力，不再发送明文 `device_secret`。网关响应 `authenticated=true` 后先保持 `CLOUD_CONFIG_COMMIT_PENDING`；收到设备 `cloud_config_committed` 才转为 active 并返回 `DEVICE_ONLINE`。

## 错误和页面状态

- `CLOUD_CONFIG_INVALID`：字段、设备身份或 URL 不合法。
- `CLOUD_SIGNATURE_INVALID`：签名不能由固件信任公钥验证。
- `CLOUD_CONFIG_ROLLBACK`：版本回滚或同版本不同内容。
- `CLOUD_CONFIG_REQUIRED`：APP 未先完成签名云配置下发便调用 Wi-Fi set/apply。
- `CLOUD_CREDENTIAL_REJECTED`：网关在 101 前拒绝逐设备凭据。
- `CLOUD_CONNECT_TIMEOUT`：已 `WIFI_READY`，但 60 到 90 秒内未出现匹配的 `DEVICE_ONLINE`。

APP 不得把 `WIFI_READY` 显示成完整成功，也不得为了等待云端而保持 BLE 与 TLS/WebSocket 长时间并发。测试网关私钥、APP claim token 和局域网 `ws://` 仅供首轮联调，不能进入生产环境。
