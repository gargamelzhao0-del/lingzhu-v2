# B0-18 密钥、凭据与信任根全生命周期基线

生成日期：2026-08-03  
状态：**建议基线，尚未实现；A 级静态盘点完成，B/C/D 验证未执行；`KEY-001` 开放**

本文定义 `NuwaxAIPartner-v2` 的密钥、凭据、公开信任锚和设备身份从生成到销毁的最小闭环。当前源码包含局部使用逻辑，但没有统一 owner、生成、注入、激活、轮换、吊销、泄漏响应、RMA 和审计流程，因此本文是关闭 `KEY-001` 的要求，不是现状证明。

本轮未生成或导出密钥，未读取设备凭据，未构建、未烧录、未访问 COM5/NVS、未执行 OTA 或 eFuse 操作。

## 1. 基本原则

1. **用途隔离**：Secure Boot、OTA manifest、cloud device secret、BLE PoP、flash/NVS encryption 不得复用同一秘密。
2. **环境隔离**：development、CI、staging、factory、production、RMA 使用不同根和账户；测试 key 不能被 production trust store 接受。
3. **最小暴露**：发布/启动私钥保存在 HSM 或等价不可导出边界；设备只保存验证公钥、公钥摘要、verifier 或每设备秘密。
4. **每设备唯一**：BLE PoP、cloud secret、flash/NVS encryption key 均应逐设备唯一；不得以批次共享秘密替代。
5. **双人控制**：生产根生成、激活、轮换、吊销、eFuse 写入策略和紧急恢复必须至少双人审批，执行与审计角色分离。
6. **先验证后激活**：新 key 先作为 `NEXT` 分发和验证，再切 `ACTIVE`，最后撤销旧 key；禁止无回退的一步替换。
7. **不可逆操作留证**：eFuse、Secure Boot digest revoke、Flash Encryption release 和 secure-version floor 变更必须逐设备保留前后摘要。
8. **安全 floor 不在可变 NVS**：`release_sequence` 是应用策略，不是 eFuse anti-rollback；NVS 擦除不能降低硬件 secure version。
9. **日志零秘密**：任何私钥、PoP、Wi-Fi password、device secret、encryption key 或完整签名 URL 都不得进入日志、core dump、工单和普通遥测。
10. **失败关闭**：未知 key ID、签名错误、版本回退、身份不匹配、注入记录缺失或吊销状态不明时，不得继续量产、配置或 OTA。

## 2. 资产清单与当前状态

