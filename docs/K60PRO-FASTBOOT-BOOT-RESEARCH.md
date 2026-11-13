# K60 Pro（socrates）fastboot boot 调研存档

日期：2026-11-14
设备：Redmi K60 Pro，型号 22127RK46C，代号 socrates，平台 SM8550（kalama）
目的：确认这台机器能否用 fastboot boot 临时启动 UEFI，作为项目 RAM 启动流程的入口。

## 1. 实测结果

### 1.1 标准 bootloader

命令：

~~~text
adb -s 2a081a5 reboot bootloader
fastboot boot uefi/source/project/out/socrates-uefi/socrates-boot.img
~~~

返回：

~~~text
Sending 'boot.img' (2000 KB)  OKAY
Booting                       OKAY
Finished. Total time: 0.133s
~~~

但等待 30 秒：

~~~text
fastboot devices
（空）
adb devices
（空）
~~~

设备黑屏，USB 不枚举，没有出现 xnuloader。

### 1.2 对照：原始 dada uefi.img

命令：

~~~text
fastboot boot uefi/uefi.img
Sending 'boot.img' (2192 KB)  OKAY
Booting                       OKAY
~~~

结果：

~~~text
fastboot devices -> 2a081a5  fastboot
~~~

设备仍停在 fastboot，没有启动镜像。说明 bootloader 接受了下载，但没有执行 boot。

### 1.3 fastbootd

命令：

~~~text
adb -s 2a081a5 reboot fastboot
fastboot -s 2a081a5 getvar is-userspace
fastboot -s 2a081a5 getvar product
~~~

返回：

~~~text
is-userspace: yes
product: socrates
~~~

说明已进入 fastbootd。执行：

~~~text
fastboot -s 2a081a5 boot socrates-boot-simplefb-v3.img
~~~

返回：

~~~text
Sending 'boot.img' (1112 KB)  OKAY
Booting                       FAILED (remote: 'Unrecognized command boot')
fastboot: error: Command failed
~~~

fastbootd 不支持 boot 子命令。

## 2. 外部资料

- LineageOS 官方 socrates 安装页只使用 fastboot flash boot boot.img、fastboot flash dtbo、fastboot flash init_boot、fastboot flash vbmeta、fastboot flash vendor_boot，没有 fastboot boot：
  https://wiki.lineageos.org/devices/socrates/install/
- LineageOS socrates 固件更新页同样只使用 fastboot flash：
  https://wiki.lineageos.org/devices/socrates/fw_update/
- XDA 上同类报错：fastboot boot boot.img 返回 (bootloader) Command is not support：
  https://xdaforums.com/t/bootloader-command-is-not-support-when-i-try-to-run-fastboot-boot-boot-img.4348333/
- DroidWin 对 (bootloader) This command is not implemented/supported 的说明：
  https://droidwin.com/fix-bootloader-this-command-is-not-implemented-supported/

## 3. 结论

K60 Pro 的原厂 bootloader 不支持本项目的临时 fastboot boot 启动方式：

1. 标准 fastboot 下 fastboot boot 返回 OKAY，但设备仍停在 fastboot；原始 dada uefi.img 也一样。
2. fastbootd 下 fastboot boot 直接返回 Unrecognized command boot。
3. 官方 LineageOS socrates 流程也只使用 fastboot flash boot，与实测一致。

因此，只要坚持“不写 boot 分区”的 RAM-only 约束，就无法在这台 K60 Pro 上进入 UEFI。之前 socrates 镜像的黑屏/掉 USB 不能作为 UEFI 成功或失败的证据，因为 bootloader 没有执行镜像。

## 4. 可选路线

### A. 备份后 fastboot flash boot（需要额外授权）

这是目前最直接、可回滚的路线：

1. 在 Android root shell 下备份当前槽位的 boot 分区：
   ~~~bash
   adb shell su -c 'dd if=/dev/block/by-name/boot_a of=/sdcard/boot_a.img'
   adb shell su -c 'dd if=/dev/block/by-name/boot_b of=/sdcard/boot_b.img'
   adb pull /sdcard/boot_a.img
   adb pull /sdcard/boot_b.img
   sha256sum boot_a.img boot_b.img
   ~~~
2. 记录当前槽位：
   ~~~text
   fastboot getvar current-slot
   ~~~
3. 刷临时 UEFI boot 镜像：
   ~~~text
   fastboot flash boot socrates-boot.img
   fastboot reboot
   ~~~
4. 验证失败时恢复原厂 boot：
   ~~~text
   fastboot flash boot boot_a.img
   fastboot reboot
   ~~~

注意：这会写 boot 分区，超出当前“纯 RAM 启动”的约束；刷入前必须确认 boot_a/boot_b 备份可读、哈希一致。

### B. EDL / 9008（尚未建立）

工程内没有 edl、qdl、QFIL 工具，也没有 9008 驱动。EDL 主要能力是 Firehose 读写分区，不等于 fastboot boot；需要先安装工具和确认 9008 进入方式，风险更高，暂不作为第一选择。

### C. fastbootd

已排除：fastbootd 明确返回 Unrecognized command boot。

## 5. 本项目现有产物

| 文件 | 大小 | SHA256 |
|---|---:|---|
| uefi/source/project/out/socrates-uefi/SILICIUM_UEFI.fd | 4,194,304 | 6d681b4230114dad32c94706af9cb0807ff3228cf241e8ca4312ec6878ded992 |
| uefi/source/project/out/socrates-uefi/SILICIUM_UEFI-simplefb.fd | 4,194,304 | 374faa6319ba41584746475c823e3f8725f3fccba29d5ab91a3e12f990bea720 |
| uefi/source/project/out/socrates-uefi/socrates-boot.img | 2,048,000 | 681c0f3cb75fcd3dd4e97ea528d708ebcfec0768a96870f82b8bc07d9d824700 |
| uefi/source/project/out/socrates-uefi/socrates-boot-simplefb.img | 1,929,216 | 7053825e1068afeb05847368ec641e59aca4ecdc784b9b533d9f8c8ed44c19ae |
| uefi/source/project/out/socrates-uefi/socrates-boot-simplefb-v3.img | 1,138,688 | 275c90c0ec52b5316ba2a61ef4de3ac8671ff938bd81fe847726e7b4844dea09 |
| ios-payload/socrates-payload.bin | 54,382,104 | 509f6d9c4d00b598890e35eaac91e3e1ccf8dce7337490e0d6343199252681da |
| ios-payload/source/project/out/xnu-socrates/mach.development.bcm2837 | 20,435,464 | aa4886fc8eb00859b3a981252cd4893b0f913e7db3f93af5e7cba8707eaed492 |

GitHub 构建仓库：

~~~text
https://github.com/1527183723/k60pro-ios26-adaptation
~~~

成功构建：

~~~text
https://github.com/1527183723/k60pro-ios26-adaptation/actions/runs/43450623746
~~~

## 6. 明天建议顺序

1. 先确认 K60 已回 Android，adb devices 返回 2a081a5 product:socrates。
2. 如果你同意写 boot 分区：先备份 boot_a/boot_b、校验哈希，再 fastboot flash boot socrates-boot.img，验证 UEFI/USB；失败立即恢复。
3. 如果不允许写 boot：保持现状，不在真机上继续 fastboot boot；后续只能研究 EDL/其他启动链。
4. 真机启动前先确认 Binaries/socrates 仍是 nuwa 脚手架，K60 原厂 UEFI 驱动尚未替换；UEFI 能进 USB 后，再谈 payload 上传和内核。
