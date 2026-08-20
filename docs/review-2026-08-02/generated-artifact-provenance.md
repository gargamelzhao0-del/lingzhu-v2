# B0-15 生成代码与资源 provenance 静态审计

审计日期：2026-08-03  
项目：`NuwaxAIPartner-v2`  
授权级别：A 级只读静态审计  
范围：正式 UI 生成资源、生成器、原始输入、输出 hash、工具/许可证线索、CMake 成员关系和 byte-for-byte 重生成门禁。

## 1. 结论

B0-15 的 A 级静态盘点已经完成，但 B 级逐字节重生成没有执行，当前不能把正式资源标记为“可复现生成通过”。本轮确认 41 个生成输出、共 9,203,244 bytes；其中 36 个是图像 C array，另外 5 个是字体、face diff 和 face path 输出。

- `main/CMakeLists.txt` 直接编译 6 个 dialog、12 个 menu、13 个 status、3 个 face、字体和 `face_diff_patches.c`，并嵌入 `face_paths_core.bin`。`face_21_listening`、`face_22_speaking` 两个 C array 存在但不进入当前 CMake。
- 5/5 face、6/6 dialog、13/13 status C array 与同级目录外的 V28 handoff 输出逐字节一致；12/12 menu array 与 handoff 不一致，但与 V1 工程逐字节一致。
- 两个被资源注释引用的生成器并非不存在：它们位于工作区根 `tools/`，且已被 supporting-test manifest 固定 hash；但它们不属于 V2 工程自身，menu 生成器还硬编码输出到 `NuwaxAIPartner` V1。
- dialog/status/face 的原始 PNG/SVG 候选位于工程外 handoff；没有找到生成这些 C array 的确切 converter、版本和命令。文件注释所称的“accompanying PNG/SVG”不在 V2 对应资源目录中。
- `gen_face_paths.py` 位于 V2，但输入 JSON 位于 sibling handoff；字体输出头部保留的命令仍指向 V1 路径，且未固定 `lv_font_conv` 版本。
- `face_diff_patches.c` 的生成器会写入当前时间，因此即使输入相同也不能 byte-for-byte 重生成；历史调用的 `--current-dir` 和完整命令未留存。
- Source Han Sans SC 字体有随树 OFL 1.1 文本；V28 自定义图形/handoff 未发现顶层 LICENSE/NOTICE 或资产 ownership manifest。第三方工具及其依赖版本/许可证也未绑定到资源输出。

因此登记 `GEN-001` 至 `GEN-004`，均为 P1 发布阻塞。它们不证明现有固件运行时图像错误，而是证明当前发布树无法独立、确定性地重建并审核这些正式资源。

## 2. 范围分母与实际构建成员

| 资产族 | 输出数 | 源文件 bytes | 当前 CMake/嵌入 | A 级结论 |
|---|---:|---:|---|---|
| face RGB565 C array | 5 | 7,943,262 | 3 编译、2 未链接 | 外部 PNG 存在；5/5 与 handoff C 一致；converter 不明 |
| menu RGB565A8 C array | 12 | 631,536 | 12 编译 | 根生成器存在并硬编码 V1 输出；12/12 与 V1 一致；Pillow 未锁定 |
| dialog RGB565A8 C array | 6 | 106,546 | 6 编译 | 外部 PNG/SVG 存在；6/6 与 handoff C 一致；converter 不明 |
| status RGB565A8 C array | 13 | 150,771 | 13 编译 | 外部 PNG/SVG 存在；13/13 与 handoff C 一致；converter 不明 |
| CJK 字体 C array | 1 | 47,518 | 1 编译 | 输入字体/OFL 存在；命令指向 V1；converter 版本不明 |
| face diff C/H | 2 | 294,147 | C 编译、H include | 根生成器存在；current-dir/invocation 未绑定；时间戳非确定性 |
| face path BIN/H | 2 | 29,464 | BIN 嵌入、H include | V2 生成器存在；输入位于工程外；未执行重生成 |
| **合计** | **41** | **9,203,244** | 见上 | B 级重生成全部开放 |

`assets/` 下另有 6 个手写聚合 C/H 文件；它们不标注为生成输出，因此没有混入 41 项分母。整个 `assets/` 的 47 个 C/H/BIN 文件共 9,223,106 bytes，规范化集合 digest 为 `06770B18D09F7A4712999CE430ECBB7BF3155DD174EB1DA52F7A1372B837201F`。

