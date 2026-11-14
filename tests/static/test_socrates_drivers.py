#!/usr/bin/env python3
"""Read-only source contracts for the unfinished socrates driver port.

Uses only the standard library; never imports device tools or executes hardware
operations. Source assertions are offline checks, not electrical/boot validation.
"""
from pathlib import Path
import re
import struct
import unittest

PROJECT = Path(__file__).resolve().parents[2]
WORKSPACE = PROJECT.parents[2]
UEFI_PROJECT = WORKSPACE / "uefi/source/project"
# The IOS Git tree owns the kernel; UEFI is not a fabricated kernel build root.
TREES = (PROJECT,) + ((UEFI_PROJECT,) if (UEFI_PROJECT / "xnu/files").is_dir() else ())


def source(tree, relative):
    return (tree / "xnu/files" / relative).read_text(encoding="utf-8")


def masked_c(text):
    pattern = r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda m: re.sub(r"[^\n]", " ", m.group()), text,
                  flags=re.S)


def block_after(text, pattern):
    masked = masked_c(text)
    match = re.search(pattern, masked, re.S)
    if match is None:
        raise AssertionError("source entry not found: " + pattern)
    start = masked.index("{", match.start())
    depth = 1
    for end in range(start + 1, len(masked)):
        depth += (masked[end] == "{") - (masked[end] == "}")
        if depth == 0:
            return text[start + 1:end]
    raise AssertionError("unterminated source block")


