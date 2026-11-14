/* Platform descriptions and CPU affinity mapping used only by loader.c. */
#ifndef XNULOADER_PLATFORM_PRIVATE_H
#define XNULOADER_PLATFORM_PRIVATE_H

struct platform {
	const char *name;
	uint64_t io_base, io_size;       /* one window covering the SoC peripherals */
	uint64_t gic_dist, gic_redist;   /* GICv3 */
	uint64_t gic_redist_size, gic_redist_stride; /* 0 keeps the 1 MiB / 128 KiB defaults */
	uint64_t uart;                   /* PL011, 0 if none */
	int psci_hvc;                    /* the firmware's power interface is called with HVC, not SMC */
	int log_other_cpu;               /* keep notes in the log partition while a kernel starts on another core */
	int cores;                       /* at most this many processor cores, numbered from 0 ... */
	uint32_t core_mpidr[8];          /* CPU node reg values, indexed by physical core number. */
	uint32_t timeout_scale;          /* a kernel on several cores gets this much longer to wait for a lock, 0: as it is */
	uint32_t small_cores;            /* a bit for each of the slower cores, by number: "small-core" in the device tree */
	uint64_t fb_base;                /* the framebuffer the firmware reports (GOP): it tells this device apart */
};

static const struct platform platform_qemu_virt = {
	.name = "qemu-virt", .io_base = 0x08000000, .io_size = 0x08000000,
	.gic_dist = 0x08000000, .gic_redist = 0x080A0000, .uart = 0x09000000, .psci_hvc = 1,
	.cores = 8, .core_mpidr = {0, 1, 2, 3, 4, 5, 6, 7},
	/*
	 * QEMU's emulated cores stop each other all the time (every flush of a page from all the
	 * TLBs is a rendezvous of all its CPU threads), and the kernel's patience with a spin lock
	 * is that of real hardware: an eighth of a second by a 24 MHz clock, a twentieth here.
	 */
	.timeout_scale = 20,
};

static const struct platform platform_redwood = {
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC" and skips clock setup. */
	.name = "redwood", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_mpidr = {0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700},
	/*
	 * Cores 0-3 are Cortex-A55s (1.8 GHz at most), 4-7 Cortex-A78s (2.4 GHz): a big one is about
	 * 2.5 times as fast. The kernel uses the small ones only when the big ones are all busy
	 * (XNU's machine_routines.c, ml_choose_slow_core_partners()).
	 */
	.small_cores = 0x0f,
	.fb_base = 0xE1000000,
};

/*
 * annibale (REDMI K90, SM8750): the merged device tree supplies the GIC ranges and
 * 0x40000 redistributor stride, PSCI method "smc", and splash region at 0xFC800000.
 * MPIDRs are 0..500 and 10000/10100; the linear core enumeration below reaches the first six.
 * DIRECT_PHONE_PLAT selects this platform even without GOP. See docs/analysis/annibale-hw-map.md
 * in the parent workspace for the device-tree measurements.
 */
static const struct platform platform_annibale = {
	.name = "annibale", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x16000000, .gic_redist = 0x16080000,
	.gic_redist_size = 0x200000, .gic_redist_stride = 0x40000,
	/*
	 * uart stays 0 on purpose: the console of this device is the GENI uart at 0xa9c000
	 * (115200n8, docs/analysis/annibale-hw-map.md), but its QUP clock is gated until a
	 * driver enables it, so writing the register blind risks hanging the kernel's early
	 * console; and with uart==0 the loader's cmdline omits serial= entirely, which is
	 * what a device without a probe wire wants. Flip when the pad is instrumented.
	 */
	.uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_mpidr = {0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x10000, 0x10100},
	.small_cores = 0x3f,
	.fb_base = 0xFC800000,
};

/*
 * warsaw (REDMI K90 Ultra, SM8750-AB): annibale's sibling — same SoC, same GIC layout, same core
 * topology, same splash region. Every value below was read from the phone's own device tree
 * (common/devices/warsaw.dtb), and they come out identical; what differs is the panel
 * (1280x2772 here, 1156x2510 on annibale), which the loader's DIRECT_PHONE_PLAT path hardcodes.
 * DIRECT_PHONE_PLAT selects this platform even without GOP.
 */
static const struct platform platform_warsaw = {
	.name = "warsaw", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x16000000, .gic_redist = 0x16080000,
	.gic_redist_size = 0x200000, .gic_redist_stride = 0x40000,
	.uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_mpidr = {0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x10000, 0x10100},
	.small_cores = 0x3f,
	.fb_base = 0xFC800000,
};

/*
 * socrates (REDMI K60 Pro, SM8550-AB): values from common/devices/socrates.dtb,
 * pulled from the phone's own /sys/firmware/fdt. The GIC, CPU topology and splash
 * region are kalama, not sun; the panel is 1440x3200.
 */
#include "../common/devices/socrates.platform.c"

/*
 * The devices the loader knows, told apart by the framebuffer base their UEFI reports. A port to another
 * device adds its description here (common/new-device.sh writes a starting point, docs/PORTING.md says
 * where each value comes from); a display that matches none is QEMU's ramfb.
 */
static const struct platform *const platforms[] = {
	&platform_redwood,
	&platform_annibale,
	&platform_warsaw,
	&platform_socrates,
};

/*
 * A phone build is built for exactly one device, and the profile says which: warsaw.conf passes
 * -DWARSAW, socrates.conf passes -DSOCRATES, every other phone keeps annibale. warsaw, annibale
 * and socrates report their own framebuffer bases; this macro also carries the panel geometry the
 * DIRECT_PHONE_PLAT path has to hardcode.
 */
#ifdef WARSAW
#define PLAT_PHONE      (&platform_warsaw)
#ifdef DADA_DISPLAY_ONLY
/* Display-only experiment: keep warsaw's platform and every peripheral path. */
#define PLAT_PANEL_W    1200
#define PLAT_PANEL_H    2670
#else
#define PLAT_PANEL_W    1280
#define PLAT_PANEL_H    2772
#endif
#elif defined(SOCRATES)
#define PLAT_PHONE      (&platform_socrates)
#define PLAT_PANEL_W    SOCRATES_WIDTH
#define PLAT_PANEL_H    SOCRATES_HEIGHT
#else
#define PLAT_PHONE      (&platform_annibale)
#define PLAT_PANEL_W    1156
#define PLAT_PANEL_H    2510
#endif

static int
platform_core_index(const struct platform *plat, uint64_t mpidr)
{
	for (int core = 0; core < plat->cores; core++) {
		if (plat->core_mpidr[core] == mpidr) { return core; }
	}
	return -1;
}

#endif
