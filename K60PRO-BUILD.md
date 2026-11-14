# K60 Pro（socrates）内核构建状态与后续封装

日期：2026-11-14

**状态：静态修复与封装完成，运行就绪状态仍为 `runtime_ready=false`。**
新 FD、Loader、BootShim 和容器确已生成；其中的 XNU kernel 仍是旧构建。今天全部设备交互使用 mock，未执行实际 ADB、fastboot 或手机指令。

## 1. 历史 kernel：保留事实，不冒充今日重编

[现有 kernel](<F:/Xiaomi15-iOS26.2-Dev-fastboot/ios-payload/source/project/out/xnu-socrates/mach.development.bcm2837>)为 20,435,464 字节，SHA256 `aa4886fc8eb00859b3a981252cd4893b0f913e7db3f93af5e7cba8707eaed492`。其历史归档仓库为 [k60pro-ios26-adaptation](https://github.com/1527183723/k60pro-ios26-adaptation)，旧记录指向 [CI 归档 run 43450623746](https://github.com/1527183723/k60pro-ios26-adaptation/actions/runs/43450623746)。这些是历史构建记录，不是今日 CI 执行；今天没有 push 或运行远程 CI。

包含 socrates 字符串不能证明包含今日修复。该 kernel 没有 `K60PRO_STATIC_FIX_V1`，本轮没有重新编译 XNU。

## 2. 今日真正重建的组件

FD 由已有 venv / clang 21.1.8 完整 fresh build；已有 native LLVM 22.1.3 编译 Loader 和 368 字节 BootShim。新最小 bootstrap、三份 Android images、完整 payload 已封装，并通过 inspect。哈希/大小/工具链账本见 [静态修复账本](<F:/Xiaomi15-iOS26.2-Dev-fastboot/K60PRO-STATIC-REPAIR.md>)与 [component metadata](<F:/Xiaomi15-iOS26.2-Dev-fastboot/uefi/source/project/out/socrates-static-fixed/components.json>)。这些新件不改变旧 kernel 的构建事实。

新 [完整 payload](<F:/Xiaomi15-iOS26.2-Dev-fastboot/uefi/source/project/out/socrates-static-fixed/socrates-payload.bin>)为 54,367,768 字节 / SHA256 `176185c897c81c7f8b658e80c93ec34c290c77bb481a264220bfc47e747afbcc`，仍含上述旧 kernel，[封装记录](<F:/Xiaomi15-iOS26.2-Dev-fastboot/uefi/source/project/out/socrates-static-fixed/packaging.json>)明确 kernel_rebuilt=false。

## 3. 当前合同与启动参数

[设备 contract](<F:/Xiaomi15-iOS26.2-Dev-fastboot/ios-payload/source/project/common/devices/socrates.contract.json>)规定 FD A7、payload C8、panic CB7、framebuffer B8，以及 cpus=7 / bootcpu=7。

```text
debug=0x10e rootdev=md0 launchd_unsecure_cache=1 panic_on_crash_disabled=1 small_cores_after=0 fb_threads=2 applog=2 autounlock=1 touch_poll_us=4000 epoch=1794583659 touchscreen=0 fb_flip=0 fb_dcs=0 cpus=7 bootcpu=7 fbwidth=1440 fbheight=3200 fb_hz=60 fb_ca_hz=60
```

触摸默认 0；flip / DCS 默认 0，CPU framebuffer 回退。audio / IRIS / camera gate、Goodix DRV2 以及 display fallback 已做源码/静态修复，尚未做任何触摸、音频或显示硬件验收。fbpitch=560 已删除，不能解释其旧消费者；fb_hz / fb_ca_hz=60只是默认参数。9 GiB window 是上界，不是已证实 usable RAM。

## 4. 新 kernel 的发布前提

macOS/Xcode 构建应使用包含本轮源补丁与修复标记的目标源码。必须得到真正不同且符合当前来源的 Mach-O kernel，不能对旧 kernel 改名、改 manifest 或补字符串后称重编。

新内核生成后，用实际源树与 binary 生成来源侧车：

```powershell
tools/python/python.exe -B tools/k60pro/kernel_provenance.py --kernel "<NEW_KERNEL>" --repo "ios-payload/source/project" --output "firmware/socrates-kernel-provenance.json"
```

上述命令从发行包根执行，只读新 kernel / 源树并写 provenance 文件；[实现](<F:/Xiaomi15-iOS26.2-Dev-fastboot/tools/k60pro/kernel_provenance.py>)会拒绝缺少修复标记的旧 kernel。需要重新生成完整 XNUP、组件/镜像账本，并更新 `firmware/k60pro-static-manifest.json`。旧 kernel sidecar 已诚实标记 stale，当前 runtime_ready=false不能提升为真。

## 5. 离线检查

以下命令从发行包根执行，只有离线检查与源码构建/封装；本记录不列出自动设备操作命令。

```powershell
tools/python/python.exe -B tools/k60pro/preflight.py --root . --json
```

完整测试使用本轮已经存在的 Bash；下面只给测试子进程临时变量，避免 shell fixture 因缺少 K60PRO_TEST_BASH 被跳过，系统环境不变：

```bash
K60PRO_TEST_BASH="$(cygpath -w "$BASH")" tools/python/python.exe -B -m unittest discover -s tools/k60pro/tests -p "test_*.py"
```

当前离线预检应保持失败/阻断，而不是为了通过检查去改就绪位。缺少最终 manifest、sidecar 或发现旧内核都应在设备 subprocess 之前退出。

最终离线回归共95项：C7+A6+B20+父artifacts13+kernel provenance21+native provenance2+packaging1+preflight25，全部通过。源码 builder 的真实 CLI 见 [build_static.py](<F:/Xiaomi15-iOS26.2-Dev-fastboot/tools/k60pro/build_static.py>)与 [离线重封装步骤](<F:/Xiaomi15-iOS26.2-Dev-fastboot/K60PRO-STATIC-REPAIR.md>)；重新封装不是 XNU 编译。

## 6. RAM boot 历史记录纠正

历史 bootloader 返回 OKAY 后观察到的是黑屏 / absent xnuloader；fastboot reappearance未观察。fastbootd 的 Unrecognized command boot只属于fastbootd。原图缺bootstrap，因此这些事实不能证明原厂bootloader不支持RAM boot。原“无法继续/必须写boot”结论撤销，正确记录见 [RAM boot 研究](<F:/Xiaomi15-iOS26.2-Dev-fastboot/K60PRO-FASTBOOT-BOOT-RESEARCH.md>)。

真正的新 kernel 和匹配元数据完成、离线预检通过后，才由用户连接指定手机分阶段验收；本记录没有设备命令，也没有宣称今天已实现物理触摸、音频或显示验证。
