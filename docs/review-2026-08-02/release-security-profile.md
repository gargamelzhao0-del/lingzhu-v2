# B0-18 量产发布安全配置基线

生成日期：2026-08-03  
状态：**建议基线，尚未实现；A 级静态差异分析完成，B/C/D 验证未执行；量产发布阻塞**

本文定义 `NuwaxAIPartner-v2` 从当前开发/集成配置迁移到量产配置时必须满足的安全属性。它不是可直接烧录的 `sdkconfig`，也不证明任何设备已启用 Secure Boot、Flash Encryption、NVS Encryption、anti-rollback 或 eFuse 策略。

本轮未构建、未运行测试、未访问 COM5、未读写 NVS、未执行 OTA、未烧录，也未执行任何 eFuse 操作。eFuse 和量产密钥注入均为不可逆或高影响流程，必须经过独立书面批准、样机验证和逐设备留证。

## 1. 当前判定

当前 `sdkconfig.defaults;sdkconfig.product-ota.defaults` 组合是可用于开发集成的产品功能候选，但不是量产安全 profile：

- `sdkconfig:697,803-805,4188` 显示 bootloader anti-rollback、Secure Boot、Flash Encryption、NVS Encryption 均未启用；
- `sdkconfig:1017-1023` 显示固定开发 PoP、`ws://` 动态配置和 USB 配网控制均启用；
- `sdkconfig:807,2401,2833-2855,2989,3440-3446,5310-5316` 显示 ROM download mode、USB Serial/JTAG、UART/USB 控制台、OCD-aware debug，以及应用与 bootloader 的 INFO 默认日志开启；
- `sdkconfig.product-ota.defaults:1-150` 只覆盖业务、OTA、rollback、模型、音频、BLE 和网络资源参数，没有覆盖上述安全开关；
- `partitions.customwake.csv:2-8` 没有 NVS key 分区、独立受控恢复分区或 core-dump 分区；
- `main/Kconfig.projbuild:126-165` 明确把固定 PoP、明文 WS 和 USB 控制定义为开发/测试能力；
- `main/Kconfig.projbuild:1514-1521` 的 trycloudflare 固定 CA 是开发隧道能力，不应默认进入正式域名产品配置。

因此 `CONFIG-004`、`CONFIG-001`、`CONFIG-002`、`NVS-002` 和 `SEC-001~003` 仍开放。本文提出的配置不能替代 `SEC-004~007` 与 `KEY-001` 所要求的协议、身份和密钥生命周期改造。

## 2. Profile 分层

| Profile | 允许用途 | 安全边界 | 禁止用途 |
|---|---|---|---|
| `DEV-INTEGRATION` | 本地网关、调试、协议联调、受控实验室 | 可保留串口、INFO 日志、固定测试 PoP 和明确标记的测试凭据；设备与网络必须隔离 | 交付用户、正式云、生产签名、量产 OTA、RMA 返回用户 |
| `FACTORY-PROVISIONING` | 受控产线首次烧录、密钥注入、功能测试 | 仅受控工站可访问；使用量产签名镜像；注入后执行 read-back 摘要验证和审计 | 暴露量产私钥、使用共享 PoP、让设备以工厂调试态出厂 |
| `PRODUCTION` | 正式交付、正式云、量产 OTA | 启动链和静态数据保护开启；WSS-only；每设备凭据；签名配置和 OTA；调试面最小化 | 明文 WS、固定 PoP、测试 CA/测试私钥、无授权本地控制、未签名镜像/配置/OTA |
| `RMA-SERVICE` | 授权维修、诊断、清除用户数据、返厂验证 | 仍保持 Secure Boot、Flash Encryption 和 anti-rollback；只运行生产根信任签名的受限服务镜像 | 降级为开发 profile、烧入未签名固件、导出用户凭据、清除硬件安全 floor |

`FACTORY-PROVISIONING` 和 `RMA-SERVICE` 是受控操作流程，不是面向用户永久运行的固件模式。一个设备不得仅凭 GPIO、串口命令或普通云命令从 `PRODUCTION` 降级到这两个 profile。

## 3. 当前与量产目标差异

