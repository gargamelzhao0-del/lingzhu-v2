# 本地测试网关说明

## 作用

本地测试网关模拟正式云端中与设备 onboarding 直接相关的最小能力：

1. APP 使用测试 Token claim 设备。
2. 网关生成动态 `credentialId` 和 `deviceSecret`。
3. 网关使用与固件测试公钥匹配的私钥签名 bundle。
4. 网关保存 pending 的 secret hash。
5. 设备通过 Wi-Fi 发起 WebSocket Upgrade，网关在 101 前鉴权。
6. 设备提交 `cloud_config_committed` 后，网关把 pending 提升 active。
7. APP 查询状态得到 `DEVICE_ONLINE`。

它验证通信和事务逻辑，不实现正式用户账户、所有权、解绑或计费服务。

## 启动方式

推荐从新交付包启动：

```powershell
cd docs\ble-dynamic-cloud-binding-delivery-package\simulator
.\start-full-stack.ps1
```

启动脚本会：

- 向上查找 `NuwaxAIPartner` 工程根目录和构建信息。
- 选择设备可访问的本机局域网 IPv4。
- 在 `8799..8810` 选择空闲端口。
- 生成本次进程专用 APP claim Token 和管理 Token。
- 引用工程 `cloud-gateway/test-device-config-signing-private-key.pem`，不复制私钥到交付包。
- 启动 `cloud-gateway/cloud-gateway.js`。
- 设置 Bridge 使用的网关 HTTP URL/claim Token。
- 启动 `127.0.0.1:8765` BLE Bridge。
- 把 stdout/stderr 放到模拟器本地 `runtime-logs/`；日志目录不进入交付或版本控制。

关键环境变量由脚本动态设置：

```text
CLOUD_DEVICE_ONBOARDING_ENABLE=1
CLOUD_DEVICE_ONBOARDING_ALLOW_INSECURE_WS=1
CLOUD_DEVICE_PUBLIC_WS_URL=ws://<LAN-IP>:<PORT>/api/device/ws
CLOUD_DEVICE_REGISTRY_FILE=<cloud-gateway>/runtime/test-device-registry.json
CLOUD_DEVICE_SIGNING_KEY_FILE=<cloud-gateway>/test-device-config-signing-private-key.pem
CLOUD_APP_CLAIM_TOKEN=<本次随机Token>
NUWAX_TEST_GATEWAY_HTTP_URL=http://<LAN-IP>:<PORT>
NUWAX_TEST_GATEWAY_CLAIM_TOKEN=<本次随机Token>
```

## HTTP API

### Claim

```text
POST /api/onboarding/claim
Authorization: Bearer <claim-token>
Body: {"deviceId":"NX-...."}
```

每次调用生成新的 pending bundle。当前测试实现不是幂等生产接口，因此模拟器在同一次任务只 claim 一次。

### Status

```text
GET /api/onboarding/status?device_id=NX-....
Authorization: Bearer <claim-token>
```

状态含义：

| 状态 | 条件 |
| --- | --- |
| `WAITING_FOR_DEVICE` | 设备尚未用本次 credential 连接 |
| `CLOUD_CONFIG_COMMIT_PENDING` | 101 前鉴权成功，但设备 NVS 提交回执未被接受 |
| `DEVICE_ONLINE` | WebSocket 在线且 `cloud_config_committed` 已匹配 |
| `CLOUD_CREDENTIAL_REJECTED` | 最近两分钟出现该设备鉴权失败 |

## 注册表事务

测试注册表逻辑结构：

```json
{
  "version": 1,
  "devices": {
    "NX-3927-2693-9288": {
      "active": {
        "credentialId": "cred_...",
        "secretHash": "<sha256>",
        "bundleVersion": 1784954350746,
        "revoked": false
      },
      "pending": null
    }
  }
}
```

注册表不保存明文 `deviceSecret`。claim 响应只在 HTTPS/本地测试 HTTP 请求中向已授权 APP 返回一次明文；服务端落盘保存 SHA-256 摘要。

## WebSocket 鉴权和提交

设备请求 `/api/device/ws`，网关从 Upgrade headers 读取 `X-Device-ID`、`X-Credential-ID` 和 Bearer secret。错误凭据在 101 前返回 401。成功后仍先处于 commit pending；只有收到匹配 `cloud_config_committed` 才更新注册表并让 status 返回 `DEVICE_ONLINE`。

## 测试与生产差异

| 本地测试 | 正式云要求 |
| --- | --- |
| 随机进程级 claim Token | APP 用户 OAuth/JWT 与权限校验 |
| `ws://<LAN-IP>` | `wss://` + 有效证书校验 |
| 文件注册表 | 事务数据库、唯一约束、审计 |
| 固定测试签名私钥 | HSM/KMS 托管生产私钥和轮换策略 |
| 每次 claim 生成新 pending | 幂等 attempt、超时、撤销和恢复 |
| 无账户所有权 | 绑定、解绑、转移、吊销、防抢绑 |

APP 工程师不应把本地 claim Token、局域网 URL 或测试签名材料写入移动端正式配置。