边界补充：`main/nuwax_ui_v28/face/face_path_blob.c/.h` 是手写的 NXFP 运行时 parser/接口，`main/face_path_blob_host_check.c` 是手写 host 检查；三者都不是由 `gen_face_paths.py` 生成的资源输出，因此不会改变上述 41 项、9,203,244 bytes 的 provenance 分母。生成物仍只有 `face_paths_core.bin` 与 `assets/face_paths.h`；parser 的内存安全状态由 B0-20/`MEM-002` 单独跟踪。

集合 digest 算法：按相对路径排序，逐行编码为 `path|bytes|lowercase_sha256\n`，再对 UTF-8 字节计算 SHA-256。它用于本轮静态集合比对，不替代逐文件 release manifest。

## 3. Generator、输入与输出链

| 资产族 | generator/版本 | 输入 | 输出/静态一致性 | 许可证状态 | 重生成状态 |
|---|---|---|---|---|---|
| face | 未发现确切 converter；handoff 仅声明 RGB565 规则 | sibling handoff `02_assets/faces/png_360/*.png` | 5/5 V2 C 与 handoff C hash 相同 | V28 资产 ownership/license 未记录 | 未执行；工具/命令缺失 |
| menu | 工作区根 `tools/generate-menu-icon-assets.py`，SHA-256 `CDA758...73F52` | 几何直接编码在脚本；Python/Pillow | 12/12 V2 与 V1 相同，0/12 与 handoff C 相同 | 脚本/图形 ownership 和 Pillow 版本/许可证未绑定输出 | 未执行；脚本硬编码 V1 输出 |
| dialog/status | 注释仅建议 official LVGL image converter | sibling handoff PNG 和 SVG 均存在，不能静态确定哪一个是 authoritative input | 19/19 V2 与 handoff C 相同 | V28 资产 ownership/license 未记录 | 未执行；converter/参数/版本缺失 |
| CJK font | `lv_font_conv`，版本未记录；当前 PATH 未发现命令 | V2 SourceHan OTF，SHA-256 `1EE89E...E25B` | 输出 SHA-256 `97BADC...254E6` | `font_license/SourceHanSansSC/LICENSE.txt` 为 OFL 1.1，SHA-256 `F55C2D...67CD` | 未执行；命令输入/输出仍写 V1 路径 |
| face diff | 工作区根 `tools/generate-face-diff-patches.py`，SHA-256 `86BDFE...2982` | 25 个 golden + 25 个 current capture；历史 `--current-dir`/命令未记录 | C SHA-256 `06D4CE...2F1`，H SHA-256 `6EF67E...82D` | 脚本/截图 ownership 未绑定输出 | 未执行；生成器嵌入当前时间，天然非 byte-identical |
| face paths | V2 `tools/gen_face_paths.py`，SHA-256 `68C4B3...C0B` | sibling handoff JSON，SHA-256 `8276E2...59C0` | BIN SHA-256 `9BC157...D3D`；H SHA-256 `B88176...E1B8` | JSON ownership/license 未记录 | 未执行；输入不在 V2 source manifest |

### 3.1 非图像输出与生成器完整 hash

| 类型 | 路径 | bytes | SHA-256 |
|---|---|---:|---|
| generator | 工作区根 `tools/generate-menu-icon-assets.py` | 7,139 | `CDA758EC02DDECA27B6BE6C5DCCD832B39BC062B32E143EE876D4EF191173F52` |
| generator | 工作区根 `tools/generate-face-diff-patches.py` | 9,324 | `86BDFEF1DC8BEA479ABA11EB0194A3E79226B025E957048DDA8082F15B922982` |
| generator | V2 `tools/gen_face_paths.py` | 6,003 | `68C4B36F33C945F1C91035CED1EC930E7FD949D134F7D4C6318A0FE7581B2C0B` |
| input | sibling `01_reference/nuwax_expressions_v18.json` | 62,141 | `8276E2AB8FAE1C746B0D86E1518630DC62CB1B59BA58FB39A35896877AC959C0` |
| input | `lib/lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf` | 16,414,944 | `1EE89E1669362DEE13851129C0A8A791A87521EB4148E5EFBF5D26596738E25B` |
| license | `lib/lvgl/scripts/built_in_font/font_license/SourceHanSansSC/LICENSE.txt` | 4,463 | `F55C2D43DD905011515F5E46BA78D180027E314EF8CCAAF53A9E88FE316767CD` |
| output | `nuwax_font_drawer_cjk_9.c` | 47,518 | `97BADC43BE81A377282529C9842CA44F2442AECB58058BC536A6CE21EFD254E6` |
| output | `face_diff_patches.c` | 293,993 | `06D4CEAF734F15CAEAC73232AAE1D94FB104EEFAB46EFA18DCABE817245D82F1` |
| output | `face_diff_patches.h` | 154 | `6EF67E1BC77C84569C6D39B6869EC6CA234FEB255F74790806CC5511EE60B82D` |
| output | `face_paths_core.bin` | 29,197 | `9BC157B8149189A3804E1F4ABA73812F3623D988947BCFD08C600B5DCD7A1D3D` |
| output | `face_paths.h` | 267 | `B88176EECA9CD1B2768E990B8C98B56D6E159B3CC66FF4459F21EFFBD36CE1B8` |

