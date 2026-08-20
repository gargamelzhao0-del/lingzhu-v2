# NuwaxAIPartner-v2 实现修复与构建验证

日期：2026-08-03  
对象：`NuwaxAIPartner-v2` 7.0.22 / release sequence 7000022  
边界：只修改和验证 V2 产品树；未访问 COM5/COM8，未烧录、未 OTA、未擦写 NVS，未获得当前 7.0.22 实机证据。

## 1. 状态定义

- 代码：当前源码已消除已登记的直接根因。
- Host：相关纯 C 测试或 source contract 已通过，只证明主机可验证边界。
- Build：ESP-IDF 6.0 ESP32-S3 全新 configure/build/link 成功。
- 实机：必须由当前镜像、板卡身份、日志和场景证据证明；本轮全部未执行。

## 2. 本轮问题状态

| ID | 代码 | Host | Build | 实机与当前裁决 |
|---|---|---|---|---|
| `TIME-001` | 8 个固定 1 ms wait 和 1 个动态最小 wait 统一 clamp 到至少 1 tick | conversation contract 通过 | 通过 | 未做 contention/heartbeat；实现完成，产品验证开放 |
| `TIMER-002` | status bar/focus menu timer 判 NULL，错误沿 `nuwax_app_create()` 传播 | product contract 通过 | 通过 | 未做 allocator 注入；实现完成，故障验证开放 |
| `START-001` | `display_err != ESP_OK` 时不创建 LVGL/app shell，`s_ui_ready=false` | product contract 通过 | 通过 | 未做 display 失败注入；实现完成，降级验证开放 |
| `RTOS-006` | metadata 用 portMUX；slot 写前标记 rendering 并取消旧命中，完整绘制后发布；`get` 原子保护 LVGL buffer；双 buffer transition 先 suspend | product contract 通过 | 通过 | 未做双核 stress/快速切换/画面检查；实现完成，动态证明开放 |
| `SEC-006` | OTA queue/reserve/acceptance 绑定认证 WebSocket epoch | product contract 通过 | 通过 | 断线全交错和 terminal ACK 实机开放 |
| `SEC-007` | UID 每次从 eFuse/MAC 派生，NVS 仅为校验缓存 | product contract 通过 | 通过 | NVS 篡改/擦除实机开放 |
| `LIFE-009` | WebSocket RX owner 发布 handle 和 OPEN 后才允许 worker 运行 | product/build 检查通过 | 通过 | 高频 connect/close stress 开放 |
| `MEM-002` | NXFP 使用独立 byte-wise LE parser，验证 offset/count/尾部并用安全交点运算 | 14 项 parser contract 通过 | 通过 | fuzz/sanitizer corpus 和快速 UI 切换开放 |

这些状态不能解释为 `START-002`、`RTOS-008`、`LIFE-004/008` 或其他 P1 已关闭；它们仍按原审计开放。

## 3. 验证结果

全部通过：

1. `tools/run-v2-product-host-checks.ps1`
2. `tools/run-conversation-host-checks.ps1`
3. `tools/run-meeting-host-checks.ps1`
4. `tools/run-monitoring-host-checks.ps1`
5. `tools/run-provisioning-host-checks.ps1`
6. `tools/run-wake-word-host-check.ps1`
7. `test/host/run_tests.sh`：121 checks，0 failures

AEC replay 本轮未重跑。此前行为指标通过，但 wrapper 因要求实验性 256-tap FIR 而退出 1；product profile 明确关闭该 FIR，并由 product contract 固定。该结果不能写成当前实机 AEC 通过。

## 4. 全新构建证据

- Build：`C:\tmp\nuwax-v2-review-20260803-2`
- SDKCONFIG：`C:\tmp\nuwax-v2-review-20260803-2.sdkconfig`
- Defaults：`sdkconfig.defaults;sdkconfig.product-ota.defaults`
- Target/toolchain：ESP32-S3，ESP-IDF v6.0，GCC 15.2.0，CMake 4.0.3，Ninja 1.12.1，Python 3.13.12，esptool 5.3.0
- configure/build/link：exit 0；2,221 个顶层 Ninja step 完成
- app descriptor：`NuwaxAIPartner`，7.0.22，secure version 0
- image：3,566,240 bytes（`0x366aa0`）
- OTA slot：`0x4c0000`；剩余 `0x159560`（28%）
- image footer validation hash：`f28f8fae40dd343a581ec07547fc05e340790cb500b8c7e0822ddb91bfbb35f2`，valid

构建观察到两个既有 `-Wunused-function` warning：`face_engine.c:live_loader_step_timer_cb` 和 `face_components.c:loader_step_timer_cb`。本轮修改文件未出现新增 compiler warning。

环境有两个可复现性边界：Windows 初始化器先对 v6.0 registry 返回 `null`，随后 v6.0 `export.ps1` 成功激活正确环境；首次 configure 的 target 从仓库 `sdkconfig` 推断为 esp32s3，而最终外部 SDKCONFIG 和 image 均确认 esp32s3。后续发布 runner 应直接显式传入 target，避免依赖推断。

## 5. 冻结哈希

| 产物 | Bytes | SHA-256 |
|---|---:|---|
| current source manifest（7,917 项） | - | `659f8e21a6cf487c34530b03342765c38ce9777c773c413d2f72439a1d2d3d33` |
| `NuwaxAIPartner.bin` | 3,566,240 | `cdefba1d8bde614495761c8025403e8a1852127012e126939674baba570754df` |
| `NuwaxAIPartner.elf` | 26,103,920 | `b24c3a61d75bfb89b3f891bfe136d5bfde78dba8d825cba90fa9881e9dbbef17` |
| `NuwaxAIPartner.map` | 15,760,130 | `4e307b651a49e2ec5eb9e630f586fc8f20c8ac199ce5a345be17e5974a0aa773` |
| generated SDKCONFIG | 135,837 | `527b2137315965c46a411a3b4bdc6fae1c4ffc8485c52a42a3989817eeee8e53` |
| bootloader | 21,392 | `0ef6679dbe0d9a5c5350b84409f934dec54d49ebf5f3ba01a09320af7c88ea59` |
| partition table | 3,072 | `aad81c7e6e4686758d711bf7336ff59e4c3e816af34cf0bb795300b5e3bf8cdf` |
| OTA initial data | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| SR models | 2,681,351 | `dec3196aaadcf4e6305434462f5735e1148e5ddfe1f6a0696776d666f9066d34` |

## 6. 配置与发布边界

Fresh product SDKCONFIG 与仓库 `sdkconfig` 逐行只有一项值差异：product defaults 生成 `CONFIG_NUWAX_OTA_MIN_BATTERY_PERCENT=20`，仓库 `sdkconfig` 为 30。发布前必须裁决唯一权威值并把它固定到受控 profile；本轮没有静默选择或回写仓库配置。

Fresh 配置仍明确包含以下发布阻塞：

- `CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE=y`
- 固定 PoP `Nuwax7-Dev-PoP`
- `CONFIG_NUWAX_PROVISION_USB_CONTROL_ENABLE=y`
- `CONFIG_NUWAX_CLOUD_CONFIG_ALLOW_INSECURE_WS=y`
- Secure Boot、Flash Encryption、NVS Encryption 未启用
- secure version 0，未建立硬件 anti-rollback
- OTA manifest 和 cloud config 仍缺独立 release signature、`key_id`、单调版本/重放保护闭环

因此该镜像是可构建的开发/集成候选，不是可发布、可量产或已完成安全验收的镜像。
