# Nuwax 360 屏幕 UI 重设计 HTML 原型

## 目的

本目录用于评审 ESP32-S3 设备 360 × 360 圆形屏幕的新视觉方案。原型只调整界面样式、布局和动效，不修改当前固件的功能逻辑、状态来源或路由关系。

## 已覆盖界面

- 主表情界面与当前固件全部 25 种表情状态
- 顶部 Wi-Fi、Cloud、Mic、Power 和对话状态
- 左滑进入的六项焦点菜单：Claw、Work、Issues、Wi-Fi、Volume、Device
- 菜单详情页和 Device 下的 Meeting、Device QR、BLE Provisioning
- 下滑状态抽屉及音量调节
- 设备二维码视觉页
- BLE 配网等待和阶段进度视觉
- 从系统信号到眼睛苏醒、状态自检、进入主表情的开机动画

## 本地运行

在 `NuwaxAIPartner` 工程根目录执行：

```powershell
python -m http.server 8788 --directory docs/ui-redesign-html-prototype
```

浏览器打开 `http://127.0.0.1:8788/`。

## 操作方式

- 右侧“界面”控制器可直接切换表情、菜单、状态抽屉和 Device 详情。
- 表情选择器可逐项检查 25 种状态。
- 在设备屏幕上左滑进入菜单，菜单内左右滑切换项目，下滑打开状态抽屉，上滑关闭。
- Device 详情可预览 Meeting 开关、设备二维码和 BLE 配网状态。
- Wi-Fi、Cloud、Dialog、Power 和音量控件会同步更新设备顶部状态和状态抽屉。
- “播放开机动画”可重复检查完整启动流程。

## 与固件的边界

- 原型没有连接真实设备、BLE、Wi-Fi 或云端。
- 二维码为确定性的视觉占位矩阵，用于评审屏幕尺寸和可读性，不是正式绑定二维码编码结果。
- BLE 配网步骤是对现有流程的视觉演示，不替代 `network_provisioning` 和 `protocomm` 实现。
- 视觉方案通过后，再按 LVGL 控件、图层和状态桥接关系迁移到 `main/nuwax_ui_v28`，避免在视觉尚未冻结时改动固件。
