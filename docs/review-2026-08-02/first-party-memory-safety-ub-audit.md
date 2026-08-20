# B0-20 第一方 C/H 内存安全与未定义行为 A 级静态审计

生成日期：2026-08-03

状态：**A 级静态证据已闭合；B/C/D 证据均未执行，不能据此宣称产品已通过 memory-safety、sanitizer、构建或实机验收。**

## 1. 结论

本轮对排除任意 `_archive` 路径后的 350 个第一方 `.c/.h` 文件建立逐文件覆盖矩阵，并对正式产品入口、协议、音频、OTA、配网、UI、bootloader 与未链接原型实施分级静态审计。

确认 5 个独立 `MEM-*` 问题：

| ID | 等级 | 当前镜像关系 | 结论 |
|---|---|---|---|
| `MEM-001` | P1 | `p3_afe_l3.c` 已链接；需启用 L3 路径 | AFE chunksize 未验证，影响分配、除法、固定 256 样本栈数组和复制 |
| `MEM-002` | P1（历史） | face cache 正式 UI 路径 | 原 NXFP 未对齐读取、无尾界解析和 signed overflow 路径已完成 A 级静态修复；B/C/D 验证开放 |
| `MEM-003` | P2 | audio frame 正式协议 helper | PCM 样本数乘法和 byte-buffer -> `int16_t *` 的对齐/effective-type 契约未封闭 |
| `MEM-004` | P2 | 两个 prototype **未进入当前固件** | dormant reliable playback/capture API 对负长度与 NULL 指针防御不足 |
| `MEM-005` | P2 | drawer/state 正式 UI 路径 | 三个枚举直接索引固定数组，公开状态入口未统一验证枚举范围 |

本轮没有确认新的 P0。当前仍开放的 `MEM-*` 为 P1=1、P2=3；另保留 1 个已 A 级静态修复但尚未完成 B/C/D 验证的历史 P1（`MEM-002`）。既有 UAF、泄漏、并发数据竞争、跨核可见性和 cache-disabled 风险沿用原编号，不以 `MEM-*` 重复计数。

## 2. 范围、分母与构建角色

统计口径：`main/`、`components/`、`bootloader_components/`、`test/`、`tools/` 下所有 `.c/.h`，排除路径组件名为 `_archive` 的历史资产。第三方 `lib/` 与 `managed_components/` 不做逐文件第一方审计；项目兼容层、回调、长度和所有权边界仍在本轮范围内。

上述五个第一方活动区域没有 `.cpp/.hpp` 文件，因此计划中的 C/C++ 分母在当前基线实际收敛为本矩阵的 204 个 C 与 146 个 H；这不是对现存 C++ 文件的排除。

| 区域 | C | H | 合计 |
|---|---:|---:|---:|
| `main/` | 194 | 141 | 335 |
| `components/` | 1 | 0 | 1 |
| `bootloader_components/` | 1 | 0 | 1 |
| `test/` | 7 | 4 | 11 |
| `tools/` | 1 | 1 | 2 |
| **总计** | **204** | **146** | **350** |

`main/CMakeLists.txt` 静态解析得到 150 个唯一正式 C source 条目；44 个活动 `main/*.c` 未进入正式 target，其中 36 个是 host check，8 个是非 host 原型/生成资源。另有 bootloader C 1 个、compat component C 1 个。

逐文件真值位于 `first-party-memory-safety-file-matrix.csv`。矩阵的 build role 明确区分正式链接、条件 OTA、生成资源、host、未链接原型、bootloader、兼容层和接口文件。

A2-04 续接基线发生 9 个第一方 C/H 内容漂移后，已重新定向检查监控 VAD/能量算术、preroll 分配与环形索引、pending-frame 固定容量、capture ACK queue/retry、协议唯一字段和主入口/session 接线，并刷新对应矩阵行数。未确认新的独立 A 级内存缺陷；这不等价于 host、构建、设备或监控产品语义通过。

## 3. 证据等级与方法

| 覆盖等级 | 文件数 | 本轮含义 |
|---|---:|---|
| `A-manual-deep` | 37 | 对高风险正式 C 入口沿长度、乘法、索引、对齐、aliasing、栈、heap、错误回滚和跨任务所有权深查 |
| `A-manual-targeted` | 82 | 对其余产品/bootloader/component C 与 dormant prototype 做定向人工审阅 |
| `A-pattern-scan` | 44 | 对 host/test/tool C 做全文件危险模式扫描；不等于执行 |
| `A-header-contract` | 146 | 审阅公开类型、指针+长度、枚举、布局、ownership、callback 与 mutability 契约 |
| `A-generated-data-scan` | 41 | 审阅生成数组/descriptor 的静态形状和链接角色；不替代 generator/render 验证 |
| **合计** | **350** | 路径唯一、无遗漏 |

