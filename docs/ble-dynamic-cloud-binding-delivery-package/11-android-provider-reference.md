# Android Provider 实现参考

## 冻结参考版本

首轮 Android 接入按以下 Espressif 库 API 评审：

```gradle
implementation 'com.github.espressif:esp-idf-provisioning-android:lib-2.4.4'
```

该依赖由 JitPack 发布；若 APP 工程尚未配置，需要在集中仓库配置中加入：

```kotlin
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven(url = "https://jitpack.io")
    }
}
```

对应 tag/commit：

```text
lib-2.4.4
99719ae2d89b9a209cc83ca88bc6f3f3f4318028
```

状态是 `REFERENCE_API_REVIEWED_ANDROID_DEVICE_VALIDATION_PENDING`：API 和固件 endpoint 机制已对照源码，尚未替代 Android 真机联合验收。APP 仓库最终必须锁定依赖版本/commit，不能使用动态版本。

## 权限

APP 至少处理：

- 摄像头：扫描设备二维码时申请 `CAMERA`。
- Android 12 及以上：运行时申请 `BLUETOOTH_SCAN`、`BLUETOOTH_CONNECT`。
- Android 11 及以下：根据 target SDK 和系统要求申请 BLE 扫描所需蓝牙/位置权限。
- 用户拒绝权限、系统蓝牙关闭、APP 进入后台时，状态机应可停止并安全恢复，不得继续假装扫描。

## 不直接使用默认二维码日志路径

`lib-2.4.4` 的默认二维码处理和 Security 2 初始化源码包含原始二维码、PoP 的 `Log.d`。推荐 APP 使用自身二维码扫描器和严格 Nuwax v1 parser，然后手动创建 `ESPDevice`；同时使用经审计的 patched/forked 依赖删除 PoP 日志，或验证 R8 和 release 产物。普通 wrapper 与仅在 UI 隐藏 PoP 都不能消除库内部日志。

二维码必须校验：

```text
ver == v1
transport == ble
network == wifi
security == 2
name matches PROV_[0-9A-F]{6}
username matches NX-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}
PoP from QR or controlled source, UTF-8 length within contract
```

## BLE 扫描和连接

扫描必须同时匹配完整名称和 Service UUID：

```kotlin
val serviceUuid = UUID.fromString("0000ffff-0000-1000-8000-00805f9b34fb")
val filter = ScanFilter.Builder()
    .setDeviceName(qr.name)
    .setServiceUuid(ParcelUuid(serviceUuid))
    .build()
```

不能只调用前缀扫描后自动选择 RSSI 最强设备。BLE address 只用于本次连接，不保存为业务 ID。

本协议不要求 Android 系统 BLE 配对或持久 bond。APP 直接连接 GATT，再由 protocomm Security 2 建立应用层安全会话；不得把系统配对成功当成 Security 2 成功。

创建 Provider 对象：

```kotlin
val espDevice = ESPProvisionManager.getInstance(context).createESPDevice(
    ESPConstants.TransportType.TRANSPORT_BLE,
    ESPConstants.SecurityType.SECURITY_2
)
espDevice.setDeviceName(qr.name)
espDevice.setUserName(qr.username)
espDevice.setProofOfPossession(pop)
espDevice.connectBLEDevice(bluetoothDevice, serviceUuid.toString())
```

username 和 PoP 必须在第一次 `initSession`、`scanNetworks`、`sendDataToCustomEndPoint` 或 `provision` 之前设置，因为这些 API 可能惰性创建 Security 2 session。

## 服务发现和 capabilities

收到库的 BLE 连接完成事件后，先读取：

```kotlin
val version = JSONObject(espDevice.getVersionInfo())
val prov = version.getJSONObject("prov")
val nuwax = version.getJSONObject("nuwax")

require(prov.getInt("sec_ver") == 2)
require(prov.getJSONArray("cap").contains("wifi_prov"))
require(prov.getJSONArray("cap").contains("wifi_scan"))
require(nuwax.getJSONArray("cap").contains("cloud_prov"))
```

上面 `contains` 代表 APP 自己对 JSONArray 的遍历帮助函数。

重要：`ESPDevice.getDeviceCapabilities()` 只包含 `prov.cap`。用它检查 `cloud_prov` 会错误失败，必须解析 `getVersionInfo()` 的 `nuwax.cap`。

