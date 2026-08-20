# B0-18 安全威胁模型与静态审计

生成日期：2026-08-03

状态：**A 级静态证据已完成；B/C/D 验证均未执行；量产发布阻塞。**

本文件只描述当前源码、配置和协议的静态事实。未执行构建、secret scanner、攻击测试、eFuse 操作、NVS 读写、COM5、OTA 或烧录。本文中的量产设置和 eFuse 流程是待实现、待审批的安全要求，不是已经生效的设备状态。

## 1. 结论

当前工程具有一组可用的开发期安全控制：BLE 使用 protocomm Security 2；正式 WebSocket 可使用证书校验的 WSS；WebSocket Upgrade 携带设备 secret；敏感 hello 缓冲区发送后清零；OTA 只接受已认证云会话中的命令，下载强制 HTTPS、证书/主机名校验、精确 Content-Length、SHA-256、目标板/芯片/工程/构建 flavor/分区 schema/版本/release sequence 检查，并使用双 OTA 槽、pending/accepted 事务和应用回滚。

这些控制尚不足以支持量产放行。当前候选配置仍是开发安全 profile：Secure Boot、Flash Encryption、NVS Encryption 和 bootloader anti-rollback 均关闭；固定开发 PoP、`ws://`、USB 配网控制、UART/USB Serial-JTAG 控制台和 INFO 日志仍开启。云配置没有后端签名、凭据版本和防回滚；OTA manifest/image 没有独立发布签名或 `key_id`；工程没有闭合的密钥生成、注入、轮换、吊销、销毁和 RMA 流程。

本轮确认的新 canonical 问题为：

| ID | 等级 | 结论 |
|---|---|---|
| `CONFIG-004` | P1 | `sdkconfig.product-ota.defaults` 继承开发安全默认值，不是独立量产 profile |
| `SEC-004` | P1 | OTA manifest/image 缺少独立发布签名和 `key_id` |
| `SEC-005` | P1 | 云配置协议缺少签名、`credentialId` 和单调 `bundleVersion` |
| `SEC-006` | P1 历史项 | OTA 命令跨连接新鲜度的原缺陷已完成 A 级静态修复；B/D 断线交错验证开放 |
| `SEC-007` | P1 历史项 | UID cache 权威性原缺陷已完成 A 级静态修复；B/D 篡改/迁移验证开放 |
| `KEY-001` | P1 | 密钥全生命周期、RMA 和签名仪式未闭合 |

既有 `CONFIG-001`、`CONFIG-002`、`NVS-002`、`SEC-001`、`SEC-002`、`SEC-003`、`NVS-003` 和 `NVS-001` 继续开放，不重复计数。

## 2. 审计边界和假设

### 2.1 范围

- 设备身份、BLE Security 2、Wi-Fi 和云凭据；
- 动态云配置、WSS/WebSocket 鉴权和远程命令；
- OTA 命令、manifest、HTTPS 下载、镜像校验、回滚和版本 floor；
- Secure Boot、Flash/NVS Encryption、anti-rollback；
- UART、USB Serial/JTAG、BLE、日志、core dump 和 RMA；
- 测试私钥、发布密钥、轮换、吊销和销毁边界。

工程外正式云端、APP 后端、对象存储、制造系统、HSM、PKI、账号权限和组织流程没有源码证据。本文把这些系统标为外部信任边界，不假设它们已经满足要求。

### 2.2 攻击者模型

