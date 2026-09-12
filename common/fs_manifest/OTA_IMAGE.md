# OTA 镜像 AB 对（MCU / Galvo）

改 `/userdata/image/mcu`、`/userdata/image/galvo` 的落盘、扫描或刷写前读本文。
实现：`common/fs_manifest/ota_image_pair.*`。路径宏：`image_paths.hpp`。

SOC 升级（全包 `.img` / 应用层 A/B）不走本规则，见文末「SOC 升级」。

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

## SOC 升级（component=`soc`）

实现：`app/mqtt/lib/mqtt_service_soc_ota.cpp`。不用 AB 对、不做整目录替换、不在下载线程里刷写。两种形态共用下载/校验/待应用状态/UI 弹窗，只在落盘与应用环节分流。

### 判别升级方式

包内可含 `ota_manifest.json`，`type` 取 `image`（全包）或 `app`（应用层）：

```json
{ "type": "app", "version": "V4.1.2.3", "files": ["rongsu_broker", "..."], "min_compat": "V4.0.0" }
```

没有这个文件就按解包内容推断：只有 `.img` → 全包，只有 `rongsu_*` → 应用层，两者都有或都没有 → `kind_ambiguous`。保留推断是为了兼容加入描述文件之前打好的纯 `.img` 包，云端无需任何改动（它只校验整包 SHA256，不拆包）。

包内 `version` 与云端 manifest 对不上 → `version_mismatch`（打包或分发错配，放过去会让云端的心跳版本比对永远判不出完成）。当前版本低于 `min_compat` → `incompatible_version`。

统一解包到 `/userdata` 下的 `image/tmp` 子目录再分流：全包在同分区 `rename` 是零拷贝，应用层只有十几 MB，跨分区拷进槽可忽略；反过来先解到 `/home` 的话，几百 MB 的全包既放不下也要多搬一次。不先 `tar -tzvf`：gzip 无法随机访问，列目录等于白解压一整遍。

`image/tmp` 在**进程启动时**和**每个下载任务开始时**各整目录清空一次。临时文件名带 `task_id`，掉电中断的任务绕过代码里全部清理路径，残留既不会被新任务覆盖也没人删——全包一次能留几百 MB，攒几次就把 `/userdata` 占满，之后所有 OTA 都卡在 `no_space`。整目录清空是安全的：同一时刻只有一个下载任务（`ota_download_running_`），待应用的固件也不在这里（全包在 `UPC_SOC_OTA_IMG`，应用层在槽内）。

### 全包 `.img`

包内**恰好一个 `.img`**，文件名不限。失败码：`img_missing`、`img_ambiguous`、`no_space`、`img_install_failed`。

先清掉上一次遗留的固件与状态 → 按压缩包大小（或 manifest `size_bytes`）的约 2 倍 + 64MB 粗估 `statvfs` 空间 → 解包 → 校验平铺且恰好一个 `.img` → **改名为 `UPC_SOC_OTA_IMG`（`/userdata/update_ota.img`）**。固定单文件，多次升级不会累积。

选 Reboot 时分两步执行——先 `updateEngine --image_url=<img> --misc=update` 拿退出码（坏包在这里就被 RKFW 头校验挡下，不必白重启），成功再 `sync` + `reboot` 进 recovery 刷写。

### 应用层 A/B（`/home/APP1`、`/home/APP2`）

槽内只放随版本走的东西：八个 `rongsu_*` 加配套的 `rongsu_app.sh`。`snowslice`、相机 IQ、rknn 几何契约等共享资源留在 `/home` 顶层不参与轮换。一份槽约 17MB。

`/home/rongsu.sh` 是薄 bootstrap，只负责选槽、试运行回滚、交棒；进程列表在槽内的 `rongsu_app.sh` 里跟可执行文件一起升级。这样新增进程只影响新槽，回滚到旧槽时用的是旧槽自己的脚本，不会去找不存在的进程。三个子命令是 OTA 侧唯一的槽管理入口：`slot-status` / `slot-switch <槽> [版本]` / `slot-confirm`。

落盘流程：用 `/proc/self/exe` 反推当前槽（比读状态文件可靠，绝不会覆盖正在运行的槽；判不出说明设备还是迁移前的扁平布局 → `slot_layout_missing`）→ 按 `files` 清单校验齐备，没给清单则用本版本已知的八个二进制兜底，`rongsu_app.sh` 无论如何必须在（→ `app_file_missing`）→ **先删非活动槽**（当前跑的才是回滚目标，删掉不损失回滚能力，峰值占用从三份压到两份）→ 检查空间 → 拷到 `APPx.new` 并统一补可执行位 → `sync` → 整目录 `rename` 成 `APPx`。此时**不切槽**，用户可能选「稍后」，提前切了的话中途任意一次意外重启都会跑上未确认的新版本。

选 Reboot 时调 `rongsu.sh slot-switch` 写状态文件（`active=新槽 prev=当前槽 trial=1 boot_count=0`）再 `sync` + `reboot`，失败回 `SlotSwitchFailed`。

**试运行与自动回滚**是这条路相对全包的核心价值（全包有 recovery 兜底，这里没有）。新槽起来后 `rongsu_app.sh` 后台 `setsid` 一个守护，观察 60 秒、确认八个 pid 都还在，就调 `slot-confirm` 清掉 `trial`。判据取「服务存活」而不是「MQTT 连上云端」，否则无网环境的设备永远确认不了会被一路回滚。连续 3 次启动仍未确认（容忍偶发开机异常），bootstrap 自动切回 `prev` 并置 `rollback=1`；MQTT 开机读到这个标记就丢弃待应用状态，不然用户点一次被回滚一次，陷入提示死循环。

### 待应用状态与开机收尾

状态存 `UPC_SOC_OTA_PENDING_FILE`（`/userdata/snowpod/soc-ota-pending`，与设备密钥同目录一并活过 OTA），含 `kind` / `payloadPath` / `targetSlot`；旧键名 `imgPath` / `imgBytes` 仍可读，否则跨这次升级时旧状态读不出来，遗留的 `.img` 就没人清了。

进度到 100% 后发 `APP_Soc_Ota_Ready_Ntf`（带 `kind`），UI 弹「Discard / Later / Reboot」，打印中拦截 Reboot。MQTT 每次首次连上云端时比对 `pending.version` 与 `UPC_SW_VERSION`：相同说明已生效，全包删 `.img`、应用层只清状态（payload 就是正在跑的槽，删不得）；不同则重新提示 UI。云端另有自己的心跳版本比对，此处比对只决定本地文件去留。

### 打包与迁移

`./build.sh` 自动产出 `output/soc_app_ota_<版本>.tar.gz` 与 `.sha256`：平铺、`root:root`（默认会写进构建机的 uid/gid，设备上解包会改错属主甚至直接失败，而设备端是按 tar 退出码判成败的）、自带描述文件。全包 `.img` 不在这里打，那是 SDK 侧产物。

现网设备从扁平布局迁到 A/B 用 `scripts/migrate_ab_slot.sh`（与两个 `.sh` 一起拷到设备任意目录执行）。它先**拷贝**而非移动二进制进 `APP1`，最后一步才替换 `/home/rongsu.sh`，中途断电旧脚本配顶层原件仍能开机；原件默认保留，重启验证通过后跑 `--cleanup` 释放空间。bootstrap 另有扁平布局回退分支，装了新脚本但还没迁移的设备不会起不来。
