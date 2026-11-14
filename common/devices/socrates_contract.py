"""Socrates device contract: validated input and pure, deterministic generation.

The Git tree's socrates.contract.json is canonical. Mirror this module and JSON
into the UEFI source tree. No import, --check or --render operation writes files.
Host packers should call load_contract(); consumers are generated_files() output.
"""
from pathlib import Path
import argparse
import json
import re
import shlex
import sys

HEX_FIELDS = ("fd_base", "fd_size", "payload_base", "payload_cap",
              "payload_region_size", "panic_log_base", "fb_base")
FIELDS = set(HEX_FIELDS) | {"device", "platform", "width", "height", "ram_only", "bootargs"}
SAFE_ARGS = {"touchscreen": "0", "fb_flip": "0", "fb_dcs": "0", "cpus": "7", "bootcpu": "7"}
PCDS = {"payload_base": "PcdXnuPayloadBase", "payload_cap": "PcdXnuPayloadMaxSize",
        "payload_region_size": "PcdXnuPayloadRegionSize", "fb_base": "PcdXnuFramebufferBase"}
MU_REL = Path("Platforms/Xiaomi/socratesPkg/Include/SocratesContract.dsc.inc")


def validate_contract(c):
    """Return a detached validated mapping, or raise ValueError before use."""
    if not isinstance(c, dict) or set(c) != FIELDS:
        raise ValueError("contract fields must exactly match the socrates schema")
    if c["device"] != "socrates" or c["platform"] != "sm8550":
        raise ValueError("contract must identify socrates/sm8550")
    n = {}
    for key in HEX_FIELDS:
        if not isinstance(c[key], str) or not re.fullmatch(r"0x[0-9a-fA-F]+", c[key]):
            raise ValueError(key + " must be a 0x-prefixed hex string")
        n[key] = int(c[key], 16)
        if n[key] <= 0 or n[key] >= 1 << 64:
            raise ValueError(key + " is outside the supported address range")
    for key in ("width", "height"):
        if type(c[key]) is not int or not 0 < c[key] <= 8192:
            raise ValueError(key + " must be a positive pixel count")
    if c["ram_only"] is not True:
        raise ValueError("socrates requires ram_only=true")
    base, cap, region = (n[k] for k in ("payload_base", "payload_cap", "payload_region_size"))
    if any(n[k] % 0x1000 for k in HEX_FIELDS):
        raise ValueError("addresses and bounds must be page aligned")
    if region - cap != 0x100000 or n["panic_log_base"] != base + cap:
        raise ValueError("panic tail must follow the container and reserve exactly 1 MiB")
    if base < 0xC7CC0000 or base + region > 0xD8800000:
        raise ValueError("payload overlaps captured K60 firmware reservations")
    if n["fd_base"] != 0xA7000000 or n["fd_size"] != 0x400000:
        raise ValueError("FD must match the captured K60 firmware region")
    if n["fb_base"] != 0xB8000000:
        raise ValueError("framebuffer must match captured K60 splash_region")
    if not isinstance(c["bootargs"], str):
        raise ValueError("bootargs must be a string")
    args = {}
    for item in shlex.split(c["bootargs"]):
        key, sep, value = item.partition("=")
        if not sep or key in args:
            raise ValueError("bootargs must contain unique key=value options")
        args[key] = value
    if any(args.get(k) != v for k, v in SAFE_ARGS.items()):
        raise ValueError("bootargs must retain the static RAM-only/7-CPU baseline")
    return dict(c)


def load_contract(path=None):
    if path is None:
        path = Path(__file__).with_name("socrates.contract.json")
    return validate_contract(json.loads(Path(path).read_text(encoding="utf-8")))


