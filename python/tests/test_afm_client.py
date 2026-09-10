#!/usr/bin/env python3
"""Unit tests for the AFM SCPI client library (no hardware required)."""

import socketserver
import sys
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from afm_client import (  # noqa: E402
    AFMBusyError,
    AFMClient,
    AFMCommandError,
    AFMConnectionError,
    AFMProtocolError,
    AFMTimeoutError,
    BoardStatus,
    OperatingMode,
    SystemStatus,
)


def spectrum_payload(rows):
    """Build a CSV payload in the same format as buildSpectrumResponse."""
    return "".join(
        f"{freq:.3f},{mag:.6e},{phase:.6f}\n" for freq, mag, phase in rows
    )


def spectrum_response(rows, with_bytes=True):
    payload = spectrum_payload(rows)
    if with_bytes:
        return f"OK DATA {len(rows)} {len(payload)}\n" + payload
    return f"OK DATA {len(rows)}\n" + payload


class _FakeHandler(socketserver.StreamRequestHandler):
    def handle(self):
        server = self.server
        try:
            if server.welcome is not None:
                self._write(server.welcome.encode("ascii"))
            while True:
                line = self.rfile.readline()
                if not line:
                    break
                command = line.decode("ascii").strip()
                response = server.respond(command, self)
                if response is None:
                    break
                if isinstance(response, str):
                    response = response.encode("ascii")
                self._write(response)
        except OSError:
            pass
        except Exception as exc:  # keep the test failure visible
            server.error = exc

    def _write(self, data):
        chunk = self.server.chunk_size
        if chunk > 0:
            for start in range(0, len(data), chunk):
                self.wfile.write(data[start:start + chunk])
                self.wfile.flush()
        else:
            self.wfile.write(data)
            self.wfile.flush()


class FakeSCPIServer(socketserver.ThreadingTCPServer):
    """Minimal SCPI server that scripts responses per command."""

    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, respond, welcome="AFM SCPI Server v2.3.0 Ready\n", chunk_size=0):
        super().__init__(("127.0.0.1", 0), _FakeHandler)
        self.respond = respond
        self.welcome = welcome
        self.chunk_size = chunk_size
        self.error = None
        self._thread = threading.Thread(target=self.serve_forever, daemon=True)

    @property
    def port(self):
        return self.server_address[1]

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self.shutdown()
        self.server_close()


class ClientTestCase(unittest.TestCase):
    def setUp(self):
        self.servers = []

    def tearDown(self):
        for server in self.servers:
            server.stop()

    def make_server(self, respond, **kwargs):
        server = FakeSCPIServer(respond, **kwargs)
        self.servers.append(server)
        return server.start()

    def make_client(self, server, timeout=2.0):
        client = AFMClient("127.0.0.1", server.port, timeout=timeout)
        client.connect()
        self.addCleanup(client.disconnect)
        return client