| 攻击者 | 能力 | 不默认具备的能力 |
|---|---|---|
| 邻近 BLE 攻击者 | 扫描、连接、重放、耗尽连接/内存 | 不知道每设备 PoP 时不能完成 Security 2 |
| 局域网/互联网 MITM | 控制 DNS、AP、路由和明文流量 | 不能伪造受信 CA 下正确主机名证书 |
| 被盗 PoP/APP 会话持有者 | 建立合法 Security 2 会话并提交配置 | 不应自动拥有后台签发或设备所有权权限 |
| 云账号或云服务入侵者 | 下发业务/OTA 命令、替换 manifest URL/hash | 不应拥有离线发布签名私钥 |
| CDN/对象存储入侵者 | 替换下载对象 | 不应能伪造独立发布签名 |
| 本地物理攻击者 | 读取/改写 flash、接入 UART/USB/JTAG、触发恢复 | 在量产 profile 下不应能绕过 Secure Boot/Encryption/debug gate |
| 构建/供应链攻击者 | 修改源码、依赖、资源、配置或发布包 | 不应同时控制签名审批、HSM 和独立审计 |
| RMA/维修人员 | 持有设备并执行诊断/擦除 | 不应读取用户 Wi-Fi/cloud secret 或降级安全 floor |

## 3. 资产与安全目标

| 资产 | 机密性 | 完整性/真实性 | 可用性和恢复 |
|---|---|---|---|
| 设备 UID/绑定关系 | UID 可公开，映射关系受控 | 必须与不可变硬件身份一致 | NVS 损坏后可确定恢复，不能漂移身份 |
| BLE PoP / SRP verifier | PoP 机密；salt/verifier 受保护 | 每设备唯一、可吊销/重置 | 丢失后走受控恢复，不回退固定 PoP |
| Wi-Fi SSID/password | password 机密 | 新旧配置事务一致 | 配网失败可回滚，RMA 可清除 |
| cloud URL/device secret | secret 机密 | URL/secret 必须由授权后端签发且防回滚 | 支持轮换、撤销、失败回退和审计 |
| 正常/会议/监控音频 | 用户隐私数据 | session/sequence/模式语义正确 | 云故障不能导致无界缓存或泄漏 |
| OTA manifest/image | 下载 URL 可见 | 发布者签名、目标、版本、hash 和 anti-rollback 完整 | 掉电可恢复，坏镜像自动回滚 |
| 发布/签名私钥 | 最高机密 | 双人审批、不可导出、可轮换/吊销 | 有离线恢复和灾难流程 |
| 日志/core dump | 可能包含身份、音频和 secret 残留 | 来源、时间、版本可追溯 | 有容量、访问、保留和删除策略 |

音频同意、录音留存和删除的详细数据流已在 `docs/review-2026-08-02/phase0-b014-privacy-data-flow-retention.md` 审计；本文件只引用其安全边界。

## 4. 信任边界与数据流

| 流程 | 信任边界 | 当前正向控制 | 当前缺口 |
|---|---|---|---|
| 制造身份/PoP -> 设备 | 工厂系统、标签、NVS/eFuse | 生产分支可读取 `nuwax_factory/sec2_salt` 和 `sec2_verifier`，不在设备重构明文 PoP，见 `main/nuwax_provision_new/nuwax_provision_main.c:945-968` | 当前仍使用固定开发 PoP；无注入证明、批次审计、轮换/吊销/RMA 流程 |
| APP -> BLE -> Wi-Fi/cloud config | 手机、BLE 空口、设备 | Security 2；`deviceId` 必须匹配本地 UID；云配置使用 pending -> commit，见 `main/nuwax_provision_new/nuwax_provision_main.c:763-803,1410-1488` | payload 只有 deviceId/URL/secret；没有后端签名或版本；当前固定 PoP 扩大攻击面 |
| 设备 -> cloud WebSocket | DNS/AP/Internet/云入口 | WSS 使用 CA bundle；Upgrade 校验 101/Upgrade/Connection/Accept；token/hello buffer 清零，见 `main/ws_transport.c:712-765,769-864`、`main/session_protocol.c:194-210,213-364` | 当前允许 `ws://`；secret 在 header 和 hello 中重复发送；业务授权依赖工程外云端 |
| cloud -> 设备命令 | 云账号/服务、连接 epoch、任务队列 | transport 101 后才置 authenticated；OTA item 保存 epoch，入队和消费都与当前 authenticated epoch 比较，见 `main/nuwax_ai_partner_main.c:3979-4002`、`main/nuwax_ota_new/nuwax_ota_device.c:54-62,778-798,1319-1321,1397-1401,1462-1499` | epoch 交错仅有 A 级静态证据；远程命令的用户所有权授权不在本仓库 |
| cloud -> HTTPS/CDN -> OTA slot | 云、CDN、TLS、flash | HTTPS、hostname、Content-Length、SHA-256、target/version/sequence、descriptor、双槽 rollback，见 `main/nuwax_ota_new/nuwax_ota_transport.c:245-337,431-475,646-724` | manifest/image 无独立签名；cloud/CDN 与 hash 可被同一受损控制面替换；Secure Boot 当前关闭 |
| flash/NVS -> runtime | 外部 flash、NVS parser、启动恢复 | UID 每次从 eFuse/MAC 派生，NVS 仅作匹配缓存；部分 blob 有版本/长度/magic/hash 和 pending/accepted 事务 | Flash/NVS Encryption 关闭；启动可全擦 NVS；release floor 仍是可变 flash 状态 |
| UART/USB/JTAG/log -> 操作者 | 物理端口、维修台、日志系统 | USB 配网命令很窄；敏感 hello/request 主动清零 | 产品候选仍开放控制台、USB control 和调试感知；无授权维修模式或审计 |