| 资产 | 类型/敏感度 | 当前代码/配置事实 | 量产 owner | 当前缺口 |
|---|---|---|---|---|
| Secure Boot release root | 非对称私钥，最高机密；设备保存 digest/public trust | `sdkconfig:803` 未启用 Secure Boot | Product Security + Release Engineering | 未定义 key、slot、签名仪式、轮换/吊销、灾备和 eFuse 记录 |
| OTA manifest signing key | 非对称私钥，最高机密 | `main/nuwax_ota_new/nuwax_ota_manifest.h:19-43` 无 signature/`key_id` | Release Authority，与云/CDN分权 | 协议和设备验证均未实现，`SEC-004` |
| Flash Encryption key | 每设备对称 key，最高机密/宜不可导出 | `sdkconfig:805` 未启用 | Manufacturing Security | 未定义生成方式、release mode、注入验证、返修/报废策略 |
| NVS Encryption key | 每设备对称 key，最高机密 | `sdkconfig:4188` 未启用；`partitions.customwake.csv:2-8` 无 key partition | Manufacturing Security | 分区、生成/注入、迁移、损坏恢复和 crypto erase 未定义 |
| BLE Security 2 PoP | 每设备所有权秘密 | 当前 `sdkconfig:1017-1019` 使用固定开发 PoP；生产分支在 `main/nuwax_provision_new/nuwax_provision_main.c:945-968` 读取 `nuwax_factory/sec2_salt`、`sec2_verifier` | Device Identity/Manufacturing | 每设备生成、PoP安全交付、轮换、换绑、吊销和RMA未闭合，`SEC-001` |
| BLE salt/verifier | verifier 数据，敏感但非明文 PoP | 生产分支可从 NVS 读取；当前开发分支由固定 PoP 运行期生成 | Device Identity/Manufacturing | NVS 未加密；无 version/key ID/注入证明；释放前无统一 scrub |
| Cloud device secret | 每设备认证秘密，最高机密 | `nuwax_cloud_config_t` 明文 blob 保存；同时用于 `X-Bridge-Token` 和 hello；发送后局部缓冲清零 | Cloud Credential Authority | 无 `credentialId`、签名 bundle、版本 floor、双 secret grace、吊销/RMA，`SEC-005` |
| Wi-Fi credentials | 用户秘密/个人数据 | `main/wifi_manager.c:28-32,58-110` 写入 `nuwax_wifi` NVS；ESP Wi-Fi driver 也保存 | Device owner；设备仅托管 | NVS/flash 明文、重复存储、删除/RMA/迁移证据不足，`NVS-002/003` |
| TLS trust anchors | 公钥/证书，非秘密但完整性关键 | `main/ws_transport.c:712-765` 用 ESP CA bundle；`main/ws_transport.c:146-147,719-724` 含 trycloudflare GTS Root R4 | Product Security/PKI | 正式域名 anchor policy、更新/吊销、时间失败策略和开发 CA 排除门未闭合 |
| Device UID/MAC binding | 稳定标识，不是秘密；真实性关键 | `main/nuwax_provision_new/nuwax_device_uid.c:41-75,158-184` 每次从 factory MAC/eFuse 派生权威值，NVS 仅作匹配缓存，不一致时修复 | Device Identity/Manufacturing | 原 `SEC-007` 已静态修复；制造 UID-MAC-批次绑定和 cache 篡改/迁移 B/D 证据仍未闭合 |
| OTA `release_sequence` | 单调策略状态，不是 key | `main/nuwax_ota_new/nuwax_ota_release_policy.h:12-23` 有基线/固件序号；accepted floor 在 NVS | Release Engineering | 可被 NVS 擦除/修改；不能代替 secure version，`NVS-001/CONFIG-002` |
| 测试 PoP/测试私钥 | 测试秘密，禁止生产 | 固定 PoP 存在；工作区已识别 `cloud-gateway/test-device-config-signing-private-key.pem` | Test Infrastructure | 只有人工约定；无自动 allowlist、expiry 和发布包阻断，`SEC-002/003` |

设备 UID、TLS CA 和签名公钥不是秘密，但其完整性、来源和版本必须与秘密同样纳入生命周期管理。

## 3. 角色与职责分离

| 角色 | 可以做什么 | 不得单独做什么 |
|---|---|---|
| Product Security | 批准算法、profile、trust store、key policy 和吊销 | 独自生成并使用生产私钥签名发布物 |
| Release Engineering | 生成可复现 release candidate、提交签名请求、核对产物 hash | 访问可导出的生产根私钥、修改审计记录 |
| Release Authority | 在 HSM 内签名获批 manifest/image，管理 active/next key | 同时修改源码、构建产物、云 manifest 和审批 |
| Manufacturing Security | 生成/注入每设备 key、写入获批 eFuse、输出注入摘要 | 获取 release signing 私钥、把共享测试凭据注入量产设备 |
| Cloud Credential Authority | 签发/轮换/吊销 device secret 和签名 cloud bundle | 修改设备 Secure Boot/Flash key 或跳过 ownership 验证 |
| RMA Operator | 在工单授权下执行签名 service workflow 和用户数据擦除 | 导出 secret、降低 secure version、关闭 Secure Boot/Encryption |
| Auditor | 读取不可变摘要、审批和事件记录 | 持有生产签名或注入能力 |

同一人员在紧急情况下跨角色操作时，必须有事前批准或双人 break-glass、限定时窗和事后独立复核。

## 4. 统一状态机

每个 key/credential 至少具有以下状态：

`PLANNED -> GENERATED -> STAGED -> ACTIVE -> RETIRING -> REVOKED -> DESTROYED`

- `GENERATED`：已在批准的熵源/HSM/制造安全边界生成，尚未被任何产品接受；
- `STAGED`：公钥、verifier 或新 credential 已预置，但旧 key 仍是唯一 active 授权；
- `ACTIVE`：可签发、认证或解密；同一用途的 active 数量受策略限制；
- `RETIRING`：只用于验证已有对象或有限 grace，不再签发新对象；
- `REVOKED`：设备/服务必须拒绝；不得因为时钟、网络或 NVS 丢失自动恢复；
- `DESTROYED`：所有 HSM slot、备份、工站缓存和纸面/数字恢复材料按策略销毁并留证。

