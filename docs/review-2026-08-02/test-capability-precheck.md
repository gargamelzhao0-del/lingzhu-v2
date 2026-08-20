# NuwaxAIPartner-v2 测试能力、脚本质量门与敏感资产预检

生成日期：2026-08-02

状态：B0-11 A 级只读预检。未运行 host 编译测试、网关功能测试、协议 fixture、设备测试、COM5 或 OTA。

## 1. 测试入口存在性

以下入口均存在：

- 6 个 `tools/run-*-host-checks.ps1`；
- `test/host/run_tests.sh`；
- protocol validator、fixture generator、fake-device conformance；
- AEC human-protection replay；
- 工作区 monitoring longrun、smart-barge-in、speaker-cancel recovery 和 audio sample manifest 脚本；
- cloud-gateway 的 `test`、`test:device-hello`、`test:conversation-endpoint`、`test:barge-in`、`test:meeting`、`test:onboarding`、`test:ota`。

存在不等于已执行或已通过；其中 host C 编译、gateway 状态变更、长稳、真人/设备、串口和 OTA 必须按 B/D 授权分类。

## 2. 无编译语法预检

| 类型 | 范围 | 结果 | 说明 |
|---|---:|---|---|
| PowerShell AST parse | 16 个 `.ps1` | 16/16 无 parse error | 仅语法，不执行脚本 |
| Node `--check` | 24 个 `.js` | 24/24 exit 0 | 含 docs simulator、协议工具和本地 LVGL docs JS；不执行逻辑 |
| Python `ast.parse` | 10 个活动第一方 `.py` | 10/10 通过 | 排除第三方、vendor、`_archive`、build/cache；不生成 pyc |
| Shell `bash -n` | `test/host/run_tests.sh` | 未执行 | 当前 Windows PATH 无 bash/WSL 入口 |
| CMake/Kconfig | 工程配置 | 未执行 parser/lint | CMake 4.0.3 有固定绝对路径但未在普通 PATH；无独立 Kconfig lint 流程 |

## 3. 静态/动态分析能力

- 普通 PowerShell PATH 中缺 `cppcheck`、`clang-tidy`、`clang`、`bash`。
- ESP-IDF 内含目标编译器，但没有获 B/C 级授权，尚未执行 `-Wall/-Wextra/-Werror` host 或目标构建。
- 当前没有统一 ASan/UBSan、allocation failure、I2C/TLS fault injection 或结构化 fuzz harness。
- Windows 原生环境不能假定 ESP-IDF host sanitizer 可用；需选择 WSL/Clang 或 CI 容器并冻结版本。

建议能力建设顺序：先用现有 host checks 建统一 runner/退出码/证据目录，再为纯 C 状态机和协议 parser 增加 Clang ASan/UBSan，最后接 libFuzzer/property corpus 与 allocation/I/O fault adapters。

## 4. 敏感资产预检

本轮只记录路径/类别/长度，不复制任何有效值。

- 当前 `sdkconfig` 的两个 build-time Wi-Fi password 均为空；运行凭据来自 NVS。
- `CONFIG_NUWAX_PROVISION_SEC2_DEV_POP` 为非空固定开发值（14 chars），同时存在于生成配置和开发源 fallback；这是 CONFIG-001 的量产阻塞证据。
- 未发现 `CONFIG_NUWAX_CLOUD_DEVICE_SECRET` build-time 配置项；动态 `device_secret` 字段引用本身不是硬编码密钥证明。
- `cloud-gateway/test-device-config-signing-private-key.pem` 存在，241 bytes，SHA-256 `cc99e6b9f623ed42ba1907ae307cf9d5190645f102d4de77227f4308a7937ca1`；必须列入“测试专用私钥” allowlist，禁止进入产品镜像/公开交付包。
- 当前没有 `gitleaks`、`trufflehog` 或 `detect-secrets`，尚未完成高熵/历史提交/二进制 secret scan。

## 5. 能力问题

- `TESTCAP-001 P2`：Bash/WSL、Clang/ASan/UBSan、cppcheck/clang-tidy 和 Kconfig lint 没有冻结可执行环境。
- `TESTCAP-002 P2`：fault injection/fuzz 只有分散脚本或计划，没有统一 harness、corpus、seed、crash artifact 和 CI 门禁。
- `SEC-001 P1`：固定开发 PoP 仍在产品候选配置/源码 fallback，量产必须改为每设备或工厂注入并可轮换/吊销。
- `SEC-002 P1`：测试签名私钥位于工作区，当前只有人为约定，没有自动 allowlist/交付包阻断。
- `SEC-003 P2`：没有正式 secret scanner 和脱敏发布门，当前路径扫描不能排除高熵、历史或二进制泄漏。

## 6. 结论

B0-11 的 A 级能力预检完成。现有回归入口较多且脚本基础语法通过，但 sanitizer/fuzz/fault injection、Shell/C/CMake/Kconfig 静态门和自动 secret gate 尚未建立；不能把“脚本存在”写成“全工程测试通过”。
