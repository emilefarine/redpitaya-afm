#!/usr/bin/env python3
"""PyQt6 smoke tests for the GUI and schematic dialog.

They run with the offscreen platform plugin and skip automatically when
PyQt6/pyqtgraph are not installed, so the base test run only needs numpy.
"""

import os
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

try:
    from PyQt6.QtWidgets import QApplication
    import pyqtgraph  # noqa: F401

    HAVE_QT = True
except ImportError:
    HAVE_QT = False

import numpy as np  # noqa: E402

from test_afm_client import FakeSCPIServer, spectrum_response  # noqa: E402

if HAVE_QT:
    from unittest import mock

    from afm_client import AFMClient, OperatingMode, SpectrumData, SystemStatus
    from afm_gui import AFMMainWindow
    from afm_schematic import MockAFMClient, RoutingSchematicDialog, SchemNode


@unittest.skipUnless(HAVE_QT, "PyQt6/pyqtgraph not installed")
class GuiSmokeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.servers = []
        self.window = AFMMainWindow()

    def tearDown(self):
        self.window.close()
        for server in self.servers:
            server.stop()

    def make_server(self, respond):
        server = FakeSCPIServer(respond)
        self.servers.append(server)
        return server.start()

    def wait_until(self, condition, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            QApplication.processEvents()
            if condition():
                return
            time.sleep(0.01)
        self.fail("condition not reached")

    def wait_idle(self):
        """Run the event loop until queued commands and the measurement are done."""
        worker = self.window._worker
        self.wait_until(lambda: self.window._commands_idle()
                        and (worker is None or not worker.isRunning()))

    def test_schematic_dialog_construction(self):
        dialog = RoutingSchematicDialog(MockAFMClient())
        self.assertIsNone(dialog.routing[4])
        dialog.close()

    def test_schematic_reports_offline_client(self):
        dialog = RoutingSchematicDialog(MockAFMClient())
        dialog.set_client(None)
        dialog.refresh_state()
        self.assertEqual(dialog.status_label.text(), "Not connected")
        dialog.close()

    def test_rp_only_disables_board_panel(self):
        self.window._set_controls_enabled(True)
        status = SystemStatus.from_response("HW_INIT=1 BOARD=0 DEC=64 MODE=RP_ONLY")
        self.window._update_board_controls(status)
        self.assertFalse(self.window.board_panel.isEnabled())
        self.assertFalse(self.window.schematic_btn.isEnabled())
        self.assertFalse(self.window.board_available)
        self.assertIn("RP-only", self.window.board_panel.toolTip())

    def test_connected_board_enables_board_panel(self):
        self.window._set_controls_enabled(True)
        status = SystemStatus.from_response("HW_INIT=1 BOARD=1 DEC=64 MODE=FULL")
        self.window._update_board_controls(status)
        self.assertTrue(self.window.board_panel.isEnabled())
        self.assertTrue(self.window.schematic_btn.isEnabled())
        self.assertTrue(self.window.board_available)

    def test_board_panel_restored_after_measure_error(self):
        self.window._set_controls_enabled(True)
        status = SystemStatus.from_response("HW_INIT=1 BOARD=1 DEC=64 MODE=FULL")
        self.window._update_board_controls(status)

        self.window._set_controls_enabled(False)
        self.assertFalse(self.window.board_panel.isEnabled())

        self.window._set_controls_enabled(True)
        self.assertTrue(self.window.board_panel.isEnabled())

    def test_update_board_controls_respects_disabled_state(self):
        self.window._set_controls_enabled(False)
        status = SystemStatus.from_response("HW_INIT=1 BOARD=1 DEC=64 MODE=FULL")
        self.window._update_board_controls(status)
        self.assertTrue(self.window.board_available)
        self.assertFalse(self.window.board_panel.isEnabled())
        self.assertFalse(self.window.schematic_btn.isEnabled())

    def test_measure_error_syncs_connection_ui(self):
        from unittest import mock

        server = self.make_server(lambda command, handler: "OK\n")
        client = AFMClient("127.0.0.1", server.port, timeout=2.0)
        client.connect()
        self.addCleanup(client.disconnect)
        self.window.client = client
        self.window._update_connection_ui(True)

        with mock.patch("afm_gui.QMessageBox"):
            # Connected client: an error keeps the controls enabled.
            self.window._on_measure_error("boom")
            self.assertEqual(self.window.connect_btn.text(), "Disconnect")
            self.assertTrue(self.window.measure_btn.isEnabled())

            # A read timeout closes the connection; the UI must show offline.
            client.disconnect()
            self.window._on_measure_error("timeout")
            self.assertEqual(self.window.connect_btn.text(), "Connect")
            self.assertFalse(self.window.measure_btn.isEnabled())

    def test_gain_warn_is_shown_separately(self):
        def respond(command, handler):
            if command == "BOARD:GAIN 1,7":
                return "OK 16 WARN: expected ADC peak ~10.50 V exceeds 1 V full scale\n"
            return "OK\n"

        server = self.make_server(respond)
        client = AFMClient("127.0.0.1", server.port, timeout=2.0)
        client.connect()
        self.addCleanup(client.disconnect)
        self.window.client = client
        self.window._update_connection_ui(True)

        self.window.gain_ch.setCurrentIndex(0)
        self.window.gain_val.setCurrentIndex(7)
        self.window._on_gain_set()
        self.wait_idle()

        message = self.window.statusBar().currentMessage()
        self.assertIn("Channel 1 set to x16", message)
        self.assertIn("expected ADC peak", message)
        self.assertNotIn("WARN:", message)

    def connect_window(self, respond):
        server = self.make_server(respond)
        client = AFMClient("127.0.0.1", server.port, timeout=2.0)
        client.connect()
        self.addCleanup(client.disconnect)
        self.window.client = client
        self.window._update_connection_ui(True)
        return client

    def test_refresh_keeps_status_bar_message(self):
        self.connect_window(
            lambda command, handler: "OK HW_INIT=0 BOARD=0 BUSY=0 DEC=64\n")

        # The refresh after connect / init / measure must not erase their message
        self.window.statusBar().showMessage("Measurement complete - 32 points in 1.23s")
        self.window._on_refresh_status()
        self.wait_idle()
        self.assertEqual(self.window.statusBar().currentMessage(),
                         "Measurement complete - 32 points in 1.23s")

    def test_adc_indicator_reports_ok_when_monitored(self):
        self.connect_window(lambda command, handler: (
            "OK HW_INIT=1 BOARD=1 BUSY=0 DEC=64 MODE=FULL LOOP=1,3 "
            "OVERDRIVE=0 SAT=0 PROTECT=1\n"))

        self.window._on_refresh_status()
        self.wait_idle()
        self.assertEqual(self.window.adc_status_label.text(), "OK")
        self.assertIn("1,3", self.window.adc_status_label.toolTip())
        self.assertTrue(self.window.adc_protect_check.isChecked())

    def test_adc_indicator_shows_overdrive_saturation_and_override(self):
        self.connect_window(lambda command, handler: (
            "OK HW_INIT=1 BOARD=1 BUSY=0 DEC=64 MODE=FULL LOOP=1,3 "
            "OVERDRIVE=1 SAT=1 PROTECT=0\n"))

        self.window._on_refresh_status()
        self.wait_idle()
        text = self.window.adc_status_label.text()
        self.assertIn("SATURATED", text)
        self.assertIn("overdrive risk", text)
        self.assertIn("protection OFF", text)
        # The checkbox follows the server state
        self.assertFalse(self.window.adc_protect_check.isChecked())

    def test_adc_indicator_for_legacy_server(self):
        self.connect_window(
            lambda command, handler: "OK HW_INIT=1 BOARD=1 BUSY=0 DEC=64\n")

        self.window._on_refresh_status()
        self.wait_idle()
        self.assertEqual(self.window.adc_status_label.text(), "not monitored")
        self.assertTrue(self.window.adc_protect_check.isChecked())

    def test_adc_indicator_resets_when_disconnected(self):
        self.connect_window(lambda command, handler: (
            "OK HW_INIT=1 BOARD=1 BUSY=0 DEC=64 MODE=FULL LOOP=1,3 "
            "OVERDRIVE=1 SAT=0 PROTECT=1\n"))
        self.window._on_refresh_status()
        self.wait_idle()
        self.assertIn("overdrive risk", self.window.adc_status_label.text())

        self.window._update_connection_ui(False)
        self.assertEqual(self.window.adc_status_label.text(), "N/A")

    def test_adc_protect_override_needs_confirmation(self):
        from unittest import mock

        commands = []

        def respond(command, handler):
            commands.append(command)
            if command.startswith("SYSTEM:STATUS"):
                protect = 0 if "BOARD:ADC:PROTECT OFF" in commands else 1
                return ("OK HW_INIT=1 BOARD=1 BUSY=0 DEC=64 MODE=FULL LOOP=1,X "
                        f"OVERDRIVE=0 SAT=0 PROTECT={protect}\n")
            return "OK OFF\n" if command.endswith("OFF") else "OK ON\n"

        self.connect_window(respond)
        self.window._on_refresh_status()
        self.wait_idle()

        with mock.patch("afm_gui.QMessageBox") as box:
            # Declined: nothing sent, box ticked again
            box.question.return_value = box.StandardButton.No
            self.window.adc_protect_check.setChecked(False)
            self.assertTrue(self.window.adc_protect_check.isChecked())
            self.assertNotIn("BOARD:ADC:PROTECT OFF", commands)

            # Confirmed: override sent and shown
            box.question.return_value = box.StandardButton.Yes
            self.window.adc_protect_check.setChecked(False)
            self.wait_idle()
            self.assertIn("BOARD:ADC:PROTECT OFF", commands)
            self.assertFalse(self.window.adc_protect_check.isChecked())
            self.assertIn("protection OFF", self.window.adc_status_label.text())

    def test_sample_gain_follows_status_and_is_sent_when_edited(self):
        commands = []

        def respond(command, handler):
            commands.append(command)
            if command.startswith("SYSTEM:STATUS"):
                gain = "0.1" if "BOARD:ADC:SAMPLEGAIN 0.1" in commands else "0.01"
                return ("OK HW_INIT=1 BOARD=1 BUSY=0 DEC=64 MODE=FULL LOOP=1,3 "
                        f"OVERDRIVE=0 SAT=0 PROTECT=1 SAMPLEGAIN={gain}\n")
            return "OK 0.1\n"

        self.connect_window(respond)
        self.window._on_refresh_status()
        self.wait_idle()
        self.assertAlmostEqual(self.window.sample_gain_spin.value(), 0.01)
        self.assertIn("Sample gain: 0.01", self.window.adc_status_label.toolTip())

        # Unchanged value: nothing sent
        self.window.sample_gain_spin.editingFinished.emit()
        self.assertFalse(any("SAMPLEGAIN" in c for c in commands))

        self.window.sample_gain_spin.setValue(0.1)
        self.window.sample_gain_spin.editingFinished.emit()
        self.wait_idle()
        self.assertIn("BOARD:ADC:SAMPLEGAIN 0.1", commands)
        self.assertAlmostEqual(self.window.sample_gain_spin.value(), 0.1)

    def test_refused_measurement_shows_overdrive_dialog(self):
        from unittest import mock

        self.connect_window(lambda command, handler: "OK\n")

        with mock.patch("afm_gui.QMessageBox") as box:
            self.window._on_measure_error(
                "ERR_PARAM: ADC overdrive: expected ADC peak ~2.00 V exceeds 1.00 V "
                "full scale; suggested: BOARD:GAIN 3,2; BOARD:ADC:PROTECT OFF overrides")
            box.warning.assert_called_once()
            box.critical.assert_not_called()
            self.assertIn("suggested: BOARD:GAIN 3,2", box.warning.call_args[0][2])
        self.assertEqual(self.window.statusBar().currentMessage(),
                         "Measurement refused: ADC overdrive")
        self.assertTrue(self.window.measure_btn.isEnabled())

    def test_status_timeout_goes_offline(self):
        started = threading.Event()
        release = threading.Event()

        def respond(command, handler):
            if command == "SYSTEM:STATUS?":
                started.set()
                release.wait(5.0)
            return "OK HW_INIT=0 BOARD=0 BUSY=0 DEC=64\n"

        server = self.make_server(respond)
        client = AFMClient("127.0.0.1", server.port, timeout=0.2)
        client.connect()
        self.addCleanup(client.disconnect)
        self.window.client = client
        self.window._update_connection_ui(True)

        try:
            self.window._on_refresh_status()
            self.wait_idle()
        finally:
            release.set()

        self.assertTrue(started.is_set())
        self.assertFalse(client.is_connected)
        self.assertEqual(self.window.connect_btn.text(), "Connect")

    @staticmethod
    def _resonance(freq, f0, q, delay_s):
        """Lorentzian response and its phase with a path delay (rad)."""
        response = 1.0 / (1.0 + 1j * 2.0 * q * (freq - f0) / f0)
        return response, np.angle(response) - 2.0 * np.pi * freq * 1e3 * delay_s

    def _displayed_phase(self, freq, magnitude, phase_rad, mode="sweep"):
        self.window._last_measure_mode = mode
        spectrum = SpectrumData(freq_kHz=freq, magnitude=magnitude, phase_rad=phase_rad)
        return self.window._prepare_display_data(spectrum)[2]

    def test_sweep_phase_keeps_the_resonance_step(self):
        f0, q = 32.768, 5000.0
        bandwidth = f0 / q
        for span in (4, 10, 40, 200):
            freq = np.linspace(f0 - span * bandwidth / 2, f0 + span * bandwidth / 2, 401)
            response, phase = self._resonance(freq, f0, q, 2e-6)
            true_deg = np.degrees(np.unwrap(np.angle(response)))
            shown = self._displayed_phase(freq, np.abs(response), phase)

            peak = int(np.argmax(np.abs(response)))
            error = np.abs((shown - shown[peak]) - (true_deg - true_deg[peak]))
            self.assertLess(error.max(), 12.0, f"span {span} bandwidths")
            # A lag across the resonance stays a lag
            self.assertGreater(shown[0] - shown[-1], 140.0, f"span {span} bandwidths")

    def test_sinc_phase_removes_delay_and_keeps_the_step(self):
        freq = np.linspace(150.0, 250.0, 419)
        response, phase = self._resonance(freq, 200.0, 400.0, 20e-6)
        phase = phase.copy()
        phase[::2] += np.pi  # the alternation of a centred sinc pulse
        shown = self._displayed_phase(freq, np.abs(response), phase, mode="sinc")

        true_deg = np.degrees(np.unwrap(np.angle(response)))
        peak = int(np.argmax(np.abs(response)))
        error = np.abs((shown - shown[peak]) - (true_deg - true_deg[peak]))
        self.assertLess(error.max(), 6.0)

    def test_flat_response_delay_is_removed(self):
        freq = np.linspace(150.0, 250.0, 201)
        phase = -2.0 * np.pi * freq * 1e3 * 20e-6
        shown = self._displayed_phase(freq, np.ones_like(freq), phase)
        self.assertLess(np.ptp(shown), 0.5)

    def test_raw_phase_when_delay_removal_is_off(self):
        f0, q = 32.768, 5000.0
        freq = np.linspace(f0 - 0.03, f0 + 0.03, 201)
        response, phase = self._resonance(freq, f0, q, 2e-6)
        self.window.remove_delay_check.setChecked(False)
        shown = self._displayed_phase(freq, np.abs(response), phase)
        np.testing.assert_allclose(shown, np.degrees(np.unwrap(phase)), atol=1e-6)

    def test_narrow_sweep_inside_resonance_is_left_untouched(self):
        f0, q = 32.768, 5000.0
        bandwidth = f0 / q
        freq = np.linspace(f0 - 0.75 * bandwidth, f0 + 0.75 * bandwidth, 201)
        response, phase = self._resonance(freq, f0, q, 2e-6)
        shown = self._displayed_phase(freq, np.abs(response), phase)
        np.testing.assert_allclose(shown, np.degrees(np.unwrap(phase)), atol=1e-6)

    def test_peak_detection_and_display(self):
        freq = np.linspace(100.0, 200.0, 256)
        mag = np.exp(-((freq - 150.0) / 5.0) ** 2) + 0.01
        spectrum = SpectrumData(
            freq_kHz=freq, magnitude=mag, phase_rad=np.zeros_like(freq))
        self.window._last_measure_mode = "sinc"
        self.window._update_plots(spectrum)
        self.window._update_peak_info(spectrum)
        peak = self.window._find_peak_index(self.window._mag_display)
        self.assertLess(abs(self.window._freq_display[peak] - 150.0), 2.0)

    def test_empty_spectrum_does_not_crash(self):
        self.window._update_plots(SpectrumData())
        self.window._update_peak_info(SpectrumData())
        self.assertEqual(self.window.info_labels["points"].text(), "0")

    def test_measure_worker_updates_window(self):
        rows = [(1.0, 0.5, 0.25), (2.0, 0.75, -0.5)]

        def respond(command, handler):
            if command.startswith("MEASURE:SINC"):
                return spectrum_response(rows)
            if command == "SYSTEM:STATUS?":
                return "OK HW_INIT=1 BOARD=0 BUSY=0 DEC=64\n"
            return "OK\n"

        server = self.make_server(respond)
        client = AFMClient("127.0.0.1", server.port, timeout=2.0)
        client.connect()
        self.addCleanup(client.disconnect)
        self.window.client = client
        self.window._update_connection_ui(True)

        self.window._on_measure()
        self.assertIn("Cancel", self.window.measure_btn.text())
        self.assertFalse(self.window.init_hw_btn.isEnabled())

        deadline = time.time() + 10.0
        while time.time() < deadline and self.window._last_spectrum is None:
            QApplication.processEvents()
            time.sleep(0.01)

        self.assertIsNotNone(self.window._last_spectrum)
        self.assertEqual(self.window._last_spectrum.num_points, 2)
        self.assertTrue(self.window.measure_btn.isEnabled())

    def test_control_commands_run_off_the_gui_thread(self):
        release = threading.Event()
        self.addCleanup(release.set)

        def respond(command, handler):
            if command.startswith("BOARD:MUX:ROUTE"):
                release.wait(5.0)
            return "OK\n"

        self.connect_window(respond)
        start = time.time()
        self.window._on_mux_set()
        self.assertLess(time.time() - start, 0.5)
        self.assertFalse(self.window._commands_idle())

        release.set()
        self.wait_idle()
        self.assertEqual(self.window.statusBar().currentMessage(), "MUX: Input 1 -> Output 1")

    def test_control_commands_run_in_order(self):
        commands = []

        def respond(command, handler):
            commands.append(command)
            return "OK 1\n"

        self.connect_window(respond)
        self.window._on_mux_set()
        self.window._on_gain_set()
        self.window._on_mux_disconnect()
        self.wait_idle()
        self.assertEqual(commands, ["BOARD:MUX:ROUTE 1,1", "BOARD:GAIN 1,3",
                                    "BOARD:MUX:DISCONNECT 1"])

    def test_cancel_measurement_reconnects(self):
        started = threading.Event()
        release = threading.Event()
        self.addCleanup(release.set)

        def respond(command, handler):
            if command.startswith("MEASURE:SWEEP"):
                started.set()
                release.wait(5.0)
                return spectrum_response([(1.0, 0.5, 0.0)])
            if command == "SYSTEM:STATUS?":
                return "OK HW_INIT=1 BOARD=0 BUSY=0 DEC=64\n"
            return "OK\n"

        first_client = self.connect_window(respond)
        self.window.radio_sweep.setChecked(True)
        with mock.patch("afm_gui.QMessageBox") as box:
            self.window._on_measure()
            self.assertTrue(self.window.measure_btn.isEnabled())
            self.assertTrue(started.wait(2.0))

            self.window._on_measure()  # Cancel
            self.wait_until(lambda: self.window.client is not first_client
                            and self.window.client.is_connected)
            self.wait_idle()
            box.critical.assert_not_called()

        self.assertIsNone(self.window._last_spectrum)
        self.assertEqual(self.window.connect_btn.text(), "Disconnect")
        self.assertIn("Measure", self.window.measure_btn.text())
        self.assertTrue(self.window.measure_btn.isEnabled())

    def test_schematic_dialog_is_reused_and_refreshed_only_when_shown(self):
        commands = []

        def respond(command, handler):
            commands.append(command)
            if command == "BOARD:STATUS?":
                return "OK LINES 2\nRouting:\n  OUT1 <- IN2\n"
            return "OK HW_INIT=1 BOARD=1 BUSY=0 DEC=64 MODE=FULL\n"

        self.connect_window(respond)
        self.window._on_open_schematic()
        self.wait_idle()
        dialog = self.window._schematic_dialog
        self.assertEqual(dialog.routing[1], 2)
        dialog.close()

        self.window._on_open_schematic()
        self.wait_idle()
        self.assertIs(self.window._schematic_dialog, dialog)
        self.assertEqual(len(self.window.findChildren(RoutingSchematicDialog)), 1)
        self.assertEqual(commands.count("BOARD:STATUS?"), 2)

        dialog.close()
        self.window._on_connected("welcome")
        self.wait_idle()
        self.assertEqual(commands.count("BOARD:STATUS?"), 2)
        self.assertNotIn("SYSTEM:MODE?", commands)

    def test_schematic_lost_connection_reaches_main_window(self):
        release = threading.Event()
        self.addCleanup(release.set)

        def respond(command, handler):
            if command == "BOARD:STATUS?":
                release.wait(5.0)
            return "OK HW_INIT=1 BOARD=1 BUSY=0 DEC=64 MODE=FULL\n"

        server = self.make_server(respond)
        client = AFMClient("127.0.0.1", server.port, timeout=0.3)
        client.connect()
        self.addCleanup(client.disconnect)
        self.window.client = client
        self.window._update_connection_ui(True)

        self.window._on_open_schematic()
        self.wait_idle()
        dialog = self.window._schematic_dialog
        self.assertEqual(self.window.connect_btn.text(), "Connect")
        self.assertIsNone(dialog.client)
        self.assertIn("Status error", dialog.status_label.text())
        dialog.close()

    def test_schematic_rp_only_is_not_interactive(self):
        client = MockAFMClient()
        calls = []
        client.set_mux = lambda out, inp: calls.append((out, inp))
        dialog = RoutingSchematicDialog(client, mode=OperatingMode.RP_ONLY)
        dialog.on_node_clicked(SchemNode.KIND_OUT, 1)
        dialog.on_node_clicked(SchemNode.KIND_IN, 2)
        self.assertEqual(calls, [])
        self.assertIsNone(dialog.selecting_out)
        self.assertIn("RP-only", dialog.status_label.text())
        dialog.close()

    def test_schematic_click_while_offline_is_refused(self):
        dialog = RoutingSchematicDialog(MockAFMClient())
        dialog.set_client(None)
        with mock.patch("afm_schematic.QMessageBox") as box:
            dialog.on_node_clicked(SchemNode.KIND_OUT, 1)
            dialog.on_route_clicked(1)
            box.question.assert_not_called()
        self.assertIsNone(dialog.selecting_out)
        self.assertEqual(dialog.status_label.text(), "Not connected")
        dialog.close()

    def test_schematic_mock_routes_without_a_worker(self):
        dialog = RoutingSchematicDialog(MockAFMClient())
        dialog.on_node_clicked(SchemNode.KIND_OUT, 2)
        dialog.on_node_clicked(SchemNode.KIND_IN, 3)
        self.assertEqual(dialog.routing[2], 3)
        dialog.close()


if __name__ == "__main__":
    unittest.main()
