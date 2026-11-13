# K60 Pro (socrates) 内核构建记录

- GitHub 仓库：https://github.com/1527183723/k60pro-ios26-adaptation
- 构建 workflow：.github/workflows/build-xnu-socrates.yml
- 成功运行：https://github.com/1527183723/k60pro-ios26-adaptation/actions/runs/43450623746
- 内核产物：mach.development.bcm2837
- 内核大小：20,435,464 字节
- 内核 SHA256：aa4886fc8eb00859b3a981252cd4893b0f913e7db3f93af5e7cba8707eaed492
- 内核包含 socrates 字符串：是
- 对应 payload：ios-payload/socrates-payload.bin
- payload 大小：54,382,104 字节
- payload SHA256：509f6d9c4d00b598890e35eaac91e3e1ccf8dce7337490e0d6343199252681da

当前 payload cmdline：

```text
debug=0x10e rootdev=md0 launchd_unsecure_cache=1 panic_on_crash_disabled=1 cpus=8 bootcpu=7 small_cores_after=0 touchscreen=3 fb_threads=2 fb_flip=2 fbwidth=1440 fbheight=3200 fbpitch=560 applog=2 autounlock=1 fb_hz=120 fb_ca_hz=120 touch_poll_us=4000 epoch=1794583659
```

说明：

- 触摸已按 socrates 的 QUP1 SE4 / Goodix m11 参数修改，但尚未真机验证。
- CPU 调频、按键、电量、GPU、Wi-Fi 仍未启用；当前 payload 不带 adreno8、cpufreq、battery、wifi、pcie、metal。
- 真机测试前请确认连接的是 K60 Pro（ro.product.device=socrates），不要把它和 haotian 混淆。
