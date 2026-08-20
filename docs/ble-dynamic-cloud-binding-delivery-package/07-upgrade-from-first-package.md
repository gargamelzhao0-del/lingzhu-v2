# 相对首轮交付包的升级

这里的“旧版”指最初只跑通“二维码识别 + Security 2 + BLE 下发 Wi-Fi”的功能基线。现有 `docs/ble-provisioning-delivery-package/` 后续已经同步过部分动态云配置说明，因此不能仅通过当前文件内容判断历史先后；本表描述的是功能架构升级。

## 功能对比

| 项目 | 首轮基线 | 当前动态绑定版本 |
| --- | --- | --- |
| 最终目标 | 设备取得 Wi-Fi | 设备取得 Wi-Fi并用逐设备凭据上线 |
| 云 URL | 固件/Kconfig 固定或未纳入配网 | claim 返回并经签名的 `cloudUrl` |
| 云密钥 | 固件共享 Token/固定配置 | 每设备动态 `deviceSecret` |
| APP 云接口 | 无 | claim + onboarding status |
| BLE endpoint | 官方 Wi-Fi endpoints | 新增 `cloud-config` |
| 配置可信性 | APP 输入即信任 | 固件 ECDSA 验签，APP 不能篡改 |
| 防回滚 | 无 | `bundleVersion` 单调递增、同内容幂等 |
| 设备保存 | 单份或编译值 | NVS `pending/active` 双槽事务 |
| 网关保存 | 共享密钥 | 每设备 credential、secret hash、pending/active |
| WebSocket 鉴权 | 可能在连接后或共享 Token | 101 前校验设备 ID、credential ID、Bearer secret |
| 提交确认 | 无 | authenticated hello + `cloud_config_committed/ack` |
| APP 成功条件 | `prov-config=connected` | `WIFI_READY + DEVICE_ONLINE` |
| 重新配网失败 | 可能丢失旧 Wi-Fi/云配置 | Wi-Fi 回滚记录 + 旧 active 云配置保留 |
| 模拟器 | BLE/Wi-Fi 状态模拟 | 真实 claim、签名下发、云鉴权和在线状态 |
| 错误码 | Security/Wi-Fi/超时 | 增加云配置、签名、回滚、云拒绝和云超时 |

## 升级功能及逻辑

### 1. 设备业务身份闭环

二维码 username 只用于定位还不够。新版在 Security 2 后读取 `device-info.serialNumber`，并要求它等于二维码、claim、签名包和 WebSocket identity。作用是防止 APP 把附近另一台同名/误选设备绑定到错误账户。

### 2. 云端动态签发

APP 不再持有固定设备 URL/密钥。它携带用户登录态和 deviceId 向云端 claim；云端生成 credential 并签名。作用是支持不同环境、不同设备、密钥轮换和账户授权，而无需每次改变 URL/secret 都重新编译固件。

### 3. `cloud-config` 自定义 endpoint

官方 `prov-config` 只定义 Wi-Fi，不能安全承载产品云配置。新版增加受同一 Security 2 保护的 `cloud-config`，在 Wi-Fi 前原子暂存签名 bundle。作用是保持 Espressif Wi-Fi 协议兼容，同时扩展产品 onboarding。

### 4. ECDSA 验签和版本控制

设备只信任固件内的云配置签名公钥。任何 URL/secret 被 APP、代理或恶意软件修改都会验签失败；版本规则阻止旧 credential 覆盖新 credential。

### 5. 双端两阶段提交

设备和网关都有 pending/active。只有新 credential 真正完成 WebSocket 鉴权并且设备 NVS 提交成功，两边才转 active。作用是避免 Wi-Fi 成功但错误云配置覆盖旧工作配置。

### 6. 最终成功标准升级

`GOT_IP` 只证明局域网可用，不证明设备绑定或云服务可用。APP 新增 `WAITING_DEVICE_ONLINE`，把 `CLOUD_CONFIG_COMMIT_PENDING` 作为中间态，只有网关确认同一 credential 在线才完成页面。

### 7. 可回滚重新配网

本地按钮进入配网时保存原 STA；新 Wi-Fi 失败/超时恢复旧 STA。云端新 pending 失败时旧 active 保持。两个回滚相互独立，切 Wi-Fi不必强制轮换云 credential。

### 8. 模拟器升级

Real 模式不再只验证 BLE 到 GOT_IP。Bridge 先向测试网关 claim，再通过真实 Security 2 发 `cloud-config`，最后等待设备 WebSocket 和 commit。事件轨迹可以区分 Wi-Fi 成功、云鉴权成功、NVS 提交中和最终在线。

### 9. PoP 没有被删除

动态 `deviceSecret` 不能替代 PoP：PoP 保护 APP 到近端设备，deviceSecret 保护设备到云端，APP Token 保护用户到账户 API。三个安全域同时存在才构成完整绑定。
