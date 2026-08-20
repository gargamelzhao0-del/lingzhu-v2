# Third-Party Notices — Nuwax AI Partner 7.0

本文件登记 7.0 工程中**嫁接/复用的第三方代码**及其许可证,履行 G1 门禁(定稿 §2 G1 / §15)与各许可证的署名义务。**凡从下列来源移植代码进本工程,必须:① 在对应移植文件头保留来源与许可注释;② 在本表新增/更新条目;③ 在定稿 §1.6 证据登记表补 file:line。** 未登记不得合入。

---

## 1. EchoEar 参考固件(主要嫁接源)

- **项目**:EchoEar(xiaozhi-esp32 板型固件),`PROJECT_VER 2.0.5.3`
- **版权**:Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.; Copyright (c) 2025 Project Contributors
- **许可证**:MIT
- **来源仓库**:内网 GitLab `embedded-team/echoear-updated-version-s3`,branch `main`,commit `de71f1fa938a970b9b26243d966b1f3c0a96fc1c`(内网地址不外发,见定稿内部访问说明)
- **本地副本**:`../Nuwax AI Partner硬件相关资料/参考代码/`
- **义务**:在分发物中保留下方 MIT 许可全文与版权声明。
- **计划嫁接范围**(随实际移植更新"已移植"列):

| 参考文件 | 本工程落点 | 状态 |
| --- | --- | --- |
| `main/boards/echoear/config.h` | `main/board/esp_vocat_v1_0_pins.h` | 待移植 |
| `main/audio/codecs/box_audio_codec.cc` | `main/audio/audio_board.c/.h` | 待移植 |
| `main/audio/processors/afe_audio_processor.cc` | `main/audio/afe_processor.c` | 待移植 |
| `main/boards/echoear/EchoEar.cc`(POWER_CTRL/PA 初始化) | board 早期初始化 | 待移植 |
| `main/boards/echoear/bq27220/*` | `main/drivers/bq27220.c` | 待移植 |
| `main/boards/echoear/BMI270/*` | `main/drivers/bmi270.c` | 待移植 |
| `components/espfriends__audio_doa`(可选) | P7 创新项 | 评估中 |

### MIT License 全文(EchoEar)

```
MIT License

Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.
Copyright (c) 2025 Project Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 2. ESP-IDF 托管组件(随 idf_component.yml 引入)

经 ESP Component Registry 引入的组件(`espressif/esp-sr`、`espressif/esp_audio_codec`、`lvgl/lvgl`、`espressif/cjson`、`espressif/dl_fft`、`espressif/esp-dsp`,以及 7.0 计划引入的 `espressif/esp_codec_dev`、`espressif2022/bmi270` 等)各自携带许可证(多为 Apache-2.0 / MIT),由组件管理器在 `managed_components/` 内附带。**正式发布前**汇总 `managed_components/*/LICENSE` 到本表第 2 节。

---

## 3. 移植文件头注释模板(MUST)

每个从参考嫁接的文件,头部加注:

```c
/*
 * Adapted from EchoEar firmware (MIT).
 * Source: embedded-team/echoear-updated-version-s3 @ de71f1fa
 * Original: main/audio/codecs/box_audio_codec.cc
 * (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd. and contributors.
 * Modified for Nuwax AI Partner 7.0 (ESP-VoCat V1.0): <一句话改动摘要>
 */
```
