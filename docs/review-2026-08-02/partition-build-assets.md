# NuwaxAIPartner-v2 分区与已有构建产物基线

生成日期：2026-08-02

状态：B0-04 只读证据。未构建、未操作 COM5、未 OTA。已有镜像均不能替代当前源码的可复现构建。

## 1. 活动布局

当前 `sdkconfig` 指向 `partitions.customwake.csv`，目标 ESP32-S3、16 MB flash、app rollback 开启。

| 分区 | 起始 | 大小 | 结束 |
|---|---:|---:|---:|
| `nvs` | `0x9000` | `0x6000` | `0xF000` |
| `otadata` | `0xF000` | `0x2000` | `0x11000` |
| `phy_init` | `0x11000` | `0x1000` | `0x12000` |
| `model` | `0x20000` | `0x400000` | `0x420000` |
| `ota_0` | `0x420000` | `0x4C0000` | `0x8E0000` |
| `ota_1` | `0x8E0000` | `0x4C0000` | `0xDA0000` |
| `storage` | `0xDA0000` | `0x260000` | `0x1000000` |

活动布局无重叠并恰好结束于 16 MB。两个 OTA 槽各 4,980,736 bytes。

## 2. 三套 CSV 兼容性

| 文件 | SHA-256 | 结论 |
|---|---|---|
| `partitions.customwake.csv` | `e498063228a1a46792d3243a8ec29c2c5e40b7556e9885f16dc6e5729ee05a19` | 唯一与当前完整烧录集合兼容的活动表 |
| `partitions.wakeword.csv` | `16c99b2dad815e7eacad26c24df0684dcb56ed6ab97b0ca360613dfd901071c8` | model 仅 1,048,576 bytes；当前 2,681,351-byte `srmodels.bin` 溢出 1,632,775 bytes |
| `partitions.csv` | `c693eadce94dcf8fdc35686cb55d33b99f8589b31beae2efc52df29b828b6af0` | 无 model，factory 从 `0x20000` 开始；同地址写 `srmodels.bin` 会覆盖 app |

三套表都能通过 ESP-IDF 的 16 MB 结构校验，但用途不能混用。

## 3. 容量

| 项目 | 容量 | 当前/候选大小 | 剩余 | 使用率 |
|---|---:|---:|---:|---:|
| bootloader 至 `0x8000`，外部 7.0.23 构建 | 32,768 | 21,408 | 11,360 | 65.33% |
| 分区表区间 | 4,096 | 3,072 | 1,024 | 75.00% |
| model | 4,194,304 | 2,681,351 | 1,512,953 | 63.93% |
| app 槽，已有 7.0.21 | 4,980,736 | 3,547,248 | 1,433,488 | 71.22% |
| app 槽，外部 7.0.23 构建 | 4,980,736 | 3,554,880 | 1,425,856 | 71.37% |
| OTA manifest 4 MiB 上限，外部 7.0.23 构建 | 4,194,304 | 3,554,880 | 639,424 | 84.75% |

代码中的 `NUWAX_OTA_MANIFEST_MAX_SIZE` 为 4 MiB，因此有效 OTA 容量不是物理槽的 4.75 MiB。

## 4. 已有镜像

工程内 `build/` 为 7.0.21 陈旧产物：

| 文件 | 大小 | SHA-256 |
|---|---:|---|
| `bootloader/bootloader.bin` | 21,392 | `cf6701edd2859bd27c1c55dbabe7da0d6a579f7397c6083c8cbf4cf783a3487f` |
| `partition_table/partition-table.bin` | 3,072 | `aad81c7e6e4686758d711bf7336ff59e4c3e816af34cf0bb795300b5e3bf8cdf` |
| `ota_data_initial.bin` | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `srmodels/srmodels.bin` | 2,681,351 | `dec3196aaadcf4e6305434462f5735e1148e5ddfe1f6a0696776d666f9066d34` |
| `NuwaxAIPartner.bin` | 3,547,248 | `b09cca5e66aa744b4c9a0ae51bab406980365829bd2936eb13ebb62420c2850d` |