补充静态事实：

- 根 `generate-menu-icon-assets.py` 的 `OUT_DIR` 是 `ROOT/NuwaxAIPartner/main/...`，不是 `NuwaxAIPartner-v2`；它依赖 Pillow 的 rasterization/resize 行为，但工作区没有该生成链的 hash-locked requirements。
- 根 `generate-face-diff-patches.py` 的默认 golden 位于 sibling handoff，但 `--current-dir` 必填。候选 `tools/lvgl-simulator/reports/hybrid-overlay-v47-faces` 有完整 25 个 current 文件，集合 digest 为 `04D05D617029B7DC89EB81D50883E61C7A2AB916535AD16266E27E494DAE3C04`；代码或日志没有把该目录绑定到已提交输出。
- 25 个 golden 集合 digest 为 `197172E7B4D42F0235E60E87D78DDB1E492E5598ECA505A9C6BDC8DAF60EEEE8`。
- 当前标准 IDF Python 3.13.12 环境没有 Pillow，`lv_font_conv` 也不在 PATH。这只是当前能力预检，不是历史生成环境的版本证明。

## 4. 图像输出逐项 hash

### 4.1 Face

| 输出 | 输出 SHA-256 | 外部 PNG 输入候选 SHA-256 |
|---|---|---|
| `face_05_thinking_360_rgb565.c` | `CBA68B178CDE09DB9C020D8386554303A8BC081A09DE22D9790A7B4B1FDA1DA5` | `7215E930AF51B7480E6CE385008E3D48190A7F37BE225D9628E0A24DE337A3B3` |
| `face_21_listening_360_rgb565.c` | `289023589980BF235EB425DAE30E1AA56A7EBEF4B6DFD08E9141557B4AFCD3EB` | `6A46ED32138F2067F1B48A4C730AC9345D85BCD082183330ECCA63DE5F428D2F` |
| `face_22_speaking_360_rgb565.c` | `2C8254C71B8743315469031906D5BA06DA5CA27EF45CC67554EEA3D2A806A4C7` | `75389A883E67EBF2DBE7687691DDCB71E4D985366C5A3E1BC01311E771F4509C` |
| `face_23_low_battery_360_rgb565.c` | `6AED308113705AB25B87056FCF2D0DF3889BF82B6F1D6C97CE3FCA0101F41ED3` | `D57FC03EFD0029BF74503F2590EF083ACC56049950E144BFB1744343ECF9F74E` |
| `face_24_charging_360_rgb565.c` | `420CE32F82AFD513078BEAFE207610DBD9021AD3395639E592A5FE2051FC053F` | `A5D9CC335E34A18B0B712AB6FBDD0F7CD69B9B1D84DD18D1F8B129FDC1FF8215` |

### 4.2 Menu

以下“输入”是把几何与颜色直接编码在其中的根生成器，统一 SHA-256 为 `CDA758EC02DDECA27B6BE6C5DCCD832B39BC062B32E143EE876D4EF191173F52`。