Espressif `BLETransport` 会读取 characteristic 的 `0x2901 User Description` descriptor，自动建立 endpoint name -> UUID 映射。APP 不需要也不允许写死 `device-info` 或 `cloud-config` characteristic UUID。连接完成前调用 endpoint，或 descriptor 中缺少 endpoint，统一映射 `PROTOCOL_ERROR`。

## 建立 Security 2 和读取设备身份

推荐显式调用 `initSession`，便于把认证失败准确映射为 `SECURITY_AUTH_FAILED`。成功后：

```kotlin
espDevice.sendDataToCustomEndPoint(
    "device-info",
    "{}".toByteArray(StandardCharsets.UTF_8),
    responseListener
)
```

`ResponseListener.onSuccess(returnData)` 收到的是库已经 Security 2 解密后的 UTF-8 JSON。必须验证：

```text
deviceInfo.serialNumber == qr.username
deviceInfo.capabilities contains wifi_prov, wifi_scan, cloud_prov
```

不一致立即断开，不 claim、不发送 cloud-config、不发送 Wi-Fi。

## Claim 和 cloud-config

使用用户登录态调用：

```http
POST {apiBaseUrl}/api/onboarding/claim
Authorization: Bearer <user-token>
Content-Type: application/json

{"deviceId":"<deviceInfo.serialNumber>"}
```

APP 验证 HTTP 201、`ok=true`、`bundle.version == 1`、bundle deviceId 一致和七个字段全部存在；`bundleVersion` 必须是范围内的 `Long`。APP 不自行签名、不修改 URL/secret/版本，ECDSA 的最终权威校验由设备完成。

```kotlin
val payload = json.encodeToString(
    CloudConfigSetRequest(op = "set", bundle = bundle)
).toByteArray(StandardCharsets.UTF_8)

espDevice.sendDataToCustomEndPoint("cloud-config", payload, responseListener)
```

只有解密响应满足以下条件才能继续：

```text
ok == true
state == pending
credentialId == claimed bundle.credentialId
bundleVersion == claimed bundle.bundleVersion
```

响应不应出现 `deviceSecret`；出现时按协议错误处理并禁止记录。

## Wi-Fi 和最终云状态

设备 Wi-Fi 扫描：

```kotlin
espDevice.scanNetworks(wifiScanListener)
```

开始配网前停止周期扫描，等待当前 scan callback 返回，再调用：

```kotlin
espDevice.provision(ssid, passphrase, provisionListener)
```

`ProvisionListener.deviceProvisioningSuccess()` 只代表官方 Wi-Fi provisioning 成功，即 `WIFI_READY`，不是完整绑定成功。此时 APP：

1. 进入 `WIFI_READY/WAITING_DEVICE_ONLINE`。
2. 释放 BLE，避免 BLE 与设备 TLS/WebSocket 长期并发。
3. 每约 1 秒查询 status，最多 90 秒。
4. 只接受相同 `deviceId + credentialId` 的 `DEVICE_ONLINE`。

35 秒是 Wi-Fi `prov-config` guard；90 秒是到达 WIFI_READY 后的云上线等待，两者不能合并。

## 串行和生命周期

- 所有 `ESPDevice` 操作放入单一串行协程/队列；不并发 scan、custom endpoint、provision。
- Activity/Fragment 重建不应复制执行 claim 或 provision；状态归 ViewModel/用例层持有。
- BLE 断开后废弃旧 Security 2 session，重新连接并重建 session。
- cloud-config 响应丢失时重发同一个 bundle；不重新 claim。
- 当前测试 claim 非幂等。APP 进程死亡恢复必须等待生产 `onboardingAttemptId`/Idempotency-Key 方案，不能凭 `cloudConfigState=pending` 猜测 credentialId。

## APP 最低单元测试

- capability 分别从 `prov.cap`/`nuwax.cap` 读取。
- JSON `bundleVersion` 保持 `Long`，不经过 Double。
- cloud-config 响应 credential/version 不匹配时拒绝继续。
- Wi-Fi success 不直接进入 SUCCESS。
- status 返回旧 credential 的 DEVICE_ONLINE 时继续等待/失败，不误报。
- Activity 重建和 BLE 重连不重复 claim。
- 日志、埋点、崩溃信息不含二维码、PoP、Wi-Fi 密码、APP Token、`deviceSecret`。
