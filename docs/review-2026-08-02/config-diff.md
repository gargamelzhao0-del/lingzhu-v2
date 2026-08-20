# NuwaxAIPartner-v2 配置与条件构建矩阵

生成日期：2026-08-02

状态：阶段 0 静态配置证据；尚未执行干净 reconfigure/build。

## 1. 产品基线候选

当前计划定义的产品配置组合：

```powershell
$env:SDKCONFIG_DEFAULTS = "sdkconfig.defaults;sdkconfig.product-ota.defaults"
```

当前生成的 `sdkconfig` 具备以下关键产品特征：ESP32-S3、16 MB flash、`partitions.customwake.csv`、新版 Vocat board、OTA 开启、诊断模式关闭、PCM16/16 kHz/20 ms、MultiNet 自定义唤醒、显示和触摸开启、PSRAM 开启、app rollback 开启；监控模式还启用了 120 s 静默 pause、500 ms preroll、ESP VAD 和 capture ACK/重试参数。

这只能证明当前文件内容，不能证明它由上述 overlay 在干净环境中生成；该结论留给 B0-06 可复现构建检查。

2026-08-03 A2-05 续接确认：当前 `sdkconfig` 的 `CONFIG_NUWAX_OTA_MIN_BATTERY_PERCENT=30`、未认证 gauge 电压门槛为 4000 mV，但 `sdkconfig.product-ota.defaults` 只启用 battery/OTA，没有冻结这两个安全参数；干净配置会采用 Kconfig 默认 20%/4000 mV。也就是说当前运行候选的 30% 不能由正式 overlay 重现。该差异沿用 `CONFIG-003/BUILD-001`，产品发布前必须把电池/OTA 安全阈值写入唯一 overlay，并用 C 级最终 `sdkconfig` 和镜像 hash 证明。

## 2. 配置文件分类

| 文件 | 分类 | 用途/限制 | SHA-256 |
|---|---|---|---|
| `sdkconfig` | 当前生成配置 | 只作为本机当前候选；必须与 overlay 重建结果比较 | `ea74b3ffc7ce84c2a02af8cf3ef022267bffd12eb6489721d77d2f32ab97e07d` |
| `sdkconfig.barge-final` | 历史生成快照 | 与当前配置有 39 个归一化配置行的对称差异，不作为产品输入 | `4f6ecb41e2af8de18449e7307dade998f78acfeabd0e343ea533fd36805e52b9` |
| `sdkconfig.defaults` | 公共基础 overlay | 正常产品、诊断和 CI 共用基础 | `22418aa88f1dbd384b1d5a535afe91bd3755ea65230d77dbac2efc1f61236117` |
| `sdkconfig.product-ota.defaults` | 产品 overlay | 正常业务、battery UI、OTA、rollback；显式关闭隔离诊断 | `d5e9aabf4ffc78811820a6329936ccc7a47345168705978a417cbb9c30284a6a` |
| `sdkconfig.ci` | CI overlay | 主任务栈、WDT、TLS CA 尝试和 NTP 参数 | `81f16f60ef4ca9809d1cd1d0d93c5bb5b931bf711f5ca57fa78424d071b58c36` |
| `sdkconfig.opus.defaults` | 可选 codec overlay | 将上行从 PCM16 切为 Opus；不是当前默认产品协议 | `885a6250ddb5bc23f42ab69a0c1d0b4de9e3c7902ede42110755009da8981be7` |
| `sdkconfig.full-hardware-acceptance.defaults` | COM5 诊断 overlay | 关闭 OTA，开启板级、SD、音频 smoke、硬参考 A/B；不是产品固件 | `143dde71f1e91e2eb41bba714c9e21fb50d48deb9e27f352cda21a01cf1e3854` |
| `sdkconfig.full-hardware-fixture.defaults` | 外部 fixture overlay | 4G/UART；文件明确禁止在缺少外设时烧录 | `914292912c5022f8ed735b682275bc5dc0e494eb34997117331f30ac094809cc` |
| `sdkconfig.vendor-v2-audio.defaults` | 历史/隔离诊断 | 商家 V2 音频时序对比，不是产品配置 | `eafcb788b2bd708291c2c70b7733618ce199814e70906c50322da4720edc666e` |
| `sdkconfig.vendor-v2-clock-aligned.defaults` | 历史/隔离诊断 | 商家 24 kHz 共享时钟对比，不是产品配置 | `9004197ffe92d552f6448da14453927152fee55aa7f274ad05c80dc16e14e822` |
| `sdkconfig.vendor-v2-clock-aligned-touch.defaults` | COM5 诊断补充 | 诊断后开启触摸/UI，不是产品配置 | `771fdaec0bc1b7f582c3c8facaa62fb0f58e87be27f8bd6690f803581351b43e` |

