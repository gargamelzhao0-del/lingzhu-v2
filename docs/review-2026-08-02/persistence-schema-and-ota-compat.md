# NuwaxAIPartner-v2 持久化 Schema 与 app-only OTA 兼容基线

生成日期：2026-08-02

状态：B0-10 只读源码证据。未读写设备 NVS，未执行掉电/损坏注入。

## 1. NVS 数据清单

| Namespace | Key/记录 | 数据与版本 | 写入时机 | 恢复/迁移 |
|---|---|---|---|---|
| `nuwax_uid` | `device_uid_v1` | eFuse/MAC 派生 UID 的 hex cache | 首次缺失时一次 | 缺失/无效时可稳定重算；写入 best effort |
| `nuwax_cloud` | `active`, `pending` | `nuwax_cloud_config_t` blob，内部 `version=1`、严格 size/字段校验 | BLE vox-config stage/commit | 无效 active/pending 会单 key 擦除；pending 提供事务候选 |
| `nuwax_reprov` | `request`, `wifi_verified`, `commit_pending` | 带 magic 的 Wi-Fi rollback/验证/提交标记 blob | 重配网事务 | 支持旧 Wi-Fi 回滚和重启恢复；无统一 schema version |
| `nuwax_wifi` | `ssid`, `password`, `last_working`, `saved_count`, `saved_<n>_ssid/_pwd` | 应用侧 Wi-Fi 凭据/历史列表 | 配网导入、保存网络、成功连接和 fallback | 多 key，无全局版本/magic/checksum；与 ESP Wi-Fi driver 存储重复 |
| ESP Wi-Fi/provisioning 内部 namespace | STA 配置和 manager 状态 | ESP-IDF 内部格式 | `esp_wifi_set_config`/配网 manager | 依赖 IDF 兼容性；应用不能假定内部 schema |
| `nuwax` | `spk_vol` | `u8` 0..100 | 持久化音量设置 | 无 schema；无效值回默认，保存失败只记日志 |
| `wake_word` | `command` | 归一化 MultiNet command string | 运行期命令 persist | 更新失败会尝试恢复旧命令；无独立 schema |
| `nuwax_l3` | `en` | `u8` AFE L3 开关 | 诊断/运行期持久化切换 | set/commit 返回值未传播，可能误报已保存 |
| `nuwax_ota` | `last_status`, legacy `pending_ver/pending_seq`, `accepted_seq`, `pending_txn`, `accepted_txn` | OTA transaction magic + format v3 + size + sequence/address/image size/version/SHA | OTA 启动、状态转换、commit/rollback | pending 支持 legacy v1/v2 识别并 fail-closed/reconcile；critical sequence/identity 写失败会阻止下一次 OTA |
| `nuwax_ota_ctx` | `request_id`, `update_id`, `version`, `image_sha256`, `release_seq`, `target_addr` | OTA 上报上下文，多 key | 接受命令时保存，终态后清理 | 整 namespace `erase_all`；无结构版本，部分写靠逐项错误返回拒绝 |

## 2. 正向设计

- 云配置使用 pending -> 验证联网 -> active 的流程，并对设备 ID、URL、secret、结构版本和长度做校验。
- BLE 重配网保留旧 `wifi_config_t` rollback 记录，失败时尝试恢复旧 Wi-Fi；验证/commit marker 用于跨重启恢复。
- OTA pending transaction v3 同时保存 release sequence、目标地址、image size、version 和 SHA-256；启动后会将实际 running partition hash 与记录核对。
- OTA accepted sequence 持久化失败时阻止下一次更新，避免静默接受未持久化的 anti-rollback 状态。
- UID 可从稳定硬件身份重新派生，不依赖 NVS 作为唯一真值。

## 3. 风险

### NVS-001 P1：启动时全量擦除

`nuwax_ai_partner_main.c` 在 `nvs_flash_init()` 返回 `ESP_ERR_NVS_NO_FREE_PAGES` 或 `ESP_ERR_NVS_NEW_VERSION_FOUND` 时直接执行 `nvs_flash_erase()`。这会同时清除 Wi-Fi、云 URL/device secret、OTA accepted sequence/identity、音量、唤醒词和事务恢复记录。

影响不仅是重新配网：OTA anti-rollback floor 会退回固件编译基线；旧固件读取新 NVS 格式时还可能先擦掉新版本状态。量产需要受控恢复分区/备份、保留安全 floor 的策略，或至少进入明确 recovery 状态，不能静默全擦后继续启动。