def function_body(text, name):
    return block_after(text, r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{")


def fdt_nodes(data):
    header = struct.unpack_from(">10I", data)
    if header[0] != 0xD00DFEED or header[1] != len(data):
        raise AssertionError("invalid reference FDT header/size")
    strings = data[header[3]:header[3] + header[8]]
    pos, stack, nodes = header[2], [], {}
    while True:
        tag = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        if tag == 1:
            end = data.index(0, pos)
            stack.append(data[pos:end].decode("ascii"))
            pos = (end + 4) & ~3
            nodes.setdefault("/" + "/".join(stack[1:]), {})
        elif tag == 2:
            stack.pop()
        elif tag == 3:
            size, offset = struct.unpack_from(">II", data, pos)
            pos += 8
            end = strings.index(0, offset)
            name = strings[offset:end].decode("ascii")
            nodes["/" + "/".join(stack[1:])][name] = data[pos:pos + size]
            pos = (pos + size + 3) & ~3
        elif tag == 4:
            pass
        elif tag == 9:
            return nodes
        else:
            raise AssertionError("invalid reference FDT token")


class SocratesDriverContracts(unittest.TestCase):
    def test_goodix_uses_dtb_drv2_and_tcs_bank(self):
        nodes = fdt_nodes((PROJECT / "common/devices/socrates.dtb").read_bytes())
        node = nodes["/soc/apps_rsc@17a00000"]
        drv = nodes["/soc/apps_rsc@17a00000/drv@2"]
        driver_id = struct.unpack(">I", drv["qcom,drv-id"])[0]
        names = node["reg-names"].rstrip(b"\0").decode("ascii").split("\0")
        regs = struct.unpack(">6I", node["reg"])
        expected_rsc = regs[2 * names.index("drv-" + str(driver_id))]
        expected_tcs = struct.unpack(">I", drv["qcom,tcs-offset"])[0]
        self.assertEqual((driver_id, expected_rsc, expected_tcs),
                         (2, 0x17A20000, 0xD00))
        for tree in TREES:
            with self.subTest(tree=tree.name, path=str(tree)):
                board = function_body(source(tree, "bsd/dev/redwood_goodix.c"), "board_setup")
                socrates = block_after(board, r"else\s+if\s*\(which\s*==\s*3\)\s*\{")
                value = re.search(r"rsc_phys\s*=\s*(0x[0-9a-fA-F]+)", masked_c(socrates))
                self.assertIsNotNone(value)
                self.assertEqual(int(value.group(1), 16), expected_rsc)
                bus = source(tree, "bsd/dev/redwood_touch_bus_private.h")
                offset = re.search(r"#define\s+TCS\(reg\)\s*\(rsc\s*\+\s*(0x[0-9a-fA-F]+)",
                                   masked_c(bus))
                self.assertIsNotNone(offset)
                self.assertEqual(int(offset.group(1), 16), expected_tcs)

    def assert_socrates_guard_first(self, text, name, return_value=""):
        body = function_body(text, name)
        self.assertRegex(masked_c(body),
                         r"^\s*if\s*\(\s*redwood_platform_is_socrates\s*\(\s*\)\s*\)\s*\{")
        guard = block_after(body, r"if\s*\(\s*redwood_platform_is_socrates\s*\(\s*\)\s*\)\s*\{")
        self.assertRegex(masked_c(guard), r"\breturn\s*" + re.escape(return_value) + r"\s*;")
        self.assertNotRegex(masked_c(guard),
                            r"\b(?:ml_io_map\w*|smmu_\w+|gic_v3_\w+|bzero|cdevsw_add|flip_(?:rd|wr|clocks))\s*\(")
        return body, guard

    def test_annibale_init_cannot_reach_mmio_or_dma_on_socrates(self):
        entries = {
            "bsd/dev/redwood_audio.c": "redwood_audio_init",
            "bsd/dev/annibale_audio_reach.c": "annibale_audio_reach_init",
            "bsd/dev/annibale_iris.c": "annibale_iris_init",
            "bsd/dev/annibale_camera.c": "annibale_camera_init",
        }
        for tree in TREES:
            for relative, name in entries.items():
                with self.subTest(tree=str(tree), entry=name):
                    text = source(tree, relative)
                    self.assert_socrates_guard_first(text, name)
                    self.assertIn("#include <sys/redwood_input.h>", text)

    def test_display_entries_stop_before_legacy_mmio(self):
        entries = {
            "iokit/Kernel/MiniFramebufferFlip.inc": {"flip_start": "", "flip_clocks": "false"},
            "iokit/Kernel/MiniFramebufferDim.inc": {
                "dim_find": "", "MiniIOMobileFramebufferDim": "",
                "MiniIOMobileFramebufferDimShown": "-1", "dim_quiesce": "", "dim_poweroff": ""},
            "iokit/Kernel/MiniFramebufferDCS.inc": {"dcs_start": "", "MiniIOMobileFramebufferDCS": "false"},
        }
        for tree in TREES:
            for relative, names in entries.items():
                for name, value in names.items():
                    with self.subTest(tree=str(tree), entry=name):
                        self.assert_socrates_guard_first(source(tree, relative), name, value)

    def test_old_payload_flags_are_overridden_and_cpu_rendering_survives(self):
        for tree in TREES:
            text = source(tree, "iokit/Kernel/MiniFramebufferFlip.inc")
            _, guard = self.assert_socrates_guard_first(text, "flip_start")
            assignments = dict(re.findall(r"\b(gFlip|gDim|gDcs)\s*=\s*([0-9]+)\s*;", masked_c(guard)))
            self.assertEqual(assignments, {"gFlip": "0", "gDim": "0", "gDcs": "0"})
            self.assertRegex(masked_c(guard), r"flip\.ready\s*=\s*false\s*;")
            target = function_body(text, "flip_target")
            self.assertRegex(masked_c(target), r"if\s*\(\s*!flip\.ready\s*\)\s*\{\s*return\s+base\s*;")
            start = function_body(source(tree, "iokit/Kernel/MiniIOMobileFramebuffer.cpp"),
                                  "IOMobileFramebuffer::start")
            self.assertNotIn("redwood_platform_is_socrates", masked_c(start))
            self.assertIn("flip_start", masked_c(start))

    def test_platform_api_is_c_linked_before_cpp_display_includes(self):
        for tree in TREES:
            header = source(tree, "bsd/sys/redwood_input.h")
            self.assertIn('extern "C" {', header)
            linked = block_after(header, r"extern\s*\{")
            self.assertIn("bool redwood_platform_is_socrates(void);", linked)
            self.assertIn("extern uint32_t redwood_frames_shown;", linked)
            cpp = source(tree, "iokit/Kernel/MiniIOMobileFramebuffer.cpp")
            self.assertLess(cpp.index("#include <sys/redwood_input.h>"),
                            cpp.index('#include "MiniFramebufferFlip.inc"'))

    def test_revision_diagnostic_is_socrates_only_and_once(self):
        for tree in TREES:
            text = source(tree, "bsd/dev/redwood_input.c")
            self.assertEqual(text.count("K60PRO_STATIC_FIX_V1"), 1)
            body = function_body(text, "redwood_input_init")
            self.assertRegex(masked_c(body), r"static\s+bool\s+socrates_revision_logged\s*;")
            guard = block_after(body, r"if\s*\(\s*redwood_platform_is_socrates\(\)\s*&&\s*!socrates_revision_logged\s*\)\s*\{")
            self.assertIn("K60PRO_STATIC_FIX_V1", guard)
            self.assertRegex(masked_c(guard), r"socrates_revision_logged\s*=\s*true\s*;")


if __name__ == "__main__":
    unittest.main(verbosity=2)
