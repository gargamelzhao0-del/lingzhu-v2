# 27 NVS 持久化、迁移与掉电边界

<!-- technical-edition-20261009 -->

> 持久化设计关心的是中断后读到什么。写入顺序、版本、commit 和启动恢复共同决定设备能否解释一份半完成配置。

**学习目标：** 画出 pending 到 active 的提交边界，并识别擦除恢复的影响范围。

```zhizhan-map
验证候选
pending 持久化
满足提交条件
active 或恢复
```

## 27.1 内存状态、NVS 和 SD 不同

RAM 保存运行时对象，重启即丢失；NVS 保存小型配置和事务记录；SD 保存大量录音。把几小时 PCM 放 NVS 不合适，把一个待确认 OTA 事务只放 RAM 也不合适。是否调用了 `set` 不等于数据已提交，是否提交了一条记录也不等于整个跨模块业务已完成。

| 数据 | 当前源码入口 | 关键约束 |
| --- | --- | --- |
| 动态云配置 | [nuwax_cloud_config.c](../../main/nuwax_provision_new/nuwax_cloud_config.c)，`nuwax_cloud` | active 与 pending 分开 |
| Wi-Fi 保存列表 | [wifi_manager.c](../../main/wifi_manager.c)，`nuwax_wifi` | 还要与 IDF STA 保存状态及配网 owner 一致 |
| OTA 事务/序列 | [nuwax_ota_main.c](../../main/nuwax_ota_new/nuwax_ota_main.c)，`nuwax_ota` | 版本化记录、重启后确认和迁移 |
| 充电历史 | [battery_service.c](../../main/board/battery_service.c) | RAM 历史，不跨复位 |
| 录音归档 | [offline_audio_spool.c](../../main/offline_audio_spool.c) | 大文件、封存、恢复、提交后删除 |

学习时记录存储边界，不导出实际密码或密钥。读取源码里的 namespace/key 名可以解释机制，实际 NVS 内容不适合作为公开教材样例。

## 27.2 pending 不是只存在 RAM 的草稿

当前 `nuwax_cloud_config_stage` 会写 pending blob 并调用 `nvs_commit`，成功后才更新内存中的 pending。头文件明确说明 staged 记录可以跨 reset 保留，但在 Wi-Fi 成功并提交前，对 WebSocket 客户端不可见。

提交步骤为：验证候选 → 写 active → 删除 pending → 提交 → 更新 RAM active 并清除 RAM pending。失败时返回错误，不能先向应用发布新配置再检查磁盘写入结果。discard 也需要持久化删除，而不是只把内存布尔量清零。

这是一套应用事务设计，不应扩展解读为“整个配网跨多个 namespace 和 Wi-Fi 驱动状态的任意掉电原子事务”。要结合配网回滚记录和启动恢复逐个检验中断点。

## 27.3 格式升级怎样兼容旧设备

云配置结构体带 version，并检查 blob 长度和字段有效性。当前初始化会清理特定不兼容旧槽，要求重新获得可用配置；不是所有格式都可自动迁移。OTA 持久事务则有显式格式版本，当前 `NUWAX_OTA_PENDING_TXN_VERSION` 为 3，并保留兼容恢复逻辑。

为什么长度检查重要？给 C 结构体增加字段后，旧 blob 不会自动增加空间；直接按新布局读取可能把默认值或旧填充字节误当配置。迁移要明确旧版大小、允许值、新字段默认值和提交顺序，不能仅修改一个 version 常量。

## 27.4 必须揭示的现有擦除路径

当前 `app_main` 在 `nvs_flash_init()` 返回 `ESP_ERR_NVS_NO_FREE_PAGES` 或 `ESP_ERR_NVS_NEW_VERSION_FOUND` 时，会调用 `nvs_flash_erase()`，随后重新初始化。它的影响范围是默认 NVS 分区，不是只删掉某个失败字段。

这意味着“烧录脚本不主动擦 NVS”与“启动时所有条件下都保留 NVS”是两种不同保证。这个自动恢复分支可能清除该分区内的网络、云配置和业务记录。教材补充此事实，当前轮次没有更改固件策略；量产应根据实际工厂凭据分区、恢复渠道和迁移策略审查其适用性，不能笼统承诺配置永不丢失。

这也是不推荐初学者用 `erase-flash` 解决所有问题的原因：删除存储会改变故障前提，可能让原问题暂时消失，但同时失去诊断材料和可恢复身份。

## 27.5 写入频率也是系统调度问题

Flash 写入可能触发缓存或任务协调。逐帧保存音量、每次状态回调都写一次 NVS、每条错误都立即持久化，会增加运行干扰和写入量。应合并重复值、设置合理保存边界，并协调音频暂停/恢复。

工程中的 Wi-Fi 保存还要考虑 `esp_wifi_set_config()` 可能经过驱动自己的 NVS 路径，不能只搜索应用 `nvs_open()` 就宣称覆盖所有 Flash 写入。声音突然有短缺口时，应把配置持久化时间也加入时间线。

## 27.6 掉电与迁移测试矩阵

对一项配置分别测试：写前、pending 提交后、Wi-Fi 成功前、active 更新期间、最终成功后。重启后记录 active/pending 是否可解释，原网络能否恢复，云端是否用了错误的凭据组合。

OTA 还要测试新旧事务格式、错误长度、缺字段和重复结果确认。目标不是任何中断都无痕，而是不会把半完成操作冒充完成、不会无限重试同一个不可恢复状态，并能进入明确恢复流程。



<!-- implementation-workshop-20261009 -->

## 把设置写入做成可解释的提交过程

先给每一种数据指定持久化时机：云配置需要候选与活动记录，音量等频繁交互可以合并写入，诊断需要有界历史，OTA 结果需要跨重启继续交付。它们都用 NVS，不代表都应该在 UI 回调里立即 commit。

例如音量拖动期间，界面预览可以频繁变化，而落盘应交给受控路径；否则高频 Flash 操作既增加写入压力，也会影响依赖缓存的音频任务。生产播放接口将 preview、apply 和 persistence service 分开，见 [speaker_playback.h](../../main/conversation/speaker_playback.h)。

与唤醒并行写 Flash 时，先读 `begin_flash_operation()` 的守卫契约：取得守卫后，所有成功取得路径都要由同一任务结束守卫；主管不能在操作期间重新 arm detector。这里解决的是资源时序，不是把 NVS 内容变成天然原子的大事务。

联调记录“用户要求的值”“内存生效的值”“提交成功的值”“重启后读回的值”。四者不一致时，沿这一顺序定位。修改设置后不等落盘就立刻断电，验证的是丢电边界；正常提交后仍回到旧值，则要查命名空间、key、版本与恢复路径。

## 技术要点与自测

**设计要点：** 单个 namespace 提交不能自动保证跨模块、跨驱动、跨介质的任意掉电原子性。

**自测：** 脚本烧录时不擦 NVS，是否可以承诺任何重启后凭据都保留？

**参考答案：** 不能。应用启动自身存在默认 NVS 初始化错误后的擦除恢复路径，也需纳入保证范围。
