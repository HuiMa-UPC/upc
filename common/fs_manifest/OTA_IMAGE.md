# OTA 镜像 AB 对（MCU / Galvo）

改 `/data/image/mcu`、`/data/image/galvo` 的落盘、扫描或刷写前读本文。
实现：`common/fs_manifest/ota_image_pair.*`。路径宏：`image_paths.hpp`。

SOC（`/data/image/soc`）不走本规则。

## 文件名

必须带 `.bin`，大小写敏感，版本三段数字：

| 目录 | SlotA | SlotB |
|------|--------|--------|
| `/data/image/mcu` | `MainBoard_SlotA_V3.0.7.bin` | `MainBoard_SlotB_V3.0.7.bin` |
| `/data/image/galvo` | `Galvo_SlotA_V4.0.0.bin` | `Galvo_SlotB_V4.0.0.bin` |

版本按 `major.minor.patch` **数值**比较（`3.0.10` > `3.0.9`）。

## 选取

扫描该目录下全部常规文件（跳过 `.` / `..` 与非常规文件）：

1. 每个文件必须能解析为**本目录对应**前缀；否则整次 OTA **失败**。
2. 按版本分组：每一组必须恰好 1×SlotA + 1×SlotB；缺边、同槽重复、A/B 版本不一致 → **失败**。
3. 多组成对时只取**版本最高**的一组，候选顺序固定 **SlotA → SlotB**。
4. **不降级**：最高这一对 header/首包都被拒或刷写失败 → **失败**，不试更低版本。
5. 同一对内 SlotA 被拒再试 SlotB，这是槽位匹配，不是降级。

云端 tar（component=`mcu`/`galvo`）解包后必须正好这一对名字；MQTT 用同一套解析，失败码 `ab_pair_invalid` / `tar_entry_invalid`。