| 输出 | 输出 SHA-256 |
|---|---|
| `icon_menu_claw_current_52.c` | `0E1720EF9AF89AED3F2ED392832B4FEE8F9077263723103E123EAD1560D43229` |
| `icon_menu_claw_normal_52.c` | `FC0465E21B45A67507907EE24F75B153E01CB0D43ACB32E7E518C4C8F14F558E` |
| `icon_menu_device_current_52.c` | `51215692715B15EDDF530E61DC8910CABA3E0EBE85D819627F6E37761FA12B9F` |
| `icon_menu_device_normal_52.c` | `9748A7EF545EF3AC16D9F5469985CB4603861D95F5A9CF0E21E3804AF8C8088A` |
| `icon_menu_issues_current_52.c` | `72444877B304B2086F004C1A947E055170AF7FB82CB8767BC2EF6FC395D80C5F` |
| `icon_menu_issues_normal_52.c` | `F69C2F5044151B69BED7626118E3AF5EDA994E50CE714B27251CA0D8687044AD` |
| `icon_menu_volume_current_52.c` | `4E409147D26406A4501B633CD98332D74D1AB8A0C28880541AB992CF2BD84C55` |
| `icon_menu_volume_normal_52.c` | `8023430C08BCCB9A4953BD570A5E5A84B36C26050B720BC235CD94CFCF60EA40` |
| `icon_menu_wifi_current_52.c` | `ECBA0846127FA599A9B9DEC964436019227FF25959DF79940C7DCE89327E4DE3` |
| `icon_menu_wifi_normal_52.c` | `223D3A22793CCE40D2C8494EDBD3F28B6B8888FFB60BD793D9EE68E1A7CE2EF0` |
| `icon_menu_work_current_52.c` | `4A26A5D3F9CA08A706736D52923A2414EACDF38339116CFD0B4AA5C07AB830E2` |
| `icon_menu_work_normal_52.c` | `F490B27DC462282DE0303DA12C6B737165EA0850AC6A23832A1437814606527A` |

### 4.3 Dialog/status

这里同时列出外部 PNG 和 SVG 候选；没有 converter/命令证据可证明哪一个是生成 C array 时的 authoritative input。

