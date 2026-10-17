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


class AdcProtectionTests(unittest.TestCase):
    STATUS = ("OK HW_INIT=1 BOARD=1 BUSY=0 DEC=64 MODE=FULL LOOP=1,3 "
              "OVERDRIVE=0 SAT=0 PROTECT=1\n")

    def make_shell(self, respond):
        server = FakeSCPIServer(respond).start()
        self.addCleanup(server.stop)
        client = AFMClient("127.0.0.1", server.port, timeout=2.0)
        client.connect()
        self.addCleanup(client.disconnect)
        return AFMShell(client)

    def run_command(self, shell, method, arg=""):
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            getattr(shell, method)(arg)
        return buffer.getvalue()

    def test_adc_protect_query_and_override(self):
        commands = []

        def respond(command, handler):
            commands.append(command)
            if command.startswith("SYSTEM:STATUS"):
                return self.STATUS
            if command == "BOARD:ADC:PROTECT?":
                return "OK ON\n"
            return "OK OFF\n"

        shell = self.make_shell(respond)
        self.assertIn("ADC Protection: ON", self.run_command(shell, "do_adc_protect"))

        output = self.run_command(shell, "do_adc_protect", "off")
        self.assertIn("BOARD:ADC:PROTECT OFF", commands)
        self.assertIn("no longer protected", output)

        self.assertIn("Usage", self.run_command(shell, "do_adc_protect", "maybe"))

    def test_status_shows_protection_state(self):
        shell = self.make_shell(lambda command, handler: self.STATUS)
        output = self.run_command(shell, "do_status")
        self.assertIn("ADC Loop (exc,ret):   1,3", output)
        self.assertIn("ADC Protection:       ON", output)

    def test_adc_loop_auto(self):
        commands = []

        def respond(command, handler):
            commands.append(command)
            return self.STATUS if command.startswith("SYSTEM:STATUS") else "OK AUTO\n"

        shell = self.make_shell(respond)
        self.assertIn("OK: AUTO", self.run_command(shell, "do_adc_loop", "auto"))
        self.assertIn("BOARD:ADC:LOOP AUTO", commands)

    def test_refused_measurement_prints_override_hint(self):
        refusal = ("ERR_PARAM: ADC overdrive: expected ADC peak ~2.00 V exceeds 1.00 V "
                   "full scale; suggested: BOARD:GAIN 3,2; BOARD:ADC:PROTECT OFF overrides\n")
        shell = self.make_shell(lambda command, handler: refusal)

        for method, arg in (("do_sinc", "200 100"), ("do_sweep", "200 10")):
            output = self.run_command(shell, method, arg)
            self.assertIn("ADC overdrive", output)
            self.assertIn("adc_protect off", output)

    def test_adc_sample_gain_query_set_and_invalid(self):
        commands = []

        def respond(command, handler):
            commands.append(command)
            if command.startswith("SYSTEM:STATUS"):
                return self.STATUS
            return "OK 0.01\n"

        shell = self.make_shell(respond)
        self.assertIn("ADC Sample Gain: 0.01", self.run_command(shell, "do_adc_sample_gain"))
        self.assertIn("OK: 0.01", self.run_command(shell, "do_adc_sample_gain", "0.01"))
        self.assertIn("BOARD:ADC:SAMPLEGAIN 0.01", commands)
        self.assertIn("Usage", self.run_command(shell, "do_adc_sample_gain", "weak"))


class ConnectionHintTests(unittest.TestCase):
    def run_line(self, shell, line):
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            shell.postcmd(shell.onecmd(line), line)
        return buffer.getvalue()

    def test_lost_connection_points_to_connect(self):
        server = FakeSCPIServer(lambda command, handler: b"").start()
        self.addCleanup(server.stop)
        client = AFMClient("127.0.0.1", server.port, timeout=0.2)
        client.connect()
        shell = AFMShell(client)
        output = self.run_line(shell, "ping")
        self.assertIn("Read timeout", output)
        self.assertIn("Use 'connect", output)

    def test_local_commands_print_no_hint(self):
        shell = AFMShell(AFMClient("127.0.0.1", 1))
        for line in ("help", "data_show", "quit"):
            with self.subTest(line=line):
                self.assertNotIn("Use 'connect", self.run_line(shell, line))


class BoardCommandTests(unittest.TestCase):
    def make_shell(self, respond):
        server = FakeSCPIServer(respond).start()
        self.addCleanup(server.stop)
        client = AFMClient("127.0.0.1", server.port, timeout=2.0)
        client.connect()
        self.addCleanup(client.disconnect)
        return AFMShell(client)

    def test_board_command_sends_no_status_query_first(self):
        commands = []

        def respond(command, handler):
            commands.append(command)
            return "OK\n"

        shell = self.make_shell(respond)
        with redirect_stdout(io.StringIO()):
            shell.do_mux("1 2")
        self.assertEqual(commands, ["BOARD:MUX:ROUTE 1,2"])

    def test_board_error_is_shown(self):
        shell = self.make_shell(
            lambda command, handler: "ERR_HARDWARE: Electronic board disabled (RP-only mode)\n")
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            shell.do_gain("1 3")
        self.assertIn("RP-only mode", buffer.getvalue())

    def test_raw_board_status_prints_the_block(self):
        shell = self.make_shell(
            lambda command, handler: "OK LINES 2\nRouting:\n  OUT1 <- IN1\n")
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            shell.default("BOARD:STATUS?")
        self.assertIn("OUT1 <- IN1", buffer.getvalue())


class HelpTests(unittest.TestCase):
    def test_gain_indices_are_listed_from_the_client_labels(self):
        shell = AFMShell(AFMClient("127.0.0.1", 1))
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            shell.do_help("")
            shell.do_gain("")
        self.assertEqual(buffer.getvalue().count("0=1/8, 1=1/4, 2=1/2, 3=1"), 2)


if __name__ == "__main__":
    unittest.main()