## 3. Kconfig 与条件构建

- `main/Kconfig.projbuild`：352 个 `config` 符号。
- `bootloader_components/nuwax_power_hold/Kconfig`：2 个符号。
- `main/CMakeLists.txt`：74 个公共源、5 个条件 OTA 源、71 个 UI 源。
- OTA 源仅在 `CONFIG_NUWAX_OTA_ENABLE=y` 时加入。
- CMake 显式禁止产品 OTA 与硬件/AEC 隔离诊断组合；这一配置冲突门是正确的，但仍需构建矩阵验证。

当前 `sdkconfig` 与 `sdkconfig.barge-final` 的归一化 `CONFIG_*` 对称差异为 49 行（current-only 34、barge-only 15）。A2-05 并行改动新增 speaker buffer/backpressure 配置后，旧的 45 行计数已失效。

建议的受支持构建矩阵：

| Build ID | `SDKCONFIG_DEFAULTS` | 目的 | 当前状态 |
|---|---|---|---|
| PRODUCT-PCM | `sdkconfig.defaults;sdkconfig.product-ota.defaults` | 正式 PCM16 产品候选 | 待干净构建 |
| PRODUCT-OPUS | `sdkconfig.defaults;sdkconfig.product-ota.defaults;sdkconfig.opus.defaults` | 可选 Opus 协议候选 | 产品是否支持待决策；待构建 |
| CI | `sdkconfig.defaults;sdkconfig.product-ota.defaults;sdkconfig.ci` | 主机/固件 CI | 待构建 |
| HW-ACCEPT | `sdkconfig.defaults;sdkconfig.full-hardware-acceptance.defaults` | 新板硬件诊断 | 延期，且需单独烧录批准 |
| HW-FIXTURE | 上项加 `sdkconfig.full-hardware-fixture.defaults` | SD/UART/4G 外部 fixture | 当前延期，不得烧录 |
| VENDOR-V2-* | 对应 vendor overlay | 历史音频比较 | 历史/诊断，不纳入产品发布 |

## 4. 已确认配置风险

### CONFIG-001 固定开发配网凭据和不安全 WS

- 当前 `sdkconfig` 启用 `CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE`、固定开发 PoP，并启用 `CONFIG_NUWAX_CLOUD_CONFIG_ALLOW_INSECURE_WS`。
- 影响：开发测试可接受，但量产凭据唯一性和 WSS-only 门禁不满足。
- 等级：P1 发布安全阻塞；不等同当前联网功能故障。

### CONFIG-002 量产启动/存储安全未启用

- 当前 Secure Boot、Flash Encryption 均未启用。
- app rollback 已启用，但不能替代镜像真实性和静态数据保护。
- 等级：P1 发布安全阻塞；任何 eFuse 操作必须单独审批。

### CONFIG-003 历史生成配置漂移

- `sdkconfig.barge-final` 与当前 `sdkconfig` 有 39 个归一化配置行的对称差异。
- 主要差异包括 Wi-Fi buffer 数量、UART/monitor baud、实时音频诊断日志，以及当前配置新增的 9 个监控静默/VAD/preroll/capture ACK 参数。
- 等级：P2 可复现性/维护风险；该文件应明确归档或标注用途。

## 5. 对应计划校验

- 覆盖计划 2.2、阶段 0.3、0.7、阶段 6 安全配置入口。
- 已完成配置文件分类、hash、条件源码和支持构建矩阵草案。
- 未完成：干净 reconfigure/build、生成配置逐项 diff、镜像/分区验证。
