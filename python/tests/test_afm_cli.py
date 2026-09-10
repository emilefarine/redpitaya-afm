#!/usr/bin/env python3
"""Unit tests for the CLI shell helpers (no server required)."""

import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from afm_cli import AFMShell  # noqa: E402
from afm_client import AFMClient, SpectrumData  # noqa: E402
from test_afm_client import FakeSCPIServer  # noqa: E402


class SpectrumSummaryTests(unittest.TestCase):
    def test_empty_spectrum_summary(self):
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            AFMShell._print_spectrum_summary(SpectrumData())
        self.assertIn("Points:         0", buffer.getvalue())

    def test_populated_spectrum_summary(self):
        spectrum = SpectrumData(
            freq_kHz=np.array([1.0, 2.0]),
            magnitude=np.array([0.5, 1.0]),
            phase_rad=np.array([0.0, 0.5]),
        )
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            AFMShell._print_spectrum_summary(spectrum)
        output = buffer.getvalue()
        self.assertIn("Peak frequency: 2.000 kHz", output)
        self.assertIn("Points:         2", output)


class ConnectTests(unittest.TestCase):
    def test_connect_after_lost_connection(self):
        server = FakeSCPIServer(lambda command, handler: "OK PONG\n").start()
        self.addCleanup(server.stop)

        # Placeholder client pointing at a dead port, as after a timeout.
        shell = AFMShell(AFMClient("127.0.0.1", 1, timeout=0.5))
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            shell.do_connect(f"127.0.0.1 {server.port}")
        try:
            self.assertTrue(shell.client.is_connected)
            self.assertIn("Connected to", buffer.getvalue())
        finally:
            shell.client.disconnect()

    def test_connect_failure_reports_error(self):
        shell = AFMShell(AFMClient("127.0.0.1", 1, timeout=0.5))
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            shell.do_connect("127.0.0.1 1")
        self.assertFalse(shell.client.is_connected)
        self.assertIn("Connection failed", buffer.getvalue())

    def test_connect_rejects_non_numeric_port(self):
        shell = AFMShell(AFMClient("127.0.0.1", 5025))
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            shell.do_connect("127.0.0.1 not-a-port")
        self.assertIn("port must be an integer", buffer.getvalue())


if __name__ == "__main__":
    unittest.main()
