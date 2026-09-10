#!/usr/bin/env python3
"""
AFM Client Library - Python client for AFM SCPI measurement server on Red Pitaya

The server implements an SCPI-inspired (Standard Commands for Programmable
Instruments) protocol over TCP. Commands are case-insensitive, hierarchical,
and use colon-separated keywords with comma-separated arguments.

Usage:
    from afm_client import AFMClient

    # Connect to server
    client = AFMClient('192.168.1.100', 5025)
    client.connect()

    # Initialize hardware
    client.init()

    # Configure board
    client.set_mux(1, 1)       # Route input 1 to output 1
    client.set_gain(1, 2)       # Set channel 1 to gain index 2 (1x)

    # Broadband sinc measurement (returns SpectrumData)
    spectrum = client.measure_sinc(center_kHz=1000, bandwidth_kHz=200)
    print(f"Peak at {spectrum.freq_kHz[spectrum.magnitude.argmax()]:.1f} kHz")

    # Frequency sweep measurement
    sweep = client.measure_sweep(center_kHz=1000, range_kHz=100, step_kHz=0.5)

    # Cleanup
    client.deinit()
    client.disconnect()
"""

import logging
import math
import re
import socket
import threading
from contextlib import contextmanager
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional, Union

import numpy as np

# Library code must not configure the root logger; applications set that up.
logger = logging.getLogger(__name__)
logger.addHandler(logging.NullHandler())


class AFMError(Exception):
    """Base exception for AFM client errors"""
    pass


class AFMConnectionError(AFMError):
    """Connection-related errors"""
    pass


class AFMCommandError(AFMError):
    """Command execution errors reported by the server as ERR_*."""

    def __init__(self, response: str):
        super().__init__(response)
        self.response = response
        match = re.match(r'^(ERR_[A-Z_]+)(?::\s*(.*))?$', response)
        self.status = match.group(1) if match else None
        self.message = (match.group(2) or "") if match else response


class AFMProtocolError(AFMError):
    """Malformed or oversized response that breaks the wire protocol."""
    pass


class AFMBusyError(AFMError):
    """Another command or measurement is already using this connection."""
    pass


class AFMTimeoutError(AFMError):
    """Timeout errors"""
    pass


class GainSetting(Enum):
    """Gain settings matching the server's GainSetting enum"""
    GAIN_1_8 = 0
    GAIN_1_4 = 1
    GAIN_1_2 = 2
    GAIN_1 = 3  
    GAIN_2 = 4  
    GAIN_4 = 5  
    GAIN_8 = 6  
    GAIN_16 = 7


@dataclass
class BoardStatus:
    """Electronic board state parsed from BOARD:STATUS? text block.

    routing maps output connector number (1-4) to input connector number
    (1-4), or None when the output is disconnected. gains maps input
    connector number (1-4) to its gain label string ("1/8" ... "16").
    """
    routing: dict = field(default_factory=dict)
    gains: dict = field(default_factory=dict)

    @classmethod
    def from_response(cls, response: str) -> 'BoardStatus':
        """Parse the multi-line MUX status block printed by the board."""
        status = cls()
        for line in response.splitlines():
            line = line.strip()
            m = re.match(r'^OUT(\d)\s*<-\s*IN(\d)', line)
            if m:
                status.routing[int(m.group(1))] = int(m.group(2))
                continue
            m = re.match(r'^OUT(\d)\s*<-\s*X', line)
            if m:
                status.routing[int(m.group(1))] = None
                continue
            m = re.match(r'^IN(\d):\s*x(\S+)', line)
            if m:
                status.gains[int(m.group(1))] = m.group(2)
        return status


@dataclass
class SpectrumData:
    """Spectrum measurement result from MEASURE:SINC or MEASURE:SWEEP"""
    freq_kHz: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float64))
    magnitude: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float64))
    phase_rad: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float64))

    @property
    def num_points(self) -> int:
        return len(self.freq_kHz)


