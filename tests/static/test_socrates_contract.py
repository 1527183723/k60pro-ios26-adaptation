"""Offline public-contract checks; no device, build or network commands."""
from pathlib import Path
import importlib.util
import unittest
import shutil
import subprocess
import re

ROOT = Path(__file__).resolve().parents[2]
API_PATH = ROOT / "common/devices/socrates_contract.py"


def api():
    spec = importlib.util.spec_from_file_location("socrates_contract", API_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def cpp_path(path):
    value = path.as_posix()
    if len(value) > 2 and value[1] == ":":
        return "/mnt/" + value[0].lower() + value[2:]
    return value


def preprocess_output(source):
    compiler = [shutil.which("gcc")] if shutil.which("gcc") else ["wsl.exe", "-e", "gcc"]
    result = subprocess.run(compiler + ["-E", "-P", "-x", "c", "-"],
                            input=source.encode(), capture_output=True, timeout=30)
    return result.returncode, result.stdout.decode("utf-8", "replace"), result.stderr.decode("utf-8", "replace")


def preprocess(source):
    code, output, stderr = preprocess_output(source)
    return code, stderr


class SocratesContractTests(unittest.TestCase):
    def test_load_contract_has_the_captured_k60_reserved_window(self):
        c = api().load_contract(ROOT / "common/devices/socrates.contract.json")
        self.assertEqual(c["device"], "socrates")
        self.assertEqual(int(c["payload_base"], 16), 0xC8000000)
        self.assertEqual(int(c["payload_cap"], 16), 0x03700000)
        self.assertEqual(int(c["payload_region_size"], 16), 0x03800000)
        self.assertEqual(int(c["panic_log_base"], 16), 0xCB700000)
        self.assertEqual((c["width"], c["height"]), (1440, 3200))

    def test_both_source_trees_and_mu_have_current_generated_consumers(self):
        module = api()
        uefi = ROOT.parents[2] / "uefi/source/project"
        mu = Path("C:/Users/Administrator/Desktop/dsh/.mu-build/Mu-Silicium")
        if not mu.is_dir():
            mu = Path("/mnt/c/Users/Administrator/Desktop/dsh/.mu-build/Mu-Silicium")
        self.assertEqual(module.check_generated(ROOT), [])
        self.assertEqual(module.check_generated(uefi, mu if mu.is_dir() else None), [])
        self.assertEqual(module.load_contract(), module.load_contract(uefi / "common/devices/socrates.contract.json"))
        self.assertEqual(API_PATH.read_bytes(), (uefi / "common/devices/socrates_contract.py").read_bytes())

    def test_socrates_native_loader_uses_c8_and_forces_ram_only_in_both_trees(self):
        roots = [ROOT, ROOT.parents[2] / "uefi/source/project"]
        for root in roots:
            with self.subTest(root=str(root)):
                source = '#define SOCRATES 1\n#include "' + cpp_path(root / "xnuloader/platform_private.h") + '"\n'
                source += '#include "' + cpp_path(root / "xnuloader/loader_01_base_log_dt.inc") + '"\n'
                source += """
#if PAYLOAD_BASE != 0xC8000000ULL || PAYLOAD_CONTAINER_MAX != 0x03700000ULL
#error wrong socrates container address or cap
#endif
#if PAYLOAD_RESERVE_SIZE != 0x03800000ULL || PANIC_LOG_BASE != 0xCB700000ULL
#error wrong socrates reserved window or panic address
#endif
#if PLAT_PANEL_W != 1440 || PLAT_PANEL_H != 3200
#error wrong socrates display geometry
#endif
#ifndef DADA_USB_RAM_ONLY
#error socrates must force RAM-only even for a direct compiler invocation
#endif
"""
                code, stderr = preprocess(source.replace("\\n", "\n"))
                self.assertEqual(code, 0, stderr)

    def test_effective_ram_only_loader_has_zero_persistent_block_write_calls(self):
        for root in [ROOT, ROOT.parents[2] / "uefi/source/project"]:
            with self.subTest(root=str(root)):
                code, output, stderr = preprocess_output('#define SOCRATES 1\n#include "' + cpp_path(root / "xnuloader/loader.c") + '"\n')
                self.assertEqual(code, 0, stderr)
                self.assertEqual(re.findall(r"->\s*write_blocks\s*\(", output), [], "persistent BlockIo writes survive RAM-only preprocessing")
                self.assertEqual(output.count("K60PRO_USB_RAM_ONLY_V1"), 1)
                self.assertIn('fb_reply("FAIL", "RAM-only: WRITE_PROTECTED")', output)
                logs = re.search(r"write_logs\([^\n]*\)\n\{(.*?)\n\}", output, re.S).group(1)
                self.assertIn("compose_logs(klog, klog_len)", logs)
                self.assertIn("put_char(klog[i])", logs)

    def test_legacy_warsaw_keeps_f0_and_its_original_write_paths(self):
        for root in [ROOT, ROOT.parents[2] / "uefi/source/project"]:
            with self.subTest(root=str(root)):
                source = '#define WARSAW 1\n#include "' + cpp_path(root / "xnuloader/loader.c") + '"\n'
                source += """
#if PAYLOAD_BASE != 0xF0000000ULL || PANIC_LOG_BASE != 0xF3700000ULL
#error legacy address changed
#endif
#ifdef DADA_USB_RAM_ONLY
#error socrates RAM-only leaked into warsaw
#endif
"""
                code, output, stderr = preprocess_output(source)
                self.assertEqual(code, 0, stderr)
                self.assertGreater(len(re.findall(r"->\s*write_blocks\s*\(", output)), 0)
                self.assertNotIn("K60PRO_USB_RAM_ONLY_V1", output)

    def test_invalid_or_stale_contract_is_rejected(self):
        module = api()
        valid = module.load_contract()
        for field, value in [("ram_only", False), ("payload_base", "0xF0000000"),
                             ("panic_log_base", "0xF3700000"), ("bootargs", "touchscreen=3 cpus=8")]:
            with self.subTest(field=field):
                invalid = dict(valid)
                invalid[field] = value
                with self.assertRaises(ValueError):
                    module.validate_contract(invalid)

    def test_mu_consumers_bind_the_generated_fixed_pcd_contract(self):
        overlay = ROOT.parents[2] / "uefi/source/project/uefi/files"
        mu = Path("C:/Users/Administrator/Desktop/dsh/.mu-build/Mu-Silicium")
        if not mu.is_dir():
            mu = Path("/mnt/c/Users/Administrator/Desktop/dsh/.mu-build/Mu-Silicium")
        shared = "Silicon/Silicium/SiliciumPkg/"
        platform = "Platforms/Xiaomi/socratesPkg/"
        for base in [overlay] + ([mu] if mu.is_dir() else []):
            with self.subTest(mu=str(base)):
                dsc = (base / (platform + "socrates.dsc")).read_text()
                self.assertIn("!include socratesPkg/Include/SocratesContract.dsc.inc", dsc)
                self.assertRegex(dsc, r"USE_CUSTOM_DISPLAY_DRIVER\s*=\s*0")
                header = (base / (shared + "Library/BootManagerLib/XnuLoader.h")).read_text()
                for pcd in ["PcdXnuPayloadBase", "PcdXnuPayloadRegionSize", "PcdXnuPayloadMaxSize"]:
                    self.assertIn("FixedPcdGet64 (" + pcd + ")", header)
                self.assertIn("FixedPcdGetBool (PcdXnuRamOnly)", header)
                memory = (base / (platform + "Library/MemoryMapLib/MemoryMapLib.c")).read_text()
                for pcd in ["PcdXnuPayloadBase", "PcdXnuPayloadRegionSize", "PcdXnuFramebufferBase", "PcdFdBaseAddress"]:
                    self.assertIn("FixedPcdGet64 (" + pcd + ")", memory)
                self.assertIn("FixedPcdGet32 (PcdFdSize)", memory)
                dec = (base / (shared + "SiliciumPkg.dec")).read_text()
                self.assertIn("PcdXnuPayloadBase|0xF0000000|UINT64", dec)
                self.assertIn("PcdXnuRamOnly|FALSE|BOOLEAN", dec)
                terminal = (base / (shared + "Library/BootManagerLib/XnuLoader.c")).read_text()
                tail = terminal[terminal.index("XnuLoaderMain ()"):]
                self.assertIn("if (XNU_PAYLOAD_RAM_ONLY)", tail)
                self.assertLess(tail.index("if (XNU_PAYLOAD_RAM_ONLY)"), tail.index("XnuTerminalReset ();"))
                self.assertIn('__asm__ volatile ("wfi")', tail)
                if base == mu:
                    for suffix in [shared + "SiliciumPkg.dec", shared + "Library/BootManagerLib/BootManagerLib.inf",
                                   shared + "Library/BootManagerLib/XnuLoader.h", shared + "Library/BootManagerLib/XnuLoader.c",
                                   platform + "socrates.dsc", platform + "Library/MemoryMapLib/MemoryMapLib.c",
                                   platform + "Library/MemoryMapLib/MemoryMapLib.inf", platform + "Include/SocratesContract.dsc.inc"]:
                        self.assertEqual((overlay / suffix).read_text().rstrip(), (mu / suffix).read_text().rstrip())


if __name__ == "__main__":
    unittest.main()