class CommunicationTests(ClientTestCase):
    def test_ping_and_idn(self):
        def respond(command, handler):
            if command == "SYSTEM:PING":
                return "OK PONG\n"
            if command == "*IDN?":
                return "OK RedPitaya,AFM_SERVER,2.3.0\n"
            return "ERR_SYNTAX: unknown\n"

        server = self.make_server(respond)
        client = self.make_client(server)
        self.assertTrue(client.ping())
        self.assertEqual(client.idn(), "RedPitaya,AFM_SERVER,2.3.0")

    def test_error_status_and_message(self):
        server = self.make_server(
            lambda command, handler: "ERR_HARDWARE: Board not connected\n")
        client = self.make_client(server)
        with self.assertRaises(AFMCommandError) as ctx:
            client.send_command("SYSTEM:INIT")
        self.assertEqual(ctx.exception.status, "ERR_HARDWARE")
        self.assertEqual(ctx.exception.message, "Board not connected")

    def test_okay_is_not_treated_as_success(self):
        server = self.make_server(lambda command, handler: "OKAY\n")
        client = self.make_client(server)
        with self.assertRaises(AFMCommandError):
            client.send_command("SYSTEM:PING")

    def test_empty_ok_data_is_allowed(self):
        server = self.make_server(lambda command, handler: "OK\n")
        client = self.make_client(server)
        self.assertEqual(client.send_command("SYSTEM:INIT"), "")

    def test_read_timeout_raises(self):
        server = self.make_server(lambda command, handler: b"")
        client = self.make_client(server, timeout=0.2)
        with self.assertRaises(AFMTimeoutError):
            client.send_command("SYSTEM:PING")

    def test_non_ascii_command_rejected(self):
        server = self.make_server(lambda command, handler: "OK PONG\n")
        client = self.make_client(server)
        with self.assertRaises(AFMCommandError):
            client.send_command("SYSTEM:PING\u00e9")

    def test_multiline_command_rejected(self):
        server = self.make_server(lambda command, handler: "OK PONG\n")
        client = self.make_client(server)
        with self.assertRaises(AFMCommandError):
            client.send_command("SYSTEM:PING\nSYSTEM:INIT")

    def test_line_limit_disconnects(self):
        server = self.make_server(
            lambda command, handler: b"A" * 8192)
        client = self.make_client(server)
        client.MAX_LINE_BYTES = 4096
        with self.assertRaises(AFMProtocolError):
            client.send_command("SYSTEM:PING")
        self.assertFalse(client.is_connected)

    def test_welcome_timeout_cleans_up_socket(self):
        server = self.make_server(lambda command, handler: "OK PONG\n", welcome=None)
        client = AFMClient("127.0.0.1", server.port, timeout=0.2)
        with self.assertRaises(AFMConnectionError):
            client.connect()
        self.assertFalse(client.is_connected)

    def test_busy_raises_while_measurement_runs(self):
        started = threading.Event()
        release = threading.Event()
        rows = [(1.0, 1.0, 0.0)]

        def respond(command, handler):
            if command.startswith("MEASURE:SINC"):
                started.set()
                release.wait(5.0)
                return spectrum_response(rows)
            return "OK PONG\n"

        server = self.make_server(respond)
        client = self.make_client(server)
        result = {}

        def run_measurement():
            result["spectrum"] = client.measure_sinc(1.0, 1.0)

        worker = threading.Thread(target=run_measurement)
        worker.start()
        self.assertTrue(started.wait(2.0))
        with self.assertRaises(AFMBusyError):
            client.send_command("SYSTEM:PING")
        release.set()
        worker.join(5.0)
        self.assertFalse(worker.is_alive())
        self.assertEqual(result["spectrum"].num_points, 1)

    def test_closed_socket_translated_to_connection_error(self):
        server = self.make_server(lambda command, handler: "OK PONG\n")
        client = self.make_client(server)
        client._socket.close()
        with self.assertRaises(AFMConnectionError):
            client.send_command("SYSTEM:PING")

    def test_shutdown_busy_keeps_connection(self):
        server = self.make_server(lambda command, handler: "OK Shutting down\n")
        client = self.make_client(server)
        self.assertTrue(client._lock.acquire(blocking=False))
        try:
            with self.assertRaises(AFMBusyError):
                client.shutdown()
            self.assertTrue(client.is_connected)
        finally:
            client._lock.release()


class SpectrumTests(ClientTestCase):
    def test_fast_path_parses_payload(self):
        rows = [(1.0, 0.5, 0.25), (2.0, 0.75, -0.5)]
        server = self.make_server(lambda command, handler: spectrum_response(rows))
        client = self.make_client(server)
        spectrum = client.measure_sinc(1.5, 1.0)
        self.assertEqual(spectrum.num_points, 2)
        self.assertAlmostEqual(spectrum.freq_kHz[0], 1.0, places=3)
        self.assertAlmostEqual(spectrum.magnitude[1], 0.75, places=6)
        self.assertAlmostEqual(spectrum.phase_rad[1], -0.5, places=6)

    def test_legacy_servers_omit_byte_count(self):
        rows = [(3.0, 1.0, 0.0), (4.0, 0.5, 1.0)]
        server = self.make_server(
            lambda command, handler: spectrum_response(rows, with_bytes=False))
        client = self.make_client(server)
        spectrum = client.measure_sweep(3.5, 1.0)
        self.assertEqual(spectrum.num_points, 2)
        self.assertAlmostEqual(spectrum.freq_kHz[1], 4.0, places=3)

    def test_fragmented_payload(self):
        rows = [(5.0, 2.0, -1.0)]
        server = self.make_server(
            lambda command, handler: spectrum_response(rows), chunk_size=1)
        client = self.make_client(server)
        spectrum = client.measure_sinc(5.0, 1.0)
        self.assertEqual(spectrum.num_points, 1)
        self.assertAlmostEqual(spectrum.magnitude[0], 2.0, places=6)

    def test_malformed_row_count(self):
        server = self.make_server(
            lambda command, handler: "OK DATA abc 12\n")
        client = self.make_client(server)
        with self.assertRaises(AFMProtocolError):
            client.measure_sinc(1.0, 1.0)

    def test_row_count_out_of_range(self):
        server = self.make_server(
            lambda command, handler: "OK DATA 100000000 12\n")
        client = self.make_client(server)
        with self.assertRaises(AFMProtocolError):
            client.measure_sinc(1.0, 1.0)

    def test_byte_count_out_of_range(self):
        server = self.make_server(
            lambda command, handler: "OK DATA 1 999999999999\n")
        client = self.make_client(server)
        with self.assertRaises(AFMProtocolError):
            client.measure_sinc(1.0, 1.0)

    def test_malformed_row_values(self):
        payload = "x,y,z\n"
        server = self.make_server(
            lambda command, handler: f"OK DATA 1 {len(payload)}\n" + payload)
        client = self.make_client(server)
        with self.assertRaises(AFMProtocolError):
            client.measure_sinc(1.0, 1.0)

    def test_payload_row_count_mismatch(self):
        payload = "1.000,1.000000e+00,0.000000\n"
        server = self.make_server(
            lambda command, handler: f"OK DATA 2 {len(payload)}\n" + payload)
        client = self.make_client(server)
        with self.assertRaises(AFMProtocolError):
            client.measure_sinc(1.0, 1.0)

    def test_raw_measure_command_drains_stream(self):
        rows = [(1.0, 0.5, 0.25)]

        def respond(command, handler):
            if command.startswith("MEASURE:SINC"):
                return spectrum_response(rows)
            return "OK PONG\n"

        server = self.make_server(respond)
        client = self.make_client(server)
        with self.assertRaises(AFMProtocolError):
            client.send_command("MEASURE:SINC 1,1")
        self.assertTrue(client.ping())

    def test_empty_spectrum_is_accepted(self):
        for response in ("OK DATA 0 0\n", "OK DATA 0\n"):
            with self.subTest(response=response):
                server = self.make_server(
                    lambda command, handler, r=response: r)
                client = self.make_client(server)
                spectrum = client.measure_sinc(1.0, 1.0)
                self.assertEqual(spectrum.num_points, 0)


