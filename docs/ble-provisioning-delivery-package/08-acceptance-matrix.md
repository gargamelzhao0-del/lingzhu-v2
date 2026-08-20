# 首轮联调验收矩阵

| 项目 | 当前状态 | 发布要求 |
| --- | --- | --- |
| ESP-IDF 6.0 + network_provisioning 1.2.4 构建 | 最终镜像构建、应用分区烧录和启动通过 | 保持版本锁定，Android 版本记录固件 SHA-256 |
| NimBLE 广播 UUID/名称 | 最终镜像 + 本地真实 Provider 真机通过 | Android 再验证 |
| Security 2 正确 PoP | 最终镜像 + 本地真实 Provider 真机通过 | Android 再验证 |
| 错误 PoP | 历史真机通过 | APP 映射 `SECURITY_AUTH_FAILED` |
| `device-info` UID 校验 | 最终镜像 + 本地真实 Provider 真机通过 | 不匹配时禁止下发 Wi-Fi |
| `cloud_prov` / `cloud-config` | 最终镜像真机通过签名配置暂存与提交 | Android 再验证 |
| ECDSA 云配置签名和防回滚 | 正确签名包真机通过；网关单元测试通过 | 篡改包和旧版本包真机验证 |
| NVS pending/active 提升 | 成功路径真机通过，网关确认 active 且 pending 为空 | 断电、写入失败和幂等回执故障注入 |
| 逐设备 WebSocket 101 前鉴权 | 测试网关成功路径真机通过 | Android 联调验证；401 拒绝路径补真机用例 |
| 设备 `prov-scan` | 当前镜像真机通过 | APP 串行刷新、不得并发配网 |
| 正确 Wi-Fi | 最终镜像 + 本地真实 Provider 使用 `xspaceagi` 真机通过 | Android 至少 20 次稳定性 |
| 错误密码 | 历史真机通过 | `WIFI_AUTH_FAILED` 且窗口内可重试 |
| SSID 不存在 | 历史真机通过 | `NETWORK_NOT_FOUND` |
| Wi-Fi 里程碑 | 最终镜像真机 14.3 s 到达 `GOT_IP` | `GOT_IP` 即 `WIFI_READY` |
| 完整 onboarding 成功 | 本地三端真机 33.6 s 到达 `WIFI_READY + DEVICE_ONLINE` | Android Provider 再验证 |
| 成功后 BLE 15 秒 | 历史真机通过 | Android 查询后主动释放 |
| 300 秒窗口 + 末端宽限 | 代码通过 | 故障注入待测 |
| 本地重配网按钮 | 最终镜像可进入广播并完成成功重配网 | 失败回滚和掉电恢复故障注入 |
| Android 断连恢复 | 未联合验证 | 必测 |
| 纯 WPA3-only | 未验证 | 若产品要求则补测 |
| SoftAP fallback | 明确不支持 | APP 不显示 fallback |
| 生产逐设备 PoP/安全启动 | 未实现 | 量产 P0 阻塞 |
| 测试云 claim/在线查询 | 最终镜像真机通过，credential active、pending 为空 | Android Provider 接口适配 |
| 生产账户绑定/解绑/吊销 | 未实现 | 量产 P0 阻塞 |

当前本地三端真机阻断已解除，详细结果见 `10-final-device-acceptance.md`。但只有继续完成 Android 联合验收、20 次稳定性、300 秒边界故障注入和所有 P0 生产项，才能把契约状态升级为 `INTEGRATION_VERIFIED` 或生产状态。