## 5. 威胁清单

| 威胁 | 场景 | 当前控制 | 残余风险/映射 |
|---|---|---|---|
| T01 身份冒用 | 改写 `nuwax_uid/device_uid_v1` 试图改变设备 UID | 每次启动先从 eFuse/MAC 派生权威值；cache mismatch 只告警并修复，不进入身份输出 | 原 `SEC-007` A 级静态修复；仍需 cache 篡改/迁移 B/D 验证和制造绑定记录 |
| T02 BLE 未授权重配 | 固定 PoP 泄露后邻近攻击者提交新 Wi-Fi/cloud secret | Security 2、防 Security 0 降级 | 固定 PoP 进入当前镜像/屏幕 QR，`SEC-001`/`CONFIG-001` |
| T03 云配置重放/降级 | 重放旧 URL/secret 或把设备导向攻击者 WSS | deviceId、URL/token 字符检查、pending transaction | 无签名/credential version/anti-rollback，`SEC-005` |
| T04 明文云链路劫持 | `ws://` 上窃取 token/hello 并注入命令/audio | 仅 test Kconfig 约束 | 当前候选实际开启，`CONFIG-001`/`CONFIG-004` |
| T05 云/CDN 联合替换 OTA | 被入侵云下发恶意 URL 和对应 SHA | HTTPS、SHA、目标/版本检查 | 无独立 release signature，`SEC-004` |
| T06 OTA 重放 | 下发低版本或重复序号 | SemVer + NVS `release_sequence` | NVS floor 可被擦除/改写，非 eFuse anti-rollback，`NVS-001`/`CONFIG-002` |
| T07 跨连接命令混淆 | epoch N 的 OTA 入队，断线，epoch N+1 认证后旧项出队 | item 保存 epoch；reserve 和 controller acceptance 都检查同一 authenticated epoch；通过 acceptance 后事务明确转为可跨重连恢复 | 原 `SEC-006` A 级静态修复；B/D 仍需覆盖断线发生在入队、出队和 acceptance 两侧的全交错 |
| T08 本地固件/凭据提取 | 物理读取外部 flash 或写入自定义 app | 当前无有效控制 | Secure Boot/Flash/NVS Encryption 关闭，`CONFIG-002`/`NVS-002` |
| T09 调试入口滥用 | UART/USB/JTAG 获取日志、触发配网或调试 | 需要物理接入 | 当前接口和 INFO 日志开启，`CONFIG-004`/`SEC-003` |
| T10 secret 泄漏 | 构建包、历史、日志、core dump 含密钥 | hello/request 清零；已知测试私钥单独识别 | 无正式 secret scanner/发布门，`SEC-002`/`SEC-003` |
| T11 恢复导致安全降级 | NVS 版本/页错误触发全擦，失去 secret 和 release floor | 重新初始化可恢复可用性 | 安全 floor 和身份/绑定状态被破坏，`NVS-001`/`NVS-003` |
| T12 RMA 数据泄漏或降级 | 维修人员读取凭据或烧入低版本 | 无完整流程 | `KEY-001`/`NVS-003` |
| T13 日志/core dump 泄漏 | panic 或 INFO 日志暴露身份、URL、内存 | 当前未配置 core dump 分区；部分敏感缓冲清零 | 没有量产日志/core-dump profile、访问和删除证据，复用 `PRIV-005`/`SEC-003` |
| T14 供应链替换 | 修改源码、依赖、资源或配置后签名 | 已有 source manifest/provenance 审计草案 | 无干净构建、最终 ELF/SBOM、签名仪式闭环，复用 `BUILD-001`/`GEN-*`/`SBOM-001`/`KEY-001` |