| 输出 | 输出 SHA-256 | PNG SHA-256 | SVG SHA-256 |
|---|---|---|---|
| `icon_dialog_error_30.c` | `57866084135863224299E379F242D0C5F584FAEA9E0D6405BB6ABA55128EB776` | `87BC41D34BAE9967659155A375517AA1F7E01B0B5909705E75F258D372C585EF` | `A15FA8B260FD627C353FCB93CA25FFE1D0031AFA4FEAD1769448789C617A984D` |
| `icon_dialog_listening_30.c` | `CEE34241F7223D1A531C8E2C486BFD17D1066516A6C8F7C3EFA35E8C485FE568` | `8286833CB1CCE5D60EBD7666A5CE13F6C084231EFED05402753834288BC5BCCF` | `F2C6B9DCE2DC2B6A16765A1AAAE3CD065FBB9F6C2404D82BBEF853E13408DDFB` |
| `icon_dialog_recognizing_30.c` | `2EE17AA1ECD5AF423A71CDB8F4CFFC3930B83CB239AF0B1C801F04C98F8A3E96` | `84EAF51703A4E7495B2FA807179152E9961D78213B5E8969A2F0B6BBEFB7128F` | `86A9C9AAAA9547B2BB8BF8834824D38E5CFD22F6EA853D4A32DB27AED828857A` |
| `icon_dialog_sleep_30.c` | `8EA131910C441A21809B2A50D1225A59896A090958C723FA409E13B8A9F71A5F` | `CE8A33CBA98D74083F77A36C44A29968523C84E07B2536560E84EDF0E5C3061E` | `EA75E61E3760C7BE969F631716BCD9733832DC668FD80B14518DEBCF59349EC8` |
| `icon_dialog_speaking_30.c` | `21313F8FD1A62733C5222CDFF1A7F8BEF766B50CA4E76C6D7DE96C82EC8CF315` | `152102E43450C6685B709544C73C8479D52506CE1486C7277EAE7C889EA9176F` | `C1AF7E65F634431E12BEF211FFFDAA75607528D545824319F7C4B4F064B0EA41` |
| `icon_dialog_thinking_30.c` | `60E65F5833E6745EA8515D3FECF6C8928427AE882769023716E2915B4D6C8EC7` | `7E9C58951CD9E59A7F715CEF7CDA47B3A038F7C4DDFC361B2A3E1FF817FDC576` | `87E3E6CE4220DE790AE823E047CF52722F03B21E7E95A92B3CA44C2A36698353` |
| `icon_cloud_authfail_24.c` | `1AD278645319AE76E18D7D378EDB988852FABD0775318CAA72394D6F7A66BE1B` | `71CDECE28B358741B812E25F73B0079BBB075CA658B81FB34ED82F0C4F310825` | `4577D801C0FB4DD9B112DB49A9FC5BA02A181661832C52F6DC245B8C0D0A25FC` |
| `icon_cloud_connected_24.c` | `EE1346DF6AB8558569787551598AA4DC9DF6C94DCC0C85D2DFB034C2CEF6C932` | `F0AFC92FB6E43D408B3E989823F0ACEBC51391EE55188D6D29A0C82B74F9192A` | `2EF71F4E2B8F5B324FC1871E8E15919EBD89039ED34995805FB2E722400AAD24` |
| `icon_cloud_connecting_24.c` | `EFB50B544C38DB25C8CDE7EE2530A9FE48AAB67C7DDF9D6DC72DD912E6D1710A` | `ACFAE5BA0B8DA9EB35F00964E80E7DF6C331DA5D5F30B2A278B7576D092FDC4A` | `90F05C1F43412EFF27A5427A510FC5EA49B5A98968002B03A9832261F1E95186` |
| `icon_cloud_disconnected_24.c` | `DC03BA1F608D6EF9F6AAD3CCCF0181208BBEFA63D6BE4DF2F1F25672EBCAF27A` | `71CDECE28B358741B812E25F73B0079BBB075CA658B81FB34ED82F0C4F310825` | `5D82CF8C78F2C1A65C56439C767066FDD552796490615F59BBA4E097424711CB` |
| `icon_power_battery_24.c` | `0A6B5E5ACF93445510168ED528F88F472F18B8CFDFD4C9E127CBF5F665CDAC34` | `5C76F525F3324E04EC5F4AA21B3994F3E0CA523F1405E4A3CEAE2288C8FDCF72` | `2FBB303BE85B63DA4EE7398E90108638DA3B0200651770E3D769F2932C97AF5B` |
| `icon_power_charging_24.c` | `9338B6E91D9310C58A27FD8291A7772449FAEA7BBD2C3B6B94081E7095C21FFB` | `F6EF7C29E77DDDB10604458E8648B8C5AD6EC5C673E634BD237D53658926A0DE` | `413623C65B3AE76417E0833484D611A666CFD259999000B4679EB5D5879798D1` |
| `icon_power_external_24.c` | `9A54AD9CCA6187AFE414ABABF27B3FBD3C40013265202FD4E71D8C0F8CC046E6` | `F6EF7C29E77DDDB10604458E8648B8C5AD6EC5C673E634BD237D53658926A0DE` | `6D60F7D4D8D7BA05D474A99B59AF6DD8217304800AA5C0C2B9F0C8DC90623045` |
| `icon_power_low_24.c` | `BA35EA28F54BF22D45518A05D07F97F4D44320F2E18B1D1258F927BF80730701` | `D14483B78D66A6988A21EFBF969F930C1FA29BBDD0BFE020622EECA4836F765C` | `3E9739847C6994645B9E641DC917034191AD39AE7CD14B7003E724E259BEF90A` |
| `icon_volume_muted_24.c` | `5EC250830860A798F55C3B71FD99D3D51F56EAC096937FAED4E446395E106391` | `A3AEDF03D2F4DB66DAA9D7BD11F15EADE5C1FEFFD0D6B23DA15187871BACD6F9` | `88D09E311E0F82F7E2D3AB6762DDE70EB4B2EE15EB47179FFBDB641FA5E6FFA4` |
| `icon_volume_normal_24.c` | `A232B46787F3ECAACD61EF4FD27FE72CCBE9697E777906F947B13D9EA1E56635` | `DA56E3397E8A31F82588A1104169F81ADCA71C76685DA715D84726AD0BEB901E` | `AA466C9FDD769B6952F18F96A433CB7B00C84075BC069951B735E63C1618157F` |
| `icon_wifi_connected_24.c` | `CC12C206DCD0FA97FB9355A2F63F6BDF0657D18073A61F6D341799260BC78F0B` | `1BF9FE9B0374209CA3B146F78453FC7E9FE1FAF35A4EB0813DA00D82C764EBE9` | `5EE67030C0DF9A661B6DF762AE096E50AF142399F0EB1C4E68AE94DEF802E0BE` |
| `icon_wifi_disconnected_24.c` | `87364D72B72E3457A34D0606B4D59B9A62ED3C8AC47A30FC76A4A1886201705B` | `41DDD3B70BD306D9AF7F4FF66A119B62BD896AD0185F83492215A97EB95FD9B0` | `7B432D1A0BE80A96CE35283D8A21DBDE583AACD6A8A56693D4AB94D2BA860781` |
| `icon_wifi_weak_24.c` | `16BDA6513B83A984DF55087546976E4218674393663C0B50C895E9B5B52F84E9` | `F897657CD535129700241888C4F80298C903B216868088B5DF991117B6B4B650` | `4570981A62B4F15F170C248B78FE1FE78BAC8EFC65DC3511AE24600027747588` |

