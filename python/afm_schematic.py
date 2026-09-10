#!/usr/bin/env python3
"""
Visual routing schematic for the AFM interface board.

Draws the physical signal chain inside the case - Red Pitaya, interface
board (inputs, PGAs, 4x4 crossbar, math stage) and front-panel BNC
connectors - and lets the user route the crossbar by clicking:

  - Click a disconnected output node, then click an input node to route it.
  - Click a connected output node (or its green route path) to disconnect.
  - Right-click or press Esc to cancel an ongoing selection.

The view re-reads BOARD:STATUS? on open, after every action and via the
Refresh button, so it always mirrors the real board state.

Standalone preview without hardware:
    python afm_schematic.py
"""

import sys

from PyQt6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QGraphicsView, QGraphicsScene,
    QPushButton, QLabel, QMessageBox, QGraphicsRectItem, QGraphicsPathItem,
)
from PyQt6.QtGui import (
    QPainter, QPen, QBrush, QColor, QFont, QPolygonF, QPainterPath,
)
from PyQt6.QtCore import Qt, QPointF, QRectF

from afm_client import AFMError, BoardStatus, OperatingMode

# ---------------------------------------------------------------------------
# Color palette (mirrors afm_gui.py; duplicated to avoid circular imports)
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

WIRE_COLOR = "#55556e"
ACTIVE_COLOR = GREEN
CANDIDATE_COLOR = AMBER

NODE_W = 130
NODE_H = 44
MUX_W = 100
MUX_H = 40


class SchemNode(QGraphicsRectItem):
    """Clickable box in the schematic (RP block, connector, mux, ...)."""

    KIND_RP = "rp"
    KIND_BNC = "bnc"
    KIND_IN = "in"
    KIND_MUX = "mux"
    KIND_OUT = "out"
    KIND_MATH = "math"

    def __init__(self, dialog, kind, index, rect, label, sub="",
                 clickable=False):
        super().__init__(rect)
        self.dialog = dialog
        self.kind = kind
        self.index = index
        self.label = label
        self.sub = sub
        self.clickable = clickable
        self.candidate = False
        self.setAcceptHoverEvents(clickable)
        self.setZValue(2)
        self._apply_style()

    def _apply_style(self):
        if self.kind == self.KIND_RP:
            fill, border = PANEL_BG, ACCENT
        elif self.kind == self.KIND_MATH:
            fill, border = PANEL_BG, TEXT_DIM
        else:
            fill, border = PANEL_BG, BORDER

        pen_color = border
        pen_width = 1.5
        if self.candidate:
            pen_color = CANDIDATE_COLOR
            pen_width = 2.5
        elif self.kind == self.KIND_OUT and self.dialog is not None \
                and self.dialog.routing.get(self.index):
            pen_color = ACTIVE_COLOR
            pen_width = 2.0

        self.setBrush(QBrush(QColor(fill)))
        self.setPen(QPen(QColor(pen_color), pen_width))

        if self.clickable:
            self.setCursor(Qt.CursorShape.PointingHandCursor)

    def set_candidate(self, on):
        self.candidate = on
        self._apply_style()
        self.update()

    def refresh(self):
        self._apply_style()
        self.update()

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        super().paint(painter, option, widget)

        font = QFont("Segoe UI", 9)
        font.setBold(True)
        painter.setFont(font)
        painter.setPen(QColor(TEXT))
        rect = self.rect()

        if self.sub:
            main_rect = QRectF(rect.x(), rect.y() + 4, rect.width(),
                               rect.height() / 2 - 2)
            painter.drawText(main_rect, Qt.AlignmentFlag.AlignCenter,
                             self.label)
            small = QFont("Segoe UI", 8)
            small.setBold(False)
            painter.setFont(small)

            if self.kind == self.KIND_IN:
                gain = self.dialog.gains.get(self.index, "?") \
                    if self.dialog else "?"
                sub_text = f"{self.sub}   ×{gain}"
                painter.setPen(QColor(CANDIDATE_COLOR))
            else:
                sub_text = self.sub
                painter.setPen(QColor(TEXT_DIM))
            sub_rect = QRectF(rect.x(), rect.y() + rect.height() / 2,
                              rect.width(), rect.height() / 2 - 3)
            painter.drawText(sub_rect, Qt.AlignmentFlag.AlignCenter,
                             sub_text)
        else:
            painter.drawText(rect, Qt.AlignmentFlag.AlignCenter, self.label)

    def hoverEnterEvent(self, event):
        if self.clickable and not self.candidate:
            self.setPen(QPen(QColor(ACCENT_HOVER), 2.0))
        super().hoverEnterEvent(event)

    def hoverLeaveEvent(self, event):
        self.refresh()
        super().hoverLeaveEvent(event)

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and self.clickable:
            self.dialog.on_node_clicked(self.kind, self.index)
            event.accept()
            return
        super().mousePressEvent(event)


