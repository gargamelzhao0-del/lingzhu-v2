# NuwaxAIPartner-v2 工具链与依赖基线

生成日期：2026-08-02

状态：B0-05 只读证据。未执行 host test、固件构建或设备操作。

## 1. 工具链

| 工具 | 绝对路径/来源 | 版本 |
|---|---|---|
| ESP-IDF | `C:\esp\v6.0\esp-idf` | tag `v6.0`，commit `662a3be354759d9487bf4b1a629fadb766cb1800`，工作区 clean |
| Python | `C:\Espressif\python_env\idf6.0_py3.13_env\Scripts\python.exe` | 3.13.12 |
| Xtensa GCC | `%USERPROFILE%\.espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin\xtensa-esp32s3-elf-gcc.exe` | 15.2.0 |
| CMake | `C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe` | 4.0.3 |
| Ninja | `%USERPROFILE%\.espressif\tools\ninja\1.12.1\ninja.exe` | 1.12.1 |
| esptool | 上述 IDF Python 环境 | 5.3.0 |
| IDF Component Manager | 上述 IDF Python 环境 | 3.0.2 |
| Git | `C:\Program Files\Git\cmd\git.exe` | 2.52.0.windows.1 |
| Node.js | `C:\Program Files\nodejs\node.exe` | 24.15.0 |
| npm | `C:\Program Files\nodejs\npm.cmd` | 11.12.1 |

普通 PowerShell 当前没有完整加载上述 IDF 工具 PATH，机器同时存在 ESP-IDF 5.5.4 与 6.0；后续构建不能依赖隐式命令解析。

## 2. 依赖锁

`dependencies.lock` SHA-256：`565cf544cc62a643f10c2e025b28b155088d95c7400f97f203e116e05b4f2265`。

| 依赖 | 版本 |
|---|---|
| lock format / target | 3.0.0 / esp32s3 |
| manifest hash | `b7721595bb677d8aaf64eb8e357c0dd2e1e0fb0c0a4f442ee185b3420b83f1df` |
| ESP-IDF | 6.0.0 |
| cJSON | 1.7.19~1 |
| dl_fft | 0.6.0 |
| ESP-DSP | 1.7.0 |
| ESP-SR | 2.3.1 |
| esp_audio_codec | 1.0.1 |
| esp_codec_dev | 1.5.11 |
| network_provisioning | 1.2.4 |
| LVGL | 9.5.0 |

七个 `managed_components` 的 `.component_hash` 与 lock 中对应值一致。构建通过 `EXTRA_COMPONENT_DIRS` 实际选择本地 `lib/lvgl`，其版本宏为 9.5.0；registry 中 LVGL 的 component hash 不能证明本地副本内容。

## 3. 既有构建缓存

工程内 `build/project_description.json` 记录 Ninja、ESP32-S3 和项目版本 7.0.21。当前源码版本为 7.0.24，因此该缓存只可用于追溯上次工具选择，不能作为当前可复现构建证据。外部 `C:\tmp\nuwax-v2-fix-7.0.23-20260803-d` 虽为 7.0.23，但未与当前 source/config/OTA manifest 绑定，同样不能替代可复现构建证据。

## 4. 问题

- `TOOL-001 P2`：工具来自 `C:\Espressif` 与 `%USERPROFILE%\.espressif` 两套根，普通 shell 又未加载统一环境；需冻结初始化脚本和全部绝对路径。
- `DEP-001 P1`：本地 `lib/lvgl` 覆盖 registry 组件且没有 `.component_hash`；必须由源码 manifest/独立 vendored provenance 证明实际参与编译的内容。
- `BASE-VER P2`：当前源码为 7.0.24；工程内 build 是 7.0.21，外部 build 是 7.0.23 且未绑定当前 manifest，二者均不能作为当前发布证据。
- `TOOL-002 P3`：支持脚本的 Python requirements 只固定直接版本、没有 hash 锁定的传递依赖。

## 5. 结论

B0-05 的只读工具链和依赖锁盘点完成。后续 C 级构建必须用固定的 ESP-IDF 6.0 初始化入口、独立 `SDKCONFIG` 和新外部 build 目录，并把工具绝对路径写入构建证据。
