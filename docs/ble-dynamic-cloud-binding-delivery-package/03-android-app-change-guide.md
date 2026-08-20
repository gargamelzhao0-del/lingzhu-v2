# Android APP 改造指南

## 改造结论

原来的“扫码 -> Security 2 -> 下发 Wi-Fi -> connected 即成功”需要改为：

```text
扫码和身份校验
  -> 云端 claim 签名配置
  -> BLE cloud-config 暂存
  -> Wi-Fi set/apply/status
  -> WIFI_READY
  -> 云端状态查询
  -> DEVICE_ONLINE
  -> SUCCESS
```

`apiBaseUrl` 仍是 APP 自己的环境配置，不进入 BLE payload。`cloudUrl`、`credentialId`、`deviceSecret`、`bundleVersion` 和 `signature` 全部来自 claim 响应。

## 建议数据模型

```kotlin
data class ClaimRequest(
    val deviceId: String
)

data class CloudConfigBundle(
    val version: Int,
    val deviceId: String,
    val cloudUrl: String,
    val credentialId: String,
    val deviceSecret: String,
    val bundleVersion: Long,
    val signature: String
)

data class CloudConfigSetRequest(
    val op: String = "set",
    val bundle: CloudConfigBundle
)

data class CloudConfigSetResponse(
    val ok: Boolean,
    val state: String?,
    val credentialId: String?,
    val bundleVersion: Long?,
    val error: String?
)

data class OnboardingStatus(
    val ok: Boolean,
    val deviceId: String,
    val state: String,
    val credentialId: String?
)
```

`bundleVersion` 必须用 `Long`。JSON 库需要禁止把它经过 `Double`，否则大版本值可能失去精度并被固件拒绝。

## Provider 需要增加的能力

1. 读取 `ESPDevice.getVersionInfo()` 原始 `proto-ver` JSON：要求 `prov.sec_ver=2`、`prov.cap` 含 `wifi_prov/wifi_scan`，并单独要求 `nuwax.cap` 含 `cloud_prov`。不要用 `getDeviceCapabilities()` 检查 `cloud_prov`。
2. Security 2 后读取 `device-info`，严格比较 `serialNumber` 与二维码 username。
3. 使用 APP 登录态调用 claim API。
4. 在同一 Security 2 session 上调用自定义 `cloud-config` endpoint。
5. 只有 `ok=true,state=pending` 才进入官方 Wi-Fi set/apply。
6. `prov-config=connected` 后结束 BLE，每约 1 秒轮询云端状态，最多 90 秒。
7. 只有同一 `deviceId + credentialId` 为 `DEVICE_ONLINE` 才显示成功。

首轮 Android 参考基线冻结为 `com.github.espressif:esp-idf-provisioning-android:lib-2.4.4`，tag/commit `lib-2.4.4 / 99719ae2d89b9a209cc83ca88bc6f3f3f4318028`。自定义 endpoint 使用 `ESPDevice.sendDataToCustomEndPoint(path, utf8Bytes, ResponseListener)`；listener 收到的已经是 Security 2 解密后的响应。完整调用参考见 `11-android-provider-reference.md`。

自定义 endpoint UUID 通过 GATT `0x2901 User Description` descriptor 自动发现。APP 必须等 BLE service/descriptor discovery 和连接完成事件后再调用，不能硬编码 characteristic UUID，也不能在 endpoint 缺失时退回 `prov-config` characteristic。

## 页面状态机

建议新增：

```text
DEVICE_VERIFIED
CLAIMING_CLOUD_CONFIG
CLOUD_CONFIG_CLAIMED
SENDING_CLOUD_CONFIG
CLOUD_CONFIG_STAGED
SENDING_WIFI_CONFIG
WAITING_WIFI_READY
WIFI_READY
WAITING_DEVICE_ONLINE
CLOUD_CONFIG_COMMIT_PENDING
SUCCESS / FAILED
```

页面文案规则：

- `WIFI_READY`：显示“Wi-Fi 已连接，正在绑定云端”，不能显示完成。
- `CLOUD_CONFIG_COMMIT_PENDING`：显示“设备已连接服务，正在保存配置”，继续等待。
- `DEVICE_ONLINE`：显示最终成功。
- 屏幕 Wi-Fi 图标不参与协议判断。

超时必须分开：Wi-Fi `prov-config` 最多按 35 秒 APP guard 判断；只有到达 `WIFI_READY` 后才开始最多 90 秒的云端 online 等待。不能把 35 秒当成完整 onboarding 总超时。

## 重试与幂等

- BLE 短暂断开：重新建立 Security 2 后重发同一个签名 bundle；固件允许完全相同版本和内容的幂等重放。
- 不要每次 BLE 重连都重新 claim。当前测试 claim 每调用一次都会生成新 pending，重复 claim 可能使已经下发的旧 credential 无法通过网关鉴权。
- APP 在收到 `cloud-config pending` 前，应只在内存保存完整 bundle；不得写日志、埋点、崩溃报告或明文持久化。
- 收到 pending 后，APP 后续状态机只需要保留 `deviceId + credentialId`；可清除内存中的 `deviceSecret`。
- APP 进程被杀后，生产云应通过 `onboardingAttemptId`/Idempotency-Key 返回同一未完成 bundle，或明确废弃旧 pending 后签发新版本。当前测试网关尚未实现该生产级幂等接口。
- status 长时间停留 `CLOUD_CONFIG_COMMIT_PENDING` 时不得显示成功，也不得在原尝试内静默重新 claim；90 秒后返回 `CLOUD_CONNECT_TIMEOUT`。当前测试固件不补发丢失的 commit，需由固件/云端在生产前完成幂等收敛。

## 错误映射

| 来源 | APP 结果 |
| --- | --- |
| Security 2 凭据错误 | `SECURITY_AUTH_FAILED` |
| `serialNumber` 与二维码不一致 | `DEVICE_ID_MISMATCH` |
| claim 401/无绑定权限 | 账户/绑定错误，不发送设备配置 |
| cloud-config 字段错误 | `CLOUD_CONFIG_INVALID` |
| 签名错误 | `CLOUD_SIGNATURE_INVALID` |
| 版本回滚/同版本冲突 | `CLOUD_CONFIG_ROLLBACK` |
| Wi-Fi 密码错误 | `WIFI_AUTH_FAILED` |
| SSID 不存在 | `NETWORK_NOT_FOUND` |
| Wi-Fi 已连接、云端 401 | `CLOUD_CREDENTIAL_REJECTED` |
| 到达 `WIFI_READY` 后 90 秒未上线 | `CLOUD_CONNECT_TIMEOUT` |

## 正式 APP 和云端共同改动

- claim API 必须使用用户登录 Token，并校验设备是否可绑定、是否已被其他账户占用。
- claim 必须生成逐设备 credential，而不是返回全设备共享密钥。
- 正式云保存 secret hash，不保存或回传已提交后的明文 secret。
- 必须实现绑定、解绑、所有权转移、凭据吊销、审计和幂等恢复。
- APP 不提供生产 URL、密钥和签名的手工输入框。

## Android 库日志阻塞项

官方 `lib-2.4.4` 源码会通过 `Log.d` 输出原始二维码和 PoP。生产 APP 必须使用经审计的 patched/forked 依赖删除这些日志，或配置并验证 R8 确实从 release 产物移除相关调用；普通 wrapper 无法阻止库内部的 `Log.d`。不能只假设 release 环境不会采集 debug 日志。开发联调日志同样不得上传到工单、埋点或崩溃平台。