## 6. 新问题证据

### CONFIG-004（P1）：产品 OTA overlay 继承开发安全 profile

证据：

1. `sdkconfig.product-ota.defaults:1-18` 只声明业务/OTA/rollback，不关闭 Kconfig 中默认开启的开发 PoP、`ws://` 和 USB control，也不启用 Secure Boot、Flash/NVS Encryption、anti-rollback 或关闭 debug surface。
2. `main/Kconfig.projbuild:126-165` 将 fixed PoP、insecure WS 和 USB control 默认设为 `y`。
3. 当前最终配置为 `# CONFIG_SECURE_BOOT is not set`、`# CONFIG_SECURE_FLASH_ENC_ENABLED is not set`、`# CONFIG_NVS_ENCRYPTION is not set`、`# CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK is not set`，见 `sdkconfig:697,803-805,4188`。
4. 当前最终配置同时开启开发 PoP/insecure WS/USB control、UART 和 USB Serial-JTAG 控制台、INFO 日志和 OCD-aware debug，见 `sdkconfig:1017-1023,2401,2833-2855,2989,3440-3454`。
5. `partitions.customwake.csv:2-8` 没有明确的 NVS key partition 或受控 core-dump partition。

影响：名为 product/OTA 的 overlay 容易被误用为量产配置；仅凭“OTA/rollback 已启用”不能证明镜像、凭据、调试面和 anti-rollback 安全。

关闭条件：建立独立 release overlay 和机器可判定的 fail-closed 配置断言；干净 reconfigure/build 后保存最终 sdkconfig、bootloader/app/partition hash；eFuse 操作另行审批并逐设备留证。完整建议基线见 `docs/review-2026-08-02/release-security-profile.md`。

### SEC-004（P1）：OTA 缺少独立发布签名和 key ID

证据：

- `main/nuwax_ota_new/nuwax_ota_manifest.h:19-43` 的 manifest 只有 URL、版本、目标、sequence、size 和 SHA-256，没有 signature、algorithm 或 `key_id`。
- `main/nuwax_ota_new/nuwax_ota_manifest.c:220-290,293-343` 只解析和校验上述字段。
- `main/nuwax_ota_new/nuwax_ota_transport.c:326-337,670-724` 校验 TLS、hash 和 app descriptor，但当前 Secure Boot 关闭，`esp_ota_end()` 不能替代未启用的发布者签名链。

SHA-256 只证明下载内容与云命令中的 hash 相同；若云控制面能同时替换 URL、image 和 hash，设备不能区分授权发布与攻击者构造的有效 ESP image。HTTPS 认证下载服务器，不等价于离线发布授权。

关闭条件：对规范化 manifest 进行离线 release key 签名，至少覆盖 protocol/schema、update/request identity、URL 或不可变对象标识、version、release sequence、size、SHA-256、board/chip/project/flavor/partition/model compatibility、有效期和 `key_id`；设备在写 flash 前验证签名和 key policy；同时启用 Secure Boot V2 并验证签名 key 轮换/吊销。

### SEC-005（P1）：动态云配置没有来源真实性和防回滚

证据：

