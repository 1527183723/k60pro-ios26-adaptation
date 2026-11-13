/*
 * warsaw: paste into xnuloader/platform_private.h next to platform_annibale and add &platform_warsaw
 * to platforms[]. Every value is explained in docs/PORTING.md ("The loader").
 *
 * All of these were read from the device's own device tree (common/devices/warsaw.dtb, pulled from
 * /sys/firmware/fdt on the phone), not copied from annibale — but they come out identical, because the
 * two phones are the same SoC and the boot chain hands the same window over.
 */
static const struct platform platform_warsaw = {
	.name = "warsaw",
	/* One window over the SoC peripherals. Must not start at 0: XNU reads a zero base as "no SoC"
	 * and then skips clock setup. 0x400000..0x40000000 covers every node in the DT. */
	.io_base = 0x00400000, .io_size = 0x3FC00000,
	/* interrupt-controller@16000000: reg = <0x16000000 0x10000 0x16080000 0x200000>,
	 * redistributor-stride = <0x0 0x40000>. Same numbers as annibale. */
	.gic_dist = 0x16000000, .gic_redist = 0x16080000,
	.gic_redist_size = 0x200000, .gic_redist_stride = 0x40000,
	/* uart stays 0 on purpose, for the same reason as annibale: the console is the GENI uart at
	 * 0xa9c000 (115200n8) but its QUP clock is gated until a driver enables it, and with uart==0 the
	 * loader omits serial= from the cmdline instead of writing a gated register blind. */
	.uart = 0, .log_other_cpu = 1,
	/* cpu@0..cpu@500 carry capacity-dmips-mhz 0x700; cpu@10000/10100 carry 0x766. Qualcomm numbers
	 * cores in MPIDR's second affinity field, so the six little ones are 0..0x500 and the two big ones
	 * are 0x10000/0x10100 — the linear enumeration below reaches the first six only. */
	.cores = 8, .core_mpidr = {0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x10000, 0x10100},
	.small_cores = 0x3f,             /* the six 0x700 cores: the lower-capacity cluster */
	/* splash_region in the DT: reg = <0x0 0xfc800000 0x0 0x2b00000>, matching MemoryMapLib's
	 * Display_Reserved. The panel is 1280x2772 30bpp (Panel_P12X_42_02_0a_dsc_cmd.xml), not
	 * annibale's 1156x2510 — the loader's DIRECT_PHONE_PLAT screen geometry needs that number. */
	.fb_base = 0xFC800000,
};