| 控制面 | 当前静态事实 | `PRODUCTION` 必须达到的属性 | 关闭证据 |
|---|---|---|---|
| 启动真实性 | Secure Boot 关闭 | Secure Boot V2；bootloader、app 和允许的恢复镜像均由批准的 release root 签名；未知签名 fail-closed | 干净构建配置、签名记录、eFuse 摘要、未签名/错误 key/损坏镜像实机拒绝 |
| flash 机密性 | Flash Encryption 关闭 | Release 模式 Flash Encryption；禁止可逆开发加密路径；下载/调试策略与恢复方案明确 | 逐设备 eFuse 摘要、flash 原始读取不可恢复明文、授权升级/回滚仍可用 |
| NVS 机密性 | NVS Encryption 关闭；分区表无 key 分区 | NVS Encryption 和合适的 key partition；Wi-Fi、cloud secret、BLE verifier 等静态数据受保护 | 分区 hash、密钥注入记录、断电/损坏/迁移测试、原始 flash 检查 |
| anti-rollback | app rollback 开启；bootloader anti-rollback 关闭；`release_sequence` 在可变 NVS | bootloader/eFuse secure-version floor 与应用 release policy 协同；普通 OTA 不得降低硬件 floor | 高/低 secure version 镜像实机矩阵、floor 留证、RMA 不降级 |
| 云传输 | 代码支持 CA bundle WSS，但动态配置允许 `ws://` | 只允许 `wss://`；证书链、hostname、时间有效性均 fail-closed；正式域名 trust policy 明确 | parser/配置单测、MITM/错误 CA/错 hostname/错时间实机拒绝 |
| 云配置 | `{deviceId,gatewayUrl,deviceSecret}`，无签名和版本 floor | 后端签名 bundle，含 `credentialId`、单调 `bundleVersion`、用途、设备 UID、key ID 和签名 | 签名/重放/同版本冲突/错误设备 host 测试及实机重配网 |
| OTA 发布授权 | HTTPS、SHA-256、目标和版本校验；无独立 release signature | 规范化 manifest 离线签名，包含 `key_id`；镜像同时受 Secure Boot 保护；CDN/云不能单独授权新代码 | 错误/撤销 key、篡改字段、云/CDN 替换、双 key 轮换矩阵 |
| OTA 命令新鲜度 | item 已绑定 WebSocket epoch，reserve/消费按同一 authenticated epoch 校验；仅有 A 级静态证据 | 保持现有 fail-closed 设计并冻结 acceptance 线性化点；断线后未接受的旧 epoch 不得被新连接放行 | 断线/重连/乱序/积压命令 B/D 测试 |
| BLE 配网 | Security 2；当前固定开发 PoP；生产分支可读 NVS salt/verifier | 每设备随机 PoP 或等价所有权凭据；设备只保存 salt/verifier；所有权重置和吊销受控 | 批次唯一性、未知 PoP 拒绝、重放/暴力限制、换绑/RMA 测试 |
| 设备身份 | 每次从 MAC/eFuse 派生权威 UID；NVS 只作匹配缓存，不一致时告警并修复；仅有 A 级静态证据 | 保持硬件身份唯一权威；cache 不得影响 hello/QR/配网身份 | cache 篡改/擦除/迁移 B/D 测试，制造 UID-MAC-批次绑定记录 |
| UART/USB/JTAG | UART/USB 控制台和 USB Serial/JTAG 开启 | 默认关闭应用控制台和 USB 配网控制；JTAG/ROM 下载策略按威胁模型最小化；授权维修入口有认证、时限和审计 | 最终 sdkconfig/eFuse 摘要，物理端口负面测试，维修模式审计 |
| 日志 | 默认 INFO；已有正文/身份泄漏问题 | 默认 WARN/ERROR 或结构化最小日志；禁止音频、会话正文、Wi-Fi/secret/token、签名 URL；安全事件用非敏感 reason code | golden log、secret/PII scanner、故障注入日志检查 |
| core dump | 当前分区表无 core-dump 分区，未形成正式策略 | 明确二选一：量产禁用；或加密、鉴权、限量、TTL 和删除。不得无意开启明文 dump | 最终配置/分区/读取权限和故障实机证据 |
| 构建/发布 | product overlay 继承公共开发默认 | 独立 release overlay；任何开发开关、测试 key、unsigned asset 或漂移使构建失败 | 干净可复现构建、最终 sdkconfig、ELF/map/SBOM/hash、签名与审批包 |

## 4. 建议的量产配置断言

量产配置必须通过机器可判定断言，而不是依赖人工阅读 `sdkconfig`。具体 Kconfig 符号需在锁定的 ESP-IDF 6.0 工具链中由安全负责人复核；以下描述安全属性，不授权直接修改或烧写：