- `main/nuwax_provision_new/nuwax_cloud_config.h:12-21` 的持久结构只有 `version/device_id/cloud_url/device_secret`。
- `main/nuwax_provision_new/nuwax_provision_main.c:763-803` 只接收 `deviceId/gatewayUrl/deviceSecret`，随后写 pending。
- `main/nuwax_provision_new/esp32s3-idf6-provisioning-contract.json:182-188` 明确把 `credentialId/bundleVersion/signature` 列为 removed fields。
- `main/nuwax_provision_new/nuwax_cloud_config.c:100-108,183-235` 只验证本地 UID、URL 和 token 字符规则，没有签名、公钥、版本 floor 或同版本内容冲突检查。
- 启动日志 `main/nuwax_ai_partner_main.c:15519-15523` 把配置标为 `runtime-signed`，与实际协议不符，会误导诊断和发布审计。

Security 2 证明对端知道 PoP，并保护 BLE 会话；它不证明 URL/secret 由授权后端签发，也不阻止持有 PoP 的旧 APP 或重放者安装旧配置。

关闭条件：恢复带 `credentialId`、单调 `bundleVersion`、签名算法/`key_id` 和 signature 的 bundle；签名覆盖 device UID、URL、credential ID、secret 的不可逆摘要、版本和用途；设备保存 accepted floor，允许完全相同 bundle 幂等，拒绝低版本和同版本不同内容；日志只在签名验证成功后使用 `runtime-signed`。

### SEC-006（P1 历史项）：OTA queue connection epoch 已完成 A 级静态修复

当前证据链：

1. `on_ws_text()` 把当前 `ws_connection_epoch` 传入 OTA handler，见 `main/nuwax_ai_partner_main.c:3979-4002`。
2. `ota_command_item_t` 保存 `connection_epoch`，见 `main/nuwax_ota_new/nuwax_ota_device.c:54-62`。
3. handler 在解析后以该 epoch 检查当前认证状态，reserve 时在 `s_lock` 内再次比较，并把 epoch 写入 item，见 `:1319-1321,1397-1401,1462-1499`。
4. controller 在任何 manifest 持久化和 active 状态提交之前执行 `nuwax_ota_device_cloud_authenticated(item->connection_epoch)`，旧 epoch 失败即释放 reservation，见 `:778-798,853-905`。
5. `nuwax_ota_device_set_cloud_authenticated()` 同时发布 bool 和 epoch；cloud-ready 也要求同一 epoch，见 `:1547-1585`。

这已经切断原始复现链：epoch N 的 item 若尚未到达 controller acceptance 就断线，epoch N+1 的 authenticated 状态不能使它通过。代码把 `:792` 的有锁比较定义为 acceptance 线性化点；通过后持久化 context 并允许已接受 OTA 在重连后继续，是恢复语义，不再属于“旧未接受命令被新连接放行”。

残余边界：本轮没有执行 host/设备交错测试；`ota_result_ack` 依靠当前 RX callback 唯一性和 request/update/status 匹配，没有把 callback epoch 显式传入 matcher。后续 B/D 必须覆盖断线发生在 reserve、queue send、dequeue、acceptance 前后和 terminal ACK 等位置，证明旧 item fail-closed、已接受事务可恢复、无跨 epoch ACK 误归属。在这些证据完成前，`SEC-006` 记为“静态修复，B/D 开放”，不记为量产验收通过。

### SEC-007（P1 历史项）：设备身份 cache 已完成 A 级静态修复

当前证据：

- `main/nuwax_provision_new/nuwax_device_uid.c:41-75,158-165` 每次初始化先从 factory MAC/eFuse 加 domain separator 派生权威 16-byte UID。
- `:120-156,167-184` 随后读取 NVS cache；只有与硬件派生值完全相等才标记 cache match。合法但不一致的 cache 只产生告警，运行时仍使用硬件值，并尝试重写 cache。
- `main/nuwax_provision_new/nuwax_device_uid.h:6-11,36-40` 已明确 NVS 不是权威来源。

