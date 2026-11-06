# SD 断网与补传测试会话

本次使用独立构建目录 `C:/tmp/nuwax-sd-test-20260911`，配置文件为
`tmp/sdkconfig.sd-replay-test`，可复用覆盖配置为 `sdkconfig.sd-replay-test.defaults`。
启用 `NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE` 后显示 UI 模拟断网开关。
正式云端继续使用设备配网保存的地址及密钥；只有 SD 归档补传走本测试网关。

## 接收端

- 地址：`ws://192.168.31.180:8810/api/device/ws`，设备应连接能访问此 WLAN 地址的网络。
- 接收器：`tools/protocol/offline-audio-replay-gateway.js`，后台独立 Node.js 进程。
- 本次不启用 `--mode`、`--auto-open` 或故障注入，不用它控制实时会议/监控。
- 输出目录：`runtime-logs/sd-replay-test-20260911/`。
- `gateway.log` / `gateway-error.log`：运行日志；`archives/`：持久化归档。
- `report.json`：连接关闭时写入的统计报告；未关闭时以日志和归档文件为准。
- `gateway.pid`：启动的进程 ID。主机自测使用独立临时目录，不计入设备测试。

启动已检查监听端口及本机 TCP 连通；现有 Node.js Public TCP 入站规则允许访问，
Domain/Private 防火墙当前关闭。新增专用规则被系统拒绝，未改变现有防火墙设置。
本机检查不能替代设备到网关的实际网络验证。测试期间保持电脑运行及 WLAN 地址不变。

修订记录（2026-09-12）：修复模拟断网分支跳过离线采集重试、归档发送锁与 I/O
预算耦合、独立网关受主 sender 门控，以及安全发送超时不重试的问题。旧网关目录
中的 `.part` 是修复前会话的残留；重新验收时应记录其起始 `next_offset`，或使用
新的 `--storage-dir` 目录建立干净基线。

修复版测试固件最近一次构建产物为 `C:/tmp/nuwax-sd-test-20260911/NuwaxAIPartner.bin`，
SHA-256：`AD3111E8934E00F43CE0162BB67B1E049D2EA918238BEB54FD2C92B4F6D0464C`。
构建配置明确启用测试网关、八小时离线时长和 1.5 GiB 归档配额。

## 测试顺序与结果判断

1. 烧录测试固件后正常连接 Wi-Fi 和正式云端，开始会议或监控。
2. 开启模拟断网，验证会议连续录音或监控人声录音。
3. 关闭模拟断网，等待恢复联网。停止会议/结束当前人声归档并完成 SD 收尾后再观察补传；录音占用 SD 期间补传主动让出。不要把重新配网开关当作断网开关。
4. 每组测试记录模式、开始/结束时间、断网时长、有声/静音区间及设备串口日志。
5. 测试后检查网关连接、begin/chunk/commit、CRC、已提交文件及设备删除确认。
   仅有 TCP 连接或 chunk 不表示整个分段成功；需 commit 成功及完整归档校验。

测试构建中的独立 replay WebSocket 不再等待主实时 sender 的队列空闲窗口；它仍
受每个分片发送的 25 ms lifecycle 准入、250 ms I/O 预算和重试保护。正式云端共享
主连接时继续使用实时队列门控。2026-09-12 的后续修复为两个传输增加了共同的
SD 录音优先门控，覆盖任务启动、目录扫描、CRC 扫描、分片读盘和提交删除。
模拟断网分支也持续运行离线采集重试，首次 SD 准入暂时失败不会跳过整个断网窗口。

网关没有实时转写功能。测试补传数据应只在此归档接收端出现。
复核时区分历史 SD 积压与本次录音；设备 sequence/uptime 不能当作 UTC 时间。

## 烧录

在工程目录运行 `powershell -ExecutionPolicy Bypass -File tools/flash-sd-replay-test.ps1 -Port COM7`，
将 COM7 换为实际端口。脚本读取本次构建的 `flasher_args.json`，烧录全部清单镜像，
含 bootloader、分区表、OTA 选择、语音模型、应用和表情资源；不擦除 NVS 或 SD。
本次准备阶段不执行烧录，待用户自行烧录后进行设备测试。

## 2026-09-12 录音优先修复

详见 [修复与验收记录](sd-recording-priority-fix-20260912.md)。上方旧版 SHA 不代表
本次构建；以该记录及实际二进制 SHA 为准。

启动测试网关：`powershell -ExecutionPolicy Bypass -File tools/start-sd-replay-gateway.ps1`。
网关默认监听 `0.0.0.0:8810`，设备连接地址仍为 `192.168.31.180:8810`，因此
WLAN 短暂重连或地址切换不会让监听器绑定失败；可用 `-ListenAddress` 显式限制接口。
停止网关：同一指令追加 `-Stop`。监督进程每次重启保留独立日志和报告，归档仍使用
原 `runtime-logs/sd-replay-test-20260911/archives`，便于继续旧 `.part` 的断点补传。
监督进程为本次本机会话服务，不安装系统服务，不在电脑重启后自动启动。
