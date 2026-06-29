"""Repository-level static checks for scripts, docs, and config scope."""

import os
import re
import unittest


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))


class TestRepositoryStructure(unittest.TestCase):
    def test_required_scripts_exist(self):
        scripts = [
            "00_install_deps.sh",
            "01_fetch_sources.sh",
            "02_build_linux_kernel.sh",
            "03_build_initramfs.sh",
            "04_build_zephyr.sh",
            "05_build_userspace.sh",
            "06_build_ebpf.sh",
            "07_run_qemu_linux.sh",
            "08_run_qemu_zephyr.sh",
            "09_run_pty_bridge.sh",
            "10_run_demo.sh",
            "11_collect_report.sh",
            "run_valgrind.sh",
            "99_cleanup.sh",
        ]
        for script in scripts:
            with self.subTest(script=script):
                path = os.path.join(ROOT, "scripts", script)
                self.assertTrue(os.path.exists(path), path)
                self.assertTrue(os.access(path, os.X_OK), path)

    def test_protocol_copies_are_identical(self):
        canonical = os.path.join(ROOT, "include", "safety_protocol.h")
        copies = [
            os.path.join(ROOT, "userspace", "common", "safety_protocol.h"),
            os.path.join(ROOT, "linux", "drivers", "safety_copro", "safety_protocol.h"),
            os.path.join(ROOT, "zephyr", "safety_copro_firmware", "include", "safety_protocol.h"),
        ]
        with open(canonical, "rb") as f:
            expected = f.read()
        for copy in copies:
            with self.subTest(copy=copy):
                with open(copy, "rb") as f:
                    self.assertEqual(f.read(), expected)

    def test_linux_driver_patch_structure_exists(self):
        paths = [
            "linux/configs/qemu_arm64_safety_defconfig",
            "linux/kernel-patches/0001-add-safety-copro-driver.patch",
            "linux/drivers/safety_copro/Kconfig",
            "linux/drivers/safety_copro/Makefile",
            "linux/drivers/safety_copro/safety_copro_main.c",
            "linux/drivers/safety_copro/safety_copro_trace.h",
        ]
        for rel in paths:
            with self.subTest(path=rel):
                self.assertTrue(os.path.exists(os.path.join(ROOT, rel)))


class TestZephyrConfig(unittest.TestCase):
    def test_formal_prj_conf_minimal(self):
        path = os.path.join(ROOT, "zephyr", "safety_copro_firmware", "prj.conf")
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        required = [
            "CONFIG_MULTITHREADING=y",
            "CONFIG_SMP=y",
            "CONFIG_MP_MAX_NUM_CPUS=2",
            "CONFIG_SERIAL=y",
            "CONFIG_UART_INTERRUPT_DRIVEN=y",
            "CONFIG_CONSOLE=n",
            "CONFIG_UART_CONSOLE=n",
            "CONFIG_LOG=n",
            "CONFIG_PRINTK=n",
            "CONFIG_SHELL=n",
        ]
        for item in required:
            self.assertIn(item, text)
        forbidden = [
            "CONFIG_CONSOLE=y",
            "CONFIG_UART_CONSOLE=y",
            "CONFIG_LOG=y",
            "CONFIG_PRINTK=y",
            "CONFIG_SHELL=y",
            "CONFIG_TASK_WDT",
        ]
        for item in forbidden:
            self.assertNotIn(item, text)


class TestMarkdownDocs(unittest.TestCase):
    FORBIDDEN_WORDS = [
        "履歷", "resume", "新人", "新手", "面試", "企業級", "生產等級",
        "最佳實踐", "高度可擴充", "完整解決方案", "這不是玩具專案",
        "本專案旨在", "以下將帶領你", "可作為展示",
    ]

    def iter_markdown_files(self):
        for base, dirs, files in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d not in {".git", "build", "vendor", "dist", "node_modules"}]
            for name in files:
                if name.endswith(".md"):
                    yield os.path.join(base, name)

    def test_forbidden_wording_absent(self):
        for path in self.iter_markdown_files():
            with open(path, "r", encoding="utf-8") as f:
                text = f.read()
            for word in self.FORBIDDEN_WORDS:
                with self.subTest(path=os.path.relpath(path, ROOT), word=word):
                    self.assertNotIn(word, text)

    def test_relative_markdown_links_exist(self):
        link_re = re.compile(r"\[[^\]]+\]\(([^)]+)\)")
        for path in self.iter_markdown_files():
            with open(path, "r", encoding="utf-8") as f:
                text = f.read()
            base = os.path.dirname(path)
            for target in link_re.findall(text):
                if target.startswith(("http://", "https://", "mailto:")):
                    continue
                clean = target.split("#", 1)[0]
                if not clean:
                    continue
                with self.subTest(path=os.path.relpath(path, ROOT), target=target):
                    self.assertTrue(os.path.exists(os.path.normpath(os.path.join(base, clean))))


if __name__ == "__main__":
    unittest.main()
