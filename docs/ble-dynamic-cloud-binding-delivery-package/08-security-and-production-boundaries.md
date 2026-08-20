# 安全与生产边界

## 三个独立安全域

| 凭据 | 认证关系 | 泄露后的影响 | 是否可互换 |
| --- | --- | --- | --- |
| PoP | APP -> 近端 BLE 设备 | 未授权用户可能配置附近设备 | 否 |
| APP 用户 Token | 用户/APP -> 云端 API | 账户绑定权限被冒用 | 否 |
| `deviceSecret` | ESP32-S3 -> 设备云网关 | 该设备云身份可能被冒用 | 否 |

删除 PoP 会让“知道设备 ID”近似等于“可以向附近设备发送 Wi-Fi 和云密钥”，不符合物理在场授权要求。即使 URL/secret 动态化，也必须保留 Security 2 PoP。

## 敏感信息处理矩阵

| 数据 | APP UI | APP 日志/埋点 | APP 内存 | 设备 NVS | 云端持久化 |
| --- | --- | --- | --- | --- | --- |
| Wi-Fi 密码 | 仅密码框 | 禁止 | 配网期间 | ESP-IDF Wi-Fi NVS | 禁止 |
| PoP | 可受控输入 | 禁止 | Security 2期间 | 生产仅 salt/verifier | 受控凭据服务 |
| APP Token | 不展示 | 禁止 | API 会话 | 禁止 | Token 服务管理 |
| `deviceSecret` | 禁止展示 | 禁止 | 到 cloud-config pending 响应 | 加密 NVS active/pending | 只保存 hash |
| 签名私钥 | 禁止 | 禁止 | 禁止 | 禁止 | HSM/KMS 或受控服务 |
| 签名公钥 | 可公开 | 可记录版本 | 可选 | 固件信任锚 | 公钥/版本可公开 |

## 当前已实现的防护

- BLE 敏感报文位于 protocomm Security 2 AES-GCM 会话内。
- 二维码身份与 `device-info` 二次比对。
- `cloudUrl/deviceSecret` 被 ECDSA 签名覆盖。
- bundleVersion 防回滚和同版本冲突。
- WebSocket 在返回 101 前完成逐设备凭据校验。
- 网关只保存 secret SHA-256 摘要并常量时间比较。
- 设备 pending/active 与 Wi-Fi 回滚避免失败配置破坏旧配置。
- 模拟器 Bridge 只绑定 loopback，敏感字段不写事件日志。
- Flash/NVS 操作在内部 RAM 栈执行，避免 PSRAM cache assertion。

Android 参考库 `lib-2.4.4` 本身仍包含原始二维码和 PoP 的 `Log.d`。这不改变链路加密，但会形成端侧明文日志泄漏；生产必须使用去日志的受控 patched/forked 产物，或经过 release 字节码/日志实测验证的 R8 规则。普通 wrapper 不能消除库内部日志。

## 当前开发配置不能直接量产

- 开发版 `CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE=y`，开发设备可能共享测试 PoP。
- 测试网关允许局域网 `ws://`；生产必须是校验证书的 `wss://`。
- 固件包含测试云配置签名公钥；生产必须替换为生产公钥或实现密钥版本/轮换。
- NVS Encryption、Flash Encryption、Secure Boot 尚未完成。
- factory NVS 逐设备 salt/verifier 注入、PoP 发放和唯一性注册尚未完成。
- 当前设备 ID 为硬件派生并截断展示，量产需要唯一性注册表或工厂序列号策略。
- 测试网关没有正式账户所有权、反抢绑、解绑、转移、吊销和审计。

## 正式云端必须补齐

1. claim 请求使用用户登录 Token，并在事务中校验设备未被他人占有或符合转移规则。
2. 使用 Idempotency-Key/onboardingAttemptId，重复请求返回相同未完成 bundle。
3. pending 设置有效期；超时可撤销，并防止无限堆积 credential。
4. 解绑/转移时吊销旧 credential，设备端接收受签名的新 credential 或执行受控恢复。
5. 生产私钥由 HSM/KMS 管理，提供签名 key ID、轮换、审计和灾备。
6. 注册表/数据库保证 `(deviceId, credentialId, bundleVersion)` 唯一和单调。
7. status API 必须验证当前用户对 deviceId 的读取权限。
8. 对 claim、失败鉴权、提交、绑定/解绑记录安全审计，但不得记录明文 secret。
9. `cloud_config_committed` 必须可幂等重发，设备持久化服务端确认或在每次 authenticated hello 后补发，直到 status 收敛。

## 公钥轮换说明

“URL 和设备密钥动态”不等于“签名信任锚动态”。当前固件用固定公钥验证 bundle：

- 同一签名私钥下，云端可无限签发新 URL、secret 和版本，无需更新固件。
- 如果要更换签名私钥，必须确保设备同时信任对应新公钥。
- 推荐后续 bundle 增加 `signingKeyId`，固件内维护有限公钥集合并设计旧钥撤销策略；该扩展尚未冻结，不能由 APP 自行添加字段后假设固件支持。