数据库、设备、HSM 和发布系统中的状态转换必须有同一个 `key_id/credentialId`、版本、时间和审批关联。只删除文件不等于销毁，停止使用也不等于吊销。

## 5. 生成与命名

### 5.1 标识

所有非对称发布 key 使用不可复用 `key_id`，建议包含用途和世代而不泄漏秘密，例如 `ota-prod-2026-q3-g1`。每设备凭据使用后台不可猜测 `credentialId`；ID 不得直接等于 secret。

记录至少包括：算法、key size/curve、用途、环境、owner、生成系统、HSM slot/public digest、生成时间、激活/失效时间、允许验证的最低固件、备份策略和销毁策略。

### 5.2 熵源和算法

- 发布私钥只能在批准的 HSM 或离线签名边界中生成，默认不可导出；
- 每设备对称 key/PoP/cloud secret 使用批准 CSPRNG，禁止序列号、MAC、时间戳或 UID 的可逆派生；
- salt 可公开但必须随机且与 verifier/username 正确绑定；
- 算法、编码和规范化格式必须冻结并用跨语言 test vector 验证；
- 不在本文擅自冻结具体算法。Secure Boot 使用 ESP32-S3/IDF 6.0 经验证的机制；OTA manifest 独立签名算法需由安全负责人结合设备 crypto 支持评审。

### 5.3 发布签名仪式

每次正式发布应执行确定且可复核的签名仪式：

1. Release Engineering 从获批 source/toolchain/config 输入完成隔离的可复现构建，冻结 source manifest、最终 `sdkconfig`、SBOM/CVE/VEX、ELF、bootloader/app/model/partition hash 和测试结果。
2. 独立审阅者核对 release identity、版本、secure version、release sequence、兼容矩阵、例外和用户影响；构建人员不能批准自己的产物。
3. 签名请求只包含规范化 manifest、产物 digest、`key_id`、用途、有效期和审批号，不把私钥或设备秘密带入构建环境。
4. 两名获授权操作者在 HSM/离线签名环境核对显示的 digest 与审批记录；任何字段、长度、环境或 key state 不一致即拒绝。
5. HSM 仅用当前 `ACTIVE` key 签名；私钥不可导出。紧急/break-glass key 使用独立审批、时限和事后复核。
6. 签名返回后由与 HSM 独立的验证器按设备同一规范化规则验证；再把 signature、algorithm 和 `key_id` 组装进不可变发布 manifest。
7. 发布包进行第二次 secret/PII/测试 key 扫描，并验证所有 image signature、hash、SBOM 和 manifest 相互绑定。
8. 审计系统记录签名请求 hash、结果 hash、HSM slot/key ID、操作者、审批、时间、发布目标和最终对象存储 identity；不记录私钥或 secret。
9. 上传/CDN/云发布权限与签名权限分离；云端只能分发已验证的不可变对象，不能重签或替换安全字段。
10. 仪式失败的产物进入隔离并作废；不得手工编辑已签名 manifest 后继续发布。

## 6. 制造注入

每台设备的注入事务建议按以下顺序执行，具体 eFuse 顺序必须以 Espressif 官方流程和获批 SOP 为准：

1. 扫描工单、板号、MAC 和批次，设备从硬件派生 UID；制造系统拒绝重复 UID/MAC。
2. 设备与工站建立受控会话；工站证明其授权和软件版本。
3. 生成或安全装载每设备 Flash/NVS key、BLE PoP 对应 salt/verifier，以及需要的设备证书/初始 cloud bootstrap credential。
4. 写入后只读回非秘密摘要、key purpose、长度、version 和状态；不得把明文 key 写入普通日志。
5. 烧入由生产根签名的候选镜像并验证 boot、加密、端口和 anti-rollback 属性。
6. 在获批阶段写入/锁定 eFuse；保存写入前后摘要、设备 UID、镜像 hash、操作者、工站、时间和审批号。
7. 从工站内存、安全缓存和临时介质清除每设备秘密；生成不可变的注入成功/失败记录。
8. 失败设备进入隔离区，不得通过回退开发配置继续出厂。

