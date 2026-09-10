#!/usr/bin/env python3
"""Unit tests for rp_deploy helpers (no SSH required)."""

import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import rp_deploy  # noqa: E402


class BuildArtifactTests(unittest.TestCase):
    def test_keeps_build_outputs(self):
        self.assertTrue(rp_deploy._is_build_artifact("out"))
        self.assertTrue(rp_deploy._is_build_artifact("out/afm_server"))
        self.assertTrue(rp_deploy._is_build_artifact("Server/main.o"))
        self.assertTrue(rp_deploy._is_build_artifact("data/run.csv"))

    def test_deletes_sources(self):
        self.assertFalse(rp_deploy._is_build_artifact("Server/main.cpp"))
        self.assertFalse(rp_deploy._is_build_artifact("Makefile"))


class ResolveBitfileTests(unittest.TestCase):
    def test_bit_bin_is_copied_as_is(self):
        local, binpath = rp_deploy.resolve_bitfile("fpga/foo.bit.bin")
        self.assertIsNone(local)
        self.assertEqual(binpath, "fpga/foo.bit.bin")

    def test_raw_bit_is_converted(self):
        local, binpath = rp_deploy.resolve_bitfile("fpga/foo.bit")
        self.assertEqual(local, "fpga/foo.bit")
        self.assertEqual(binpath, "fpga/foo.bit.bin")

    def _with_repo_root(self, root):
        original = rp_deploy.REPO_ROOT
        rp_deploy.REPO_ROOT = Path(root)
        self.addCleanup(setattr, rp_deploy, "REPO_ROOT", original)

    def test_no_candidates(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._with_repo_root(tmp)
            self.assertEqual(rp_deploy.resolve_bitfile(None), (None, None))

    def test_newest_candidate_wins(self):
        with tempfile.TemporaryDirectory() as tmp:
            bitdir = Path(tmp) / "fpga" / "bitfiles"
            bitdir.mkdir(parents=True)
            old = bitdir / "old.bit.bin"
            new = bitdir / "new.bit.bin"
            old.write_bytes(b"old")
            new.write_bytes(b"new")
            os.utime(old, (1_000_000, 1_000_000))
            os.utime(new, (2_000_000, 2_000_000))
            self._with_repo_root(tmp)
            local, binpath = rp_deploy.resolve_bitfile(None)
            self.assertIsNone(local)
            self.assertEqual(Path(binpath).name, "new.bit.bin")


class SyncCppFilesTests(unittest.TestCase):
    def _run_sync(self, scp):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "Server" / "main.cpp"
            source.parent.mkdir()
            source.write_text("int main() {}")

            originals = {
                "LOCAL_PROJECT": rp_deploy.LOCAL_PROJECT,
                "REMOTE_PROJECT": rp_deploy.REMOTE_PROJECT,
                "run_remote": rp_deploy.run_remote,
            }
            rp_deploy.LOCAL_PROJECT = tmp
            rp_deploy.REMOTE_PROJECT = "/remote/cpp"
            rp_deploy.run_remote = lambda ssh, command: (0, "", "")
            try:
                with redirect_stdout(io.StringIO()):
                    return rp_deploy.sync_cpp_files(scp, object())
            finally:
                for name, value in originals.items():
                    setattr(rp_deploy, name, value)

    def test_success_returns_true(self):
        class RecordingSCP:
            def __init__(self):
                self.copies = []

            def put(self, local, remote):
                self.copies.append((local, remote))

        scp = RecordingSCP()
        self.assertTrue(self._run_sync(scp))
        self.assertEqual(len(scp.copies), 1)
        self.assertTrue(scp.copies[0][1].endswith("Server/main.cpp"))

    def test_scp_failure_returns_false(self):
        class FailingSCP:
            def put(self, local, remote):
                raise OSError("disk full")

        self.assertFalse(self._run_sync(FailingSCP()))

    def test_remote_delete_failure_returns_false(self):
        def run_remote(ssh, command):
            if command.startswith("find "):
                return 0, "/remote/cpp/Stale.cpp", ""
            if command.startswith("rm -f"):
                return 1, "", "permission denied"
            return 0, "", ""

        with tempfile.TemporaryDirectory() as tmp:
            originals = {
                "LOCAL_PROJECT": rp_deploy.LOCAL_PROJECT,
                "REMOTE_PROJECT": rp_deploy.REMOTE_PROJECT,
                "run_remote": rp_deploy.run_remote,
            }
            rp_deploy.LOCAL_PROJECT = tmp
            rp_deploy.REMOTE_PROJECT = "/remote/cpp"
            rp_deploy.run_remote = run_remote
            try:
                with redirect_stdout(io.StringIO()):
                    result = rp_deploy.sync_cpp_files(object(), object())
            finally:
                for name, value in originals.items():
                    setattr(rp_deploy, name, value)
        self.assertFalse(result)


if __name__ == "__main__":
    unittest.main()
