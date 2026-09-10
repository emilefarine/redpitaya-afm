#!/usr/bin/env python3
"""Unit tests for the CLI shell helpers (no server required)."""

import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from afm_cli import AFMShell  # noqa: E402
from afm_client import SpectrumData  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