A 级执行内容：

1. 复算实际文件系统分母与 CMake source 分子。
2. 对 `memcpy/memmove/memset`、字符串/格式化、分配/释放、乘法、signed/unsigned、pointer cast、固定/动态栈对象、RTOS owner、DMA/PSRAM 与 flash/persistence 进行全文件扫描。
3. 对协议和外部输入路径检查长度先验、累加溢出、结构体/字节布局、对齐、NUL 和拒绝路径。
4. 对实时音频、UI cache、OTA、BLE/配网和 worker 检查 buffer 生命周期、partial init、join/delete 和 callback ownership。
5. 既有阶段 1 问题按原 ID 交叉引用，避免同一根因重复计数。

证据边界：

- **B 级未执行**：没有运行 clang/GCC sanitizer、ASan/UBSan、fuzz/property、allocation failure 或 host 并发交错。
- **C 级未执行**：没有构建、链接、map/ELF 检查或 profile 差分；“build role”仅来自 CMake 静态解析。
- **D 级未执行**：没有操作 COM5、OTA 或设备，没有 DMA/cache-disabled、栈水位、heap poison、长稳和真实异常注入证据。

## 4. 新确认问题

### MEM-001（P1）：P3 AFE chunksize 未建立可信边界

证据：`main/p3_afe_l3.c:45-64,112-141`。

`afe_aec_get_chunksize()` 的有符号返回值在没有验证的情况下：

- 于 `:53-54` 转为 `size_t` 并参与分配乘法；
- 于 `:122` 成为除数；
- 于 `:124-138` 决定固定 `ref_buf[256]`、MR/OUT buffer 的访问和复制长度；
- 负值还会在 `nuwax_p3_software_ref_read_delayed()` 与 `memset/memcpy` 的长度转换中放大。

若返回 0，可除零；若为负数，可形成巨大的 `size_t`；若大于 256，可越过 `ref_buf`。这不是 AFE 库“通常返回正常值”能够消除的调用方内存安全契约。

关闭要求：

1. init 时验证 `0 < chunksize <= NUWAX_P3_L3_MAX_CHUNK_SAMPLES`，并在全部乘法前检查 `SIZE_MAX`。
2. 取消隐含 256 常量：按已验证 chunk 动态分配 reference buffer，或保留固定数组并以 `static_assert` 和 runtime guard 双重约束。
3. `frames > 0` 时拒绝 NULL samples；用 `size_t` 计算 chunk count 和 used frames。
4. 与既有 `API-002` 一并把 enable/teardown/process/stats 收入单 owner 或同一同步域，避免修完长度后仍发生并发 UAF。
5. B 级用 chunk=`INT_MIN,-1,0,1,256,257,INT_MAX` 的 AFE stub 跑 ASan/UBSan；D 级验证实际 AFE chunksize 与实时栈/heap 水位。

### MEM-002（P1 历史项）：NXFP parser 原始对齐/边界/overflow 路径已完成 A 级静态修复

原始证据：`main/nuwax_ui_v28/face/face_cache.c` 的旧 typed-load parser；当前实现证据为 `main/nuwax_ui_v28/face/face_path_blob.c:1-176`、`face_path_blob.h:1-31`、`face_cache.c:101-145,550-570` 和 `main/CMakeLists.txt:166`。`main/face_path_blob_host_check.c:1-121` 是独立 host 检查入口，但本轮未执行。

原始问题确实存在：旧代码把任意 byte cursor 转为 `uint32_t *`、`uint16_t *`/`int16_t *`，没有使用 `face_paths_core_end` 建立尾界，并用 signed `int` 做 scanline 交点乘法。已落盘的当前修复完成了以下静态闭合：

1. `read_u16_le/read_u32_le/read_i16_le` 逐字节读取，不依赖 blob 对齐或 effective type。
2. `nuwax_face_path_find_expression()` 以 `blob`/`blob_size` 建立表达式边界，验证 magic、offset table、offset 顺序、header 下界和 expression header。
3. `nuwax_face_path_next_layer()` 每次读取前检查 `cursor/end` 剩余长度，验证 subpath 数量、每条路径非零点数、点数总和、点数组容量，并要求最后一层精确消费 expression 尾界。
4. `face_cache.c` 从链接器提供的 `face_paths_core_start/end` 传入真实 blob 长度；扫描线中间计算改为 `int64_t` 并显式夹紧到 `int`。