### NVS-002 P1：敏感数据明文静态存储

当前 `CONFIG_NVS_ENCRYPTION`、Flash Encryption 均未启用。Wi-Fi 密码和 device secret 存在 NVS，且 Wi-Fi 凭据同时存在 ESP driver 与 `nuwax_wifi` 两套存储。开发阶段可运行，但不满足量产静态凭据保护。

### NVS-003 P2：无统一 schema/恢复出厂契约

只有云 blob、reprovision magic 和 OTA transaction 有局部版本/校验；Wi-Fi 列表、speaker/wake/L3、OTA context 没有统一设备 schema。源码没有受控 factory reset/RMA API，唯一全擦路径是 NVS 初始化错误后的自动擦除。必须定义保留 UID/anti-rollback、清除用户凭据、恢复配网和诊断日志的明确矩阵。

### NVS-004 P2：寿命与运行期容量未预算

- `last_working` 在每次成功连接时都 commit，即使值未变化；fallback 还会先写一次，重连风暴会产生不必要擦写。
- 音量 `persist=true` 时即使数值未变化仍 commit；L3 persist 不检查 set/commit 结果。
- OTA 每次更新包含多个 status/transaction/context commit，合理但未形成每次 OTA 的最大写次数预算。
- 24 KiB NVS 没有 `nvs_get_stats`/剩余 entry 观测、写入节流或寿命 SLO。

### NVS-005 P2：多 key / 多存储事务仍需掉电证明

BLE 配网按 ESP Wi-Fi store -> `nuwax_wifi` -> reprovision marker -> cloud active -> verified marker 顺序提交，并有 rollback/reconstruction 代码；但它跨 ESP 内部 Wi-Fi namespace、三个应用 namespace 和多个 commit，静态代码不能证明每个掉电点都恢复为一致的 Wi-Fi+cloud 对。OTA 的 `nuwax_ota_ctx` 也是六个 key 的单次 commit，accepted sequence/identity、pending transaction和otadata则跨 NVS 与 bootloader domain。必须逐 set/commit/otadata state 注入掉电/重启，验证配置只能为 old pair/new pair，OTA只能为旧槽、已确认新槽或明确 fail-closed，禁止 mixed generation。

## 4. app-only OTA 兼容矩阵

| 资产 | app OTA 是否更新 | 当前兼容门 | 缺口/约束 |
|---|---|---|---|
| app slot | 是 | version、release sequence、size、SHA、target/build flavor、常量 schema | 当前主要 OTA 对象 |
| bootloader/早期 power hold | 否 | 无 bootloader compatibility ID | app 不得要求新 bootloader 行为；升级需全量有线流程 |
| partition table | 否 | `partition_schema=1` + 弱运行时槽检查 | PART-001：未校验精确 label/address/size/hash |
| `srmodels` model partition | 否 | app 按 label 加载 | 无 model package version/hash 与 app 要求的握手；唤醒模型变化需有线或资源 OTA |
| NVS schema/data | 保留 | 各模块局部兼容代码 | 无全局 schema/factory reset；必须保证新旧 app 双向回滚可读或 fail-closed |
| hardware revision | 否 | build-time board/manifest board ID | 无法通过 app OTA 修正焊接/引脚/器件差异 |
| Secure Boot/eFuse/Flash/NVS encryption | 否/不可逆 | 当前未启用 | 必须独立量产迁移，不得混入普通 OTA |

## 5. 验证门

1. 为每个 namespace 生成 key/type/version/owner 清单和最大 entry 预算。
2. 对云配网、OTA pending/commit、reprovision rollback 每个 NVS commit 前后做掉电矩阵。
3. 模拟 `NO_FREE_PAGES`、损坏 key、错误 blob size、旧 schema 和 NVS 写失败；不得静默降级安全 floor。
4. 1000 次 Wi-Fi 重连/音量操作/OTA 状态循环统计实际 page erase 和剩余 entries。
5. 定义 factory reset：哪些 key 清除、哪些保留、如何重新配网、如何防止 anti-rollback 回退。
6. app-only OTA manifest 增加精确 partition/model/NVS compatibility identity 后再关闭 PART-001/PART-005。

## 6. 结论

B0-10 的持久化对象和兼容边界已建立。局部事务设计较完整，但全量 NVS 擦除、明文凭据、无统一 schema/factory recovery、未预算擦写寿命和弱 app-only 兼容门均仍开放；不能据此宣称掉电、降级或长期运行稳健。