1. `CONFIG_SECURE_BOOT` 必须开启并选择 ESP32-S3 支持的 Secure Boot V2；构建必须使用发布公钥链对应的签名流程。
2. `CONFIG_SECURE_FLASH_ENC_ENABLED` 必须开启并处于 release 策略；量产设备不得依赖可重复明文下载的开发模式。
3. `CONFIG_NVS_ENCRYPTION` 必须开启；分区表必须增加与 IDF 方案一致的 NVS key 分区，并验证首次生成、产线预生成、损坏恢复和 OTA 兼容策略。
4. bootloader app anti-rollback 必须开启并分配经过评审的 secure version。NVS `release_sequence` 只能作为业务层补充，绝不能标记为 eFuse anti-rollback。
5. `CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE`、`CONFIG_NUWAX_CLOUD_CONFIG_ALLOW_INSECURE_WS`、`CONFIG_NUWAX_PROVISION_USB_CONTROL_ENABLE` 必须关闭。
6. `CONFIG_NUWAX_TLS_TRYCLOUDFLARE_CA_ENABLE` 在正式域名 profile 中必须关闭，除非正式架构书面批准该域名和根证书用途。
7. UART/USB 控制台、USB Serial/JTAG 和 JTAG 必须符合已批准的量产/RMA策略；不能只改应用日志等级而保留可执行控制命令。
8. 默认日志不得高于经过脱敏审计的等级；任何调试日志临时开启都必须有签名授权、到期时间和回收证据。
9. 分区表、bootloader、app、`srmodels`、NVS schema 和安全配置必须组成一个不可混配的 release identity tuple。
10. CI 必须在配置阶段拒绝固定 PoP、`ws://`、测试私钥路径、空/测试发布 key ID、未签名 manifest schema、开发 CA 和未批准的 debug surface。

## 5. Secure Boot、Encryption 与 anti-rollback 的部署顺序

不可逆安全开关不能与普通 app-only OTA 混在同一发布动作中。建议的受控阶段如下：

| 阶段 | 允许动作 | 必须输出的证据 | 停止条件 |
|---|---|---|---|
| S0 设计冻结 | 冻结密钥 owner、算法、slot、secure version、下载/JTAG/RMA策略 | 威胁模型、密钥仪式、profile、恢复演练计划 | 任一 owner、恢复路径或兼容边界未决 |
| S1 主机/构建验证 | 构建签名镜像、错误镜像和双 key 轮换样本 | 可复现构建、签名验证、配置断言、artifact hash | 开发开关或 unsigned artifact 可进入发布包 |
| S2 可牺牲样机 | 仅在书面批准的非用户样机执行 eFuse/加密实验 | 烧写前后 eFuse 摘要、串号、镜像 hash、操作者和时间 | 任何不可解释 bit、恢复失败或降级路径存在 |
| S3 小批 canary | 限定批次产线注入、OTA/RMA/掉电/错误 key 验收 | 每设备注入摘要、良率、异常隔离和销毁记录 | 批次唯一性、可恢复性或审计记录不完整 |
| S4 量产 | 锁定工站权限和发布根，按批准 SOP 执行 | 逐设备追踪、双人审批、签名日志、抽检 | 无审批变更、key compromise、scanner/release gate 失败 |

任何阶段都不得用“设备仍能启动”替代安全断言。尤其需要验证未签名镜像、低 secure version、错误 flash key、被篡改 NVS 和禁用端口的负向结果。

## 6. 云配置与 OTA 的量产契约

### 6.1 云配置 bundle

建议规范化字段至少包括：`schema`、`deviceId`、`gatewayUrl`、`credentialId`、`secretDigest` 或加密 secret envelope、`bundleVersion`、`issuedAt`、`expiresAt`、`purpose`、`keyId`、`algorithm` 和 `signature`。签名必须覆盖所有有安全意义的字段。

设备应保存 accepted version/digest，允许完全相同 bundle 幂等重放，拒绝低版本和同版本不同内容。当前 `main/nuwax_provision_new/nuwax_cloud_config.h:12-21` 与 `main/nuwax_provision_new/nuwax_provision_main.c:763-803` 不具备这些字段，故该要求仍属 `SEC-005` 的待实现项。

### 6.2 OTA manifest

建议签名覆盖：schema、request/update identity、不可变对象标识、URL 或 URL 对象摘要、version、release sequence、size、SHA-256、board/chip/project/flavor、partition/model/NVS compatibility、secure version、有效期和 `key_id`。

HTTPS 与 SHA-256 继续保留，但只证明传输对象与云命令一致；它们不能替代离线发布授权。当前 `main/nuwax_ota_new/nuwax_ota_manifest.h:19-43` 没有 signature/key ID，故仍属 `SEC-004`。

### 6.3 命令连接边界

