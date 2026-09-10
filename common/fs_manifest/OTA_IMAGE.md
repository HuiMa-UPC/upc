# OTA 镜像 AB 对（MCU / Galvo）

改 `/userdata/image/mcu`、`/userdata/image/galvo` 的落盘、扫描或刷写前读本文。
实现：`common/fs_manifest/ota_image_pair.*`。路径宏：`image_paths.hpp`。

SOC 全包升级不走本规则，见文末「SOC 全包升级」。

## 文件名

必须带 `.bin`，大小写敏感，版本三段数字：

| 目录 | SlotA | SlotB |
|------|--------|--------|
| `/userdata/image/mcu` | `MainBoard_SlotA_V3.0.7.bin` | `MainBoard_SlotB_V3.0.7.bin` |
| `/userdata/image/galvo` | `Galvo_SlotA_V4.0.0.bin` | `Galvo_SlotB_V4.0.0.bin` |

版本按 `major.minor.patch` **数值**比较（`3.0.10` > `3.0.9`）。

## 选取

扫描该目录下全部常规文件（跳过 `.` / `..` 与非常规文件）：

1. 每个文件必须能解析为**本目录对应**前缀；否则整次 OTA **失败**。
2. 按版本分组：每一组必须恰好 1×SlotA + 1×SlotB；缺边、同槽重复、A/B 版本不一致 → **失败**。
3. 多组成对时只取**版本最高**的一组，候选顺序固定 **SlotA → SlotB**。
4. **不降级**：最高这一对 header/首包都被拒或刷写失败 → **失败**，不试更低版本。
5. 同一对内 SlotA 被拒再试 SlotB，这是槽位匹配，不是降级。

云端 tar（component=`mcu`/`galvo`）解包后必须正好这一对名字；MQTT 用同一套解析，失败码 `ab_pair_invalid` / `tar_entry_invalid`。落盘成功后 MQTT 分别发 `UI_Serial_Mcu_Ota_Start_Req` / `UI_Mcu_Ota_Start_Req` 走真实刷写，进度跟 `APP_Serial_Mcu_Ota_Status_Ntf` / `APP_Mcu_Ota_Status_Ntf`。

## SOC 全包升级（component=`soc`）

实现：`app/mqtt/lib/mqtt_service_soc_ota.cpp`。不用 AB 对、不做整目录替换、不在下载线程里刷写。

包内**恰好一个 `.img`**，文件名不限（云端只校验整包 SHA256，不拆包）。tar 条目仍须平铺无路径穿越。失败码：`img_missing`（没有 .img）、`img_ambiguous`（多于一个）、`no_space`（`/userdata` 空间不足）、`img_install_failed`、`pending_save_failed`。

落盘流程：`tar -tzvf` 取条目名与解包后大小 → 清掉上一次遗留的固件与状态 → `statvfs` 校验空间（需 img 大小 + 64MB 余量）→ 解包到 `image/tmp` 子目录 → **改名为 `UPC_SOC_OTA_IMG`（`/userdata/update_ota.img`）**。固定单文件，多次升级不会累积。

应用时机交给用户：进度到 100% 后发 `APP_Soc_Ota_Ready_Ntf`，UI 弹窗给「Discard / Later / Reboot」。选 Reboot 时分两步执行——先 `updateEngine --image_url=<img> --misc=update` 拿退出码（坏包在这里就被 RKFW 头校验挡下，不必白重启），成功再 `sync` + `reboot` 进 recovery 刷写。

待应用状态存 `UPC_SOC_OTA_PENDING_FILE`（`/userdata/snowpod/soc-ota-pending`，与设备密钥同目录一并活过 OTA）。MQTT 每次首次连上云端时比对 `pending.version` 与 `UPC_SW_VERSION`：相同说明 recovery 已刷成功，清掉 img 与状态；不同则说明还没应用，重新提示 UI。云端另有自己的心跳版本比对，此处比对只决定本地文件去留。
