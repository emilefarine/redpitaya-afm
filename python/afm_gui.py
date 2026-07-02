import sys
import time
import numpy as np

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QGridLayout, QGroupBox, QLabel, QLineEdit, QPushButton, QComboBox,
    QRadioButton, QButtonGroup, QSplitter, QStatusBar, QFileDialog,
    QMessageBox, QSpinBox, QDoubleSpinBox, QFrame, QSizePolicy, QCheckBox,
)
from PyQt6.QtCore import Qt, QThread, pyqtSignal, QTimer
from PyQt6.QtGui import QFont, QColor, QPainter, QIcon

import pyqtgraph as pg

from afm_client import (
    AFMClient, AFMError, AFMConnectionError, AFMCommandError, AFMTimeoutError,
    GainSetting, SpectrumData,
)


# ---------------------------------------------------------------------------
# Color palette & styling
# ---------------------------------------------------------------------------
DARK_BG = "#1e1e2e"
PANEL_BG = "#2a2a3c"
ACCENT = "#7c3aed"
ACCENT_HOVER = "#9b5de5"
GREEN = "#22c55e"
RED = "#ef4444"
AMBER = "#f59e0b"
TEXT = "#e2e8f0"
TEXT_DIM = "#94a3b8"
BORDER = "#3f3f5a"

GAIN_LABELS = ["1/8", "1/4", "1/2", "1", "2", "4", "8", "16"]

# Peak detection: fraction of bins ignored at each spectrum edge. Sinc broadband
# excitation has near-zero energy at the band edges, so the measured magnitude
# there is dominated by large noise artifacts that fool a plain argmax. Searching
# only the interior avoids latching onto those edge spikes.
PEAK_EDGE_MARGIN_FRAC = 0.02

STYLESHEET = f"""
QMainWindow, QWidget {{
    background-color: {DARK_BG};
    color: {TEXT};
    font-family: 'Segoe UI', 'Inter', sans-serif;
    font-size: 13px;
}}
QGroupBox {{
    background-color: {PANEL_BG};
    border: 1px solid {BORDER};
    border-radius: 8px;
    margin-top: 14px;
    padding: 12px 8px 8px 8px;
    font-weight: bold;
    font-size: 13px;
}}
QGroupBox::title {{
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 6px;
    color: {ACCENT_HOVER};
}}
QLabel {{
    color: {TEXT};
    font-size: 12px;
}}
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {{
    background-color: {DARK_BG};
    color: {TEXT};
    border: 1px solid {BORDER};
    border-radius: 5px;
    padding: 2px 6px;
    min-height: 18px;
    font-size: 12px;
}}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {{
    border-color: {ACCENT};
}}
QComboBox::drop-down {{
    border: none;
    width: 20px;
}}
QComboBox QAbstractItemView {{
    background-color: {PANEL_BG};
    color: {TEXT};
    selection-background-color: {ACCENT};
    border: 1px solid {BORDER};
}}
QPushButton {{
    background-color: {ACCENT};
    color: white;
    border: none;
    border-radius: 5px;
    padding: 3px 8px;
    font-weight: bold;
    font-size: 12px;
    min-height: 18px;
}}
QPushButton:hover {{
    background-color: {ACCENT_HOVER};
}}
QPushButton:pressed {{
    background-color: #6d28d9;
}}
QPushButton:disabled {{
    background-color: #4a4a5e;
    color: #7a7a8e;
}}
QPushButton#dangerBtn {{
    background-color: {RED};
}}
QPushButton#dangerBtn:hover {{
    background-color: #dc2626;
}}
QRadioButton {{
    color: {TEXT};
    spacing: 6px;
    font-size: 12px;
}}
QRadioButton::indicator {{
    width: 14px; height: 14px;
    border: 2px solid {BORDER};
    border-radius: 9px;
    background: {DARK_BG};
}}
QRadioButton::indicator:checked {{
    background: {ACCENT};
    border-color: {ACCENT};
}}
QStatusBar {{
    background-color: {PANEL_BG};
    color: {TEXT_DIM};
    border-top: 1px solid {BORDER};
    font-size: 12px;
    padding: 2px 8px;
}}
QSplitter::handle {{
    background-color: {BORDER};
    width: 2px;
}}
"""


# ---------------------------------------------------------------------------
# LED indicator widget
# ---------------------------------------------------------------------------
class LEDIndicator(QWidget):
    """Small circular LED indicator (green/red/amber)."""

    def __init__(self, size: int = 14, parent=None):
        super().__init__(parent)
        self._color = QColor(RED)
        self._size = size
        self.setFixedSize(size + 4, size + 4)

    def set_color(self, hex_color: str):
        self._color = QColor(hex_color)
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setBrush(self._color)
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(2, 2, self._size, self._size)
        p.end()


# ---------------------------------------------------------------------------
# Measurement worker thread
# ---------------------------------------------------------------------------
class MeasureWorker(QThread):
    """Runs a measurement in a background thread."""
    finished = pyqtSignal(object)   # SpectrumData or None
    error = pyqtSignal(str)
    elapsed = pyqtSignal(float)

    def __init__(self, client: AFMClient, mode: str, params: dict):
        super().__init__()
        self.client = client
        self.mode = mode
        self.params = params

    def run(self):
        try:
            t0 = time.time()
            if self.mode == "sinc":
                result = self.client.measure_sinc(
                    center_kHz=self.params["center"],
                    bandwidth_kHz=self.params["bandwidth"],
                    num_samples=self.params["samples"],
                    decimation=self.params["decimation"],
                    amplitude=self.params["amplitude"],
                )
            else:
                result = self.client.measure_sweep(
                    center_kHz=self.params["center"],
                    range_kHz=self.params["range"],
                    step_kHz=self.params["step"],
                    decimation=self.params["decimation"],
                    amplitude=self.params["amplitude"],
                )
            dt = time.time() - t0
            self.elapsed.emit(dt)
            self.finished.emit(result)
        except AFMError as exc:
            self.error.emit(str(exc))


