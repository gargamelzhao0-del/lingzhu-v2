# NuwaxAIPartner-v2 模块与源码清单

生成日期：2026-08-03（基于 7.0.24 与新增 `realtime_log.h` 刷新）

状态：阶段 0 基线证据。工程源码或 CMake 变化后必须重新生成。

## 1. 统计口径

- 第一方活动代码：`main/`、`components/`、`bootloader_components/`、`test/`、`tools/`，排除路径中名为 `_archive` 的历史内容。
- 第一方原始代码：上述目录的完整文件系统内容，包含 `_archive`；归档必须计数和 hash，但不参与正式构建能力判断。
- 第三方：`lib/`、`managed_components/`。
- 代码/脚本扩展名：`.c/.h/.cpp/.hpp/.S/.s/.ld/.py/.ps1/.js/.ts`。
- CMake 源条目从 `main/CMakeLists.txt` 的 `SRCS_COMMON`、条件 OTA `list(APPEND SRCS_COMMON ...)` 和 `SRCS_LVGL_UI` 解析。

## 2. 清单结果

| 分类 | 数量 | 说明 |
|---|---:|---|
| 第一方活动代码/脚本 | 362 | main 335、components 1、bootloader_components 1、test 11、tools 14；排除 `_archive` |
| `_archive` 历史代码/脚本 | 11 | 1 个 `.c`、3 个 `.h`、7 个 `.py`；全部属于失败的 v15 Goertzel 路径 |
| 第一方原始合计 | 373 | 活动 362 + 归档 11；归档不计为产品能力 |
| `lib/` 第三方代码 | 2209 | 主要为 LVGL；按实际链接、版本、SBOM、许可证/漏洞和封装边界审计 |
| `managed_components/` | 939 | 按依赖锁、实际链接和项目配置审计 |
| 活动代码合计 | 3510 | 活动第一方 362 + 第三方 3148；不是“全部逐行已通过” |
| 原始代码合计 | 3521 | 含 11 个归档代码/脚本；用于完整 manifest 范围 |

活动第一方扩展名：204 个 `.c`、146 个 `.h`、3 个 `.js`、7 个 `.ps1`、2 个 `.py`。原始第一方另含归档 1 个 `.c`、3 个 `.h` 和 7 个 `.py`。

## 3. 正式目标 CMake 对比

| 项目 | 数量 | 结论 |
|---|---:|---|
| `main/` 活动 `.c` | 194 | 排除 `_archive` 的文件系统清单 |
| `main/` 原始 `.c` | 195 | 含归档 `p3_device_voiceprint_goertzel.c` |
| `SRCS_COMMON` | 74 | 无条件公共源条目 |
| 条件 OTA 源条目 | 5 | 仅在 `CONFIG_NUWAX_OTA_ENABLE` 下加入 |
| `SRCS_LVGL_UI` | 71 | UI/资源源条目；含 `face_path_blob.c` |
| CMake 唯一源条目 | 150 | 全部文件存在、无重复 |
| 活动源码未进入上述源表 | 44 | 36 个 host 资产，8 个非 host 文件 |
| 原始源码未进入上述源表 | 45 | 活动 44 个 + 归档 1 个 `.c` |

## 4. 非 host 未链接文件

| 文件 | 初步判定 | 当前风险/后续动作 |
|---|---|---|
| `main/audio_reliable_capture.c` | 30 ms 可靠采集原型 | 含 I2S TODO；不得计为当前采集实现，决定删除/归档/迁移 |
| `main/audio_reliable_playback.c` | 30 ms 播放原型 | 含 I2S TODO；不得计为当前播放实现，决定删除/归档/迁移 |
| `main/conversation/d1_playback_path.c` | 历史 D1 播放草稿 | 未进入构建；正式播放以 `speaker_output`/`speaker_playback` 为准 |
| `main/filter_conservative.c` | 30 ms 简化过滤原型 | 不得计为当前环境音过滤或 AEC 证据 |
| `main/full_duplex_coord.c` | 全双工占位实现 | 仅返回初始化成功，不能作为双讲已实现证据 |
| `main/state_cmd_fast.c` | 命令队列原型 | 实际 volume/emotion/LED 动作为 TODO；正式命令链路另行审计 |
| `main/nuwax_ui_v28/assets/faces/face_21_listening_360_rgb565.c` | 生成但未列入 CMake 的大资源 | UI 资源阶段确认是否由 diff/core 资源替代或为遗漏 |
| `main/nuwax_ui_v28/assets/faces/face_22_speaking_360_rgb565.c` | 生成但未列入 CMake 的大资源 | UI 资源阶段确认是否由 diff/core 资源替代或为遗漏 |

## 5. 审计规则

1. 未链接文件不影响当前镜像的运行路径，但会造成能力判断和后续迁移风险。
2. 任何文件以后加入 CMake，原有静态结论失效，必须重新执行配置、资源、协议和实机验证。
3. host check 必须证明其参数来自当前产品配置；host stub 通过不能替代硬件或产品构建证据。
4. 第三方代码不做无差别逐行重审，但项目覆盖层、配置、回调、分配器、线程边界和已知漏洞必须审计。
5. `main/_archive/v15-goertzel-failed/` 与 `tools/_archive/v15-goertzel-failed/` 共包含 11 个代码/脚本文件；搜索命令必须使用文件系统枚举或显式 `--no-ignore`，避免被 `rg` 默认规则漏掉。

## 6. 对应计划校验

- 覆盖计划 2.1、2.2、2.4 和阶段 0.2。
- CMake 文件存在性与重复检查通过。
- 未链接非 host 文件已全部进入待处置清单。
- 当前仍未完成：各模块 owner/调用图、第三方实际链接成员、配置 profile 构建矩阵。
