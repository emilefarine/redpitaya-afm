#!/usr/bin/env python3
"""
Hardware-in-the-loop check of the measurement chain over SCPI.

Wiring: Red Pitaya OUT1 -> board IN1 and board OUT1 -> Red Pitaya IN1 (the
script routes OUT1 <- IN1 at gain x1), or a cable OUT1 -> IN1 with the server
in --no-board mode. Run it after flashing a bitstream or changing cpp/Hardware/.

Usage:
    python afm_hil_check.py 192.168.1.100 [--amplitude 0.25] [--repeat 20]

Exit code: 0 all checks passed, 1 a check failed, 2 no connection.
"""

import argparse
import sys
from dataclasses import dataclass
from typing import Callable

import numpy as np

from afm_client import (
    AFMClient,
    AFMConnectionError,
    AFMError,
    GainSetting,
    SpectrumData,
)

ADC_CLOCK_HZ = 125e6
MAX_SAMPLES = 65536

DEFAULT_AMPLITUDE = 0.25  # 0.5 V at the ADC even with a loop gain of 2
MAX_AMPLITUDE = 0.5
DEFAULT_REPEAT = 20

DECIMATION = 64
SWEEP_CENTER_KHZ = 100.0
SWEEP_RANGE_KHZ = 100.0
SWEEP_STEP_KHZ = 10.0
# Low enough for decimation 1024, where the FPGA averaging attenuates higher frequencies
DECIMATION_CENTER_KHZ = 5.0
DECIMATIONS = (16, 64, 256, 1024)
SINC_CENTER_KHZ = 100.0
SINC_BANDWIDTH_KHZ = 100.0
SINC_SAMPLES = 8192
INNER_BAND = 0.5  # Fraction of the sinc band used for levels, away from the edges
SINC_BLOCKS = 20

MIN_LOOP_GAIN = 0.2  # Below: no signal, check the cable or the routing
MAX_LOOP_GAIN = 3.0
MAX_FLATNESS = 0.10
MAX_PHASE_RESIDUAL_DEG = 5.0
MAX_DECIMATION_ERROR = 0.05
MAX_SINC_LEVEL_ERROR = 0.20
MAX_SINC_FLATNESS = 0.10
MAX_LINEARITY_ERROR = 0.03
MAX_REPEAT_ERROR = 0.03


@dataclass
class CheckResult:
    name: str
    passed: bool
    detail: str


def loop_gain(spectrum: SpectrumData, amplitude: float) -> float:
    """Mean magnitude per unit of excitation amplitude."""
    return float(np.mean(spectrum.magnitude)) / amplitude


def flatness(values: np.ndarray) -> float:
    """(max - min) / mean"""
    return float((np.max(values) - np.min(values)) / np.mean(values))


def band_flatness(values: np.ndarray) -> float:
    """Flatness of the band averaged in SINC_BLOCKS blocks, so bin noise does not count."""
    blocks = [block.mean() for block in np.array_split(values, min(SINC_BLOCKS, values.size))]
    return flatness(np.array(blocks))


def phase_fit(freq_khz: np.ndarray, phase_rad: np.ndarray) -> tuple[float, float, float]:
    """Linear fit of the unwrapped phase: (delay ns, phase at 0 Hz deg, rms residual deg)."""
    phase_deg = np.degrees(np.unwrap(phase_rad))
    slope, intercept = np.polyfit(freq_khz, phase_deg, 1)
    residual = phase_deg - (slope * freq_khz + intercept)
    delay_ns = -slope / 360.0 * 1e6
    intercept = (intercept + 180.0) % 360.0 - 180.0
    return float(delay_ns), float(intercept), float(np.sqrt(np.mean(residual ** 2)))


def inner_band(spectrum: SpectrumData, center_khz: float, bandwidth_khz: float) -> np.ndarray:
    """Magnitudes in the middle INNER_BAND of the sinc band."""
    half = bandwidth_khz * INNER_BAND / 2.0
    mask = np.abs(spectrum.freq_kHz - center_khz) <= half
    return spectrum.magnitude[mask]


