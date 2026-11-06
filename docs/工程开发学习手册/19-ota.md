# 19 OTA、分区、身份与回滚

<!-- technical-edition-20261009 -->

> OTA 的完成点在新镜像运行并被确认之后。下载只是前半程，目标身份、资源准入、完整性、启动确认和回滚共同决定是否可交付。

**学习目标：** 按提交点分析升级，区分下载电量准入与启动健康确认。

```zhizhan-map
manifest 与资源准入
下载和校验
选择新槽并启动
健康确认或回滚
```

## 19.1 OTA 是受控事务

应用 OTA 写非运行槽，验证成功后才改变下一次启动选择；启用回滚时，新镜像首次运行需要完成健康确认。ESP-IDF 的基本状态与接口见 [IDF 6.0 OTA 文档](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-reference/system/ota.html)。本工程在此之上增加 manifest、业务准入、身份报告和结果确认。

不要在下载开始时就把版本显示改成目标版本，也不要下载结束就认定设备已完成升级。至少要区分下载、写入、校验、选择启动槽、重启、新版健康确认和结果上报。

## 19.2 manifest 防止刷错目标

当前要求 update_id、url、version、project_name、target_board、target_chip、partition_schema、build_flavor、release_sequence、size_bytes、image_sha256。`force` 和 release_notes 可选。旧 `size`、`sha256` 别名不作为兼容输入。

版本显示与发布序列分开：force 不能绕过严格递增的 release_sequence。当前构建规则是 `7.0.N`，旧 README 中 `7.1.0` 是历史示例，不应直接复制成现行产物配置。

入口：[nuwax_ota_manifest.c](../../main/nuwax_ota_new/nuwax_ota_manifest.c)、[OTA 组件说明](../../main/nuwax_ota_new/README.md)。

## 19.3 准入不是下载功能坏了

会议、监控、通知、配网、离线 capture/archive/replay 等资源可能与 OTA 冲突。日志 `audio_busy` 或 `OTA admission blocked` 表示准入拒绝，需要看具体 owner，不能只按“停止会议”一句提示操作。

历史上独立测试网关连接失败时也可能留有 replay 任务状态。当前接口区分空闲测试建连与实际补传占用，OTA 可在规定条件下取消前者，但不能越过正在归档的数据所有权。

合理的准入失败应可重试并给出具体原因。不得通过强制删除持有 FILE 或 socket 的任务来腾空间。

## 19.4 下载、校验与有限重试

当前 HTTPS 保留证书及主机名验证，拒绝重定向，要求 HTTP 200 和精确 Content-Length；所有写入字节参与 SHA-256。`esp_ota_end` 验证镜像后，还核对描述符中的项目/版本，最后才选择启动分区。

网络/传输失败可按现有策略从零重新下载，最多三次总尝试；旧 HTTP client、SHA 状态与 OTA handle 必须先清理。当前产品总期限 2,100,000 ms，即 35 分钟，不会每次重试重置成无限时间。

取消在下载/写入阶段协作执行；短暂的验证提交阶段有不同约束。上层应呈现真实阶段，不能用“已点取消”代替“工作线程已停止”。

## 19.5 为什么 OTA 栈提前分配

HTTPS 与 Flash 操作需要受约束资源。若接受 OTA 后才临时申请大块内部栈，运行数小时后的碎片可能让它失败。工程在启动阶段创建 controller，并为 I/O 请求 4 KiB 内部缓冲，必要时退到 2 KiB。

这不是把所有东西放 PSRAM 的理由。Flash 操作与 cache/外部内存可访问性有特定约束，必须遵守 IDF 支持边界。外部 RAM 基础限制见 [IDF 6.0 PSRAM 文档](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-guides/external-ram.html)，本工程还用 owner 暂停和内部缓冲降低冲突。

## 19.6 启动摘要缓存为什么安全边界要窄

历史版本在启动、hello、身份报告和确认中重复扫描整包镜像，产生多次长等待。后续改为本次启动有效的运行分区摘要缓存，键包含分区地址、长度、版本和 ELF SHA。缓存源于真实完整扫描，不是相信 manifest 自己声明的摘要。

只缓存运行分区、禁止 OTA 写运行分区、不跨启动沿用缓存，是成立条件。若把“任何文件相同大小都复用哈希”当优化，会破坏完整性校验。详情见 [启动身份校验优化](../2026-09-27-OTA启动身份校验优化与验证.md)。

## 19.7 发布包和回滚验收

一次 OTA 包至少关联应用 BIN、SHA、版本序列、manifest、ELF、构建配置和验证记录。测试包括错误哈希、错误大小、目标不匹配、同版本/低序列、下载中断、掉电、首次启动失败及正常确认。

