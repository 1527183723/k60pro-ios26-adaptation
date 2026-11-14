#!/bin/bash
set -e -o pipefail
# Boot HackiOS on a device whose images are built (common/build.sh <device>).
#
# usage: common/run.sh <device> [seconds]
#   common/run.sh qemu-virt          QEMU window, 20 minutes (mouse = finger, H = home, L = lock)
#   common/run.sh redwood 900        the phone over USB: loader in RAM, system image in RAM, 15 minutes
# Phone bounds in seconds: XL_ENTER_TIMEOUT=120, XL_RAMDISK_TIMEOUT=7200,
# XL_RUN_TIMEOUT defaults to the requested session duration plus 120 seconds.
#
# Nothing is flashed: on a phone everything lives in RAM and `./xl reboot` (or a power-key reset)
# brings back Android. The session's log is printed (QEMU) or saved in logs/ (phones).
. "$(dirname "$(readlink -f "$0")")/lib.sh"
load_device "$1"
SECS=${2:-$DEFAULT_SECS}
mkdir -p logs
[ -f userland/root.apfs ] && [ -f userland/system.apfs ] || die "no images: run common/build.sh $DEVICE first"

timed_xl() {
	local limit=$1
	shift
	timeout --kill-after=3 "$limit" ./xl "$@"
}

case "$KIND" in
qemu)
	QEMU_SMP=${QEMU_SMP:-$SMP} CMDLINE="${CMDLINE:-$BOOTARGS}" qemu/run-ios.sh "$SECS" -display "${DISPLAY_KIND:-gtk}" |
		tee logs/qemu-$(date +%H%M%S).log
	;;
phone)
	command -v fastboot >/dev/null || die "fastboot is not installed"
	command -v timeout >/dev/null || die "GNU timeout is required for bounded phone commands"
	[[ "$SECS" =~ ^[0-9]+$ ]] || die "seconds must be a non-negative integer"
	ENTER_TIMEOUT=${XL_ENTER_TIMEOUT:-120}
	RAMDISK_TIMEOUT=${XL_RAMDISK_TIMEOUT:-7200}
	RUN_TIMEOUT=${XL_RUN_TIMEOUT:-$((10#$SECS + 120))}
	for limit in "$ENTER_TIMEOUT" "$RAMDISK_TIMEOUT" "$RUN_TIMEOUT"; do
		[[ "$limit" =~ ^[0-9]+([.][0-9]+)?$ && "$limit" =~ [1-9] ]] || die "phone timeouts must be positive seconds"
	done
	timed_xl "$ENTER_TIMEOUT" enter || { status=$?; echo "run: loader entry failed (exit $status)" >&2; exit "$status"; }
	timed_xl "$RAMDISK_TIMEOUT" ramdisk userland/system.apfs
	XL_SECS=$SECS XL_CMDLINE="${XL_CMDLINE:-$BOOTARGS}" timed_xl "$RUN_TIMEOUT" run
	echo "run: './xl screenshot shot.png' for the screen, './xl reboot' to go back to Android"
	;;
*) die "unknown KIND=$KIND in common/devices/$DEVICE.conf" ;;
esac