class BoardStatusTests(ClientTestCase):
    BLOCK = (
        "OK STATUS\n"
        "=== MUX Status ===\n"
        "Routing:\n"
        "  OUT1 <- IN1\n"
        "  OUT2 <- X (disconnected)\n"
        "Gains:\n"
        "  IN1: x1\n"
        "  IN2: x4\n"
        "==================\n"
    )

    def test_board_status_block(self):
        server = self.make_server(lambda command, handler: self.BLOCK)
        client = self.make_client(server)
        text = client.get_board_status()
        self.assertIn("=== MUX Status ===", text)
        self.assertNotIn("STATUS\n", text)
        state = client.get_board_state()
        self.assertEqual(state.routing[1], 1)
        self.assertIsNone(state.routing[2])
        self.assertEqual(state.gains[1], "1")
        self.assertEqual(state.gains[2], "4")

    def test_standalone_parser(self):
        state = BoardStatus.from_response(
            "Routing:\n  OUT3 <- IN4\nGains:\n  IN4: x8")
        self.assertEqual(state.routing[3], 4)
        self.assertEqual(state.gains[4], "8")


class SystemStatusTests(unittest.TestCase):
    def test_full_status(self):
        status = SystemStatus.from_response("HW_INIT=1 BOARD=0 BUSY=1 DEC=128")
        self.assertTrue(status.hardware_initialized)
        self.assertFalse(status.board_connected)
        self.assertTrue(status.measurement_in_progress)
        self.assertEqual(status.decimation, 128)
        self.assertEqual(status.mode, OperatingMode.FULL)

    def test_bad_decimation_is_ignored(self):
        status = SystemStatus.from_response("HW_INIT=1 DEC=abc")
        self.assertEqual(status.decimation, 64)

    def test_rp_only_mode_is_parsed(self):
        status = SystemStatus.from_response(
            "HW_INIT=1 BOARD=0 BUSY=0 DEC=64 MODE=RP_ONLY")
        self.assertEqual(status.mode, OperatingMode.RP_ONLY)


class OperatingModeTests(ClientTestCase):
    def test_get_mode_rp_only(self):
        server = self.make_server(
            lambda command, handler: "OK RP_ONLY\n"
            if command == "SYSTEM:MODE?" else "ERR_SYNTAX: unknown\n")
        client = self.make_client(server)
        self.assertEqual(client.get_mode(), OperatingMode.RP_ONLY)

    def test_get_mode_unknown_replies_default_full(self):
        server = self.make_server(
            lambda command, handler: "OK WEIRD\n"
            if command == "SYSTEM:MODE?" else "ERR_SYNTAX: unknown\n")
        client = self.make_client(server)
        self.assertEqual(client.get_mode(), OperatingMode.FULL)

    def test_get_mode_legacy_server_defaults_full(self):
        server = self.make_server(
            lambda command, handler: "ERR_SYNTAX: Unknown command: SYSTEM:MODE\n")
        client = self.make_client(server)
        self.assertEqual(client.get_mode(), OperatingMode.FULL)

    def test_board_disabled_error_is_parsed(self):
        server = self.make_server(
            lambda command, handler:
            "ERR_HARDWARE: Electronic board disabled (RP-only mode)\n")
        client = self.make_client(server)
        with self.assertRaises(AFMCommandError) as ctx:
            client.send_command("BOARD:GAIN 1,3")
        self.assertEqual(ctx.exception.status, "ERR_HARDWARE")
        self.assertEqual(ctx.exception.message,
                         "Electronic board disabled (RP-only mode)")


if __name__ == "__main__":
    unittest.main()
