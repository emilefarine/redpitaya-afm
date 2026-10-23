#!/usr/bin/env python3
"""Unit tests for the hardware-in-the-loop check, against a simulated loop."""

import io
import re
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import afm_hil_check as hil  # noqa: E402
from afm_client import (  # noqa: E402
    AFMClient,
    AFMCommandError,
    AFMConnectionError,
    GainSetting,
    OperatingMode,
    SpectrumData,
    SystemStatus,
)


class FakeLoop:
    """Server plus OUT1 -> IN1 loop, with the server's sweep and sinc processing."""

    def __init__(self, gain=1.9, delay_s=600e-9, board=True, initialized=True,
                 noise_v=0.5e-3, fault=None):
        self.gain = gain
        self.delay_s = delay_s
        self.board = board
        self.initialized = initialized
        self.noise_v = noise_v
        self.fault = fault
        self.calls = []
        self.loop = "1,X"
        self.saturated = False
        self.first_sinc = {}
        self.rng = np.random.default_rng(0)

    def idn(self):
        return "AFM,Server,2.7.0"

    def init(self):
        self.calls.append("init")
        self.initialized = True
        return "Red Pitaya OK"

    def get_status(self):
        return SystemStatus(
            hardware_initialized=self.initialized, board_connected=self.board,
            mode=OperatingMode.FULL if self.board else OperatingMode.RP_ONLY,
            adc_loop=self.loop if self.board else None, adc_saturated=self.saturated)

    def set_mux(self, output, input_ch):
        self.calls.append(("mux", output, input_ch))
        if output == 1:
            self.loop = f"1,{input_ch}"

    def set_gain(self, channel, gain):
        self.calls.append(("gain", channel, gain))
        return "1"

    def set_adc_loop_auto(self):
        self.calls.append("loop auto")
        return "OK"

    def set_adc_sample_gain(self, gain):
        self.calls.append(("sample gain", gain))
        return "OK"

    def _loop_gain(self, decimation, amplitude):
        gain = self.gain
        if self.fault == "decimation" and decimation == 1024:
            gain *= 0.8
        if self.fault == "unstable":
            gain *= 1.0 + 0.1 * self.rng.standard_normal()
        return gain

    def _noise(self, n):
        return self.noise_v * self.rng.standard_normal(n)

    def measure_sweep(self, center_kHz, range_kHz, step_kHz, decimation, amplitude):
        self.saturated = self.fault == "saturated"
        fs = AFMClient.ADC_SAMPLE_RATE_HZ / decimation
        start, stop = center_kHz - range_kHz / 2, center_kHz + range_kHz / 2
        count = int(np.floor((stop - start) / step_kHz * (1 + 1e-6))) + 1
        n = np.arange(8192)
        rows = []
        for point in range(count):
            freq_khz = min(start + point * step_kHz, stop)
            omega = 2 * np.pi * freq_khz * 1e3 / fs
            lag = 2 * np.pi * freq_khz * 1e3 * self.delay_s
            if self.fault == "phase" and freq_khz > center_kHz:
                lag += np.radians(30)
            acquired = (self._loop_gain(decimation, amplitude) * amplitude *
                        np.sin(omega * n - lag) + self._noise(n.size))
            s = 2 / n.size * np.sum(acquired * np.sin(omega * n))
            c = 2 / n.size * np.sum(acquired * np.cos(omega * n))
            rows.append((freq_khz, np.hypot(s, c), np.arctan2(c, s)))
        return self._spectrum(rows)

    def measure_sinc(self, center_kHz, bandwidth_kHz, num_samples, decimation, amplitude):
        self.saturated = self.fault == "saturated"
        if self.fault == "stale" and num_samples in self.first_sinc:
            return self.first_sinc[num_samples]
        fs = AFMClient.ADC_SAMPLE_RATE_HZ / decimation
        t = (np.arange(num_samples) - num_samples // 2) / fs - self.delay_s
        bw, fc = bandwidth_kHz * 1e3, center_kHz * 1e3
        acquired = (self._loop_gain(decimation, amplitude) * amplitude *
                    np.sinc(bw * t) * np.cos(2 * np.pi * fc * t) + self._noise(num_samples))
        acquired *= np.hanning(num_samples)
        spectrum = np.fft.rfft(acquired) / num_samples
        freq = np.arange(spectrum.size) * fs / num_samples
        band = (freq >= fc - bw / 2) & (freq <= fc + bw / 2)
        result = self._spectrum(zip(freq[band] / 1e3, np.abs(spectrum[band]),
                                    np.angle(spectrum[band])))
        self.first_sinc.setdefault(num_samples, result)
        return result

    @staticmethod
    def _spectrum(rows):
        freq, mag, phase = (np.array(col, dtype=float) for col in zip(*rows))
        return SpectrumData(freq_kHz=freq, magnitude=mag, phase_rad=phase)


def run_checks(loop, repeat=4):
    results = hil.LoopbackCheck(loop, repeat=repeat, log=lambda _: None).run()
    return {result.name: result for result in results}


class TestHealthyLoop(unittest.TestCase):
    def test_every_check_passes_through_the_board(self):
        loop = FakeLoop()
        results = run_checks(loop)
        for result in results.values():
            self.assertTrue(result.passed, f"{result.name}: {result.detail}")
        self.assertIn(("mux", 1, 1), loop.calls)
        self.assertIn(("gain", 1, GainSetting.GAIN_1), loop.calls)

    def test_every_check_passes_with_a_direct_cable(self):
        loop = FakeLoop(board=False)
        results = run_checks(loop)
        self.assertTrue(all(result.passed for result in results.values()))
        self.assertIn("direct cable", results["setup"].detail)
        self.assertFalse(any(isinstance(call, tuple) for call in loop.calls))

    def test_uninitialized_server_is_initialized(self):
        loop = FakeLoop(initialized=False)
        self.assertTrue(run_checks(loop)["setup"].passed)
        self.assertIn("init", loop.calls)

    def test_sweep_reports_the_loop_gain_and_delay(self):
        detail = run_checks(FakeLoop(gain=1.5, delay_s=800e-9))["sweep"].detail
        self.assertIn("gain 1.500", detail)
        delay_ns = float(re.search(r"delay (-?\d+) ns", detail).group(1))
        self.assertAlmostEqual(delay_ns, 800, delta=10)


class TestFaults(unittest.TestCase):
    def assert_fails(self, loop, *names):
        results = run_checks(loop)
        failed = {name for name, result in results.items() if not result.passed}
        self.assertEqual(failed, set(names), {n: r.detail for n, r in results.items()})

    def test_missing_signal(self):
        results = run_checks(FakeLoop(gain=0.001))
        self.assertFalse(results["sweep"].passed)
        self.assertIn("no signal", results["sweep"].detail)
        self.assertFalse(results["decimation"].passed)

    def test_wrong_routing_on_the_board(self):
        loop = FakeLoop()
        loop.set_mux = lambda output, input_ch: None
        self.assertFalse(run_checks(loop)["setup"].passed)

    def test_wrong_scaling_at_one_decimation(self):
        self.assert_fails(FakeLoop(fault="decimation"), "decimation")

    def test_phase_step(self):
        self.assert_fails(FakeLoop(fault="phase"), "sweep")

    def test_stale_acquisition(self):
        self.assert_fails(FakeLoop(fault="stale"), "amplitude linearity")

    def test_unstable_level(self):
        results = run_checks(FakeLoop(fault="unstable"), repeat=8)
        self.assertFalse(results["repeatability"].passed)

    def test_saturation_fails_every_measurement(self):
        self.assert_fails(FakeLoop(fault="saturated"), "sweep", "decimation", "sinc level",
                          "max depth", "amplitude linearity", "repeatability")

    def test_server_error_is_a_failure_not_a_crash(self):
        loop = FakeLoop()
        loop.measure_sinc = mock.Mock(side_effect=AFMCommandError("ERR_HARDWARE: timeout"))
        results = run_checks(loop)
        self.assertFalse(results["max depth"].passed)
        self.assertIn("ERR_HARDWARE", results["max depth"].detail)
        self.assertTrue(results["sweep"].passed)


class TestHelpers(unittest.TestCase):
    def test_phase_fit_recovers_delay_and_offset(self):
        freq = np.linspace(50, 150, 11)
        phase = np.angle(np.exp(1j * (-2 * np.pi * freq * 1e3 * 2e-6 + np.radians(10))))
        delay_ns, intercept, residual = hil.phase_fit(freq, phase)
        self.assertAlmostEqual(delay_ns, 2000, places=3)
        self.assertAlmostEqual(intercept, 10, places=3)
        self.assertLess(residual, 1e-9)

    def test_expected_sinc_level(self):
        # 0.25 * 1953125 / (2 * 100e3 * 8192)
        self.assertAlmostEqual(hil.expected_sinc_level(0.25, 1.0, 64, 100.0, 8192),
                               2.98e-4, places=6)


class TestMain(unittest.TestCase):
    def test_connection_failure_exits_with_2(self):
        with mock.patch.object(hil, "AFMClient") as client_cls, \
                redirect_stderr(io.StringIO()):
            client_cls.return_value.connect.side_effect = AFMConnectionError("refused")
            self.assertEqual(hil.main(["192.0.2.1"]), 2)

    def test_exit_code_follows_the_results(self):
        for loop, code in ((FakeLoop(), 0), (FakeLoop(gain=0.001), 1)):
            with mock.patch.object(hil, "AFMClient") as client_cls, \
                    redirect_stdout(io.StringIO()):
                client_cls.return_value = loop
                loop.connect = mock.Mock()
                loop.disconnect = mock.Mock()
                self.assertEqual(hil.main(["192.0.2.1", "--repeat", "4"]), code)
                loop.disconnect.assert_called_once()

    def test_amplitude_is_limited(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            hil.main(["192.0.2.1", "--amplitude", "0.8"])


if __name__ == "__main__":
    unittest.main()