## 5. Canonical 问题

### GEN-001（P1，发布阻塞）

V2 正式资源生成链不是 self-contained。menu/face-diff 生成器位于工作区根，face/icon/path 输入位于 sibling handoff；这些外部文件虽有部分 hash 基线，但不属于 V2 发布源和同一版本控制 identity。应把必要 generator、authoritative input、lockfile 和 license/ownership manifest 纳入 V2 的受控 `tools/assets-source`，或以有版本/hash/签名的独立资产包依赖声明。

### GEN-002（P1，发布阻塞）

face、dialog、status 和 face-diff 缺少完整的 `generator + exact invocation + authoritative input + output` 绑定。现有 handoff C 一致性只能说明拷贝来源，不能证明 PNG/SVG 到 C 的转换可复现；face-diff 更没有记录实际 current capture 目录。自定义图形的 ownership/license 也未随输出登记。应为每个输出生成机器可解析 provenance 行，并拒绝 input/tool/license 字段为空。

### GEN-003（P1，发布阻塞）

工具链和路径存在确定漂移：menu generator 写 V1，字体命令读写 V1，face path 读取工程外目录；Python、Pillow、`lv_font_conv` 及图像 converter 版本没有锁定。应提供 hermetic container/venv/npm lock，路径以显式 project root/output 参数传入，禁止硬编码 V1 或 sibling 目录；Source Han OFL 和所有工具许可证进入 notices/SBOM。

### GEN-004（P1，发布阻塞）

没有 byte-for-byte 重生成门禁，且 face-diff generator 把 wall-clock 时间写入输出，按设计即非确定性。应移除时间戳或使用固定 `SOURCE_DATE_EPOCH`，在干净临时目录生成全部 41 项，逐项比较 SHA-256/bytes，并把生成 manifest、工具镜像 digest、source/config commit 和 release identity 绑定；任何额外、缺失或 mismatch 都必须失败。

## 6. B/C/D 关闭条件

### B 级

1. 冻结 Python/Pillow、Node/`lv_font_conv`、图片 converter 的精确版本、传递依赖 hash 和许可证。
2. 将所有 authoritative input 与 generator 放入受控版本，或绑定不可变、可验证的资产包；生成器只能写显式临时输出目录。
3. 在两次全新临时目录中重生成 41 项，第一次与 checked-in golden 逐字节一致，第二次与第一次一致；输出机器可读 provenance manifest。
4. 对 face-diff 移除时间非确定性并明确 current/golden 集合；对 menu 修正 V2 target；对字体修正 V2 输入输出并验证字符覆盖。

### C 级

1. 在经授权的干净固件构建中，以 map/ELF 证明实际链接/嵌入的生成资产集合与 provenance manifest 一致。
2. 把资产集合 digest、generator/input manifest、最终 image hash 和 `sdkconfig` 绑定到同一 build run；未链接资源必须显式标记用途或移出发布源。

### D 级

1. 设备/UI 像素验收使用由同一 authoritative input 生成的 golden，验证 face、menu、dialog、status、字体和 diff patch。
2. 发布包附带适用的 source/asset/tool notices 和可审计 provenance；从发布包可反向定位每个 image member 的输入与生成环境。

## 7. 反向核查

- 本轮只读取源文件、外部 handoff、根工具、CMake、hash 和工具可用性；没有执行任何 generator、host test 或 build。
- “与 handoff/V1 C 文件一致”不等于“由候选 PNG/SVG 或脚本重新生成通过”；逐字节重生成仍是 B 级开放项。
- 未修改业务源码、配置或资产输出，未访问 COM5/NVS，未执行 OTA。
- B0-15 状态应为 `completed-A;B/C/D-open;release-blocked`。
