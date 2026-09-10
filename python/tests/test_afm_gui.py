#!/usr/bin/env python3
"""PyQt6 smoke tests for the GUI and schematic dialog.

They run with the offscreen platform plugin and skip automatically when
PyQt6/pyqtgraph are not installed, so the base test run only needs numpy.
"""

import os
import sys
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
    from afm_client import AFMClient, SpectrumData, SystemStatus
    from afm_gui import AFMMainWindow
    from afm_schematic import MockAFMClient, RoutingSchematicDialog


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
        status = SystemStatus.from_response("HW_INIT=1 BOARD=0 DEC=64 MODE=RP_ONLY")
        self.window._update_board_controls(status)
        self.assertFalse(self.window.board_panel.isEnabled())
        self.assertFalse(self.window.board_available)
        self.assertIn("RP-only", self.window.board_panel.toolTip())

    def test_connected_board_enables_board_panel(self):
        status = SystemStatus.from_response("HW_INIT=1 BOARD=1 DEC=64 MODE=FULL")
        self.window._update_board_controls(status)
        self.assertTrue(self.window.board_panel.isEnabled())
        self.assertTrue(self.window.board_available)

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
        self.assertFalse(self.window.measure_btn.isEnabled())

        deadline = time.time() + 10.0
        while time.time() < deadline and self.window._last_spectrum is None:
            QApplication.processEvents()
            time.sleep(0.01)

        self.assertIsNotNone(self.window._last_spectrum)
        self.assertEqual(self.window._last_spectrum.num_points, 2)
        self.assertTrue(self.window.measure_btn.isEnabled())


if __name__ == "__main__":
    unittest.main()