def expected_sinc_level(amplitude: float, gain: float, decimation: int,
                        bandwidth_khz: float, num_samples: int) -> float:
    """In-band FFT magnitude of the sinc excitation (rectangle of height A / 2B, scaled 1/N)."""
    fs = ADC_CLOCK_HZ / decimation
    return amplitude * gain * fs / (2.0 * bandwidth_khz * 1e3 * num_samples)


def _all_finite(spectrum: SpectrumData) -> bool:
    return bool(np.all(np.isfinite(spectrum.magnitude)) and np.all(np.isfinite(spectrum.phase_rad)))


class LoopbackCheck:
    """Runs the checks against a connected AFMClient."""

    def __init__(self, client: AFMClient, amplitude: float = DEFAULT_AMPLITUDE,
                 repeat: int = DEFAULT_REPEAT, log: Callable[[str], None] = print):
        self.client = client
        self.amplitude = amplitude
        self.repeat = repeat
        self.log = log
        self.gain = None  # Loop gain from the sweep, used by the sinc level check

    def run(self) -> list[CheckResult]:
        checks = [
            ("setup", self._check_setup),
            ("sweep", self._check_sweep),
            ("decimation", self._check_decimation),
            ("sinc level", self._check_sinc_level),
            ("max depth", self._check_max_depth),
            ("amplitude linearity", self._check_linearity),
            ("repeatability", self._check_repeatability),
        ]
        results = []
        for name, check in checks:
            try:
                problems, detail = check()
            except AFMError as exc:
                problems, detail = [f"{type(exc).__name__}: {exc}"], ""
            text = "; ".join(filter(None, [detail] + problems))
            results.append(CheckResult(name, not problems, text))
            self.log(f"{'PASS' if not problems else 'FAIL'}  {name:<20} {text}")
        return results

    def _measure(self, measure: Callable[[], SpectrumData],
                 problems: list[str]) -> SpectrumData:
        spectrum = measure()
        if spectrum.num_points == 0 or not _all_finite(spectrum):
            problems.append("empty or non-finite spectrum")
        if self.client.get_status().adc_saturated:
            problems.append("ADC saturated (SAT=1)")
        return spectrum

    def _sinc(self, num_samples: int, amplitude: float, problems: list[str]) -> SpectrumData:
        return self._measure(lambda: self.client.measure_sinc(
            SINC_CENTER_KHZ, SINC_BANDWIDTH_KHZ, num_samples, DECIMATION, amplitude), problems)

    def _check_setup(self):
        problems = []
        status = self.client.get_status()
        if not status.hardware_initialized:
            self.client.init()
            status = self.client.get_status()
            if not status.hardware_initialized:
                return ["SYSTEM:INIT did not initialize the hardware"], ""
        if status.board_connected:
            self.client.set_mux(1, 1)
            self.client.set_gain(1, GainSetting.GAIN_1)
            self.client.set_adc_loop_auto()
            self.client.set_adc_sample_gain(1.0)
            status = self.client.get_status()
            path = "board OUT1 <- IN1 at x1"
            if status.adc_loop != "1,1":
                problems.append(f"loop is {status.adc_loop}, expected 1,1")
            if status.adc_overdrive:
                problems.append("overdrive predicted")
        else:
            path = "direct cable OUT1 -> IN1"
        return problems, f"{self.client.idn()}, {status.mode.value}, {path}"

    def _check_sweep(self):
        problems = []
        spectrum = self._measure(lambda: self.client.measure_sweep(
            SWEEP_CENTER_KHZ, SWEEP_RANGE_KHZ, SWEEP_STEP_KHZ, DECIMATION, self.amplitude),
            problems)
        start = SWEEP_CENTER_KHZ - SWEEP_RANGE_KHZ / 2
        expected = start + SWEEP_STEP_KHZ * np.arange(round(SWEEP_RANGE_KHZ / SWEEP_STEP_KHZ) + 1)
        if spectrum.num_points != expected.size or not np.allclose(spectrum.freq_kHz, expected):
            return problems + [f"{spectrum.num_points} points, expected {expected.size} "
                               f"from {expected[0]:g} to {expected[-1]:g} kHz"], ""
        if problems:
            return problems, ""

        gain = loop_gain(spectrum, self.amplitude)
        flat = flatness(spectrum.magnitude)
        delay_ns, intercept_deg, residual_deg = phase_fit(spectrum.freq_kHz, spectrum.phase_rad)
        if gain < MIN_LOOP_GAIN:
            problems.append(f"no signal (gain below {MIN_LOOP_GAIN}): check the cable "
                            "and the routing")
        elif gain > MAX_LOOP_GAIN:
            problems.append(f"gain above {MAX_LOOP_GAIN}")
        else:
            self.gain = gain
        if flat > MAX_FLATNESS:
            problems.append(f"magnitude not flat (limit {MAX_FLATNESS:.0%})")
        if residual_deg > MAX_PHASE_RESIDUAL_DEG:
            problems.append(f"phase not linear (limit {MAX_PHASE_RESIDUAL_DEG:g} deg)")
        detail = (f"gain {gain:.3f}, flatness {flat:.1%}, phase residual {residual_deg:.1f} deg, "
                  f"delay {delay_ns:.0f} ns, phase at 0 Hz {intercept_deg:.0f} deg")
        return problems, detail

    def _check_decimation(self):
        problems = []
        gains = {}
        for decimation in DECIMATIONS:
            spectrum = self._measure(lambda: self.client.measure_sweep(
                DECIMATION_CENTER_KHZ, 2.0, 1.0, decimation, self.amplitude), problems)
            gains[decimation] = loop_gain(spectrum, self.amplitude)
        if problems:
            return problems, ""
        reference = gains[DECIMATION]
        if reference < MIN_LOOP_GAIN:
            return [f"no signal at decimation {DECIMATION}"], ""
        for decimation, gain in gains.items():
            if abs(gain / reference - 1.0) > MAX_DECIMATION_ERROR:
                problems.append(f"decimation {decimation} differs by "
                                f"{gain / reference - 1.0:+.1%} (limit {MAX_DECIMATION_ERROR:.0%})")
        detail = "gain " + ", ".join(f"{g:.3f} (dec {d})" for d, g in gains.items())
        return problems, detail

    def _check_sinc_band(self, num_samples: int):
        problems = []
        spectrum = self._sinc(num_samples, self.amplitude, problems)
        if problems:
            return problems, ""
        inner = inner_band(spectrum, SINC_CENTER_KHZ, SINC_BANDWIDTH_KHZ)
        if inner.size == 0:
            return [f"no bin within the inner band ({spectrum.num_points} points)"], ""
        level = float(np.median(inner))
        flat = band_flatness(inner)
        detail = f"{spectrum.num_points} points, level {level:.3e}, flatness {flat:.1%}"
        if flat > MAX_SINC_FLATNESS:
            problems.append(f"band not flat (limit {MAX_SINC_FLATNESS:.0%})")
        if self.gain is None:
            problems.append("no loop gain from the sweep to compare with")
            return problems, detail
        expected = expected_sinc_level(self.amplitude, self.gain, DECIMATION,
                                       SINC_BANDWIDTH_KHZ, num_samples)
        error = level / expected - 1.0
        detail += f", {error:+.1%} from the sweep gain"
        if abs(error) > MAX_SINC_LEVEL_ERROR:
            problems.append(f"level off by more than {MAX_SINC_LEVEL_ERROR:.0%}")
        return problems, detail

    def _check_sinc_level(self):
        return self._check_sinc_band(SINC_SAMPLES)

    def _check_max_depth(self):
        return self._check_sinc_band(MAX_SAMPLES)

    def _check_linearity(self):
        problems = []
        full = self._sinc(SINC_SAMPLES, self.amplitude, problems)
        half = self._sinc(SINC_SAMPLES, self.amplitude / 2.0, problems)
        if problems:
            return problems, ""
        ratio = float(np.median(inner_band(half, SINC_CENTER_KHZ, SINC_BANDWIDTH_KHZ)) /
                      np.median(inner_band(full, SINC_CENTER_KHZ, SINC_BANDWIDTH_KHZ)))
        if abs(ratio / 0.5 - 1.0) > MAX_LINEARITY_ERROR:
            problems.append("level does not follow the amplitude (stale or clipped data?)")
        return problems, f"half amplitude gives {ratio:.3f} of the level (expected 0.5)"

    def _check_repeatability(self):
        problems = []
        sizes = (SINC_SAMPLES, MAX_SAMPLES)
        references = {}
        worst_level = 0.0
        worst_phase = 0.0
        for run in range(self.repeat):
            num_samples = sizes[run % len(sizes)]
            run_problems = []
            spectrum = self._sinc(num_samples, self.amplitude, run_problems)
            if run_problems:
                problems += [f"run {run + 1}: {p}" for p in run_problems]
                continue
            mask = np.abs(spectrum.freq_kHz - SINC_CENTER_KHZ) <= SINC_BANDWIDTH_KHZ * INNER_BAND / 2
            if num_samples not in references:
                references[num_samples] = (spectrum.magnitude[mask], spectrum.phase_rad[mask])
                continue
            ref_mag, ref_phase = references[num_samples]
            if ref_mag.size != np.count_nonzero(mask):
                problems.append(f"run {run + 1}: bin count changed")
                continue
            level = abs(float(np.median(spectrum.magnitude[mask]) / np.median(ref_mag)) - 1.0)
            phase = np.degrees(np.angle(np.exp(1j * (spectrum.phase_rad[mask] - ref_phase))))
            worst_level = max(worst_level, level)
            worst_phase = max(worst_phase, float(np.median(np.abs(phase))))
        if worst_level > MAX_REPEAT_ERROR:
            problems.append(f"level drift above {MAX_REPEAT_ERROR:.0%}")
        detail = (f"{self.repeat} runs, level drift {worst_level:.2%}, "
                  f"phase drift {worst_phase:.1f} deg")
        return problems, detail


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Hardware-in-the-loop check: Red Pitaya OUT1 looped back to IN1, "
                    "through the board (OUT1 <- IN1 at x1) or a direct cable.")
    parser.add_argument('host', help='Red Pitaya IP address or hostname')
    parser.add_argument('-p', '--port', type=int, default=AFMClient.DEFAULT_PORT)
    parser.add_argument('-a', '--amplitude', type=float, default=DEFAULT_AMPLITUDE,
                        help=f'excitation amplitude, 0 to {MAX_AMPLITUDE} '
                             f'(default {DEFAULT_AMPLITUDE})')
    parser.add_argument('-r', '--repeat', type=int, default=DEFAULT_REPEAT,
                        help=f'runs of the repeatability check (default {DEFAULT_REPEAT})')
    args = parser.parse_args(argv)
    if not 0.0 < args.amplitude <= MAX_AMPLITUDE:
        parser.error(f"--amplitude must be in (0, {MAX_AMPLITUDE}]")
    if args.repeat < 4:
        parser.error("--repeat must be at least 4")

    client = AFMClient(args.host, args.port)
    try:
        client.connect()
    except AFMConnectionError as exc:
        print(f"Cannot connect: {exc}", file=sys.stderr)
        return 2
    try:
        results = LoopbackCheck(client, args.amplitude, args.repeat).run()
    finally:
        client.disconnect()

    passed = sum(result.passed for result in results)
    print(f"\n{passed}/{len(results)} checks passed")
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