应用层序列控制不是 eFuse 安全版本防回滚的同义词；有 TLS 也不自动等于 Secure Boot、Flash Encryption、NVS 加密已经启用。量产安全能力需要逐项核对实际配置和生产流程，不能从一个 OTA 开关推导全部成立。

## 19.8 下载准入和新固件确认使用不同电量规则

[nuwax_ota_power_policy.h](../../main/nuwax_ota_new/nuwax_ota_power_policy.h) 定义了两个目的不同的判断。开始下载时，[device 的 battery_preflight_code](../../main/nuwax_ota_new/nuwax_ota_device.c) 要求可用、新鲜、有电池且有效的样本，百分比须在 0～100，硬低压不能存在。当前正式配置 `OTA_MIN_BATTERY_PERCENT=5`，低于 5 返回 low_power；样本缺失/过期/无效返回 power_unknown。

这条生产准入入口没有“插着 USB 就忽略低电量”的旁路。不要仅看到同一头文件里的通用函数带 external_power 参数，就推断调用方启用了外部电源豁免。UI 的低电提醒、充电图标与 OTA 准入是不同决策。

新镜像首次启动健康确认使用 `power_valid`、`power_stale` 和 `hard_low_voltage`，不重复拿下载最低百分比判失败。应用已写好后，SOC 估计的小幅变化不应直接被当成镜像损坏。主控遇到电源状态未知/低压时返回尚未完成，仍受整个启动确认期限约束；也还要通过其他本地资源和云端活动检查。

## 19.9 电源准入练习

假设除表中变化项外，样本新鲜、有效、电池存在、无硬低压，最低准入值为 5：

| 输入 | 下载准入结果 | 解释 |
| --- | --- | --- |
| percent=4 | low_power | 低于当前配置门槛 |
| percent=5 | allowed | 恰好达到门槛，仅电源这一关通过 |
| percent=101 | power_unknown | 不接受越界百分比 |
| percent=80，样本过期 | power_unknown | 高电量不能替代新鲜测量 |
| percent=80，硬低压 | low_power | 百分比不能覆盖物理低压 |

再比较：下载开始时 percent=5，重启确认时 SOC 变为 4，但物理电源样本新鲜、有效且无硬低压。仅依据 SOC 不应把启动电源检查判失败；它仍不表示整个 OTA 已确认成功。练习完成条件是能找到两个真实调用点，说明各自为什么使用不同字段。



<!-- implementation-workshop-20261009 -->

## 把升级控制器接进正在运行的产品

OTA 不是在菜单按钮里直接下载一个 BIN。[nuwax_ota_device_callbacks_t](../../main/nuwax_ota_new/nuwax_ota_device.h) 将它接入整机：`continuous_audio_busy` 查询业务占用，`prepare_audio_for_update` 完成资源收尾，`show_local_progress` 更新本地画面，`check_boot_health` 验证新启动，发送与重连回调负责结果交付。

先注册 callbacks，再把文本接收中的 OTA 指令交给 `nuwax_ota_device_handle_text()`，同时传入真实连接 epoch。控制器明确接受或拒绝后才开始后续工作。旧连接中接收的任务不能因为新连接后来鉴权成功而获得新授权。

接线时逐项检查返回语义：prepare 成功是否真的没有录音/补传 worker 持有资源；blocks_audio 为真期间普通路径是否仍可能启动；下载失败之后本地 UI 与唤醒能否恢复；结果未送达时是否保留上下文到下一次鉴权。只实现下载进度会遗漏产品中最常见的失败路径。

新镜像首次启动时，健康检查返回 NOT_FINISHED 表示还需要等待必需条件，例如第一份电池有效样本；不能立即当作永久失败，也不能直接忽略确认新镜像。下载电量门槛与新镜像启动健康的条件不同，继续对照本章电源策略。

联调按四轮递进：身份错误的 manifest 应拒绝；资源忙应拒绝或按契约处理；下载中断应有界退出；正确镜像启动后应完成健康确认与版本报告。菜单拖动中的 OTA 则是独立的 UI 接管用例。运行 [OTA 身份缓存回归](../../tools/test-ota-identity-cache.py) 后，再用匹配产物进行真实升级；旧版下载新版时仍执行旧 UI，不能借那次下载过程验收新版绘图修复。

## 技术要点与自测

**设计要点：** 百分比门槛不能机械用于所有阶段；已写好的镜像启动确认关注真实电源健康及其他系统条件。

**自测：** 下载成功且 SHA 正确，是否意味着型号和分区一定兼容？

**参考答案：** 不一定。还要核对项目、板、芯片、分区方案、构建类型和序列等契约字段。