因此，`MEM-002` 的**原始 A 级静态缺陷不再对当前代码成立**，但它仍保留为历史 P1 追踪项：

- B 级尚未用 ASan/UBSan、fuzz 和未对齐/截断/极端计数组合执行；host check 的存在不等于通过。
- C 级尚未构建、确认该 parser 与 embedded blob 的实际 ELF 链接及符号边界。
- D 级尚未做快速表情切换、画面完整性、低内存和异常资源验证。
- `RTOS-006` 的跨核 cache metadata 发布和 `LIFE-008` 的重复 init 泄漏仍独立开放，不能因 parser 修复而关闭。

### MEM-003（P2）：audio frame 的长度乘法与 typed payload 契约未封闭

证据：`main/audio_frame.c:63-85,127-158`。

`nuwax_audio_frame_build_pcm16()` 在 wrapper 内直接计算 `sample_count * sizeof(int16_t)`。generic builder 虽然检查 payload 协议宽度和目标容量，但乘法已经先发生；超大 `sample_count` 可回绕。parser 又把 byte frame 的 payload 地址直接返回为 `const int16_t *`，没有冻结“输入地址至少 2 字节对齐、对象可按 int16_t 有效类型读取”的契约。

当前正式 WS buffer 来自对齐 heap，header 22 字节也是偶数，且业务帧长有上限，因此不能声称已观察到设备崩溃；公共 helper 仍不应依赖这些隐含调用点条件。

关闭要求：

- wrapper 先检查 `sample_count <= UINT16_MAX / sizeof(int16_t)`、乘法不溢出，且非零 count 要求 samples 非 NULL。
- parser 同时验证 version/codec/payload，并选择逐样本 LE 解码到 caller buffer；若坚持 zero-copy，API 必须返回 bytes 并由调用者用安全 reader 消费，或以可证明的对齐/对象契约和静态断言封闭。
- B 级对 0、边界、`SIZE_MAX`、奇数 payload、未对齐 base、截断帧运行 sanitizer/fuzz。

### MEM-004（P2，当前固件不受影响）：未链接 reliable audio prototype 长度/指针防御不足

证据：`main/audio_reliable_playback.c:22-32`、`main/audio_reliable_capture.c:43-68`；二者均不在 `main/CMakeLists.txt`。

`audio_playback_push()` 只截断 `n > 480`，没有拒绝 `n < 0`。负数与 `sizeof(int16_t)` 相乘后转换成巨大 `size_t` 传给 `memcpy`，可直接越界；`pcm == NULL` 和 `audio_playback_pop(out == NULL)` 也没有保护。capture 原型会拒绝非 480 长度，因此负数不会进入 copy，但输入/输出 NULL 仍依赖未声明契约。

这是 definite dormant API defect，不进入当前固件，不得写成当前播放/采集故障。

关闭要求：删除/归档原型，或在未来链接前把 count 改为 `size_t`、验证所有指针/容量、保存实际帧长/清零尾部，并补 SPSC/多核内存模型；随后执行 B 级 sanitizer/并发测试和 C 级正式构建。

### MEM-005（P2）：UI 枚举索引与公开状态入口缺少统一验证

证据：

- `main/nuwax_ui_v28/nuwax_drawer.c:245-253,276-278` 直接执行 `values[wifi/cloud/power]`；
- `dialog_text():255-261` 已有正确的 unsigned 范围检查，形成明确对照；
- `main/nuwax_ui_v28/nuwax_device_state.c:30-36` 的公开 whole-state setter 直接复制任意 struct；
- `main/app_shell.c:114-167,174-203` 的公开 delta 入口验证 field mask，但没有统一验证所有 enum payload。

当前正式 Wi-Fi、cloud、电量生产者通常夹紧或只写合法常量，因此本项不是“外部网络已可直接触发”的结论。但公开状态 API、初始化错误、未来新增调用或内存损坏均可把负数/超范围枚举送入 drawer，导致数组越界读指针。

关闭要求：

1. 为 Wi-Fi/cloud/power/dialog/meeting/monitoring 增加显式 `*_COUNT` sentinel。
2. 在 whole-state 和 delta 发布边界统一 normalize/reject；不能只依赖各 setter。
3. 所有 table lookup 使用 `(unsigned)value < ARRAY_SIZE(values)`，非法值返回“未知”并记录受限告警。
4. B 级枚举全域/property test 覆盖负值、count、`INT_MAX` 和并发 mailbox publish。