@dataclass
class SystemStatus:
    """System status information from server"""
    hardware_initialized: bool = False
    board_connected: bool = False
    measurement_in_progress: bool = False
    decimation: int = 64

    @classmethod
    def from_response(cls, response: str) -> 'SystemStatus':
        """Parse status from server response"""
        status = cls()
        parts = response.split()

        for part in parts:
            if '=' in part:
                key, value = part.split('=', 1)
                if key == 'HW_INIT':
                    status.hardware_initialized = value == '1'
                elif key == 'BOARD':
                    status.board_connected = value == '1'
                elif key == 'BUSY':
                    status.measurement_in_progress = value == '1'
                elif key == 'DEC':
                    try:
                        status.decimation = int(value)
                    except ValueError:
                        pass

        return status


class AFMClient:
    """
    Client for communicating with the AFM measurement server on Red Pitaya.

    Commands are serialized with a non-blocking lock: concurrent callers get
    ``AFMBusyError`` instead of interleaving bytes on the shared socket. A
    measurement holds the lock for its full duration, so UIs must not issue
    other commands while one is running. ``disconnect()`` may be called from
    any thread to abort an in-flight operation.
    """

    DEFAULT_PORT = 5025
    DEFAULT_TIMEOUT = 10.0
    RECV_BUFFER_SIZE = 65536
    MAX_LINE_BYTES = 1 << 20
    MAX_SPECTRUM_ROWS = 65536
    MAX_SPECTRUM_BYTES = 8 << 20
    # Mirrors AFM::ServerConfig::MAX_SWEEP_POINTS in cpp/Server/Protocol.h
    MAX_SWEEP_POINTS = 4096
    MAX_BOARD_STATUS_LINES = 256

    def __init__(self,
                 host: str,
                 port: int = DEFAULT_PORT,
                 timeout: float = DEFAULT_TIMEOUT):
        """
        Initialize the AFM client.

        Args:
            host: IP address or hostname of the Red Pitaya
            port: TCP port number (default: 5025, SCPI standard)
            timeout: Socket timeout in seconds (default: 10.0)
        """
        self.host = host
        self.port = port
        self.timeout = timeout
        self._socket: Optional[socket.socket] = None
        self._recv_buffer: bytes = b""  # Keep as bytes for binary safety
        self._lock = threading.Lock()

    @property
    def is_connected(self) -> bool:
        """Check if client is connected to server"""
        return self._socket is not None

    def connect(self) -> str:
        """
        Connect to the AFM server.

        Returns:
            Welcome message from server

        Raises:
            AFMConnectionError: If connection fails
        """
        if self._socket:
            raise AFMConnectionError("Already connected")

        try:
            self._socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._socket.settimeout(self.timeout)
            self._socket.connect((self.host, self.port))
            self._recv_buffer = b""
        except socket.error as e:
            self._socket = None
            raise AFMConnectionError(f"Failed to connect to {self.host}:{self.port}: {e}")

        try:
            # Read welcome message
            welcome = self._read_line()
            logger.info(f"Connected to {self.host}:{self.port}")
            logger.info(f"Server: {welcome}")
            return welcome
        except AFMError as e:
            self.disconnect()
            raise AFMConnectionError(
                f"Failed to read welcome from {self.host}:{self.port}: {e}")

    def disconnect(self) -> None:
        """Disconnect from the server.

        Safe to call from another thread; it aborts any in-flight read by
        closing the socket, which surfaces there as AFMConnectionError.
        """
        sock, self._socket = self._socket, None
        if sock is None:
            return
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            sock.close()
        except OSError:
            pass
        self._recv_buffer = b""
        logger.info("Disconnected from server")

    def __enter__(self):
        """Context manager entry"""
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Context manager exit"""
        self.disconnect()
        return False

    # ------ Low-level communication --------

    @contextmanager
    def _transaction(self):
        """Hold the command lock for one complete request/response cycle."""
        if not self._lock.acquire(blocking=False):
            raise AFMBusyError("Another command or measurement is in progress")
        try:
            yield
        finally:
            self._lock.release()

    def _flush_buffer(self) -> None:
        """Drain any stale data from the receive buffer and socket.

        Called before sending a new command to prevent leftover data
        (e.g. from a timed-out sweep) from being interpreted as the
        response to the next command.
        """
        self._recv_buffer = b""
        sock = self._socket
        if sock is None:
            return
        # Drain anything sitting in the OS socket buffer. A concurrent
        # disconnect() can close the socket under us; translate that to the
        # same error callers get from a failed send or read.
        try:
            sock.setblocking(False)
        except OSError as e:
            self.disconnect()
            raise AFMConnectionError(f"Receive failed: {e}")
        try:
            while True:
                data = sock.recv(self.RECV_BUFFER_SIZE)
                if not data:
                    break
        except (BlockingIOError, socket.error):
            pass
        finally:
            try:
                sock.settimeout(self.timeout)
            except OSError:
                pass

    def _send(self, command: str) -> None:
        """Send a command to the server"""
        sock = self._socket
        if sock is None:
            raise AFMConnectionError("Not connected")

        if "\n" in command or "\r" in command:
            raise AFMCommandError("Command must be a single line")
        try:
            message = (command + "\n").encode('ascii')
        except UnicodeEncodeError:
            raise AFMCommandError("Command contains non-ASCII characters")

        try:
            sock.sendall(message)
            logger.debug(f"Sent: {command}")
        except socket.error as e:
            self.disconnect()
            raise AFMConnectionError(f"Send failed: {e}")

    def _read_line(self, timeout_override: Optional[float] = None) -> str:
        """Read a line from the server (up to newline)

        Buffer is kept as bytes to avoid UnicodeDecodeError when binary
        data is mixed with text responses. Decoding happens only when
        extracting a complete line.

        Args:
            timeout_override: If set, temporarily use this timeout (seconds)
                              for the duration of this read, then restore.
        """
        if not self._socket:
            raise AFMConnectionError("Not connected")

        sock = self._socket
        prev_timeout = sock.gettimeout()

        try:
            if timeout_override is not None:
                sock.settimeout(timeout_override)

            # Look for newline in bytes buffer
            while b'\n' not in self._recv_buffer:
                data = sock.recv(self.RECV_BUFFER_SIZE)
                if not data:
                    self.disconnect()
                    raise AFMConnectionError("Connection closed by server")
                self._recv_buffer += data
                if len(self._recv_buffer) > self.MAX_LINE_BYTES:
                    self.disconnect()
                    raise AFMProtocolError(
                        f"Response line exceeds {self.MAX_LINE_BYTES} bytes")

            # Split on newline, decode only the line portion
            line_bytes, self._recv_buffer = self._recv_buffer.split(b'\n', 1)
            try:
                line = line_bytes.decode('ascii').rstrip('\r')
            except UnicodeDecodeError:
                raise AFMProtocolError("Response line is not ASCII text")
            logger.debug(f"Received: {line}")
            return line

        except socket.timeout:
            raise AFMTimeoutError("Read timeout")
        except socket.error as e:
            self.disconnect()
            raise AFMConnectionError(f"Receive failed: {e}")
        finally:
            if timeout_override is not None and sock.fileno() >= 0:
                try:
                    sock.settimeout(prev_timeout)
                except OSError:
                    pass

    def _read_exact(self, num_bytes: int,
                    timeout_override: Optional[float] = None) -> bytes:
        """Read exactly num_bytes bytes from the server.

        Consumes any bytes already sitting in the receive buffer first, then
        reads from the socket until the requested count is satisfied. Used for
        the fast bulk read of spectrum payloads whose size is known from the
        response header.

        Args:
            num_bytes: Exact number of bytes to return.
            timeout_override: If set, temporarily use this timeout (seconds)
                              for the duration of this read, then restore.
        """
        if num_bytes < 0:
            raise AFMProtocolError(f"Invalid byte count: {num_bytes}")
        if not self._socket:
            raise AFMConnectionError("Not connected")

        sock = self._socket
        prev_timeout = sock.gettimeout()

        try:
            if timeout_override is not None:
                sock.settimeout(timeout_override)

            while len(self._recv_buffer) < num_bytes:
                data = sock.recv(self.RECV_BUFFER_SIZE)
                if not data:
                    self.disconnect()
                    raise AFMConnectionError("Connection closed by server")
                self._recv_buffer += data

            chunk = self._recv_buffer[:num_bytes]
            self._recv_buffer = self._recv_buffer[num_bytes:]
            return chunk

        except socket.timeout:
            raise AFMTimeoutError("Read timeout")
        except socket.error as e:
            self.disconnect()
            raise AFMConnectionError(f"Receive failed: {e}")
        finally:
            if timeout_override is not None and sock.fileno() >= 0:
                try:
                    sock.settimeout(prev_timeout)
                except OSError:
                    pass

    def _execute(self, command: str) -> tuple[bool, str]:
        """
        Execute a command and return the response.

        Flushes any stale data in the buffer before sending to prevent
        leftover data from a previous timed-out operation from being
        interpreted as the response.

        Args:
            command: Command string

        Returns:
            Tuple of (success, response_data)
        """
        self._flush_buffer()
        self._send(command)
        response = self._read_line()

        if response == "OK" or response.startswith("OK "):
            # Extract data after "OK "
            data = response[3:] if len(response) > 3 else ""
            if data.startswith("DATA "):
                # Drain the payload so the stream stays in sync, then tell the
                # caller to use the typed measurement API.
                self._discard_spectrum_payload(data)
                raise AFMProtocolError(
                    "Server returned a spectrum response; use measure_sinc() "
                    "or measure_sweep() to read it")
            return True, data
        elif response.startswith("ERR"):
            return False, response
        else:
            return False, f"Unexpected response: {response}"

    def send_command(self, command: str) -> str:
        """
        Send a command and return the response.

        Args:
            command: Command string

        Returns:
            Response data (without OK prefix)

        Raises:
            AFMCommandError: If command fails
            AFMBusyError: If another command or measurement is in progress
        """
        with self._transaction():
            success, data = self._execute(command)
        if not success:
            raise AFMCommandError(data)
        return data

    def _read_board_status_response(self, timeout: Optional[float] = None) -> str:
        """Read the multi-line BOARD:STATUS? response.

        The electronic board replies with a human-readable block that ends
        with a separator line. The first payload line is the board's echoed
        STATUS command, which is skipped here.
        """
        header = self._read_line(timeout_override=timeout)

        if header.startswith("ERR"):
            raise AFMCommandError(header)
        if header != "OK" and not header.startswith("OK "):
            raise AFMProtocolError(f"Unexpected board status header: {header}")

        lines: list[str] = []

        _, _, first_line = header.partition(" ")
        if first_line and first_line != "STATUS":
            lines.append(first_line)

        while True:
            line = self._read_line(timeout_override=timeout)

            if line.startswith("ERR"):
                raise AFMCommandError(line)

            lines.append(line)
            if line == "==================":
                break
            if len(lines) > self.MAX_BOARD_STATUS_LINES:
                raise AFMProtocolError("Board status block has no terminator")

        return "\n".join(lines)

    # ------ System Commands --------

    def idn(self) -> str:
        """IEEE 488.2 identification query (*IDN?)"""
        return self.send_command("*IDN?")

    def reset(self) -> None:
        """IEEE 488.2 reset (*RST) - returns hardware to power-on state"""
        self.send_command("*RST")

    def opc(self) -> bool:
        """IEEE 488.2 operation complete query (*OPC?)"""
        return self.send_command("*OPC?") == "1"

    def ping(self) -> bool:
        """
        Test connection to server.

        Returns:
            True if server responds with PONG. A server error reply counts
            as no response; AFMBusyError and connection errors propagate.
        """
        try:
            response = self.send_command("SYSTEM:PING")
            return response == "PONG"
        except AFMCommandError:
            return False

    def get_version(self) -> str:
        """Get server version string"""
        return self.send_command("SYSTEM:VERSION")

    def get_status(self) -> SystemStatus:
        """Get current system status"""
        response = self.send_command("SYSTEM:STATUS?")
        return SystemStatus.from_response(response)

    def init(self) -> str:
        """
        Initialize hardware.

        Returns:
            Initialization status message
        """
        return self.send_command("SYSTEM:INIT")

    def deinit(self) -> str:
        """Deinitialize hardware"""
        return self.send_command("SYSTEM:DEINIT")

    def shutdown(self) -> None:
        """Shutdown the server.

        Raises:
            AFMBusyError: If a measurement or command is in progress; the
                connection is left open so shutdown can be retried.
        """
        try:
            with self._transaction():
                self._send("SYSTEM:SHUTDOWN")
                # Server will close connection
                self._read_line()
        except AFMBusyError:
            raise
        except AFMError:
            pass
        self.disconnect()

    # ------ Electronic Board Commands --------

    def set_mux(self, output: int, input_ch: int) -> None:
        """
        Set multiplexer routing.

        Args:
            output: Output channel (1-4, matching board connector label OUT1-OUT4)
            input_ch: Input channel (1-4, matching board connector label IN1-IN4)
        """
        self.send_command(f"BOARD:MUX:ROUTE {output},{input_ch}")

    def disconnect_mux(self, output: int) -> None:
        """
        Disconnect a multiplexer output.

        Args:
            output: Output channel (1-4, matching board connector label OUT1-OUT4)
        """
        self.send_command(f"BOARD:MUX:DISCONNECT {output}")

    def set_gain(self, channel: int, gain: Union[int, GainSetting]) -> str:
        """
        Set amplifier gain for a channel.

        Args:
            channel: Channel number (1-4, matching board connector label IN1-IN4)
            gain: Gain index (0-7) or GainSetting enum

        Returns:
            Gain value string (e.g., "1/4", "1", "8")
        """
        if isinstance(gain, GainSetting):
            gain = gain.value
        return self.send_command(f"BOARD:GAIN {channel},{gain}")

    def reset_board(self) -> str:
        """Reset electronic board to defaults"""
        return self.send_command("BOARD:RESET")

    def get_board_status(self) -> str:
        """Get electronic board status as a multi-line text block."""
        with self._transaction():
            self._flush_buffer()
            self._send("BOARD:STATUS?")
            return self._read_board_status_response()

    def get_board_state(self) -> BoardStatus:
        """Get electronic board routing and gains as a parsed BoardStatus."""
        return BoardStatus.from_response(self.get_board_status())

    # ------ Measurement Commands --------

    def _parse_spectrum_header(self, header: str) -> tuple[int, Optional[int]]:
        """Parse an 'OK DATA <rows> [<bytes>]' header.

        Returns (num_rows, num_bytes); num_bytes is None for servers older
        than 2.2.0 that omit the payload byte count.
        """
        parts = header.split()
        if len(parts) < 3 or parts[0] != "OK" or parts[1] != "DATA":
            raise AFMProtocolError(f"Unexpected spectrum header: {header}")
        try:
            num_rows = int(parts[2])
            num_bytes = int(parts[3]) if len(parts) >= 4 else None
        except ValueError:
            raise AFMProtocolError(f"Malformed spectrum header: {header}")
        if num_rows < 0 or num_rows > self.MAX_SPECTRUM_ROWS:
            raise AFMProtocolError(f"Spectrum row count out of range: {num_rows}")
        if num_bytes is not None and not 0 <= num_bytes <= self.MAX_SPECTRUM_BYTES:
            raise AFMProtocolError(f"Spectrum byte count out of range: {num_bytes}")
        return num_rows, num_bytes

    def _discard_spectrum_payload(self, data: str) -> None:
        """Consume a spectrum payload received in reply to a raw command."""
        num_rows, num_bytes = self._parse_spectrum_header("OK " + data)
        if num_bytes is not None:
            self._read_exact(num_bytes)
            return
        for _ in range(num_rows):
            self._read_line()

    @staticmethod
    def _parse_spectrum_row(line: str, index: int) -> tuple[float, float, float]:
        """Parse one 'freq,magnitude,phase' row."""
        values = line.split(',')
        if len(values) != 3:
            raise AFMProtocolError(
                f"Malformed spectrum row {index}: expected 3 values, got {len(values)}"
            )
        try:
            return float(values[0]), float(values[1]), float(values[2])
        except ValueError:
            raise AFMProtocolError(f"Malformed spectrum row {index}: {line!r}")

    def _read_spectrum_response(self, timeout: Optional[float] = None) -> SpectrumData:
        """Read a multi-line spectrum response.

        Expected format (server >= 2.2.0):
            OK DATA <N> <BYTES>
            freq_kHz,magnitude,phase_rad
            ... (N lines)

        <BYTES> is the exact size of the payload that follows the header line,
        enabling a single bulk read instead of scanning line by line. When the
        header omits <BYTES> (older servers), this falls back to reading the N
        rows one line at a time.

        Args:
            timeout: Socket timeout for this read operation. If None, uses
                     the default client timeout.

        Returns:
            SpectrumData with parsed arrays
        """
        header = self._read_line(timeout_override=timeout)

        if header.startswith("ERR"):
            raise AFMCommandError(header)
        if not header.startswith("OK"):
            raise AFMProtocolError(f"Unexpected spectrum header: {header}")

        num_rows, num_bytes = self._parse_spectrum_header(header)

        # Fast path: header carries the payload byte count -> single bulk read.
        if num_bytes is not None:
            payload = self._read_exact(num_bytes, timeout_override=timeout)
            try:
                text = payload.decode('ascii')
            except UnicodeDecodeError:
                raise AFMProtocolError("Spectrum payload is not ASCII text")
            return self._parse_spectrum_payload(text, num_rows)

        # Fallback: read the N rows line by line (server < 2.2.0).
        freq = np.empty(num_rows, dtype=np.float64)
        mag = np.empty(num_rows, dtype=np.float64)
        phase = np.empty(num_rows, dtype=np.float64)

        for i in range(num_rows):
            line = self._read_line(timeout_override=timeout)
            freq[i], mag[i], phase[i] = self._parse_spectrum_row(line, i)

        return SpectrumData(freq_kHz=freq, magnitude=mag, phase_rad=phase)

    @classmethod
    def _parse_spectrum_payload(cls, payload: str, num_rows: int) -> SpectrumData:
        """Parse a CSV spectrum payload (N rows of freq,magnitude,phase)."""
        lines = payload.split('\n')
        # A trailing '\n' on the last row produces a final empty element.
        if lines and lines[-1] == '':
            lines.pop()

        if len(lines) != num_rows:
            raise AFMProtocolError(
                f"Spectrum payload row count mismatch: header said {num_rows}, "
                f"got {len(lines)}"
            )

        freq = np.empty(num_rows, dtype=np.float64)
        mag = np.empty(num_rows, dtype=np.float64)
        phase = np.empty(num_rows, dtype=np.float64)

        for i, line in enumerate(lines):
            freq[i], mag[i], phase[i] = cls._parse_spectrum_row(line, i)

        return SpectrumData(freq_kHz=freq, magnitude=mag, phase_rad=phase)

    def measure_sinc(
        self,
        center_kHz: float,
        bandwidth_kHz: float,
        num_samples: int = 8192,
        decimation: int = 64,
        amplitude: float = 1.0,
    ) -> SpectrumData:
        """
        Perform a broadband sinc measurement with FFT analysis.

        Generates a sinc excitation signal centered at center_kHz with the
        specified bandwidth, acquires the response, computes the FFT, and
        returns the spectrum within the bandwidth range.

        Args:
            center_kHz: Center frequency in kHz
            bandwidth_kHz: Bandwidth in kHz
            num_samples: Number of samples (1-65536, default 8192)
            decimation: FPGA decimation factor (power of 2, 16-1024, default 64)
            amplitude: Signal amplitude 0-1 (default 1.0)

        Returns:
            SpectrumData with freq_kHz, magnitude, and phase_rad arrays

        Raises:
            AFMCommandError: If measurement fails
            AFMTimeoutError: If server doesn't respond in time
            AFMBusyError: If another command or measurement is in progress
        """
        cmd = (f"MEASURE:SINC {center_kHz},{bandwidth_kHz},"
               f"{num_samples},{decimation},{amplitude}")
        with self._transaction():
            self._flush_buffer()
            self._send(cmd)
            return self._read_spectrum_response()

    def measure_sweep(
        self,
        center_kHz: float,
        range_kHz: float,
        step_kHz: float = 1.0,
        decimation: int = 64,
        amplitude: float = 1.0,
    ) -> SpectrumData:
        """
        Perform a frequency sweep measurement.

        Steps through frequencies from (center - range/2) to (center + range/2),
        generating a sine wave at each frequency and measuring amplitude/phase
        via lock-in detection.

        Args:
            center_kHz: Center frequency in kHz
            range_kHz: Total sweep range in kHz
            step_kHz: Frequency step in kHz (default 1.0)
            decimation: FPGA decimation factor (power of 2, 16-1024, default 64)
            amplitude: Signal amplitude 0-1 (default 1.0)

        Returns:
            SpectrumData with freq_kHz, magnitude, and phase_rad arrays

        Raises:
            AFMCommandError: If measurement fails
            AFMTimeoutError: If server doesn't respond in time
            AFMBusyError: If another command or measurement is in progress
        """
        # Calculate dynamic timeout based on sweep parameters. The server caps
        # sweeps at MAX_SWEEP_POINTS, so mirror that to bound the timeout even
        # when the user passes a huge or non-finite range/step.
        num_steps = 1
        if math.isfinite(range_kHz) and math.isfinite(step_kHz) and step_kHz > 0:
            ratio = range_kHz / step_kHz
            if math.isfinite(ratio) and ratio >= 0:
                num_steps = int(ratio) + 1
        num_steps = min(max(num_steps, 1), self.MAX_SWEEP_POINTS)
        estimated_time = num_steps * 2.0
        sweep_timeout = max(self.timeout, estimated_time + 30.0)
        logger.info(f"Sweep: {num_steps} steps, timeout set to {sweep_timeout:.0f}s")

        cmd = (f"MEASURE:SWEEP {center_kHz},{range_kHz},"
               f"{step_kHz},{decimation},{amplitude}")
        with self._transaction():
            self._flush_buffer()
            self._send(cmd)
            return self._read_spectrum_response(timeout=sweep_timeout)

# ------ Convenience Functions --------

def quick_connect(host: str, port: int = 5025) -> AFMClient:
    """
    Quick connect and initialize.

    Args:
        host: Red Pitaya IP address
        port: Server port (default: 5025, SCPI standard)

    Returns:
        Connected and initialized AFMClient
    """
    client = AFMClient(host, port)
    client.connect()
    client.init()
    return client


if __name__ == "__main__":
    # Example usage
    import sys

    if len(sys.argv) < 2:
        print("Usage: python afm_client.py <redpitaya_ip> [port]")
        print("\nExample:")
        print("  python afm_client.py 192.168.1.100")
        sys.exit(1)

    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5025

    print(f"Connecting to {host}:{port}...")

    try:
        with AFMClient(host, port) as client:
            print(f"Version: {client.get_version()}")
            print(f"Ping: {client.ping()}")

            status = client.get_status()
            print(f"Status: {status}")

    except AFMError as e:
        print(f"Error: {e}")
        sys.exit(1)