外部 `C:\tmp\nuwax-v2-722-patch-s3` 是历史 7.0.22 候选：

| 文件 | 大小 | SHA-256 |
|---|---:|---|
| `bootloader/bootloader.bin` | 21,392 | `ef9cf8cda6b9938b5aff9cd1a193e922375031d274756a118b4533ecd1c1324a` |
| `partition_table/partition-table.bin` | 3,072 | `aad81c7e6e4686758d711bf7336ff59e4c3e816af34cf0bb795300b5e3bf8cdf` |
| `ota_data_initial.bin` | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `srmodels/srmodels.bin` | 2,681,351 | `dec3196aaadcf4e6305434462f5735e1148e5ddfe1f6a0696776d666f9066d34` |
| `NuwaxAIPartner.bin` | 3,563,952 | `c2cd20d429fd205baef0c22489802b36e5e6e211e4205bca7b74214cf6cae88e` |

该历史候选 image-info 为 7.0.22、ESP-IDF 6.0、secure version 0，但没有与当前源码 manifest 和 OTA manifest 绑定。其生成配置的 OTA 最低电量为 20%，当前工程 `sdkconfig` 为 30%。它只能用于追溯历史 7.0.22 执行快照。

当前还存在外部 `C:\tmp\nuwax-v2-fix-7.0.23-20260803-d` 构建资产：

| 文件 | 大小 | SHA-256 |
|---|---:|---|
| `bootloader/bootloader.bin` | 21,408 | `ca8bf36ee65ff7c07b9bc0adbc7c4a1fa1739b53549a2bf5fda3962c63e20a74` |
| `partition_table/partition-table.bin` | 3,072 | `aad81c7e6e4686758d711bf7336ff59e4c3e816af34cf0bb795300b5e3bf8cdf` |
| `ota_data_initial.bin` | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `srmodels/srmodels.bin` | 2,681,351 | `dec3196aaadcf4e6305434462f5735e1148e5ddfe1f6a0696776d666f9066d34` |
| `NuwaxAIPartner.bin` | 3,554,880 | `774c620149ae976f943dd7c2c7b2d2d0b40181f0f1e2bd60c8a1bfbbd47873fe` |

该构建的 `project_description.json` 和 app image-info 均显示 7.0.23、ESP32-S3、ESP-IDF 6.0；app image secure version 为 0 且镜像 SHA 校验有效。其生成配置直接指向工程 `sdkconfig`，没有独立的归一化配置快照，也没有与本轮 7,918 项 source manifest、OTA manifest 形成发布绑定，因此仍只是只读构建证据，不是当前发布候选。

## 5. 问题

- `BUILD-001 P1`：当前源码为 7.0.24 / 7000024，但没有与当前源码 hash、冻结配置和 OTA manifest 绑定的发布产物；README 仍含 7.0.17 / 7000017。
- `PART-001 P1`：运行时 `partition_schema=1` 仅弱检查两个 OTA 槽和任意 otadata，不校验 label/address/size/model 或 partition table hash；不兼容旧表可能被误判兼容。
- `PART-002 P2`：4.75 MiB 物理 app 槽与 4 MiB OTA 协议上限不一致；既有 7.0.23 app 的有效 OTA 余量仅 639,424 bytes。
- `PART-003 P2`：活动布局没有 factory recovery，双 OTA 槽均失效时只能有线恢复；OTA admission 只拒绝 `NEW/PENDING_VERIFY`，没有显式要求当前运行槽为 `VALID` 或证明存在可用 rollback target，异常 otadata/历史混烧不能靠正常烧录假设放行。
- `PART-004 P2`：两套不兼容旧分区表仍在根目录，存在手工混烧风险。
- `PART-005 P2`：当前 OTA 只更新 app，不更新 `srmodels`/partition table；模型或布局变化需全量有线升级，或新增资源 OTA 与兼容标识。

## 6. 结论

活动分区本身容量和边界正确，已有二进制分区表与它一致。B0-04 的只读取证已完成，但当前发布镜像没有冻结，分区运行时兼容检查存在 P1 缺口，不能据此放行 OTA。
