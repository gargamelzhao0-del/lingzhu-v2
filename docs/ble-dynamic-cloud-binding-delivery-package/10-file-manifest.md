# 文件清单与来源

## 文档

| 文件 | 用途 |
| --- | --- |
| `README.md` | 交付入口、快速启动和状态 |
| `00-scope-and-terminology.md` | 地址、身份和密钥术语 |
| `01-three-party-architecture.md` | 本地三端架构和事务时序 |
| `02-communication-protocol.md` | HTTP/BLE/WebSocket 报文 |
| `03-android-app-change-guide.md` | Android 接口、模型和状态机 |
| `04-local-test-gateway.md` | 本地测试服务和生产差异 |
| `05-simulator-guide.md` | 模拟器组成、接口和操作 |
| `06-device-firmware-logic.md` | 固件验签、NVS、连接和回滚 |
| `07-upgrade-from-first-package.md` | 相对首轮功能升级 |
| `08-security-and-production-boundaries.md` | 凭据隔离和量产边界 |
| `09-test-and-acceptance.md` | 真机、失败、自动测试和门槛 |
| `10-file-manifest.md` | 本清单、文件来源和外部依赖边界 |
| `11-android-provider-reference.md` | Android 参考依赖、API、capability 和生命周期 |
| `12-final-audit-report.md` | 代码级审计、修复项和残余风险 |
| `provisioning-contract.json` | 机器可读冻结契约副本 |

## 模拟器

`simulator/` 来源于当前已完成真机验收的 `docs/provision_simulator/`，复制时排除：

- `*.log`
- `runtime-logs/`
- `__pycache__/`
- `*.pyc`

包内保留网页、Bridge、二维码解析、依赖声明、启动脚本和测试。Mock 服务器直接读取包内 `provisioning-contract.json`；Real Bridge 在完整工作区中使用同一包内副本，同时依赖工程 managed component 和构建信息。根目录契约与 `main/nuwax_provision_new/esp32s3-idf6-provisioning-contract.json` 在交付验证时必须字节一致。

清理运行缓存后的冻结包共 29 个文件：根目录 14 份 Markdown、1 份 JSON，`simulator/` 14 个文件。ZIP 中不得出现日志、`__pycache__`、`.pyc`、注册表或密钥文件。

## 运行时外部依赖

为了不复制无关业务网关和测试私钥，以下内容不在包中重复保存：

| 依赖 | 工程位置 | 原因 |
| --- | --- | --- |
| 完整本地测试网关 | `../cloud-gateway/cloud-gateway.js` | 同时承载设备业务协议，保持单一实现 |
| onboarding 注册逻辑 | `../cloud-gateway/device-onboarding.js` | claim、签名、pending/active 的权威测试实现 |
| 测试签名私钥 | `../cloud-gateway/test-device-config-signing-private-key.pem` | 私钥不复制进 APP 交付目录 |
| 固件机器契约 | `main/nuwax_provision_new/esp32s3-idf6-provisioning-contract.json` | 设备/模拟器运行时权威来源 |
| ESP-IDF 构建说明 | `build/project_description.json` | 定位锁定的 IDF/managed component Python 环境 |

脱离本仓库可直接运行 Mock。若要运行 Real BLE + 云端三端流程，必须由项目负责人提供完整固件工程运行环境、测试网关和匹配测试固件公钥的测试签名服务；不能把生产私钥或生产 APP Token 打入压缩包。
