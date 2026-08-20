# NuwaxAIPartner V2 问题分析报告

**文档版本**: 1.0  
**分析日期**: 2026-07-31  
**分析范围**: 正常对话模式、状态机同步、音频处理、WebSocket通信  
**分支**: codex/smart-barge-in-v1

---

## 目录

1. [问题概览](#1-问题概览)
2. [问题1: 监听模式UI状态缺失](#2-问题1-监听模式ui状态缺失)
3. [问题2: IDLE状态图标语义不准确](#3-问题2-idle状态图标语义不准确)
4. [问题3: WebSocket断连后设备卡死](#4-问题3-websocket断连后设备卡死)
5. [问题4: 对话过程中语音中断](#5-问题4-对话过程中语音中断)
6. [问题5: 启动阶段无区分图标](#6-问题5-启动阶段无区分图标)
7. [问题汇总与优先级](#7-问题汇总与优先级)
8. [附录: 关键代码位置索引](#8-附录-关键代码位置索引)

---

## 1. 问题概览

| 编号 | 问题 | 严重度 | 影响 |
|------|------|--------|------|
| 1 | 监听模式UI状态缺失 | 🔴 高 | 监听模式期间显示错误的月亮图标 |
| 2 | IDLE状态图标语义不准确 | 🟡 中 | IDLE状态显示声波图标暗示"正在聆听" |
| 3 | WebSocket断连后设备卡死 | 🔴 严重 | 设备完全无响应，只能硬重启 |
| 4 | 对话过程中语音中断 | 🟡 中 | 用户说话突然中断，对话体验差 |
| 5 | 启动阶段无区分图标 | 🟢 低 | 启动过程中无法区分阶段 |

---

## 2. 问题1: 监听模式UI状态缺失

### 2.1 问题描述

监听模式激活期间，状态栏可能显示错误的月亮图标（SLEEPING状态图标），而不是正确的LISTENING图标。

### 2.2 代码证据

**证据1: `nuwax_device_state.h:76-94`** — 设备状态结构体中只有会议模式UI状态，没有监听模式

```c
typedef struct {
    nuwax_emotion_t emotion;
    nuwax_wifi_state_t wifi;
    nuwax_cloud_state_t cloud;
    nuwax_dialog_state_t dialog;
    nuwax_power_state_t power;
    nuwax_meeting_ui_state_t meeting;   // ← 只有会议模式
    // ❌ 没有 nuwax_monitoring_ui_state_t monitoring;
    uint8_t volume;
    // ...
} nuwax_device_state_t;
```

**证据2: `nuwax_status_bar.c:936-940`** — 状态栏只检查会议模式，不检查监听模式

```c
/* Meeting capture is independent of the ordinary conversation state.  A
 * persistent listening glyph keeps continuous recording visible even when
 * the main task is otherwise idle or the menu is closed. */
const nuwax_dialog_state_t visual_dialog =
    state->meeting != NUWAX_MEETING_UI_DISABLED ? NUWAX_DIALOG_LISTENING : state->dialog;
// ❌ 没有检查 state->monitoring
```

**证据3: `nuwax_ai_partner_main.c:3518-3544`** — 监听模式状态回调不更新UI

```c
static void monitoring_state_changed(void *ctx,
                                     nuwax_monitoring_state_t state,
                                     const char *reason) {
    // ... 清理逻辑 ...
    if (state == NUWAX_MONITORING_STATE_DISABLED && s_active_ws &&
        nuwax_ws_is_connected(s_active_ws)) {
        enter_sleeping_conversation_state("monitoring_end");
    }
    // ❌ 没有调用 nuwax_app_shell_set_monitoring_state()
}
```

**对比**: 会议模式回调 (`nuwax_ai_partner_main.c:3508-3510`):

```c
if (s_ui_ready) {
    nuwax_app_shell_set_meeting_state((nuwax_meeting_ui_state_t)state);  // ✅ 更新UI
}
```

**证据4: `app_shell.h`** — 没有声明 `nuwax_app_shell_set_monitoring_state()` 函数

```c
void nuwax_app_shell_set_meeting_state(nuwax_meeting_ui_state_t state);  // ✅ 有
// ❌ 没有 nuwax_app_shell_set_monitoring_state()
```

### 2.3 影响

| 场景 | 实际行为 | 预期行为 |
|------|---------|---------|
| 监听模式激活，应用状态=IDLE | 显示**月亮图标**（SLEEPING） | 应显示LISTENING图标 |
| 监听模式激活，应用状态=LISTENING | 显示LISTENING图标（碰巧正确） | 显示LISTENING图标 |
| 监听模式结束 | 正确进入SLEEPING，显示月亮 | 显示月亮 |

### 2.4 解决方案

**方案A: 添加监控模式UI状态字段（推荐）**

1. 在 `nuwax_device_state.h` 中添加:

```c
typedef enum {
    NUWAX_MONITORING_UI_DISABLED = 0,
    NUWAX_MONITORING_UI_STARTING,
    NUWAX_MONITORING_UI_ACTIVE,
    NUWAX_MONITORING_UI_STOPPING,
    NUWAX_MONITORING_UI_ERROR,
} nuwax_monitoring_ui_state_t;

typedef struct {
    // ... 现有字段 ...
    nuwax_meeting_ui_state_t meeting;
    nuwax_monitoring_ui_state_t monitoring;  // ← 新增
    // ...
} nuwax_device_state_t;
```

2. 在 `app_shell.h` 中声明:

```c
void nuwax_app_shell_set_monitoring_state(nuwax_monitoring_ui_state_t state);
```

3. 在 `app_shell.c` 中实现（仿照 `set_meeting_state`）:

```c
void nuwax_app_shell_set_monitoring_state(nuwax_monitoring_ui_state_t state) {
    if (state > NUWAX_MONITORING_UI_ERROR) {
        state = NUWAX_MONITORING_UI_ERROR;
    }
    if (!nuwax_lvgl_port_lock(500)) {
        ESP_LOGW(TAG, "failed to lock LVGL for monitoring state=%d", (int)state);
        return;
    }
    nuwax_device_state_t snapshot = *nuwax_device_state_get();
    snapshot.monitoring = state;
    nuwax_device_state_set(&snapshot);
    nuwax_lvgl_port_unlock();
}
```

4. 在 `nuwax_status_bar.c:939` 修改:

```c
const nuwax_dialog_state_t visual_dialog =
    (state->meeting != NUWAX_MEETING_UI_DISABLED ||
     state->monitoring != NUWAX_MONITORING_UI_DISABLED)
        ? NUWAX_DIALOG_LISTENING
        : state->dialog;
```

5. 在 `nuwax_ai_partner_main.c:3518-3544` 的 `monitoring_state_changed` 中添加:

```c
if (s_ui_ready) {
    nuwax_app_shell_set_monitoring_state((nuwax_monitoring_ui_state_t)state);
}
```

---

## 3. 问题2: IDLE状态图标语义不准确

### 3.1 问题描述

IDLE状态显示声波图标（LISTENING），但设备实际**未在监听**，只是等待唤醒。用户可能误以为设备在聆听。

### 3.2 代码证据

**证据1: `nuwax_state_bridge.c:260-262`** — IDLE映射为LISTENING图标

```c
case NUWAX_APP_STATE_IDLE:
    s.dialog = NUWAX_DIALOG_LISTENING;  // ← 声波图标
    break;
```

**证据2: `nuwax_dialog_state_t` 枚举** (`nuwax_device_state.h:49-56`)

```c
typedef enum {
    NUWAX_DIALOG_SLEEP = 0,       // 月亮
    NUWAX_DIALOG_LISTENING,       // 声波
    NUWAX_DIALOG_RECOGNIZING,     // 动态点
    NUWAX_DIALOG_THINKING,        // 动态点
    NUWAX_DIALOG_SPEAKING,        // 条形图
    NUWAX_DIALOG_ERROR            // 抖动
} nuwax_dialog_state_t;
```

**没有"待机/空闲"专用图标**。

### 3.3 影响

| 状态 | 实际含义 | 显示图标 | 用户理解 |
|------|---------|---------|---------|
| IDLE | 等待唤醒，**未在监听** | 声波(LISTENING) | **以为在聆听** |
| SLEEPING | 休眠，**未在监听** | 月亮(SLEEP) | 正确理解 |
| LISTENING | **正在监听** | 声波(LISTENING) | 正确理解 |

### 3.4 解决方案

**方案A: 为IDLE添加专用图标（推荐）**

1. 在 `nuwax_dialog_state_t` 中添加:

```c
typedef enum {
    NUWAX_DIALOG_SLEEP = 0,
    NUWAX_DIALOG_IDLE,            // ← 新增：待机图标（如眼睛闭合）
    NUWAX_DIALOG_LISTENING,
    NUWAX_DIALOG_RECOGNIZING,
    NUWAX_DIALOG_THINKING,
    NUWAX_DIALOG_SPEAKING,
    NUWAX_DIALOG_ERROR
} nuwax_dialog_state_t;
```

2. 在 `nuwax_state_bridge.c:260` 修改:

```c
case NUWAX_APP_STATE_IDLE:
    s.dialog = NUWAX_DIALOG_IDLE;  // ← 使用新图标
    break;
```

3. 在 `nuwax_status_bar.c` 的 `dialog_icon_for_state()` 中添加映射

**方案B: 将IDLE映射到SLEEP图标（简单方案）**

```c
case NUWAX_APP_STATE_IDLE:
    s.dialog = NUWAX_DIALOG_SLEEP;  // ← 使用月亮图标
    break;
```

---

## 4. 问题3: WebSocket断连后设备卡死

### 4.1 问题描述

WebSocket连接断开后，设备完全无响应，日志停止输出，只能硬重启恢复。

### 4.2 日志示例

```
W (704428) NuwaxPartner6: Audio source stopped: ESP_ERR_INVALID_STATE
I (704428) NuwaxPartner6: conversation awake session cleared reason=ws_disconnect
esp-tls-mbedtls: read error :-0x004C
E (704428) nuwax_ws: WebSocket receive failed before frame header: ESP_FAIL
(之后无任何日志，设备卡死)
```

### 4.3 根因分析

**死锁场景: RX任务强制删除导致互斥锁死锁**

```
RX任务 (websocket_receive_task):
  │
  ├── 检测到TLS错误 (-0x004C)
  ├── break 退出while循环
  ├── atomic_store(&connected, false)
  ├── transport_interrupt() → shutdown(socket)
  ├── transport_close_serialized() ← 尝试获取 s_lifecycle_lock
  │   └── xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY) ← 🔴 持有锁
  │       └── transport_close() → esp_tls_conn_destroy() ← 🔴 阻塞中
  │
  ↓ (RX任务被阻塞在 esp_tls_conn_destroy)
  ↓ (rx_task_running 仍为 true)

主线程 (app_main):
  │
  ├── 检测到 nuwax_ws_is_connected() == false
  ├── 退出内层while循环
  ├── 清理代码 (行13031-13057)
  │   ├── clear_awake_session("ws_disconnect") ✓
  │   ├── nuwax_barge_dispatcher_unbind() ✓
  │   ├── s_active_ws = NULL ✓
  │   └── nuwax_ws_close(&ws)
  │       ├── transport_interrupt() → shutdown(socket) ✓
  │       ├── cleanup_rx_task()
  │       │   └── wait_for_rx_task_stop(client, 5000ms)
  │       │       └── while(rx_task_running) { vTaskDelay(10ms); }
  │       │           ← 等待RX任务设置 rx_task_running = false
  │       │           ← 但RX任务卡在 esp_tls_conn_destroy()
  │       │           ← 5秒超时后...
  │       │
  │       ├── 5秒超时，打印 "Timed out waiting for WebSocket rx task to stop"
  │       ├── vTaskDeleteWithCaps(rx_task) ← 🔴 强制删除RX任务！
  │       │   └── RX任务被删除时持有 s_lifecycle_lock！
  │       │   └── FreeRTOS不会自动释放互斥锁！
  │       │
  │       └── transport_close_serialized()
  │           └── xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY)
  │               └── 🔴 永远等待！锁被已删除的RX任务持有！
  │
  ↓ (主线程永久阻塞)
  ↓ (设备卡死，无任何后续日志)
```

### 4.4 关键代码证据

**证据1: RX任务退出时调用 `transport_close_serialized()`** (`ws_transport.c:932-934`)

```c
// RX任务退出循环后
atomic_store_explicit(&client->connected, false, memory_order_release);
transport_interrupt(client);
transport_close_serialized(client);  // ← 这里获取 s_lifecycle_lock
```

**证据2: `transport_close_serialized()` 使用 `portMAX_DELAY`** (`ws_transport.c:738`)

```c
static void transport_close_serialized(nuwax_ws_client_t *client) {
    // ...
    if (xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY) == pdTRUE) {  // ← 永久等待
        transport_close(client);
        xSemaphoreGive(s_lifecycle_lock);
    }
}
```

**证据3: `cleanup_rx_task()` 超时后强制删除RX任务** (`ws_transport.c:932-933`)

```c
wait_for_rx_task_stop(client, pdMS_TO_TICKS(NUWAX_WS_RX_TASK_STOP_TIMEOUT_MS));  // 5秒超时
vTaskDeleteWithCaps(rx_task);  // ← 强制删除！不释放互斥锁！
```

**证据4: 主线程删除RX任务后也调用 `transport_close_serialized()`** (`ws_transport.c:1049`)

```c
void nuwax_ws_close(nuwax_ws_client_t *client) {
    // ...
    transport_interrupt(client);
    cleanup_rx_task(client);  // ← 强制删除RX任务（可能持有锁）
    transport_close_serialized(client);  // ← 尝试获取锁，永久等待
}
```

### 4.5 影响

| 影响 | 严重度 | 说明 |
|------|--------|------|
| 设备完全卡死 | 🔴 严重 | 主线程永久阻塞，无法恢复 |
| 所有功能停止 | 🔴 严重 | 音频、UI、WiFi、云端全部停止 |
| 只能硬重启 | 🔴 严重 | 看门狗可能也无法恢复（任务在等锁） |

### 4.6 解决方案

**方案1: 修改 `transport_close_serialized()` 使用超时等待（推荐）**

```c
// ws_transport.c - transport_close_serialized() 修改
static void transport_close_serialized(nuwax_ws_client_t *client) {
    if (!client) {
        return;
    }
    if (ensure_lifecycle_lock() != ESP_OK) {
        transport_close(client);
        return;
    }
    // 使用有限超时而非 portMAX_DELAY
    if (xSemaphoreTake(s_lifecycle_lock, pdMS_TO_TICKS(5000)) == pdTRUE) {
        transport_close(client);
        xSemaphoreGive(s_lifecycle_lock);
    } else {
        ESP_LOGE(TAG, "Failed to acquire lifecycle lock for close; forcing close");
        transport_close(client);  // 即使没有锁也要关闭
    }
}
```

**方案2: RX任务退出时不调用 `transport_close_serialized()`**

```c
// ws_transport.c - websocket_receive_task() 修改
// 在break之前，RX任务不应该调用 transport_close_serialized()
// 只需要设置 connected = false，让主线程处理清理
static void websocket_receive_task(void *arg) {
    // ... while循环 ...
    
    atomic_store_explicit(&client->connected, false, memory_order_release);
    transport_interrupt(client);
    // 不调用 transport_close_serialized()，让主线程处理
    atomic_store_explicit(&client->rx_task_running, false,
                          memory_order_release);
    ESP_LOGW(TAG, "WebSocket receive task stopped");
    vTaskSuspend(NULL);
}
```

**方案3: 修复 `cleanup_rx_task()` 中的互斥锁泄漏**

```c
// ws_transport.c - cleanup_rx_task() 修改
static void cleanup_rx_task(nuwax_ws_client_t *client) {
    if (!client || !client->rx_task) {
        if (client) {
            atomic_store_explicit(&client->rx_task_running, false,
                                  memory_order_release);
        }
        return;
    }

    TaskHandle_t rx_task = (TaskHandle_t)client->rx_task;
    if (rx_task == xTaskGetCurrentTaskHandle()) {
        ESP_LOGW(TAG, "WebSocket rx task cleanup requested from rx task context");
        return;
    }

    wait_for_rx_task_stop(client, pdMS_TO_TICKS(NUWAX_WS_RX_TASK_STOP_TIMEOUT_MS));
    
    if (atomic_load_explicit(&client->rx_task_running, memory_order_acquire)) {
        // RX任务仍在运行，可能持有 s_lifecycle_lock
        // 先中断socket，让RX任务的I/O操作失败
        transport_interrupt(client);
        // 等待一小段时间让RX任务有机会释放锁
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    vTaskDeleteWithCaps(rx_task);
    client->rx_task = NULL;
    atomic_store_explicit(&client->rx_task_running, false,
                          memory_order_release);
}
```

---

## 5. 问题4: 对话过程中语音中断

### 5.1 问题描述

用户在对话过程中说话突然中断，日志没有出现错误，只有一条警告：

```
W (849401) NuwaxPartner6: background listen stopped during cloud wait: ESP_ERR_INVALID_STATE
```

### 5.2 代码证据

**证据1: `nuwax_ai_partner_main.c:10602-10614`** — 警告产生位置

```c
static void wait_for_cloud_updates(nuwax_ws_client_t *ws, uint32_t wait_ms) {
    const int64_t started_ms = now_ms_i64();
    while (nuwax_ws_is_connected(ws) &&
           (uint32_t)(now_ms_i64() - started_ms) < wait_ms) {
        drain_session_commands(ws);
        drain_shell_actions(ws);
#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC
        complete_during_playback_wake_stop();
        if (exclusive_audio_mode_blocks_normal_audio() ||
            s_prelisten.pending || s_prelisten.capture_active) {
            esp_err_t listen_err = run_audio_source(ws);
            if (listen_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "background listen stopped during cloud wait: %s",
                         esp_err_to_name(listen_err));
                if (!nuwax_ws_is_connected(ws)) {
                    return;
                }
            }
            continue;
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

**证据2: `run_audio_source()` → `stream_microphone_audio()` → `wait_for_mic_start_action()`** (`nuwax_ai_partner_main.c:4068-4082`)

```c
static esp_err_t wait_for_mic_start_action(nuwax_ws_client_t *ws) {
#if CONFIG_NUWAX_VOICE_MIC_TOUCH_TRIGGER
    if (!s_shell_actions) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Hold face/status card to wake and enter continuous listen");
    int64_t last_ping_ms = now_ms_i64();
    while (true) {
        if (!nuwax_ws_is_connected(ws)) {
            return ESP_ERR_INVALID_STATE;  // ← WebSocket断开
        }
        if (exclusive_audio_mode_blocks_normal_audio()) {
            ESP_LOGI(TAG, "normal listen wait preempted by exclusive audio mode");
            return ESP_ERR_INVALID_STATE;  // ← 独占音频模式
        }
        // ...
    }
```

### 5.3 根因分析

**场景推演:**

```
时间线:
─────────────────────────────────────────────────────────────────

1. 用户说话，设备进入LISTENING状态
   ├── stream_microphone_audio() 被调用
   ├── s_prelisten.capture_active = true
   ├── 音频采集开始
   └── 音频帧上传到云端

2. 云端处理请求，返回响应
   ├── stream_microphone_audio() 正常返回
   ├── s_prelisten.capture_active 被清除 (line 10497)
   └── 主循环继续

3. 主循环调用 wait_for_cloud_updates(ws, 2500)
   ├── 检查 s_prelisten.capture_active
   │   └── 如果此时 s_prelisten.capture_active 仍为 true...
   │       └── 调用 run_audio_source(ws)
   │           └── stream_microphone_audio(ws)
   │               └── wait_for_mic_start_action(ws)
   │                   └── 检查 nuwax_ws_is_connected(ws)
   │                       └── 如果WebSocket已断开 → 返回 ESP_ERR_INVALID_STATE
   │
   └── 记录警告: "background listen stopped during cloud wait: ESP_ERR_INVALID_STATE"

4. 对话中断，用户说话被截断
```

**关键问题**: `s_prelisten.capture_active` 在 `stream_microphone_audio()` 返回后仍为true，导致 `wait_for_cloud_updates()` 尝试重新启动音频采集，但此时WebSocket可能已断开。

### 5.4 影响

| 影响 | 严重度 | 说明 |
|------|--------|------|
| 用户说话突然中断 | 🟡 中 | 对话体验差 |
| 无明显错误日志 | 🟡 中 | 难以定位问题 |
| 可能触发WebSocket断连 | 🟡 中 | 连锁反应 |

### 5.5 解决方案

**方案1: 在 `wait_for_cloud_updates()` 中检查WebSocket连接状态**

```c
// nuwax_ai_partner_main.c - wait_for_cloud_updates() 修改
if (exclusive_audio_mode_blocks_normal_audio() ||
    s_prelisten.pending || s_prelisten.capture_active) {
    // 在尝试重新启动音频之前检查WebSocket连接
    if (!nuwax_ws_is_connected(ws)) {
        ESP_LOGW(TAG, "WebSocket disconnected during cloud wait; skipping audio restart");
        return;
    }
    esp_err_t listen_err = run_audio_source(ws);
    if (listen_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "background listen stopped during cloud wait: %s",
                 esp_err_to_name(listen_err));
        if (!nuwax_ws_is_connected(ws)) {
            return;
        }
    }
    continue;
}
```

**方案2: 在 `stream_microphone_audio()` 返回前确保 `s_prelisten.capture_active` 被清除**

检查所有可能的退出路径，确保 `nuwax_prelisten_state_finish_capture()` 在所有情况下都被调用。

---

## 6. 问题5: 启动阶段无区分图标

### 6.1 问题描述

BOOTING、WIFI_CONNECTING、WS_CONNECTING三个状态都显示相同的动态点图标（RECOGNIZING），用户无法区分启动阶段。

### 6.2 代码证据

**`nuwax_state_bridge.c:246-258`**:

```c
case NUWAX_APP_STATE_BOOTING:
    s.dialog = NUWAX_DIALOG_RECOGNIZING;  // 动态点
case NUWAX_APP_STATE_WIFI_CONNECTING:
    s.dialog = NUWAX_DIALOG_RECOGNIZING;  // 动态点
case NUWAX_APP_STATE_WS_CONNECTING:
    s.dialog = NUWAX_DIALOG_RECOGNIZING;  // 动态点
```

### 6.3 影响

启动过程中用户无法区分"正在启动"、"WiFi连接中"、"云连接中"。但WiFi和Cloud状态栏图标有区分，中央对话图标无区分。

### 6.4 解决方案

**低优先级，可暂不修改**。如果需要修改，可以在状态栏图标中添加启动阶段文字提示，或添加专用图标。

---

## 7. 问题汇总与优先级

| 编号 | 问题 | 严重度 | 复杂度 | 建议优先级 |
|------|------|--------|--------|-----------|
| 3 | WebSocket断连后设备卡死 | 🔴 严重 | 中 | **P0 立即修复** |
| 1 | 监听模式UI状态缺失 | 🔴 高 | 中 | **P1 尽快修复** |
| 4 | 对话过程中语音中断 | 🟡 中 | 低 | **P2 计划修复** |
| 2 | IDLE状态图标语义不准确 | 🟡 中 | 低 | **P3 后续优化** |
| 5 | 启动阶段无区分图标 | 🟢 低 | 低 | **P4 可选优化** |

---

## 8. 附录: 关键代码位置索引

### 8.1 状态机相关

| 文件 | 行号 | 功能 |
|------|------|------|
| `nuwax_state_bridge.c` | 238-299 | 应用状态到UI状态映射 |
| `nuwax_device_state.h` | 49-56 | 对话状态枚举定义 |
| `nuwax_device_state.h` | 76-94 | 设备状态结构体定义 |
| `nuwax_status_bar.c` | 867-957 | 状态栏更新函数 |
| `nuwax_status_bar.c` | 939-940 | 会议模式UI覆盖 |
| `conversation_state.c` | 3-46 | 会话状态管理 |
| `app_shell.c` | 323-330 | 应用状态设置 |
| `app_shell.c` | 401-413 | 会议状态设置 |
| `app_shell.c` | 459-489 | 会话状态设置 |

### 8.2 WebSocket相关

| 文件 | 行号 | 功能 |
|------|------|------|
| `ws_transport.c` | 730-742 | transport_close_serialized |
| `ws_transport.c` | 744-759 | wait_for_rx_task_stop |
| `ws_transport.c` | 917-939 | cleanup_rx_task |
| `ws_transport.c` | 932-938 | RX任务退出清理 |
| `ws_transport.c` | 1042-1054 | nuwax_ws_close |
| `nuwax_ai_partner_main.c` | 13031-13057 | WebSocket断连清理 |

### 8.3 音频处理相关

| 文件 | 行号 | 功能 |
|------|------|------|
| `nuwax_ai_partner_main.c` | 6427-10540 | stream_microphone_audio |
| `nuwax_ai_partner_main.c` | 10497 | prelisten_state_finish_capture |
| `nuwax_ai_partner_main.c` | 10553-10570 | run_audio_source |
| `nuwax_ai_partner_main.c` | 10594-10618 | wait_for_cloud_updates |
| `nuwax_ai_partner_main.c` | 4068-4167 | wait_for_mic_start_action |
| `prelisten_state.c` | 89-97 | prelisten_state_finish_capture |

---

**文档结束**
