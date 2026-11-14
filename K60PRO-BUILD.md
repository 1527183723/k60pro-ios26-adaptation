# K60 Pro（socrates）内核构建状态与后续封装

日期：2026-11-14

**状态：静态修复、完整引导封装和 GitHub 新内核构建完成；离线预检通过，硬件验证待做。**
新 FD、Loader、BootShim、容器和 XNU kernel 均已真正生成并通过来源绑定。今天全部设备交互使用 mock，未执行实际 ADB、fastboot 或手机指令。

## 1. 当前 kernel：GitHub Actions 真正重编

[当前 kernel](<F:/Xiaomi15-iOS26.2-Dev-fastboot/ios-payload/source/project/out/xnu-socrates/mach.development.bcm2837>)为 20,435,528 字节，SHA256 `d62d0f11ac009139cf8635b359bf8c883b5252bcbc796d610b1bb71fd574beb7`。它来自 [GitHub Actions run 43538811325](https://github.com/1527183723/k60pro-ios26-adaptation/actions/runs/43538811325)：macOS runner 完成 XNU 编译，provenance 步骤记录 `source_input_sha256=e9dfbf91977d53a5b21a9096f2aaac87dc70ed297bf2b82a071acb86180256a2`，并成功上传 kernel 与 sidecar。

历史 kernel 20,435,464 字节 / `aa4886fc8eb00859b3a981252cd4893b0f913e7db3f93af5e7cba8707eaed492` 仅作为旧归档，不再是当前 payload 内容。新 kernel 包含 `K60PRO_STATIC_FIX_V1` 和内嵌 source stamp。

## 2. 今日真正重建的组件

FD 由已有 venv / clang 21.1.8 完整 fresh build；已有 native LLVM 22.1.3 编译 Loader 和 368 字节 BootShim。GitHub Actions 生成的新 kernel 已接入完整 payload，最小 bootstrap、三份 Android images 和完整 XNUP 均已重新封装并通过 inspect。哈希/大小/工具链账本见 [静态修复账本](<F:/Xiaomi15-iOS26.2-Dev-fastboot/K60PRO-STATIC-REPAIR.md>)与 [component metadata](<F:/Xiaomi15-iOS26.2-Dev-fastboot/uefi/source/project/out/socrates-static-fixed/components.json>)。

新 [完整 payload](<F:/Xiaomi15-iOS26.2-Dev-fastboot/uefi/source/project/out/socrates-static-fixed/socrates-payload.bin>)为 54,367,832 字节 / SHA256 `a2416add60fe6a26ed22e3771a163a5d125d0b7e13d73b51f21902959ae521a8`，内含上述新 kernel，[封装记录](<F:/Xiaomi15-iOS26.2-Dev-fastboot/uefi/source/project/out/socrates-static-fixed/packaging.json>)明确 kernel_rebuilt=true。

## 3. 当前合同与启动参数

[设备 contract](<F:/Xiaomi15-iOS26.2-Dev-fastboot/ios-payload/source/project/common/devices/socrates.contract.json>)规定 FD A7、payload C8、panic CB7、framebuffer B8，以及 cpus=7 / bootcpu=7。

```text
debug=0x10e rootdev=md0 launchd_unsecure_cache=1 panic_on_crash_disabled=1 small_cores_after=0 fb_threads=2 applog=2 autounlock=1 touch_poll_us=4000 epoch=1794583659 touchscreen=0 fb_flip=0 fb_dcs=0 cpus=7 bootcpu=7 fbwidth=1440 fbheight=3200 fb_hz=60 fb_ca_hz=60
```

触摸默认 0；flip / DCS 默认 0，CPU framebuffer 回退。audio / IRIS / camera gate、Goodix DRV2 以及 display fallback 已做源码/静态修复，尚未做任何触摸、音频或显示硬件验收。fbpitch=560 已删除，不能解释其旧消费者；fb_hz / fb_ca_hz=60只是默认参数。9 GiB window 是上界，不是已证实 usable RAM。

## 4. 新 kernel 的构建与复现

新 kernel 已由 GitHub Actions 在 macOS runner 上按当前源树构建；workflow 在构建前生成/校验 source receipt，构建后生成 provenance 侧车。可复现的记录路径如下：

```powershell
tools/python/python.exe -B tools/k60pro/kernel_provenance.py --kernel "<NEW_KERNEL>" --repo "ios-payload/source/project" --output "firmware/socrates-kernel-provenance.json"
```

上述命令从发行包根执行，只读新 kernel / 源树并写 provenance 文件；[实现](<F:/Xiaomi15-iOS26.2-Dev-fastboot/tools/k60pro/kernel_provenance.py>)会拒绝缺少修复标记的旧 kernel。新 kernel 已接入完整 XNUP，组件/镜像账本已更新，`firmware/k60pro-static-manifest.json` 现为 `runtime_ready=true`。

## 5. 离线检查

以下命令从发行包根执行，只有离线检查与源码构建/封装；本记录不列出自动设备操作命令。

```powershell
tools/python/python.exe -B tools/k60pro/preflight.py --root . --json
```

完整测试使用本轮已经存在的 Bash；下面只给测试子进程临时变量，避免 shell fixture 因缺少 K60PRO_TEST_BASH 被跳过，系统环境不变：

```bash
K60PRO_TEST_BASH="$(cygpath -w "$BASH")" tools/python/python.exe -B -m unittest discover -s tools/k60pro/tests -p "test_*.py"
```

当前离线预检在当前新内核包上通过；缺少最终 manifest、sidecar 或发现旧内核/来源不符都应在设备 subprocess 之前退出。

最终离线回归共95项：C7+A6+B20+父artifacts13+kernel provenance21+native provenance2+packaging1+preflight25，全部通过；GitHub Actions run 43538811325 另行成功完成 XNU 编译与 provenance 上传。源码 builder 的真实 CLI 见 [build_static.py](<F:/Xiaomi15-iOS26.2-Dev-fastboot/tools/k60pro/build_static.py>)与 [离线重封装步骤](<F:/Xiaomi15-iOS26.2-Dev-fastboot/K60PRO-STATIC-REPAIR.md>)；重新封装不是 XNU 编译。

## 6. RAM boot 历史记录纠正

历史 bootloader 返回 OKAY 后观察到的是黑屏 / absent xnuloader；fastboot reappearance未观察。fastbootd 的 Unrecognized command boot只属于fastbootd。原图缺bootstrap，因此这些事实不能证明原厂bootloader不支持RAM boot。原“无法继续/必须写boot”结论撤销，正确记录见 [RAM boot 研究](<F:/Xiaomi15-iOS26.2-Dev-fastboot/K60PRO-FASTBOOT-BOOT-RESEARCH.md>)。

真正的新 kernel 和匹配元数据已完成、离线预检已通过；下一阶段才由用户连接指定手机分阶段验收。本记录没有设备命令，也没有宣称今天已实现物理触摸、音频或显示验证。