class SchemEdge(QGraphicsPathItem):
    """Wire between nodes. Active routes are clickable to disconnect."""

    ROLE_WIRE = "wire"
    ROLE_ACTIVE = "active"
    ROLE_CANDIDATE = "candidate"

    def __init__(self, path, role=ROLE_WIRE, dialog=None, out=None,
                 arrow=True, width=1.5, dashed=False):
        super().__init__(path)
        self.dialog = dialog
        self.out = out
        self.arrow = arrow
        self.base_width = width
        self.base_dashed = dashed
        self.role = role
        self.setZValue(1)
        self._apply_style()

    def _apply_style(self):
        if self.role == self.ROLE_ACTIVE:
            pen = QPen(QColor(ACTIVE_COLOR), 3.0)
        elif self.role == self.ROLE_CANDIDATE:
            pen = QPen(QColor(CANDIDATE_COLOR), 2.5, Qt.PenStyle.DashLine)
        else:
            style = Qt.PenStyle.DashLine if self.base_dashed \
                else Qt.PenStyle.SolidLine
            pen = QPen(QColor(WIRE_COLOR), self.base_width, style)
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        self.setPen(pen)
        if self.role == self.ROLE_ACTIVE and self.dialog is not None:
            self.setCursor(Qt.CursorShape.PointingHandCursor)
            self.setAcceptHoverEvents(True)
        else:
            self.unsetCursor()
            self.setAcceptHoverEvents(False)

    def set_role(self, role):
        self.role = role
        self._apply_style()
        self.update()

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        super().paint(painter, option, widget)

        if not self.arrow or self.path().length() < 12:
            return

        import math
        angle = self.path().angleAtPercent(1.0)
        end = self.path().currentPosition()
        rad = math.pi * angle / 180.0
        size = 8.0
        dx, dy = math.cos(rad), -math.sin(rad)
        perp_x, perp_y = -dy, dx
        tip_back = end - QPointF(dx * size, dy * size)
        arrow_poly = QPolygonF([
            end,
            tip_back + QPointF(perp_x * size / 2, perp_y * size / 2),
            tip_back - QPointF(perp_x * size / 2, perp_y * size / 2),
        ])
        painter.setBrush(QBrush(self.pen().color()))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawPolygon(arrow_poly)

    def hoverEnterEvent(self, event):
        if self.role == self.ROLE_ACTIVE:
            pen = self.pen()
            pen.setWidthF(4.5)
            pen.setColor(QColor("#4ade80"))
            self.setPen(pen)
        super().hoverEnterEvent(event)

    def hoverLeaveEvent(self, event):
        self._apply_style()
        self.update()
        super().hoverLeaveEvent(event)

    def mousePressEvent(self, event):
        if self.role == self.ROLE_ACTIVE and self.dialog is not None \
                and event.button() == Qt.MouseButton.LeftButton:
            self.dialog.on_route_clicked(self.out)
            event.accept()
            return
        super().mousePressEvent(event)