def render_profile(c):
    c = validate_contract(c)
    lines = ["# GENERATED from common/devices/socrates.contract.json; validate with socrates_contract.py --check.",
             "# Redmi K60 Pro / 22127RK46C / SM8550-AB. Static RAM-only validation baseline.",
             'NAME="REDMI K60 Pro (socrates)"', "KIND=phone",
             'SOC="Qualcomm SM8550-AB (Snapdragon 8 Gen 2)"',
             "UEFI_TARGET=socrates", "UEFI_FD=out/socrates-uefi/SILICIUM_UEFI.fd",
             "DTB=common/devices/socrates.dtb"]
    for key in HEX_FIELDS:
        lines.append(key.upper() + "=" + c[key])
    lines += ["FB_WIDTH=" + str(c["width"]), "FB_HEIGHT=" + str(c["height"]),
              "RAM_ONLY=1", '# DADA_USB_RAM_ONLY is the existing RAM-only implementation; the name is historical.',
              'LOADER_CFLAGS="-DDIRECT_PHONE_PLAT -DSOCRATES -DDADA_USB_RAM_ONLY"',
              "BOOTARGS=" + shlex.quote(c["bootargs"]),
              "# Eight physical cores; one remains in firmware/USB loader, at most seven enter XNU.",
              "WIFI_ATH11K=0", "DEFAULT_SECS=900", 'USERLAND_VARS=""', 'FIRMWARE=""']
    return "\n".join(lines) + "\n"


def render_header(c):
    c = validate_contract(c)
    lines = ["/* GENERATED from socrates.contract.json; do not edit addresses here. */",
             "#ifndef SOCRATES_CONTRACT_H", "#define SOCRATES_CONTRACT_H"]
    for key in HEX_FIELDS:
        if key != "panic_log_base":
            lines.append("#define SOCRATES_" + key.upper() + " " + c[key] + "ULL")
    lines += ["#define SOCRATES_PANIC_LOG_BASE (SOCRATES_PAYLOAD_BASE + SOCRATES_PAYLOAD_CAP)",
              "#define SOCRATES_WIDTH " + str(c["width"]), "#define SOCRATES_HEIGHT " + str(c["height"]),
              "#define SOCRATES_RAM_ONLY 1", "#endif"]
    return "\n".join(lines) + "\n"


def render_mu_dsc(c):
    c = validate_contract(c)
    lines = ["# GENERATED from common/devices/socrates.contract.json; socrates only.",
             "[Defines]", "  DEFINE FD_BASE = " + c["fd_base"],
             "  DEFINE FD_SIZE = " + c["fd_size"],
             "  DEFINE FD_BLOCKS = " + hex(int(c["fd_size"], 16) // 0x1000),
             "[PcdsFixedAtBuild]",
             "  gArmTokenSpaceGuid.PcdFdBaseAddress|" + c["fd_base"],
             "  gArmTokenSpaceGuid.PcdFdSize|" + c["fd_size"]]
    for key, pcd in PCDS.items():
        lines.append("  gSiliciumPkgTokenSpaceGuid." + pcd + "|" + c[key])
    lines += ["  gSiliciumPkgTokenSpaceGuid.PcdXnuRamOnly|TRUE",
              "  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|" + str(c["width"]),
              "  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|" + str(c["height"])]
    return "\n".join(lines) + "\n"


def generated_files(c, project_root, mu_root=None):
    """Return path->UTF-8 contents; callers publish with their observed file tools."""
    root = Path(project_root)
    files = {root / "common/devices/socrates.conf": render_profile(c),
             root / "common/devices/socrates.contract.h": render_header(c)}
    overlay = root / "uefi/files"
    if (overlay / "Platforms/Xiaomi/socratesPkg").is_dir():
        files[overlay / MU_REL] = render_mu_dsc(c)
    if mu_root is not None:
        files[Path(mu_root) / MU_REL] = render_mu_dsc(c)
    return files


def check_generated(project_root, mu_root=None):
    """Report stale/missing generated files; [] means all supplied roots agree."""
    root = Path(project_root)
    c = load_contract(root / "common/devices/socrates.contract.json")
    errors = []
    for path, expected in generated_files(c, root, mu_root).items():
        if not path.is_file() or path.read_text(encoding="utf-8") != expected:
            errors.append(str(path))
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contract", type=Path)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--mu-root", type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--render", choices=("profile", "header", "mu-dsc"))
    args = parser.parse_args()
    if args.check:
        errors = check_generated(args.root, args.mu_root)
        if errors:
            print("stale contract outputs:\n" + "\n".join(errors), file=sys.stderr)
            return 1
        print("PASS: socrates generated device contract")
    else:
        c = load_contract(args.contract)
        print({"profile": render_profile, "header": render_header, "mu-dsc": render_mu_dsc}[args.render](c), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