因此改写 `nuwax_uid/device_uid_v1` 不能再改变 hello、BLE username、QR 或云配置所使用的设备身份，原 `SEC-007` 触发链已静态消除。UID 本身不是秘密，比较是否恒定时间不影响此真实性结论；制造 UID-MAC-批次绑定、NVS 篡改/擦除/迁移和持久化失败仍需 B/D 证据。`SEC-007` 记为“静态修复，B/D 开放”。

### KEY-001（P1）：密钥生命周期与 RMA 未闭合

源码具有使用 secret/verifier/hash 的局部实现，但没有统一 key inventory、owner、环境隔离、生成熵源、注入证明、激活、轮换、双 key grace、吊销、泄漏响应、销毁、RMA、签名仪式和审计记录。详细要求见 `docs/review-2026-08-02/key-lifecycle.md`。

## 7. 已有控制及其证明边界

| 控制 | 可以证明 | 不能证明 |
|---|---|---|
| BLE Security 2 | 协议使用 SRP6a/AES-GCM，生产代码分支可只存 salt/verifier | 当前固定 PoP 安全、APP 所有权、制造注入、轮换/吊销 |
| WSS CA bundle | 正确时间和证书链下可认证 TLS 主机；hostname check 未跳过 | 当前 `ws://` 被禁用、云账号未受损、secret 生命周期完整 |
| WebSocket 101 + token | 当前正式服务契约在 Upgrade 前校验 token；客户端严格校验 101/Accept | 所有后端都履行相同契约、每条命令有用户级授权/防重放 |
| sensitive buffer zeroing | header request 和 hello JSON 的显式缓冲在发送后清零 | TLS/heap/stack/core dump 中不存在其他副本；编译器/LTO 下全部清零已验证 |
| OTA HTTPS + SHA | 传输目标和下载字节与 manifest hash 一致 | manifest 是授权发布、私钥未泄露、Secure Boot/anti-rollback 已启用 |
| app rollback + NVS sequence | 新镜像健康失败可回滚；正常 NVS 下拒绝较低 sequence | eFuse anti-rollback；NVS 被擦/改后仍保持 floor |
| 双 OTA slot/pending transaction | 掉电恢复设计有明确 identity/hash 事务 | bootloader/model/partition/NVS schema 同步升级；所有掉电点已实测 |

## 8. B/C/D 关闭矩阵

| 等级 | 必须执行的验证 | 关键断言 |
|---|---|---|
| B | 签名 cloud bundle parser、manifest signature parser、epoch queue 的 host/fuzz | tamper/重放/同版本冲突/错误 key/旧 epoch 全部 fail-closed；无 secret 输出 |
| B | secret scanner、发布包 allowlist、日志静态检查 | 当前树、Git 历史、ELF strings、partition image、文档和包中无非 allowlist secret/private key |
| C | 独立 release overlay 干净 reconfigure/build | 最终 sdkconfig 满足 profile；开发开关或 unsigned asset 使构建失败；产物/hash/SBOM/签名记录绑定 |
| C | 双签名 key 轮换构建 | active/next key、撤销旧 key、错误 key ID 和 expired policy 结果确定 |
| D | 经书面批准的 Secure Boot/Flash/NVS Encryption/eFuse 样机 | 读取/改写 flash、未签名 app、降级镜像、UART/USB/JTAG 访问均按 profile 被拒绝 |
| D | BLE/云/OTA 攻击测试 | 未知 PoP、旧 bundle、MITM、错误证书/DNS/时间、CDN 替换、云断线重连旧 epoch 全部拒绝且可恢复 |
| D | 掉电/RMA/恢复 | 每个 NVS/OTA commit cut 保持 old/new 一致；RMA 清除用户 secret 但不降低 Secure Boot/anti-rollback |

## 9. 最终判定

- `B0-18`：A 级静态威胁分析完成。
- 当前配置：开发/集成测试可用，不是量产安全 profile。
- 发布状态：阻塞。`CONFIG-001/002/004`、`NVS-001/002/003`、`SEC-001~007`、`KEY-001` 及对应 B/C/D 证据未关闭前，不得宣称安全发布、量产 OTA 或 RMA 已通过。
- eFuse 是不可逆操作，本文不构成执行授权。
