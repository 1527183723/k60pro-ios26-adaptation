# K60 Pro（socrates）RAM boot 研究记录：纠正与静态修复

日期：2026-11-14
目标：Redmi K60 Pro / socrates / SM8550，纯 RAM 启动，不以分区写入替代缺失的启动链。

## 1. 已观察事实与已撤销推断

| 场景 | 已观察到的事实 | 能证明什么 |
|---|---|---|
| 标准 bootloader 接收旧镜像 | 发送与 Booting 返回 OKAY；随后黑屏，USB 没有枚举出 xnuloader | 仅证明命令接收/应答；不能确认 UEFI 的执行阶段或 Loader 已启动 |
| “随后重新出现标准 fastboot / 一直停在 fastboot” | reappearance 未观察到；旧文档把未证实的状态写成事实 | 这段推断撤销，不能据此声称 bootloader 没有执行镜像 |
| fastbootd | userspace 模式返回 Unrecognized command boot | 只说明 fastbootd 的该命令未实现，不能外推标准 bootloader 能力 |

历史应答摘要（不是本轮执行命令）：

```text
Sending 'boot.img' ... OKAY
Booting ... OKAY
随后：黑屏 / absent xnuloader

fastbootd：FAILED (remote: 'Unrecognized command boot')
```

今日全部设备交互为 mock，没有重试任何手机命令，也没有新增真机日志。旧“原厂 bootloader 不支持本项目 RAM boot”“不写 boot 分区就无法继续”结论撤销。第三方安装流程采用分区安装、或其他机型报不支持，都不能替代本机引导程序能力证据。

## 2. 原镜像的可验证缺陷

全项目静态审计确认，原启动体只包含 368 字节 BootShim 与 4,194,304 字节 FD，缺少 bootstrap XNUP / USB Loader。项目预期链是 BootShim → FD → 内嵌最小 bootstrap 中的 Loader → RAM 上传完整 payload。缺少该组件时，未枚举 xnuloader 本身就不是“RAM boot 不受支持”的充分证据。
同时存在 DDR 消费者地址、CPU/显示参数、外设 gate 与旧 kernel 来源不闭合的问题，详见 [全项目审计](<F:/Xiaomi15-iOS26.2-Dev-fastboot/K60PRO-PROJECT-AUDIT-2026-11-14.md>)。

## 3. 今天实际生成了什么

- fresh 4 MiB FD：SHA256 `67d4675f41fda3daea5b26f0aedd13a32dcb212ae16d74305fd1a3d22754b765`，existing venv clang 21.1.8 完整构建。
- native LLVM 22.1.3 的最终 ARM64 Loader：107,008 字节，SHA256 `de214c7c0349bccfe2e92d1d242c5f47013bddd8f8cbad5a28e9799babcc77cd`；新 BootShim 368 字节。
- 107,104 字节最小 bootstrap 含 loader.efi / usb，已经进入三份新 Android image；封装 inspect 全部通过。
- 新 payload：54,367,832 字节 / SHA256 `a2416add60fe6a26ed22e3771a163a5d125d0b7e13d73b51f21902959ae521a8`；内嵌 GitHub Actions 新 kernel 20,435,528 字节 / SHA256 `d62d0f11ac009139cf8635b359bf8c883b5252bcbc796d610b1bb71fd574beb7`，包含 K60PRO_STATIC_FIX_V1。

完整可点击的文件表及哈希见 [静态修复账本](<F:/Xiaomi15-iOS26.2-Dev-fastboot/K60PRO-STATIC-REPAIR.md>)；一手输出是 [封装解析](<F:/Xiaomi15-iOS26.2-Dev-fastboot/uefi/source/project/out/socrates-static-fixed/packaging.json>)、[编译命令与源哈希](<F:/Xiaomi15-iOS26.2-Dev-fastboot/uefi/source/project/out/socrates-static-fixed/components.json>)和 [FD 来源元数据](<F:/Xiaomi15-iOS26.2-Dev-fastboot/uefi/source/project/out/socrates-static-fixed/fd-provenance.json>)。

## 4. 现在的结论

1. 旧记录不足以证明标准 bootloader 的 RAM boot 不受支持；原图缺 bootstrap 使原实验无法验证完整预期链。
2. 新容器已静态补齐该链，但尚未在手机执行；静态 inspect 与编译成功都不等于硬件启动成功。
3. `runtime_ready=true` 已由新 kernel 的修复标记、内嵌 source stamp 和 provenance 侧车闭合；[preflight](<F:/Xiaomi15-iOS26.2-Dev-fastboot/tools/k60pro/preflight.py>)通过。硬件尚未验证。
4. 已完成：GitHub Actions 新 kernel → 标记/来源侧车/哈希 → 重包 → 离线预检通过；下一步才由用户连接手机分阶段核验 bootloader→UEFI→Loader→kernel→外设。

本文件不再保留旧 flash / EDL 路线或任何自动设备操作命令。RAM-only 是工程约束，不以错误的 unsupported 推断改变它。

## 5. 可运行的离线检查

以下命令从发行包根执行，只有离线检查与源码构建/封装；本记录不列出自动设备操作命令。

```powershell
tools/python/python.exe -B tools/k60pro/preflight.py --root . --json
```

完整测试使用本轮已经存在的 Bash；下面只给测试子进程临时变量，避免 shell fixture 因缺少 K60PRO_TEST_BASH 被跳过，系统环境不变：

```bash
K60PRO_TEST_BASH="$(cygpath -w "$BASH")" tools/python/python.exe -B -m unittest discover -s tools/k60pro/tests -p "test_*.py"
```

当前离线预检在当前新内核包上通过；缺少最终 manifest、sidecar 或发现旧内核/来源不符都应在设备 subprocess 之前退出。

该结论与 [内核构建状态](<F:/Xiaomi15-iOS26.2-Dev-fastboot/ios-payload/source/project/K60PRO-BUILD.md>)一致；本源码 docs 镜像与发行包根研究记录同步。
