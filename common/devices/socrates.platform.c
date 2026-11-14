/* K60 Pro / SM8550-AB: CPU and GIC values are from the captured DTB. */
#include "socrates.contract.h"
static const struct platform platform_socrates = {
	.name = "socrates", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17100000, .gic_redist = 0x17180000,
	.gic_redist_size = 0x200000, .gic_redist_stride = 0x40000,
	.uart = 0, .log_other_cpu = 1,
	/* Eight physical CPUs; one remains resident in firmware/USB loader. */
	.cores = 8, .core_mpidr = {0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700},
	.small_cores = 0x7, .fb_base = SOCRATES_FB_BASE,
};