# ---------------------------------------------------------------------------
# Connection worker thread
# ---------------------------------------------------------------------------
class ConnectWorker(QThread):
    """Connects to the server in a background thread."""
    connected = pyqtSignal(str)     # welcome message
    error = pyqtSignal(str)

    def __init__(self, client: AFMClient):
        super().__init__()
        self.client = client

    def run(self):
        try:
            welcome = self.client.connect()
            self.connected.emit(welcome)
        except AFMConnectionError as exc:
            self.error.emit(str(exc))


# ---------------------------------------------------------------------------
# Main Window
# ---------------------------------------------------------------------------
class AFMMainWindow(QMainWindow):

    def __init__(self):
        super().__init__()
        self.setWindowTitle("AFM Measurement System")
        self.resize(1400, 850)

        self.client = AFMClient("192.168.1.100", 5025)
        self._worker: MeasureWorker | None = None
        self._connect_worker: ConnectWorker | None = None
        self._last_spectrum: SpectrumData | None = None
        self._measure_time = 0.0
        self._last_measure_mode = "sinc"
        self._pending_measure_mode = "sinc"

        # Display-scaled data (units may differ from wire-format units)
        self._freq_display: np.ndarray = np.array([], dtype=np.float64)
        self._mag_display: np.ndarray = np.array([], dtype=np.float64)
        self._phase_display_deg: np.ndarray = np.array([], dtype=np.float64)
        self._freq_unit = "kHz"
        self._mag_unit = "V"

        self._build_ui()
        self._update_connection_ui(False)
        self.statusBar().showMessage("Ready - enter IP address and click Connect")

    # ---------------------------------------------------------------
    # UI construction
    # ---------------------------------------------------------------
    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QHBoxLayout(central)
        main_layout.setContentsMargins(8, 8, 8, 8)
        main_layout.setSpacing(8)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        main_layout.addWidget(splitter)

        # --- Left sidebar ---
        sidebar = QWidget()
        sidebar.setMaximumWidth(340)
        sidebar.setMinimumWidth(280)
        sidebar_layout = QVBoxLayout(sidebar)
        sidebar_layout.setContentsMargins(0, 0, 0, 0)
        sidebar_layout.setSpacing(6)

        sidebar_layout.addWidget(self._build_connection_panel())
        sidebar_layout.addWidget(self._build_status_panel())
        sidebar_layout.addWidget(self._build_board_panel())
        sidebar_layout.addWidget(self._build_measurement_panel())
        sidebar_layout.addStretch()

        splitter.addWidget(sidebar)

        # --- Right plot area ---
        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.setSpacing(6)

        right_layout.addWidget(self._build_plot_area(), stretch=1)
        right_layout.addWidget(self._build_info_panel())

        splitter.addWidget(right)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([310, 1090])

    # --- Connection panel ---
    def _build_connection_panel(self) -> QGroupBox:
        grp = QGroupBox("Connection")
        layout = QGridLayout(grp)
        layout.setSpacing(6)

        layout.addWidget(QLabel("IP Address"), 0, 0)
        self.ip_edit = QLineEdit("192.168.1.100")
        self.ip_edit.setPlaceholderText("e.g. 192.168.1.100")
        layout.addWidget(self.ip_edit, 0, 1)

        layout.addWidget(QLabel("Port"), 1, 0)
        self.port_spin = QSpinBox()
        self.port_spin.setRange(1, 65535)
        self.port_spin.setValue(5025)
        layout.addWidget(self.port_spin, 1, 1)

        btn_row = QHBoxLayout()
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self._on_connect)
        btn_row.addWidget(self.connect_btn)

        self.conn_led = LEDIndicator(14)
        btn_row.addWidget(self.conn_led)

        self.conn_label = QLabel("Disconnected")
        self.conn_label.setStyleSheet(f"background: transparent; color: {RED}; font-weight: bold;")
        btn_row.addWidget(self.conn_label)
        btn_row.addStretch()
        layout.addLayout(btn_row, 2, 0, 1, 2)

        return grp

    # --- Status panel ---
    def _build_status_panel(self) -> QGroupBox:
        grp = QGroupBox("System Status")
        layout = QGridLayout(grp)
        layout.setSpacing(4)

        labels = ["HW Initialized", "Board Connected", "Decimation"]
        self.status_values: list[QLabel] = []
        for i, name in enumerate(labels):
            layout.addWidget(QLabel(name), i, 0)
            val = QLabel("N/A")
            val.setStyleSheet(f"color: {TEXT_DIM};")
            val.setAlignment(Qt.AlignmentFlag.AlignRight)
            layout.addWidget(val, i, 1)
            self.status_values.append(val)

        status_btns = QHBoxLayout()
        self.init_hw_btn = QPushButton("Init HW")
        self.init_hw_btn.clicked.connect(self._on_init_hw)
        status_btns.addWidget(self.init_hw_btn)

        self.refresh_status_btn = QPushButton("Refresh")
        self.refresh_status_btn.clicked.connect(self._on_refresh_status)
        status_btns.addWidget(self.refresh_status_btn)

        layout.addLayout(status_btns, len(labels), 0, 1, 2)

        return grp

    # --- Board panel ---
    def _build_board_panel(self) -> QGroupBox:
        grp = QGroupBox("Board Configuration")
        layout = QGridLayout(grp)
        layout.setSpacing(4)

        # MUX
        layout.addWidget(QLabel("MUX Output"), 0, 0)
        self.mux_out = QComboBox()
        self.mux_out.addItems([f"Out {i}" for i in range(1, 5)])
        layout.addWidget(self.mux_out, 0, 1)

        layout.addWidget(QLabel("MUX Input"), 1, 0)
        self.mux_in = QComboBox()
        self.mux_in.addItems([f"In {i}" for i in range(1, 5)])
        layout.addWidget(self.mux_in, 1, 1)

        mux_btns = QHBoxLayout()
        self.mux_set_btn = QPushButton("Set Route")
        self.mux_set_btn.clicked.connect(self._on_mux_set)
        mux_btns.addWidget(self.mux_set_btn)
        self.mux_disc_btn = QPushButton("Disconnect")
        self.mux_disc_btn.clicked.connect(self._on_mux_disconnect)
        mux_btns.addWidget(self.mux_disc_btn)
        layout.addLayout(mux_btns, 2, 0, 1, 2)

        # Separator
        sep = QFrame()
        sep.setFrameShape(QFrame.Shape.HLine)
        sep.setStyleSheet(f"color: {BORDER};")
        layout.addWidget(sep, 3, 0, 1, 2)

        # Gain
        layout.addWidget(QLabel("Gain Channel"), 4, 0)
        self.gain_ch = QComboBox()
        self.gain_ch.addItems([f"Ch {i}" for i in range(1, 5)])
        layout.addWidget(self.gain_ch, 4, 1)

        layout.addWidget(QLabel("Gain Value"), 5, 0)
        self.gain_val = QComboBox()
        for i, label in enumerate(GAIN_LABELS):
            self.gain_val.addItem(f"{i}: ×{label}", i)
        self.gain_val.setCurrentIndex(3)  # default x1
        layout.addWidget(self.gain_val, 5, 1)

        self.gain_set_btn = QPushButton("Set Gain")
        self.gain_set_btn.clicked.connect(self._on_gain_set)
        layout.addWidget(self.gain_set_btn, 6, 0, 1, 2)

        # Board reset / status
        board_btns = QHBoxLayout()
        self.board_status_btn = QPushButton("Board Status")
        self.board_status_btn.clicked.connect(self._on_board_status)
        board_btns.addWidget(self.board_status_btn)
        self.board_reset_btn = QPushButton("Reset Board")
        self.board_reset_btn.clicked.connect(self._on_board_reset)
        board_btns.addWidget(self.board_reset_btn)
        layout.addLayout(board_btns, 7, 0, 1, 2)

        return grp

    # --- Measurement panel ---
    def _build_measurement_panel(self) -> QGroupBox:
        grp = QGroupBox("Measurement")
        layout = QGridLayout(grp)
        layout.setSpacing(4)

        # Mode radio buttons
        mode_row = QHBoxLayout()
        self.mode_group = QButtonGroup(self)
        self.radio_sinc = QRadioButton("Sinc (broadband)")
        self.radio_sweep = QRadioButton("Sweep")
        self.radio_sinc.setChecked(True)
        self.mode_group.addButton(self.radio_sinc, 0)
        self.mode_group.addButton(self.radio_sweep, 1)
        mode_row.addWidget(self.radio_sinc)
        mode_row.addWidget(self.radio_sweep)
        layout.addLayout(mode_row, 0, 0, 1, 2)

        self.radio_sinc.toggled.connect(self._on_mode_changed)

        # Common parameters
        row = 1
        layout.addWidget(QLabel("Center (kHz)"), row, 0)
        self.center_spin = QDoubleSpinBox()
        self.center_spin.setRange(0.1, 65000)
        self.center_spin.setValue(400.0)
        self.center_spin.setDecimals(1)
        layout.addWidget(self.center_spin, row, 1)

        row += 1
        # Sinc: bandwidth, Sweep: range - use same widget, relabel
        self.bw_label = QLabel("Bandwidth (kHz)")
        layout.addWidget(self.bw_label, row, 0)
        self.bw_spin = QDoubleSpinBox()
        self.bw_spin.setRange(0.1, 65000)
        self.bw_spin.setValue(50.0)
        self.bw_spin.setDecimals(1)
        layout.addWidget(self.bw_spin, row, 1)

        row += 1
        # Sweep-only: step
        self.step_label = QLabel("Step (kHz)")
        layout.addWidget(self.step_label, row, 0)
        self.step_spin = QDoubleSpinBox()
        self.step_spin.setRange(0.01, 1000)
        self.step_spin.setValue(1.0)
        self.step_spin.setDecimals(2)
        layout.addWidget(self.step_spin, row, 1)

        row += 1
        # Sinc-only: samples
        self.samples_label = QLabel("Samples")
        layout.addWidget(self.samples_label, row, 0)
        self.samples_spin = QSpinBox()
        self.samples_spin.setRange(64, 65536)
        self.samples_spin.setValue(8192)
        self.samples_spin.setSingleStep(1024)
        layout.addWidget(self.samples_spin, row, 1)

        row += 1
        layout.addWidget(QLabel("Decimation"), row, 0)
        self.dec_combo = QComboBox()
        for d in [16, 32, 64, 128, 256, 512, 1024]:
            self.dec_combo.addItem(str(d), d)
        self.dec_combo.setCurrentIndex(2)  # 64
        layout.addWidget(self.dec_combo, row, 1)

        row += 1
        layout.addWidget(QLabel("Amplitude"), row, 0)
        self.amp_spin = QDoubleSpinBox()
        self.amp_spin.setRange(0.0, 1.0)
        self.amp_spin.setValue(1.0)
        self.amp_spin.setDecimals(2)
        self.amp_spin.setSingleStep(0.1)
        layout.addWidget(self.amp_spin, row, 1)

        row += 1
        self.unwrap_phase_sinc_check = QCheckBox("Unwrap phase (sinc)")
        self.unwrap_phase_sinc_check.setChecked(True)
        self.unwrap_phase_sinc_check.toggled.connect(self._on_phase_unwrap_toggled)
        layout.addWidget(self.unwrap_phase_sinc_check, row, 0, 1, 2)

        row += 1
        self.unwrap_phase_sweep_check = QCheckBox("Unwrap phase (sweep)")
        self.unwrap_phase_sweep_check.setChecked(True)
        self.unwrap_phase_sweep_check.toggled.connect(self._on_phase_unwrap_toggled)
        layout.addWidget(self.unwrap_phase_sweep_check, row, 0, 1, 2)

        row += 1
        self.measure_btn = QPushButton("▶  Measure")
        self.measure_btn.setStyleSheet(
            self.measure_btn.styleSheet() + "font-size: 14px; min-height: 36px;")
        self.measure_btn.clicked.connect(self._on_measure)
        layout.addWidget(self.measure_btn, row, 0, 1, 2)

        self._on_mode_changed()  # set initial visibility
        return grp

    # --- Plot area ---
    def _build_plot_area(self) -> QWidget:
        pg.setConfigOptions(antialias=True, background=DARK_BG, foreground=TEXT)

        container = QWidget()
        layout = QVBoxLayout(container)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)

        # Magnitude plot
        self.mag_plot = pg.PlotWidget(title="Magnitude")
        self.mag_plot.setLabel("bottom", "Frequency", units="kHz")
        self.mag_plot.setLabel("left", "Magnitude", units="V")
        self.mag_plot.showGrid(x=True, y=True, alpha=0.3)
        self.mag_curve = self.mag_plot.plot([], [], pen=pg.mkPen("#60a5fa", width=2))
        self.mag_peak_scatter = pg.ScatterPlotItem(
            size=8, brush=pg.mkBrush("#ef4444"), symbol="o", pen=pg.mkPen(None))
        self.mag_plot.addItem(self.mag_peak_scatter)
        layout.addWidget(self.mag_plot)

        # Phase plot
        self.phase_plot = pg.PlotWidget(title="Phase")
        self.phase_plot.setLabel("bottom", "Frequency", units="kHz")
        self.phase_plot.setLabel("left", "Phase", units="deg")
        self.phase_plot.getAxis("left").enableAutoSIPrefix(False)
        self.phase_plot.showGrid(x=True, y=True, alpha=0.3)
        self.phase_curve = self.phase_plot.plot([], [], pen=pg.mkPen("#f97316", width=2))
        self.phase_peak_scatter = pg.ScatterPlotItem(
            size=8, brush=pg.mkBrush("#ef4444"), symbol="o", pen=pg.mkPen(None))
        self.phase_plot.addItem(self.phase_peak_scatter)
        layout.addWidget(self.phase_plot)

        # Link X axes
        self.phase_plot.setXLink(self.mag_plot)

        # Crosshairs
        crosshair_pen = pg.mkPen(TEXT_DIM, width=1, style=Qt.PenStyle.DashLine)
        self._mag_vline = pg.InfiniteLine(angle=90, movable=False, pen=crosshair_pen)
        self._mag_hline = pg.InfiniteLine(angle=0, movable=False, pen=crosshair_pen)
        self._phase_vline = pg.InfiniteLine(angle=90, movable=False, pen=crosshair_pen)
        self._phase_hline = pg.InfiniteLine(angle=0, movable=False, pen=crosshair_pen)

        self.mag_plot.addItem(self._mag_vline, ignoreBounds=True)
        self.mag_plot.addItem(self._mag_hline, ignoreBounds=True)
        self.phase_plot.addItem(self._phase_vline, ignoreBounds=True)
        self.phase_plot.addItem(self._phase_hline, ignoreBounds=True)

        self.crosshair_label_mag = pg.TextItem(anchor=(0, 1), color=TEXT_DIM)
        self.crosshair_label_phase = pg.TextItem(anchor=(0, 1), color=TEXT_DIM)
        self.mag_plot.addItem(self.crosshair_label_mag, ignoreBounds=True)
        self.phase_plot.addItem(self.crosshair_label_phase, ignoreBounds=True)

        self.mag_plot.scene().sigMouseMoved.connect(self._on_mag_mouse_moved)
        self.phase_plot.scene().sigMouseMoved.connect(self._on_phase_mouse_moved)

        return container

    # --- Info panel (below plots) ---
    def _build_info_panel(self) -> QWidget:
        container = QWidget()
        container.setStyleSheet(
            f"background-color: {PANEL_BG}; border: 1px solid {BORDER}; border-radius: 8px;")
        layout = QHBoxLayout(container)
        layout.setContentsMargins(12, 8, 12, 8)

        # Peak info labels
        info_grid = QGridLayout()
        info_grid.setSpacing(4)

        self.info_labels: dict[str, QLabel] = {}
        fields = [
            ("Peak Freq", "peak_freq"), ("Peak Mag", "peak_mag"),
            ("Peak Phase", "peak_phase"), ("Points", "points"),
            ("-3dB BW", "bw"), ("Q-Factor", "q_factor"),
            ("Meas. Time", "meas_time"),
        ]
        for i, (display, key) in enumerate(fields):
            col = (i // 4) * 2
            row = i % 4
            lbl = QLabel(f"{display}:")
            lbl.setStyleSheet(f"color: {TEXT_DIM}; font-size: 11px; border: none;")
            info_grid.addWidget(lbl, row, col)
            val = QLabel("N/A")
            val.setStyleSheet(f"color: {TEXT}; font-weight: bold; font-size: 12px; border: none;")
            info_grid.addWidget(val, row, col + 1)
            self.info_labels[key] = val

        layout.addLayout(info_grid, stretch=1)

        # Buttons
        btn_col = QVBoxLayout()
        self.save_csv_btn = QPushButton("Save CSV")
        self.save_csv_btn.clicked.connect(self._on_save_csv)
        btn_col.addWidget(self.save_csv_btn)

        self.save_plot_btn = QPushButton("Save Plot")
        self.save_plot_btn.clicked.connect(self._on_save_plot)
        btn_col.addWidget(self.save_plot_btn)
        btn_col.addStretch()
        layout.addLayout(btn_col)

        return container

    # ---------------------------------------------------------------
    # Connection logic
    # ---------------------------------------------------------------
    def _on_connect(self):
        if self.client.is_connected:
            self.client.disconnect()
            self._update_connection_ui(False)
            self.statusBar().showMessage("Disconnected")
            return

        host = self.ip_edit.text().strip()
        port = self.port_spin.value()
        if not host:
            QMessageBox.warning(self, "Error", "Please enter an IP address")
            return

        self.client = AFMClient(host, port)
        self.connect_btn.setEnabled(False)
        self.connect_btn.setText("Connecting…")
        self.statusBar().showMessage(f"Connecting to {host}:{port}…")

        self._connect_worker = ConnectWorker(self.client)
        self._connect_worker.connected.connect(self._on_connected)
        self._connect_worker.error.connect(self._on_connect_error)
        self._connect_worker.start()

    def _on_connected(self, welcome: str):
        self._update_connection_ui(True)
        self.statusBar().showMessage(f"Connected - {welcome}")
        self._on_refresh_status()

    def _on_connect_error(self, msg: str):
        self._update_connection_ui(False)
        self.statusBar().showMessage(f"Connection failed: {msg}")
        QMessageBox.critical(self, "Connection Error", msg)

    def _update_connection_ui(self, connected: bool):
        self.connect_btn.setEnabled(True)
        if connected:
            self.connect_btn.setText("Disconnect")
            self.conn_led.set_color(GREEN)
            self.conn_label.setText("Connected")
            self.conn_label.setStyleSheet(f"background: transparent; color: {GREEN}; font-weight: bold;")
        else:
            self.connect_btn.setText("Connect")
            self.conn_led.set_color(RED)
            self.conn_label.setText("Disconnected")
            self.conn_label.setStyleSheet(f"background: transparent; color: {RED}; font-weight: bold;")

        # Enable/disable control panels
        for btn in [self.init_hw_btn, self.mux_set_btn, self.mux_disc_btn, self.gain_set_btn,
                     self.board_status_btn, self.board_reset_btn,
                     self.measure_btn, self.refresh_status_btn]:
            btn.setEnabled(connected)

    # ---------------------------------------------------------------
    # Status & Initialization
    # ---------------------------------------------------------------
    def _on_init_hw(self):
        try:
            result = self.client.init()
            self.statusBar().showMessage(f"Hardware initialization: {result}")
            self._on_refresh_status()
        except AFMError as exc:
            self.statusBar().showMessage(f"Hardware init error: {exc}")

    def _on_refresh_status(self):
        if not self.client.is_connected:
            return
        try:
            status = self.client.get_status()
            icons = ["✓" if status.hardware_initialized else "✗",
                     "✓" if status.board_connected else "✗",
                     str(status.decimation)]
            colors = [GREEN if status.hardware_initialized else RED,
                      GREEN if status.board_connected else RED,
                      TEXT]
            for val_lbl, icon, color in zip(self.status_values, icons, colors):
                val_lbl.setText(icon)
                val_lbl.setStyleSheet(f"color: {color}; font-weight: bold;")
        except AFMError as exc:
            self.statusBar().showMessage(f"Status error: {exc}")

    # ---------------------------------------------------------------
    # Board controls
    # ---------------------------------------------------------------
    def _on_mux_set(self):
        try:
            out = self.mux_out.currentIndex() + 1  # Convert to 1-based (board connector label)
            inp = self.mux_in.currentIndex() + 1   # Convert to 1-based (board connector label)
            self.client.set_mux(out, inp)
            self.statusBar().showMessage(f"MUX: Input {inp} -> Output {out}")
        except AFMError as exc:
            self.statusBar().showMessage(f"MUX error: {exc}")

    def _on_mux_disconnect(self):
        try:
            out = self.mux_out.currentIndex() + 1  # Convert to 1-based (board connector label)
            self.client.disconnect_mux(out)
            self.statusBar().showMessage(f"MUX: Output {out} disconnected")
        except AFMError as exc:
            self.statusBar().showMessage(f"MUX error: {exc}")

    def _on_gain_set(self):
        try:
            ch = self.gain_ch.currentIndex() + 1  # Convert to 1-based (board connector label)
            idx = self.gain_val.currentData()
            result = self.client.set_gain(ch, idx)
            self.statusBar().showMessage(f"Gain: Channel {ch} set to x{result}")
        except AFMError as exc:
            self.statusBar().showMessage(f"Gain error: {exc}")

    def _on_board_status(self):
        try:
            result = self.client.get_board_status()
            QMessageBox.information(self, "Board Status", result)
        except AFMError as exc:
            self.statusBar().showMessage(f"Board status error: {exc}")

    def _on_board_reset(self):
        reply = QMessageBox.question(
            self, "Confirm", "Reset electronic board to defaults?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
        if reply == QMessageBox.StandardButton.Yes:
            try:
                result = self.client.reset_board()
                self.statusBar().showMessage(f"Board reset: {result}")
            except AFMError as exc:
                self.statusBar().showMessage(f"Board reset error: {exc}")

    # ---------------------------------------------------------------
    # Measurement
    # ---------------------------------------------------------------
    def _on_mode_changed(self):
        is_sinc = self.radio_sinc.isChecked()
        self.bw_label.setText("Bandwidth (kHz)" if is_sinc else "Range (kHz)")
        self.step_label.setVisible(not is_sinc)
        self.step_spin.setVisible(not is_sinc)
        self.samples_label.setVisible(is_sinc)
        self.samples_spin.setVisible(is_sinc)
        self.unwrap_phase_sinc_check.setVisible(is_sinc)
        self.unwrap_phase_sweep_check.setVisible(not is_sinc)

    def _on_measure(self):
        if self._worker is not None and self._worker.isRunning():
            self.statusBar().showMessage("Measurement already in progress")
            return

        is_sinc = self.radio_sinc.isChecked()
        params = {
            "center": self.center_spin.value(),
            "decimation": self.dec_combo.currentData(),
            "amplitude": self.amp_spin.value(),
        }
        if is_sinc:
            params["bandwidth"] = self.bw_spin.value()
            params["samples"] = self.samples_spin.value()
            mode = "sinc"
            desc = (f"Sinc: center={params['center']:.1f} kHz, "
                    f"BW={params['bandwidth']:.1f} kHz, "
                    f"samples={params['samples']}")
        else:
            params["range"] = self.bw_spin.value()
            params["step"] = self.step_spin.value()
            mode = "sweep"
            desc = (f"Sweep: center={params['center']:.1f} kHz, "
                    f"range={params['range']:.1f} kHz, "
                    f"step={params['step']:.2f} kHz")

        self.measure_btn.setEnabled(False)
        self.measure_btn.setText("⏳ Measuring…")
        self.statusBar().showMessage(f"Measuring… {desc}")
        self._pending_measure_mode = mode

        self._worker = MeasureWorker(self.client, mode, params)
        self._worker.finished.connect(self._on_measure_done)
        self._worker.error.connect(self._on_measure_error)
        self._worker.elapsed.connect(self._on_measure_elapsed)
        self._measure_time = 0.0
        self._worker.start()

    def _on_measure_elapsed(self, dt: float):
        self._measure_time = dt

    def _on_measure_done(self, spectrum: SpectrumData):
        self._last_spectrum = spectrum
        self._last_measure_mode = self._pending_measure_mode
        self.measure_btn.setEnabled(True)
        self.measure_btn.setText("▶  Measure")
        self._update_plots(spectrum)
        self._update_peak_info(spectrum)
        if spectrum.num_points > 0:
            peak_idx = self._find_peak_index(self._mag_display)
            self._update_cursor_for_x(float(self._freq_display[peak_idx]))
        self.statusBar().showMessage(
            f"Measurement complete - {spectrum.num_points} points in {self._measure_time:.2f}s")
        self._on_refresh_status()

    def _on_measure_error(self, msg: str):
        self.measure_btn.setEnabled(True)
        self.measure_btn.setText("▶  Measure")
        self.statusBar().showMessage(f"Measurement error: {msg}")
        QMessageBox.critical(self, "Measurement Error", msg)

    def _on_phase_unwrap_toggled(self, checked: bool):
        _ = checked
        sinc_mode = "ON" if self.unwrap_phase_sinc_check.isChecked() else "OFF"
        sweep_mode = "ON" if self.unwrap_phase_sweep_check.isChecked() else "OFF"
        self.statusBar().showMessage(
            f"Phase unwrap: sinc={sinc_mode}, sweep={sweep_mode}")

        if self._last_spectrum is not None and self._last_spectrum.num_points > 0:
            self._update_plots(self._last_spectrum)
            self._update_peak_info(self._last_spectrum)
            peak_idx = self._find_peak_index(self._mag_display)
            self._update_cursor_for_x(float(self._freq_display[peak_idx]))

    # ---------------------------------------------------------------
    # Peak detection (robust against band-edge / spike noise)
    # ---------------------------------------------------------------
    @staticmethod
    def _median_filter(x: np.ndarray, k: int) -> np.ndarray:
        """Sliding-window median (odd window ``k``, edge-padded). Suppresses
        spikes narrower than ``k // 2`` bins while preserving wider features."""
        if k <= 1 or x.size < k:
            return x
        pad = k // 2
        xp = np.pad(x, pad, mode="edge")
        try:
            windows = np.lib.stride_tricks.sliding_window_view(xp, k)
        except AttributeError:
            return x  # numpy < 1.20: skip smoothing
        return np.median(windows, axis=1)

    def _find_peak_index(self, mag: np.ndarray) -> int:
        """Locate the resonance peak robustly.

        A plain ``argmax`` latches onto the large noise spikes that sinc
        broadband spectra show at the excitation band edges (where SNR
        collapses), or onto isolated single-bin glitches, instead of the true
        resonance. So we (1) median-filter to kill narrow spikes and (2) ignore
        a margin at each band edge, take the argmax of the cleaned magnitude,
        then refine back to the exact bin so the reported frequency stays
        accurate.
        """
        n = int(mag.size)
        if n == 0:
            return 0
        if n < 16:
            return int(np.argmax(mag))

        smooth = self._median_filter(mag, 5)
        margin = max(3, int(round(n * PEAK_EDGE_MARGIN_FRAC)))
        lo, hi = margin, n - margin
        if hi - lo < 1:
            lo, hi = 0, n

        region_idx = lo + int(np.argmax(smooth[lo:hi]))

        # Median filtering can shift the apparent peak by a bin or two; snap back
        # to the true maximum in a small neighbourhood.
        w = 5
        a = max(0, region_idx - w)
        b = min(n, region_idx + w + 1)
        return a + int(np.argmax(mag[a:b]))

    @staticmethod
    def _find_3db_region(mag: np.ndarray, peak_idx: int):
        """Return ``(left_idx, right_idx)`` of the contiguous -3 dB band around
        ``peak_idx``, walking outward until the magnitude drops below
        ``peak / sqrt(2)``. Unlike a global threshold crossing, this ignores the
        band-edge noise spikes when measuring bandwidth / Q."""
        n = int(mag.size)
        if n == 0:
            return 0, 0
        thr = mag[peak_idx] / np.sqrt(2.0)
        li = peak_idx
        while li > 0 and mag[li - 1] >= thr:
            li -= 1
        ri = peak_idx
        while ri < n - 1 and mag[ri + 1] >= thr:
            ri += 1
        return li, ri

    # ---------------------------------------------------------------
    # Plotting
    # ---------------------------------------------------------------
    def _prepare_display_data(self, spectrum: SpectrumData):
        """Prepare scaled arrays for plotting and labels."""
        freq_khz = spectrum.freq_kHz.astype(np.float64)
        mag_raw = spectrum.magnitude.astype(np.float64)
        phase_rad = spectrum.phase_rad.astype(np.float64)

        def _wrap_deg(values: np.ndarray) -> np.ndarray:
            return (values + 180.0) % 360.0 - 180.0

        if self._last_measure_mode == "sinc":
            # Match legacy sinc viewer behavior:
            # 1) convert rad->deg
            # 2) correct alternating 180-deg FFT bin offset
            # 3) remove global linear delay trend to avoid 10k+ deg ramps
            # 4) unwrap or wrap depending on sinc toggle
            phase_deg = np.degrees(phase_rad)
            if phase_deg.size > 1:
                phase_deg[::2] -= 180.0

            # First wrap, then unwrap in a stable way
            phase_wrapped = _wrap_deg(phase_deg)
            phase_unwrapped = np.degrees(np.unwrap(np.deg2rad(phase_wrapped)))

            # Remove linear phase slope (delay component), anchored at resonance peak
            phase_compensated = phase_unwrapped
            if phase_unwrapped.size > 1:
                peak_idx = self._find_peak_index(mag_raw)
                f0 = freq_khz[peak_idx]
                slope, _ = np.polyfit(freq_khz, phase_unwrapped, 1)
                phase_compensated = phase_unwrapped - slope * (freq_khz - f0)

            if self.unwrap_phase_sinc_check.isChecked():
                phase_deg = phase_compensated
            else:
                phase_deg = _wrap_deg(phase_compensated)
        else:
            if self.unwrap_phase_sweep_check.isChecked():
                # Sweep default: preserve phase continuity across frequency.
                phase_deg = np.degrees(np.unwrap(phase_rad))
            else:
                # Optional wrapped sweep display.
                phase_deg = np.degrees(phase_rad)
                phase_deg = _wrap_deg(phase_deg)

        max_freq_khz = float(np.max(np.abs(freq_khz))) if freq_khz.size else 0.0
        if max_freq_khz >= 1000.0:
            freq_scale = 1.0 / 1000.0
            self._freq_unit = "MHz"
        else:
            freq_scale = 1.0
            self._freq_unit = "kHz"
        freq_display = freq_khz * freq_scale

        max_mag = float(np.max(np.abs(mag_raw))) if mag_raw.size else 0.0
        if max_mag >= 1.0:
            mag_scale = 1.0
            self._mag_unit = "V"
        elif max_mag >= 1e-3:
            mag_scale = 1e3
            self._mag_unit = "mV"
        else:
            mag_scale = 1e6
            self._mag_unit = "uV"
        mag_display = mag_raw * mag_scale

        return freq_display, mag_display, phase_deg

    def _update_plots(self, spectrum: SpectrumData):
        freq, mag, phase_deg = self._prepare_display_data(spectrum)

        self._freq_display = freq
        self._mag_display = mag
        self._phase_display_deg = phase_deg

        self.mag_plot.setLabel("bottom", "Frequency", units=self._freq_unit)
        self.phase_plot.setLabel("bottom", "Frequency", units=self._freq_unit)
        self.mag_plot.setLabel("left", "Magnitude", units=self._mag_unit)

        # Magnitude
        self.mag_curve.setData(freq, mag)
        peak_idx = self._find_peak_index(mag)
        self.mag_peak_scatter.setData([freq[peak_idx]], [mag[peak_idx]])

        # -3dB lines
        # Remove old infinite lines (keep the crosshair ones)
        for item in list(self.mag_plot.items()):
            if isinstance(item, pg.InfiniteLine) and item not in (self._mag_vline, self._mag_hline):
                self.mag_plot.removeItem(item)

        threshold = mag[peak_idx] / np.sqrt(2)
        li, ri = self._find_3db_region(mag, peak_idx)
        if ri > li:
            f_low = freq[li]
            f_high = freq[ri]
            line_pen = pg.mkPen("#22c55e", width=1, style=Qt.PenStyle.DotLine)
            self.mag_plot.addItem(pg.InfiniteLine(pos=f_low, angle=90, pen=line_pen))
            self.mag_plot.addItem(pg.InfiniteLine(pos=f_high, angle=90, pen=line_pen))
            h_line = pg.InfiniteLine(pos=threshold, angle=0, pen=line_pen)
            self.mag_plot.addItem(h_line)

        # Peak annotation line on magnitude
        peak_pen = pg.mkPen(RED, width=1.5, style=Qt.PenStyle.DashLine)
        self.mag_plot.addItem(pg.InfiniteLine(pos=freq[peak_idx], angle=90, pen=peak_pen))

        # Phase
        self.phase_curve.setData(freq, phase_deg)
        self.phase_peak_scatter.setData([freq[peak_idx]], [phase_deg[peak_idx]])

        # Remove old phase infinite lines
        for item in list(self.phase_plot.items()):
            if isinstance(item, pg.InfiniteLine) and item not in (self._phase_vline, self._phase_hline):
                self.phase_plot.removeItem(item)
        self.phase_plot.addItem(pg.InfiniteLine(pos=freq[peak_idx], angle=90, pen=peak_pen))
        self.phase_plot.addItem(
            pg.InfiniteLine(pos=0, angle=0, pen=pg.mkPen(TEXT_DIM, width=0.5)))

        # Auto-range
        self.mag_plot.autoRange()
        if self._last_measure_mode == "sinc" and not self.unwrap_phase_sinc_check.isChecked():
            self.phase_plot.setYRange(-180.0, 180.0, padding=0.02)
        else:
            self.phase_plot.autoRange()

    def _update_peak_info(self, spectrum: SpectrumData):
        freq = self._freq_display
        mag = self._mag_display
        phase_deg = self._phase_display_deg
        peak_idx = self._find_peak_index(mag)

        self.info_labels["peak_freq"].setText(f"{freq[peak_idx]:.3f} {self._freq_unit}")
        self.info_labels["peak_mag"].setText(f"{mag[peak_idx]:.4e} {self._mag_unit}")
        self.info_labels["peak_phase"].setText(f"{phase_deg[peak_idx]:.1f}°")
        self.info_labels["points"].setText(str(spectrum.num_points))
        self.info_labels["meas_time"].setText(f"{self._measure_time:.2f} s")

        # -3dB bandwidth and Q-factor from raw kHz, measured as the contiguous
        # half-power band around the detected peak (walking outward), so the
        # band-edge noise spikes can't blow up the bandwidth.
        mag_raw = spectrum.magnitude
        li, ri = self._find_3db_region(mag_raw, peak_idx)
        bw_khz = spectrum.freq_kHz[ri] - spectrum.freq_kHz[li]
        if ri > li and bw_khz > 0:
            q = spectrum.freq_kHz[peak_idx] / bw_khz
            bw_hz = bw_khz * 1000.0
            if bw_hz >= 1e6:
                bw_text = f"{bw_hz / 1e6:.3f} MHz"
            elif bw_hz >= 1e3:
                bw_text = f"{bw_hz / 1e3:.3f} kHz"
            else:
                bw_text = f"{bw_hz:.1f} Hz"
            self.info_labels["bw"].setText(bw_text)
            self.info_labels["q_factor"].setText(f"{q:.0f}")
        else:
            self.info_labels["bw"].setText("N/A")
            self.info_labels["q_factor"].setText("N/A")

    # ---------------------------------------------------------------
    # Crosshair
    # ---------------------------------------------------------------
    def _update_cursor_for_x(self, x: float):
        if self._freq_display.size == 0:
            return

        idx = int(np.argmin(np.abs(self._freq_display - x)))
        freq_val = float(self._freq_display[idx])
        mag_val = float(self._mag_display[idx])
        phase_val = float(self._phase_display_deg[idx])

        self._mag_vline.setPos(freq_val)
        self._mag_hline.setPos(mag_val)
        self._phase_vline.setPos(freq_val)
        self._phase_hline.setPos(phase_val)

        self.crosshair_label_mag.setText(
            f"  {freq_val:.3f} {self._freq_unit}, {mag_val:.3e} {self._mag_unit}")
        self.crosshair_label_mag.setPos(freq_val, mag_val)

        self.crosshair_label_phase.setText(
            f"  {freq_val:.3f} {self._freq_unit}, {phase_val:.2f} deg")
        self.crosshair_label_phase.setPos(freq_val, phase_val)

    def _on_mag_mouse_moved(self, pos):
        if self._last_spectrum is None:
            return
        if not self.mag_plot.sceneBoundingRect().contains(pos):
            return
        vb = self.mag_plot.plotItem.vb
        mouse_point = vb.mapSceneToView(pos)
        self._update_cursor_for_x(float(mouse_point.x()))

    def _on_phase_mouse_moved(self, pos):
        if self._last_spectrum is None:
            return
        if not self.phase_plot.sceneBoundingRect().contains(pos):
            return
        vb = self.phase_plot.plotItem.vb
        mouse_point = vb.mapSceneToView(pos)
        self._update_cursor_for_x(float(mouse_point.x()))

    # ---------------------------------------------------------------
    # Save
    # ---------------------------------------------------------------
    def _on_save_csv(self):
        if self._last_spectrum is None or self._last_spectrum.num_points == 0:
            QMessageBox.information(self, "No Data", "Run a measurement first.")
            return

        path, _ = QFileDialog.getSaveFileName(
            self, "Save Spectrum CSV", "spectrum.csv", "CSV Files (*.csv)")
        if not path:
            return

        try:
            with open(path, "w") as f:
                f.write("freq_kHz,magnitude,phase_rad\n")
                for i in range(self._last_spectrum.num_points):
                    f.write(f"{self._last_spectrum.freq_kHz[i]:.3f},"
                            f"{self._last_spectrum.magnitude[i]:.6e},"
                            f"{self._last_spectrum.phase_rad[i]:.6f}\n")
            self.statusBar().showMessage(f"Saved {self._last_spectrum.num_points} points to {path}")
        except IOError as exc:
            QMessageBox.critical(self, "Save Error", str(exc))

    def _on_save_plot(self):
        if self._last_spectrum is None:
            QMessageBox.information(self, "No Data", "Run a measurement first.")
            return

        path, _ = QFileDialog.getSaveFileName(
            self, "Save Plot Image", "spectrum.png", "PNG (*.png);;SVG (*.svg)")
        if not path:
            return

        try:
            from pyqtgraph.exporters import ImageExporter
            # Export magnitude plot
            exporter = ImageExporter(self.mag_plot.plotItem)
            exporter.parameters()["width"] = 1600
            exporter.export(path)
            self.statusBar().showMessage(f"Plot saved to {path}")
        except Exception as exc:
            QMessageBox.critical(self, "Export Error", str(exc))

    # ---------------------------------------------------------------
    # Cleanup
    # ---------------------------------------------------------------
    def closeEvent(self, event):
        if self.client.is_connected:
            self.client.disconnect()
        event.accept()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main():
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    app.setStyleSheet(STYLESHEET)

    # Set application-wide font
    font = QFont("Segoe UI", 10)
    app.setFont(font)

    window = AFMMainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