## 5. 既有问题去重交叉引用

以下问题具有内存安全或 C UB 后果，但已在阶段 1 建立 canonical ID，本文件只补充到逐文件矩阵，不重复计数：

| 既有 ID | 内存安全/UB 关联 |
|---|---|
| `API-001` | MIC 重复 init 先清 live metadata，owner/resource 可能不可达 |
| `API-002` | P3 L3 process 与 teardown 无同步，可对已释放 AFE/buffer 访问；与 `MEM-001` 必须一起修 |
| `API-004` | audio uplink per-context 表面下仍有 process-global staging 和 callback 重入契约 |
| `API-005` | cloud config 普通全局 snapshot 并发读写构成数据竞争 |
| `API-006` | speaker 重复 init 替换 live queue/mutex，worker 可能使用旧对象 |
| `API-007` | software ref 首次建锁/重配置前置读缺少同步 |
| `API-008` | meeting/monitoring 重复 init 先清 caller object，queue owner 丢失 |
| `API-009` | wake 首次并发 init/deinit 可覆盖 singleton |
| `API-010` | LVGL callback/context 不是一个同步发布单元 |
| `LIFE-001` | barge dispatcher partial init 残留 lock/queue |
| `LIFE-002` | Vocat 诊断 TX timeout 后 worker 继续访问调用者栈，属于明确 UAF |
| `LIFE-004` | LVGL 部分初始化失败不逆序回滚，timer 可在无 owner 资源上继续 |
| `LIFE-005` | USB provisioning worker 失败后全局 callback/context 残留 |
| `LIFE-006` | LED setter/deinit 交错可使用已删除 task/mutex handle |
| `LIFE-007` | active-tone 超时后 heap ctx/task/lease 可能永久泄漏 |
| `LIFE-008` | face cache 重复 init 泄漏旧 slot/queue/task，约 1 MiB 量级 |
| `LIFE-009` | WS RX 立即退出状态可被 creator 覆盖为 OPEN，owner/resource 状态不闭合 |
| `LIFE-010` | OTA controller partial init 不撤销 callback/bind/interlock |
| `RTOS-002` | 多个关键 worker 使用 PSRAM 栈，cache-disabled/ROM 调用安全性未证明 |
| `RTOS-006` | face cache metadata 和约 259 KiB slot buffer 跨核无同步发布 |
| `RTOS-007` | BLE endpoint handler 与 stop/delete 缺少 in-flight 引用屏障 |
| `CACHE-001` | 普通运行期 NVS/flash 与实时音频缺统一仲裁；OTA 写入已有 quiesce，不能误归因 |
| `CROSS-001` | P5 worker 无同步访问完整 `s_prelisten`，C 数据竞争和跨 generation 行为风险 |
| `CROSS-002` | Vocat 诊断用 `volatile bool` 跨核同步，不建立 C11 happens-before |

## 6. 已确认正向控制

以下是 A 级正向证据，不等于 B/C/D 通过：

1. 正式 CMake product source 未发现真实 VLA；大型工作区主要使用固定上限或 heap/PSRAM。
2. `speaker_output.c:437-448,637` 在 mono->stereo 容量计算前检查 `SIZE_MAX`。
3. `ws_transport.c:52,1231-1274` 把入站 frame 限制为 16 KiB；URL/handshake 固定 buffer 有长度门。
4. OTA transport 在 `nuwax_ota_transport.c:431-481,520-681,745-795` 检查 content length、累计 size、SHA 和单一 cleanup。
5. provisioning 的 `ssize_t` 只在正值确认后转为 `size_t`；固定字段使用有界复制。
6. V2 MIC TDM 读取用 raw capacity 限制 frame 数，heap 返回对齐支撑当前宽类型访问；32-bit 公共分支的任意 count 仍列残余。
7. monitoring preroll 在 `monitoring_mode.c:1075-1087` 先检查 frames*samples 和字节乘法，再分配 current/pending/preroll buffers；内部 enqueue 调用均受固定 `frame_samples` 契约约束。
8. P3 software ref 在 `p3_software_ref.c:167,651-652` 用 `uint64_t/SIZE_MAX` 检查容量/span。
9. 主对话 realtime capture 在 `nuwax_ai_partner_main.c:5999-6000,7090-7092` 限制 stored sample count 和 samples_per_frame，slot 索引也有 queue-depth 门。
10. P5 probe 在 `p5_fullduplex_overlap_probe.c:828` 限制 frame samples，内部 chunksize 限制为 1..2048，duration 上限保持累加器可表示。
11. UI menu 的 item/style/size 索引已在使用前 clamp/量化；不能由 `MEM-005` 反向概括成“全部 UI 索引都不安全”。
12. host 中的固定 `strcpy` fixture 与 stub 均在矩阵标成 non-product；文件存在或扫描通过不构成设备证据。