产线数据库应保留 key ID、摘要和状态，不应长期保留可导出的 Flash/NVS key 或 BLE PoP 明文。若用户必须通过 QR 获取 PoP，标签生成、运输、APP读取和换绑均属于秘密交付链，必须有防复制和所有权流程。

## 7. 激活与运行期使用

### Secure Boot / Flash / NVS

- 启动时验证签名和 secure version；失败进入不可降级的受控错误状态；
- Flash/NVS key 不通过应用 API 导出；应用只能使用加密后的存储；
- 启动日志只打印 key slot/状态摘要，不打印 key、digest 全值或可用于克隆的材料；
- NVS key partition 损坏不得触发静默全擦后继续作为新身份运行。

### BLE PoP/verifier

- 设备端量产路径只保存 salt/verifier，不保存可直接读取的 PoP 明文；
- 失败尝试需要速率限制、退避和安全计数；
- 已绑定设备的重新配网必须要求本地物理确认或已认证所有权，不得仅凭邻近 BLE 连接覆盖 cloud secret；
- 轮换后旧 verifier 在限定 grace 后删除并拒绝。

### Cloud secret

- cloud bundle 必须签名并绑定 UID、`credentialId`、单调 `bundleVersion` 和用途；
- 设备原子 stage -> 验证 -> commit；旧 secret 只在明确 grace 内接受；
- RAM 中按最短生命周期持有，发送后使用不会被编译器优化掉的安全清零；
- 日志只记录 credential ID 的受限后缀/摘要和 reason code，不记录 secret。

### OTA release key

- 签名输入使用确定性规范化；签名覆盖全部安全字段；
- 设备按 `key_id` 选择信任 key，并检查 active/next/revoked policy；
- 发布私钥与 cloud/CDN 运营权限分离，使云或对象存储单点失陷不能授权恶意镜像；
- Secure Boot 镜像签名和 OTA manifest 签名是两层独立控制，不能互相替代。

## 8. 轮换方案

### 8.1 OTA/发布签名 key

1. 固件 N 接受 `ACTIVE=A` 和 `NEXT=B`，但发布系统仍只用 A 签新 manifest。
2. 通过 A 签名的更新把 B trust material 分发到足够设备，并遥测验证覆盖率。
3. 发布一个 B 签名的 canary；设备报告验证 key ID，不报告敏感材料。
4. 达到覆盖门后 B 变 `ACTIVE`，A 变 `RETIRING`；停止用 A 签新发布。
5. grace 和回滚窗口结束后撤销 A；设备/云拒绝 A。
6. Secure Boot digest slot 的轮换/撤销必须另走 eFuse 书面流程，不能把 OTA key 的软件吊销步骤直接套用到不可逆 eFuse。

### 8.2 Cloud device secret

- 后台签发 `credentialId=B,bundleVersion=n+1`，签名 bundle 同时声明旧 A 的 grace 截止；
- 设备 stage B，用 B 建立经认证连接并得到明确 ACK 后 commit；
- 云端在短 grace 内可接受 A/B，但每次连接只绑定一个 credential 和 connection epoch；
- 设备确认 B 稳定后清除 A；后台随后 revoke A；
- 同版本不同 secret、低版本、错误 UID 和过期 bundle 一律拒绝。

### 8.3 BLE PoP/verifier

仅在已认证所有权、本地确认或 RMA 工单下轮换。新 PoP 必须重新安全交付；设备先写新 salt/verifier 并验证，再删除旧 verifier。不能通过公开云命令或未认证 USB 直接重置。

### 8.4 TLS trust anchors

在旧 anchor 仍有效时通过已签名固件加入新 anchor，完成服务器证书切换验证后再移除旧 anchor。紧急 CA 吊销需要签名固件/策略更新和明确 fail-closed 行为，不能临时关闭 hostname/certificate check。

### 8.5 Flash/NVS Encryption key

这类每设备静态 key 不做普通运行期频繁轮换。需要换 key 时必须使用经 Espressif 机制验证的受控 re-encryption/重置流程，并演练掉电恢复。不能用“全擦 NVS”冒充 key 轮换；全擦会破坏 cloud credential 和 OTA floor，却不自动更换所有底层 key。