class RoutingSchematicDialog(QDialog):
    """Schematic of Red Pitaya -> interface board -> front panel."""

    def __init__(self, client, parent=None):
        super().__init__(parent)
        self.client = client
        self.routing = {i: None for i in range(1, 5)}
        self.gains = {i: "?" for i in range(1, 5)}
        self.selecting_out = None

        self.setWindowTitle("Board Routing Schematic")
        self.resize(1000, 740)
        self.setStyleSheet(f"QDialog {{ background-color: {DARK_BG}; }}")

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(6)

        toolbar = QHBoxLayout()
        title = QLabel("Signal routing - click OUT then IN to route · "
                       "click a green link to disconnect")
        title.setStyleSheet(f"color: {TEXT_DIM};")
        toolbar.addWidget(title)
        toolbar.addStretch()
        self.refresh_btn = QPushButton("Refresh")
        self.refresh_btn.clicked.connect(self.refresh_state)
        toolbar.addWidget(self.refresh_btn)
        layout.addLayout(toolbar)

        self.scene = QGraphicsScene(self)
        self.view = QGraphicsView(self.scene)
        self.view.setRenderHint(QPainter.RenderHint.Antialiasing)
        self.view.setBackgroundBrush(QBrush(QColor(DARK_BG)))
        self.view.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.view, 1)

        self.status_label = QLabel("Reading board state…")
        self.status_label.setStyleSheet(f"color: {TEXT_DIM};")
        layout.addWidget(self.status_label)

        self.nodes = {}
        self.route_edges = {}
        self._build_scene()
        self.refresh_state()

    # ---------------------------------------------------------------
    # Scene construction
    # ---------------------------------------------------------------
    def _build_scene(self):
        rp = SchemNode(
            self, SchemNode.KIND_RP, 0, QRectF(20, 80, 170, 420),
            "RED PITAYA", "STEMlab 125-14")
        self.scene.addItem(rp)
        self.nodes["rp"] = rp

        in_defs = [
            (1, "IN1", "SMA · RP link"),
            (2, "IN2", "SMA · RP link"),
            (3, "IN3", "BNC"),
            (4, "IN4", "BNC"),
        ]
        in_y = {1: 120, 2: 185, 3: 270, 4: 335}
        for idx, name, conn in in_defs:
            node = SchemNode(
                self, SchemNode.KIND_IN, idx,
                QRectF(320, in_y[idx], NODE_W, NODE_H),
                name, conn, clickable=True)
            self.scene.addItem(node)
            self.nodes[f"in{idx}"] = node

        out_defs = [
            (1, "OUT1", "SMA · RP link"),
            (2, "OUT2", "SMA · RP link"),
            (3, "OUT3", "BNC"),
            (4, "OUT4", "BNC"),
        ]
        out_y = {1: 120, 2: 185, 3: 250, 4: 315}
        for idx, name, conn in out_defs:
            node = SchemNode(
                self, SchemNode.KIND_OUT, idx,
                QRectF(720, out_y[idx], NODE_W, NODE_H),
                name, conn, clickable=True)
            self.scene.addItem(node)
            self.nodes[f"out{idx}"] = node

        mux_y = {1: 122, 2: 187, 3: 252, 4: 317}
        for idx in range(1, 5):
            node = SchemNode(
                self, SchemNode.KIND_MUX, idx,
                QRectF(550, mux_y[idx], MUX_W, MUX_H),
                f"MUX{idx}", "ADG1409")
            self.scene.addItem(node)
            self.nodes[f"mux{idx}"] = node

        sum_node = SchemNode(
            self, SchemNode.KIND_MATH, 0, QRectF(720, 395, NODE_W, NODE_H),
            "SUM Out", "BNC · MUX1+MUX2")
        self.scene.addItem(sum_node)
        self.nodes["sum"] = sum_node
        sub_node = SchemNode(
            self, SchemNode.KIND_MATH, 0, QRectF(720, 455, NODE_W, NODE_H),
            "SUB Out", "BNC · MUX3-MUX4")
        self.scene.addItem(sub_node)
        self.nodes["sub"] = sub_node

        for idx in (3, 4):
            bnc = SchemNode(
                self, SchemNode.KIND_BNC, idx,
                QRectF(220, in_y[idx] + NODE_H / 2 - 15, 70, 30),
                f"BNC {idx}", "", clickable=False)
            self.scene.addItem(bnc)
            self.nodes[f"bnc{idx}"] = bnc

        bus_x = 495
        bus_path = QPainterPath()
        bus_path.moveTo(bus_x, 110)
        bus_path.lineTo(bus_x, 350)
        bus = SchemEdge(bus_path, width=3.0, arrow=False)
        self.scene.addItem(bus)

        dac_y = {1: 140, 2: 205}
        stub_defs = [
            ("dac1", QPointF(190, dac_y[1]), "in1", QPointF(320, in_y[1] + NODE_H / 2)),
            ("dac2", QPointF(190, dac_y[2]), "in2", QPointF(320, in_y[2] + NODE_H / 2)),
            ("bnc3", QPointF(290, in_y[3] + NODE_H / 2), "in3",
             QPointF(320, in_y[3] + NODE_H / 2)),
            ("bnc4", QPointF(290, in_y[4] + NODE_H / 2), "in4",
             QPointF(320, in_y[4] + NODE_H / 2)),
        ]
        for key, start, target, end in stub_defs:
            path = QPainterPath(start)
            mid_x = (start.x() + end.x()) / 2
            path.cubicTo(QPointF(mid_x, start.y()),
                         QPointF(mid_x, end.y()), end)
            self.scene.addItem(SchemEdge(path))

        for idx in range(1, 5):
            y_in = in_y[idx] + NODE_H / 2
            p = QPainterPath(QPointF(320 + NODE_W, y_in))
            p.lineTo(bus_x, y_in)
            self.scene.addItem(SchemEdge(p, arrow=False))
            y_mux = mux_y[idx] + MUX_H / 2
            p = QPainterPath(QPointF(bus_x, y_in))
            p.lineTo(bus_x, y_mux)
            p.lineTo(QPointF(550, y_mux))
            self.scene.addItem(SchemEdge(p))
            p = QPainterPath(QPointF(550 + MUX_W, y_mux))
            p.lineTo(QPointF(720, out_y[idx] + NODE_H / 2))
            mux_out_edge = SchemEdge(p)
            self.scene.addItem(mux_out_edge)
            self.route_edges[idx] = [mux_out_edge]

        math_edges = [
            (1, sum_node.rect().topLeft() + QPointF(0, NODE_H / 2)),
            (2, sum_node.rect().topLeft() + QPointF(0, NODE_H / 2)),
            (3, sub_node.rect().topLeft() + QPointF(0, NODE_H / 2)),
            (4, sub_node.rect().topLeft() + QPointF(0, NODE_H / 2)),
        ]
        for mux_idx, target in math_edges:
            y_mux = mux_y[mux_idx] + MUX_H / 2
            start = QPointF(550 + MUX_W, y_mux)
            path = QPainterPath(start)
            path.cubicTo(QPointF((start.x() + target.x()) / 2, start.y()),
                         QPointF((start.x() + target.x()) / 2, target.y()),
                         target)
            edge = SchemEdge(path, dashed=True)
            self.scene.addItem(edge)
            self.route_edges[mux_idx].append(edge)

        for name, start, lane_x, lane_y, entry_x in [
                ("ADC1", QPointF(850, out_y[1] + NODE_H / 2), 920, 610, 120),
                ("ADC2", QPointF(850, out_y[2] + NODE_H / 2), 935, 628, 70)]:
            path = QPainterPath(start)
            path.lineTo(lane_x, start.y())
            path.lineTo(lane_x, lane_y)
            path.lineTo(entry_x, lane_y)
            path.lineTo(entry_x, 500)
            self.scene.addItem(SchemEdge(path, dashed=True))
            lbl = self.scene.addText(name, QFont("Segoe UI", 8))
            lbl.setDefaultTextColor(QColor(TEXT_DIM))
            lbl.setPos(entry_x + 6, 478)

        for idx in (1, 2):
            lbl = self.scene.addText(f"DAC{idx}", QFont("Segoe UI", 8))
            lbl.setDefaultTextColor(QColor(TEXT_DIM))
            lbl.setPos(196, dac_y[idx] - 18)

        self.active_paths = {}
        for idx in range(1, 5):
            overlay = SchemEdge(self._active_route_path(1, idx),
                                dialog=self, out=idx)
            self.scene.addItem(overlay)
            self.active_paths[idx] = overlay

        legend_y = 670
        legend_items = [
            (ACTIVE_COLOR, "active route"),
            (WIRE_COLOR, "fixed wiring"),
            (CANDIDATE_COLOR, "selectable now"),
        ]
        x = 40
        for color, text in legend_items:
            item = self.scene.addRect(
                QRectF(x, legend_y, 18, 4), QPen(Qt.PenStyle.NoPen),
                QBrush(QColor(color)))
            item.setZValue(0)
            lbl = self.scene.addText(text, QFont("Segoe UI", 8))
            lbl.setDefaultTextColor(QColor(TEXT_DIM))
            lbl.setPos(x + 24, legend_y - 8)
            x += 160

    # ---------------------------------------------------------------
    # State sync
    # ---------------------------------------------------------------
    def set_client(self, client):
        """Point the dialog at a connected client, or None when offline."""
        self.client = client

    def refresh_state(self):
        if self.client is None:
            self.status_label.setText("Not connected")
            self.status_label.setStyleSheet(f"color: {RED};")
            return
        try:
            mode = self.client.get_mode()
        except AFMError:
            mode = None
        if mode == OperatingMode.RP_ONLY:
            self.routing = {i: None for i in range(1, 5)}
            self.gains = {i: "-" for i in range(1, 5)}
            self.cancel_selection()
            self._redraw_routes()
            self.status_label.setText("RP-only mode: board routing unavailable")
            self.status_label.setStyleSheet(f"color: {TEXT_DIM};")
            return
        try:
            state = self.client.get_board_state()
        except AFMError as exc:
            self.status_label.setText(f"Status error: {exc}")
            self.status_label.setStyleSheet(f"color: {RED};")
            return
        self.routing = {i: state.routing.get(i) for i in range(1, 5)}
        self.gains = {i: state.gains.get(i, "?") for i in range(1, 5)}
        self.cancel_selection()
        self._redraw_routes()
        parts = [f"OUT{i}←{'IN' + str(v) if v else 'open'}"
                 for i, v in self.routing.items()]
        self.status_label.setText("Board state:   " + "    ".join(parts))
        self.status_label.setStyleSheet(f"color: {TEXT};")

    def _redraw_routes(self):
        for idx in range(1, 5):
            for edge in self.route_edges[idx]:
                edge.set_role(SchemEdge.ROLE_WIRE)
            self.active_paths[idx].set_role(SchemEdge.ROLE_WIRE)
            self.active_paths[idx].setVisible(False)
            self.nodes[f"mux{idx}"].refresh()
            self.nodes[f"out{idx}"].refresh()
        for idx, inp in self.routing.items():
            if inp is None:
                continue
            self.active_paths[idx].setPath(self._active_route_path(inp, idx))
            self.active_paths[idx].set_role(SchemEdge.ROLE_ACTIVE)
            self.active_paths[idx].setVisible(True)
            for edge in self.route_edges[idx]:
                edge.set_role(SchemEdge.ROLE_ACTIVE)
        for idx in range(1, 5):
            self.nodes[f"in{idx}"].refresh()

    def _active_route_path(self, inp, out):
        y_in = self._in_center_y(inp)
        y_mux = self._mux_center_y(out)
        bus_x = 495
        p = QPainterPath(QPointF(320 + NODE_W, y_in))
        p.lineTo(bus_x, y_in)
        p.lineTo(bus_x, y_mux)
        p.lineTo(QPointF(550 + MUX_W, y_mux))
        return p

    def _in_center_y(self, idx):
        ys = {1: 120, 2: 185, 3: 270, 4: 335}
        return ys[idx] + NODE_H / 2

    def _mux_center_y(self, idx):
        ys = {1: 122, 2: 187, 3: 252, 4: 317}
        return ys[idx] + MUX_H / 2

    # ---------------------------------------------------------------
    # Interaction
    # ---------------------------------------------------------------
    def on_node_clicked(self, kind, index):
        if kind == SchemNode.KIND_OUT:
            if self.selecting_out == index:
                self.cancel_selection()
                return
            if self.routing.get(index):
                self._confirm_disconnect(index)
            else:
                self.selecting_out = index
                for i in range(1, 5):
                    self.nodes[f"in{i}"].set_candidate(True)
                self.status_label.setText(
                    f"Select an input to route into OUT{index}"
                    "  (Esc or right-click to cancel)")
                self.status_label.setStyleSheet(f"color: {AMBER};")
        elif kind == SchemNode.KIND_IN:
            if self.selecting_out is not None:
                self._route(self.selecting_out, index)
            else:
                users = [o for o, v in self.routing.items() if v == index]
                where = ", ".join(f"OUT{o}" for o in users) \
                    if users else "not routed"
                self.status_label.setText(f"IN{index}: {where}")
                self.status_label.setStyleSheet(f"color: {TEXT_DIM};")

    def on_route_clicked(self, out):
        self._confirm_disconnect(out)

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.RightButton \
                and self.selecting_out is not None:
            self.cancel_selection()
        super().mousePressEvent(event)

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Escape and self.selecting_out is not None:
            self.cancel_selection()
            return
        super().keyPressEvent(event)

    def cancel_selection(self):
        self.selecting_out = None
        for i in range(1, 5):
            node = self.nodes.get(f"in{i}")
            if node:
                node.set_candidate(False)

    def _route(self, out, inp):
        try:
            self.client.set_mux(out, inp)
        except AFMError as exc:
            self.status_label.setText(f"Route error: {exc}")
            self.status_label.setStyleSheet(f"color: {RED};")
            self.cancel_selection()
            return
        self.cancel_selection()
        self.refresh_state()

    def _confirm_disconnect(self, out):
        reply = QMessageBox.question(
            self, "Disconnect",
            f"Disconnect OUT{out} (currently IN{self.routing.get(out)})?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
        if reply != QMessageBox.StandardButton.Yes:
            return
        try:
            self.client.disconnect_mux(out)
        except AFMError as exc:
            self.status_label.setText(f"Disconnect error: {exc}")
            self.status_label.setStyleSheet(f"color: {RED};")
            return
        self.refresh_state()


# ---------------------------------------------------------------------------
# Mock client for hardware-free preview
# ---------------------------------------------------------------------------
class MockAFMClient:
    """In-memory stand-in for AFMClient used by the standalone preview."""

    def __init__(self):
        self.is_connected = True
        self._routing = {1: None, 2: None, 3: None, 4: None}
        self._gains = {1: "1", 2: "2", 3: "1", 4: "1/8"}

    def get_mode(self) -> OperatingMode:
        return OperatingMode.FULL

    def get_board_status(self) -> str:
        lines = ["=== MUX Status ===", "Routing:"]
        for out in range(1, 5):
            inp = self._routing[out]
            lines.append(
                f"  OUT{out} <- IN{inp}" if inp
                else f"  OUT{out} <- X (disconnected)")
        lines.append("Gains:")
        for ch in range(1, 5):
            lines.append(f"  IN{ch}: x{self._gains[ch]}")
        lines.append("==================")
        return "\n".join(lines)

    def get_board_state(self) -> BoardStatus:
        return BoardStatus.from_response(self.get_board_status())

    def set_mux(self, output, input_ch):
        for out, inp in self._routing.items():
            if inp == input_ch and out != output:
                self._routing[out] = None
        self._routing[output] = input_ch

    def disconnect_mux(self, output):
        self._routing[output] = None


def main():
    from PyQt6.QtWidgets import QApplication
    app = QApplication(sys.argv)
    dialog = RoutingSchematicDialog(MockAFMClient())
    dialog.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
