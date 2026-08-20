# NuwaxAIPartner-v2 版本与 OTA 真值表

生成日期：2026-08-02

状态：B0-09 只读证据。当前没有与源码 manifest/冻结配置绑定的发布镜像。

| 来源 | 当前值 | 语义 | 生成/读取方式 | 当前真值? | 证据/冲突 |
|---|---|---|---|---|---|
| `CMakeLists.txt` | `PROJECT_VER=7.0.24` | 源码默认 app version | CMake `PROJECT_VER` | 是（源码） | `CMakeLists.txt:5-6` |
| `nuwax_ota_release_policy.h` | base `7000000`，compiled `7000024` | OTA release sequence | 编译常量 | 是（源码） | sequence 与 version 手工双维护 |
| app descriptor | 当前源码预期 `7.0.24` | 运行镜像版本 | `esp_app_get_description()->version` | 需当前 manifest 绑定的 C 级构建验证 | 启动日志和 BLE device-info 都读取 descriptor；外部未绑定构建只能证明自身为 7.0.23 |
| hello root `version` | `1` | hello 协议版本 | `session_protocol.c` | 是 | 不是 firmware version |
| hello `firmware.version` | 运行 descriptor | 对外固件版本 | descriptor；身份可用时附带持久化 OTA identity | 当前逻辑存在 | descriptor 与 anti-rollback floor 可能混合 |
| hello release sequence | `max(NVS accepted, compiled)`；identity 成功时可覆盖 | 当前 anti-rollback 水位 | OTA main + running identity | 语义需修正 | 可能出现低 descriptor + 高 sequence，且缺 hash |
| OTA ACK/progress/result | 主要返回 current/target version 和 update_id | OTA 事件 | `nuwax_ota_device.c` | 契约不完整 | 没有统一 release_sequence/image_sha256 |
| current工程 `build/project_description.json` | `7.0.21` | 历史缓存 | CMake cache | 否 | 不能当当前源码产物 |
| 外部 `C:\tmp\nuwax-v2-fix-7.0.23-20260803-d` | app `7.0.23`，SHA `774c620149ae976f943dd7c2c7b2d2d0b40181f0f1e2bd60c8a1bfbbd47873fe` | 既有外部构建证据 | image-info/hash | 否（未绑定） | 无当前 7,918 项源码 manifest、归一化配置或 OTA manifest 绑定 |
| `implementation-verification-2026-08-03.md` | `7.0.22 / 7000022` | 历史执行快照 | 当时的构建/host证据 | 否（非当前源码） | 必须保留历史语义，不可提升为 7.0.24 通过证据 |
| 根 `README.md` Flash/OTA 段 | `7.0.17 / 7000017` | 交付说明 | 手工文档 | 否 | 陈旧文档冲突 |
| `main/nuwax_ota_new/README.md` | 示例 `7.1.0 / 7100000` | OTA 示例 | 文档样例 | 否 | 不是当前版本 |
| 正式交接协议 schema | 未定义 firmware/OTA 字段 | 云端契约 | 文档/schema | 否 | 设备实际扩展字段无法被 schema 严格校验 |

## 1. 已确认逻辑

- 启动日志和 BLE `device-info` 的 firmware version 从运行镜像 descriptor 读取，不是手写字符串。
- OTA accepted sequence 采用 NVS durable floor 与编译 floor 的最大值，避免 NVS 中较低值立即降低当前进程水位。
- `pending_txn` 内保存 version/sequence/target address/image size/SHA，启动时可核对运行槽。

## 2. 当前风险

- `VER-001 P1`：没有把源码 manifest、defaults 归一化 hash、分区表 hash、model hash、app descriptor、OTA manifest 和镜像 hash 绑定为一个发布 truth。
- `VER-002 P1`：交付协议文档/schema 没有正式声明 firmware identity 和 OTA 消息字段，additional properties 不能替代契约校验。
- `VER-003 P1`：hello 的 release sequence 可能同时表达 compiled/accepted anti-rollback floor 与当前镜像 identity；descriptor、sequence、image hash 不是不可分割的 tuple。
- `VER-004 P1`：PROJECT_VER 与 release sequence 手工双维护，没有构建期断言二者映射一致；CMake `PROJECT_VER` 还可被外部 `-D PROJECT_VER` 覆盖。
- `VER-005 P2`：OTA ACK/progress/result 没有统一返回 release sequence/image SHA，审计依赖 update_id 和上下文 NVS。
- `VER-006 P2`：README 和历史目录命名带有陈旧版本，容易误用镜像或命令。

## 3. 关闭条件

1. 由单一版本文件生成 PROJECT_VER、release sequence、manifest 和文档值，构建时拒绝不一致。
2. 发布 manifest 绑定 source-manifest、config hash、partition table、srmodels、app image、target board/chip 和 OTA sequence。
3. hello/OTA ACK/progress/result 使用同一个 firmware identity tuple：version、release_sequence、image_sha256、partition_schema、board_id。
4. 正式云端 schema 增加并严格校验 firmware/OTA 字段；测试网关和 fake-device 同步 fixture。
5. 旧 README/示例明确标记历史或从产品交付入口移除；所有手工烧录/OTA 命令引用 manifest，不引用目录名猜版本。

## 4. 结论

B0-09 的只读版本盘点完成，但当前版本 truth 未闭合；当前源码为 7.0.24/7000024，既有 7.0.23 外部构建未绑定本轮 source/config/OTA manifest，不能作为当前发布基线。