## 7. 未升级为当前产品缺陷的残余项

| 路径 | A 级判断 | 关闭动作 |
|---|---|---|
| `display.c:596-641` | 静态 LCD bytecode parser 没有完整尾界检查；当前输入是编译期固定表，未形成独立当前触发链 | 为表项加 end/count parser 和静态生成校验 |
| `p3_software_ref.c` public read/write | 任意 `sample_count` 的乘法/调用者容量契约偏弱；正式调用固定帧 | API 增加显式 capacity 或上限，fuzz 边界 count |
| `session_protocol.c` | public `int len` ABI 偏弱；正式 WS 入口保证 0..16384 | 改为 `size_t` 或入口统一拒绝负值/窄化 |
| `lvgl_psram_mem.c` | 对齐依赖 ESP heap/ABI；当前没有证据证明错误 | 增加 `max_align_t` 静态/运行断言和 allocator 测试 |
| `vocat_audio_board.c` | AEC health 局部数组约 5360 B；当前 app_main stack 为 16 KiB，只能判为水位残余 | D 级记录高水位，故障路径也必须覆盖 |
| `live_aec.c` rescue helper | 公共 helper 假定 `segment_count <= 8`；正式 producer 通过 append helper 保持 | API 内部再次验证，property test 破坏契约 |
| `mic_input.c:414-421` 32-bit 分支 | `sample_count * 4` 没有先验防回绕；当前 V2 TDM 正式路径不走该分支且调用帧固定 | 公共 API 先做除法式上限检查并 fuzz |
| generated assets | C initializer/descriptor 可静态扫描，但不证明未来 generator 输出与 parser 一致 | 沿用生成资产 provenance，并为 NXFP 增加 schema/fuzz |

## 8. B/C/D 关闭门

### B 级：host sanitizer、fuzz 与故障注入

必须先冻结可重复的 compiler/runner，再完成：

- `audio_frame`、`session_protocol`、WS frame、OTA manifest、BLE fields 和 NXFP parser 的 ASan/UBSan/fuzz；
- allocator 逐点失败、partial init/reinit、worker timeout/join 和 enum property test；
- `MEM-001` 的恶意 chunksize AFE stub；
- `MEM-004` 若不删除，则在任何链接前通过负长度/NULL/并发 ring 测试；
- 所有测试保存命令、compiler 版本、退出码、corpus/hash 和 sanitizer 零错误证明。

当前 `TESTCAP-001` 仍开放，因此本文件不能把“建议执行”写成“已经执行”。

### C 级：构建、链接与静态布局

- 对 release、OTA、诊断等实际 profile 构建；
- 验证 150 个 main source 的条件成员、未链接原型确实不进 ELF；
- 保存 map/ELF、section、stack-usage、warning-as-error、UB 相关 compiler diagnostics；
- 对关键 struct、DMA descriptor 和 embedded blob base/end 做 `static_assert`/link symbol 检查。

### D 级：ESP32-S3 目标机

- heap poison/完整性、stack watermark、长稳和分配失败；
- NVS/flash 与实时音频并发、cache-disabled/PSRAM 栈路径；
- DMA/I2S buffer 能力、对齐和 owner 切换；
- WS 断连、OTA 中断、BLE stop、快速 UI 切换、低内存和硬件复位；
- 所有 `MEM-*` 关闭项必须有设备日志/指标与无崩溃、无越界、无泄漏判据。

## 9. 覆盖矩阵完整性与结论边界

`first-party-memory-safety-file-matrix.csv` 包含 1 个 header + 350 个数据行，路径唯一。角色计数：

- product main linked/conditional/generated：150 个 C；
- main host check：36 个 C；
- main dormant prototype：6 个 C；
- main 未链接生成资源：2 个 C；
- bootloader/component：2 个 C；
- test/tool host C：8 个；
- headers：146 个。

最终 A 级结论是：**350/350 第一方活动 C/H 已建立可追溯静态覆盖；`MEM-001~005` 均有唯一追踪状态，其中 `MEM-002` 原始路径已 A 级静态修复，其余仍开放，并把既有生命周期/并发风险去重映射到相应文件。** 它不等价于“内存安全已通过”或“设备可稳定发布”；在 B/C/D 门全部关闭前，B0-20 只能标记为“A 级完成、动态与目标机证据开放”。