### 8.6 Wi-Fi credential

由用户或管理员通过已认证配网流程替换。成功连接新网络并提交后删除旧密码；失败回滚不得在日志暴露新旧密码。factory reset/RMA 必须清除所有保存网络和 ESP driver 副本。

## 9. 吊销与泄漏响应

| 事件 | 立即动作 | 恢复动作 | 不允许的动作 |
|---|---|---|---|
| OTA manifest key 泄漏 | 停止签名、标记 key revoked、冻结发布、识别受影响 manifest | 用未受损 trust path 分发撤销/新 key；审计所有该 key 发布 | 仅替换 CDN 文件或 hash；继续信任旧 key 无截止期 |
| Secure Boot root/私钥疑似泄漏 | 冻结生产和签名，启用双人 incident response | 按预留 digest slot/官方机制在样机验证后轮换/撤销 | 未验证即批量烧 eFuse；用开发镜像“救砖” |
| 单设备 cloud secret 泄漏 | 后台 revoke credential，设备进入需重新绑定状态 | 签发更高 bundleVersion 的新 credential；核对异常连接 | 全 fleet 共用替代 secret；降低 WSS/签名要求 |
| BLE PoP 泄漏 | 暂停远程重配，要求本地所有权确认 | 轮换 PoP/verifier，撤销旧值，审计配置变更 | 保留共享开发 PoP；仅改 QR 文本不改 verifier |
| 工站/批次 key 事件 | 隔离工站、停止批次、冻结未出货设备 | 根据注入审计定位设备，逐设备重新注入/报废 | 把缺失审计的设备按“测试通过”放行 |
| Wi-Fi 凭据泄漏 | 通知 owner，更换网络凭据，撤销旧网络访问 | 设备重配网并清除旧副本 | 把密码写入日志/工单验证 |
| 测试私钥进入发布包 | 立即撤包，确认生产 trust store 是否接受 | 轮换所有可能受影响的测试/生产边界，补 scanner | 以“仅测试 key”为由忽略公开泄漏 |

所有事件必须记录发现时间、受影响 key ID/credential ID、设备/版本范围、封禁时间、恢复版本、用户影响和最终销毁证据。怀疑泄漏与已确认泄漏在初始处置上都应 fail-safe。

## 10. 备份、恢复与销毁

- 生产发布根的备份只允许在同等级 HSM/离线加密介质中，以 M-of-N 分权和异地保管；恢复必须定期演练，但演练 key 不得被生产设备信任。
- 每设备 Flash/NVS key 默认不集中保存可导出副本；若业务连续性确需托管，必须逐设备加密、访问审批、用途限制并达到生产根同等级的审计要求。
- cloud credential 数据库备份必须加密并保留 credential state/version；恢复后不得让已撤销 secret 重新变为 active。
- key 销毁必须覆盖 HSM slot、备份、工站缓存、交换文件、临时包、纸质恢复材料和测试副本；使用批准的 zeroize/介质销毁流程，并由独立人员验证。
- 公钥、证书、key ID、签名和吊销记录通常应长期保留以验证历史发布；销毁私钥不等于删除历史验证证据。
- 设备报废先吊销 cloud credential/ownership 和 BLE 所有权，再执行用户数据 crypto erase/介质清除与物理处置。eFuse bit 不能擦除，必须把最终状态和设备 UID 留在报废记录中。
- 销毁证书至少记录资产 ID、用途、最后状态、覆盖介质、方法、时间、执行者、见证者和例外；无法证明销毁的 key 按“可能泄漏”处置。

## 11. RMA、恢复出厂与报废矩阵