当前 `main/nuwax_ai_partner_main.c:3979-4002` 已把 epoch 传给 OTA，`main/nuwax_ota_new/nuwax_ota_device.c:54-62,778-798,1462-1499` 已在 item、reserve 和 controller acceptance 三处绑定/检查同一 epoch。原 `SEC-006` 已完成 A 级静态修复；量产门仍要求断线发生在 acceptance 前后、terminal ACK 和重连恢复的 B/D 全交错测试，并应把 acceptance 线性化点写入协议/实现注释，防止未来重构退化。

## 7. 调试、日志和维修策略

### 7.1 正常用户设备

- 不提供未认证的串口/USB命令；
- 不输出会话正文、音频、密码、token、device secret、BLE PoP、签名 URL 或完整网络身份；
- 只保留版本、匿名错误码、重启原因、受限计数器和经过评审的安全事件；
- JTAG、ROM 下载和 UART 下载能力按批准的物理攻击模型处理，不得由普通软件开关重新启用；
- 远程诊断必须是签名、限时、最小权限并有服务端和设备端审计。

### 7.2 RMA 设备

RMA 不得通过烧写开发镜像来获得诊断能力。应使用由生产信任根签名、权限受限、带到期/工单身份的 service image；它不得导出用户 secret，不得降低 secure version，不得关闭 Secure Boot/Encryption，也不得把测试 PoP 写回设备。

维修结束必须执行用户数据处理矩阵、重新验收安全 profile，并生成“接收状态 -> 操作 -> 擦除/保留 -> 返回状态”的审计记录。详细密钥处置见 `docs/review-2026-08-02/key-lifecycle.md`。

## 8. 发布门

### B 级：host/协议/静态门

- 配置断言拒绝所有开发开关、测试证书/私钥、明文 URL、固定 PoP 和 unsigned schema；
- cloud bundle、OTA manifest、epoch queue 的 tamper/replay/wrong-key/same-version-conflict 测试全部 fail-closed；
- secret/PII scanner 覆盖源码、配置、文档、Git 历史、构建产物、分区镜像和发布包；
- 签名 key active/next/revoked/unknown/expired 矩阵通过；
- RMA/factory reset 状态机和擦除/保留矩阵通过 host 模型检查。

### C 级：干净构建和产物门

- 使用独立 release overlay 干净 reconfigure/build；保存最终 `sdkconfig`、partition、bootloader/app/model hash、ELF/map、SBOM、toolchain 和签名记录；
- 产物检查证明无固定 PoP、Wi-Fi 密码、device secret、测试私钥、明文调试命令和未经批准的 CA；
- release identity tuple 与 OTA manifest/设备 banner 完全一致；
- 两次隔离构建的可复现边界和差异得到解释。

### D 级：样机/产线/攻击验证门

- 未签名、错误 key、撤销 key、低 secure version、损坏 bootloader/app/NVS 均按预期拒绝并可受控恢复；
- 原始 flash 读取不能恢复 Wi-Fi/cloud secret；未知 PoP、明文 WS、MITM、错 hostname/CA/time 均被拒绝；
- OTA 断线/重连/掉电/回滚/双 key 轮换和 RMA service image 完整通过；
- UART/USB/JTAG/下载模式符合批准策略，且日志与 core dump 无敏感数据；
- 每台设备 eFuse、密钥注入、镜像、批次和最终验收记录可追溯。

## 9. 量产放行清单

以下任一项为“否”均阻塞安全放行：

1. 是否存在获批且机器可验证的独立 `PRODUCTION` overlay？
2. Secure Boot V2、Flash Encryption、NVS Encryption 和硬件 anti-rollback 是否完成 S0-S3 证据？
3. 是否使用每设备 BLE/云凭据，且固定开发 PoP 与测试 key 不在产物中？
4. cloud config 与 OTA 是否分别具有独立签名、key ID、版本 floor 和轮换/吊销机制？
5. WSS-only、证书/hostname/time fail-closed 是否通过负向测试？
6. debug、console、JTAG、download、日志和 core dump 是否符合批准的生产/RMA策略？
7. UID 是否每次从硬件派生并拒绝被篡改 cache？
8. NVS 全擦、factory reset 与 RMA 是否保留硬件安全 floor 并清除用户 secret？
9. 发布包是否绑定 source manifest、toolchain、SBOM/CVE/VEX、artifact hash、签名与双人审批？
10. 是否保留每设备制造、注入、验收、维修、吊销和销毁的不可抵赖审计记录？

## 10. 最终结论

本文件完成了 `CONFIG-004` 的量产差异与关闭标准定义，但没有实现量产配置。当前工程仍只能判定为开发/集成候选；在 `CONFIG-001/002/004`、`NVS-001/002/003`、`SEC-001~007`、`KEY-001` 及 B/C/D 证据关闭前，不得宣称量产安全、正式 OTA 安全或 RMA 安全已经通过。