| 资产/状态 | 同一用户维修返回 | 翻新给新用户 | 报废/销毁 |
|---|---|---|---|
| 硬件 UID/MAC | 保留并校验 | 保留，后台解除旧所有权后重新绑定 | 保留审计映射，物理销毁设备 |
| Secure Boot digest/Flash key | 保留，不降级 | 保留，不降级；只能运行生产根签名镜像 | 不尝试“恢复明文”；按介质销毁策略处理 |
| anti-rollback secure floor | 保留或单调提升 | 保留或单调提升 | 记录最终 floor |
| NVS encryption key | 原则上保留；若安全重建则受控 re-encrypt | 按批准 crypto erase/重建流程处理 | 通过密钥不可用化加介质销毁，并留证 |
| Wi-Fi credentials/保存网络 | 工单若需保留必须取得用户授权；默认清除 | 必须清除所有应用和 ESP driver 副本 | 清除/crypto erase |
| Cloud device secret/ownership | 默认 revoke 后重新签发 | 必须 revoke 旧 credential 和 ownership | revoke 并关闭后台身份 |
| BLE PoP/verifier | 可在确认所有权后保留或轮换 | 必须轮换并安全交付新 owner | 吊销/清除可变 verifier |
| 音量、唤醒词、用户设置 | 按用户授权 | 清除 | 清除 |
| OTA accepted identity/NVS release floor | 保留；不得低于硬件/发布 floor | 保留；不得降低 | 记录后销毁 |
| 诊断日志/core dump/音频 | 最小化，维修结束删除 | 必须删除 | 删除并执行介质策略 |

RMA service image 必须由生产信任根签名且受 anti-rollback 约束。RMA 操作不得关闭 Secure Boot/Encryption、烧写开发 PoP、导出 key 或降低 secure version。当前代码没有该闭环，继续归入 `KEY-001` 与 `NVS-003`。

## 12. 审计记录

### 每个发布 key

- `key_id`、用途、算法、public digest/HSM slot；
- generated/staged/active/retiring/revoked/destroyed 时间；
- 双人审批、签名请求 hash、产物 hash、manifest hash；
- 使用次数/发布列表、异常拒绝、轮换和销毁证明。

### 每台设备

- UID、factory MAC、板号、批次、工站和操作者；
- 注入的 credential/key ID 与非秘密摘要；
- eFuse 写入前后摘要、Secure Boot/Encryption/anti-rollback 状态；
- bootloader/app/partition/model hash 和 release identity；
- 量产验收、换绑、cloud secret轮换、OTA、RMA和报废事件；
- 不保存 Flash/NVS key、BLE PoP、Wi-Fi password 或 cloud secret 明文。

审计日志必须 append-only 或具有防篡改保护、访问控制、时间同步、备份、保留期和删除/法律策略。设备本地日志不能作为唯一审计源。

## 13. 自动门禁与验证

### B 级

- secret scanner 覆盖当前树、Git 历史、文档、日志、ELF strings、partition image 和发布压缩包；
- 只允许明确 hash/路径/owner/expiry 的测试 key allowlist；
- cloud bundle、OTA signature、key ID、credential version、双 key grace、revocation 和 epoch 测试通过；
- UID cache 篡改、PoP暴力/重放、同版本不同内容、unknown/revoked key 全部 fail-closed；
- key ceremony 和 RMA 状态机 tabletop 演练完成。

### C 级

- release build 不含测试私钥、固定 PoP、Wi-Fi/cloud secret 或未批准 trust anchor；
- 最终 sdkconfig、partition、ELF/image、SBOM、签名和 public digest 与审批记录绑定；
- active/next key 构建以及撤销旧 key 的兼容矩阵通过；
- 工厂注入工具在无 secret 输出的前提下产生可复核逐设备摘要。

### D 级

- 可牺牲样机验证 Secure Boot/Flash/NVS Encryption、anti-rollback、错误 key、低版本、原始 flash读取和端口策略；
- 小批制造验证唯一性、重复 UID/credential 拒绝、工站断线/掉电、失败隔离和临时缓存清除；
- 实机完成 OTA key、cloud secret、BLE ownership、TLS anchor 轮换与紧急吊销；
- RMA/factory reset/翻新/报废按矩阵执行，并证明用户 secret 已清除、安全 floor 未降低。

## 14. 最终结论

当前工程只有局部密码学和凭据使用机制，没有可审计的全生命周期。`KEY-001` 仍为 P1 量产发布阻塞：在 owner、HSM/制造边界、注入证明、双 key 轮换、吊销、泄漏响应、RMA、销毁和 B/C/D 证据全部关闭前，不得宣称密钥管理、量产安全或远程 OTA 发布链已经完成。
