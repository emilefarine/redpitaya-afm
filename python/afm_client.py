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

import socket
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional, Union
import logging

import numpy as np

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


class AFMError(Exception):
    """Base exception for AFM client errors"""
    pass


class AFMConnectionError(AFMError):
    """Connection-related errors"""
    pass


class AFMCommandError(AFMError):
    """Command execution errors"""
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
    calibration_active: bool = False
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
                elif key == 'CAL':
                    status.calibration_active = value == '1'
                elif key == 'DEC':
                    status.decimation = int(value)

        return status


class AFMClient:
    """
    Client for communicating with the AFM measurement server on Red Pitaya.

    Thread-safe for basic operations. For concurrent access to measurements,
    external synchronization is recommended.
    """

    DEFAULT_PORT = 5025
    DEFAULT_TIMEOUT = 10.0
    RECV_BUFFER_SIZE = 65536

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

            # Read welcome message
            welcome = self._read_line()
            logger.info(f"Connected to {self.host}:{self.port}")
            logger.info(f"Server: {welcome}")
            return welcome

        except socket.error as e:
            self._socket = None
            raise AFMConnectionError(f"Failed to connect to {self.host}:{self.port}: {e}")

    def disconnect(self) -> None:
        """Disconnect from the server"""
        if self._socket:
            try:
                self._socket.close()
            except socket.error:
                pass
            finally:
                self._socket = None
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

    def _set_timeout(self, timeout: float) -> None:
        """Temporarily change socket timeout."""
        if self._socket:
            self._socket.settimeout(timeout)

    def _flush_buffer(self) -> None:
        """Drain any stale data from the receive buffer and socket.

        Called before sending a new command to prevent leftover data
        (e.g. from a timed-out sweep) from being interpreted as the
        response to the next command.
        """
        self._recv_buffer = b""
        if not self._socket:
            return
        # Drain anything sitting in the OS socket buffer
        self._socket.setblocking(False)
        try:
            while True:
                data = self._socket.recv(self.RECV_BUFFER_SIZE)
                if not data:
                    break
        except (BlockingIOError, socket.error):
            pass
        finally:
            self._socket.settimeout(self.timeout)

    def _send(self, command: str) -> None:
        """Send a command to the server"""
        if not self._socket:
            raise AFMConnectionError("Not connected")

        try:
            message = command + "\n"
            self._socket.sendall(message.encode('ascii'))
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

        prev_timeout = self._socket.gettimeout()
        if timeout_override is not None:
            self._socket.settimeout(timeout_override)

        try:
            # Look for newline in bytes buffer
            while b'\n' not in self._recv_buffer:
                data = self._socket.recv(self.RECV_BUFFER_SIZE)
                if not data:
                    self.disconnect()
                    raise AFMConnectionError("Connection closed by server")
                self._recv_buffer += data

            # Split on newline, decode only the line portion
            line_bytes, self._recv_buffer = self._recv_buffer.split(b'\n', 1)
            line = line_bytes.decode('ascii').rstrip('\r')
            logger.debug(f"Received: {line}")
            return line

        except socket.timeout:
            raise AFMTimeoutError("Read timeout")
        except socket.error as e:
            self.disconnect()
            raise AFMConnectionError(f"Receive failed: {e}")
        finally:
            if timeout_override is not None and self._socket:
                self._socket.settimeout(prev_timeout)

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
        if not self._socket:
            raise AFMConnectionError("Not connected")

        prev_timeout = self._socket.gettimeout()
        if timeout_override is not None:
            self._socket.settimeout(timeout_override)

        try:
            while len(self._recv_buffer) < num_bytes:
                data = self._socket.recv(self.RECV_BUFFER_SIZE)
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
            if timeout_override is not None and self._socket:
                self._socket.settimeout(prev_timeout)

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

        if response.startswith("OK"):
            # Extract data after "OK "
            data = response[3:] if len(response) > 3 else ""
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
        """
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
        if not header.startswith("OK"):
            raise AFMCommandError(f"Unexpected board status header: {header}")

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
            True if server responds with PONG
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
        """Shutdown the server"""
        try:
            self._send("SYSTEM:SHUTDOWN")
            # Server will close connection
            self._read_line()
        except AFMError:
            pass
        finally:
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
        self._flush_buffer()
        self._send("BOARD:STATUS?")
        return self._read_board_status_response()

    # ------ Measurement Commands --------

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

        if not header.startswith("OK"):
            raise AFMCommandError(header)

        parts = header.split()
        if len(parts) < 3 or parts[1] != "DATA":
            raise AFMCommandError(f"Unexpected spectrum header: {header}")

        num_rows = int(parts[2])
        if num_rows <= 0:
            raise AFMCommandError(f"Invalid row count: {num_rows}")

        # Fast path: header carries the payload byte count -> single bulk read.
        if len(parts) >= 4:
            num_bytes = int(parts[3])
            payload = self._read_exact(num_bytes, timeout_override=timeout)
            return self._parse_spectrum_payload(payload.decode('ascii'), num_rows)

        # Fallback: read the N rows line by line (server < 2.2.0).
        freq = np.empty(num_rows, dtype=np.float64)
        mag = np.empty(num_rows, dtype=np.float64)
        phase = np.empty(num_rows, dtype=np.float64)

        for i in range(num_rows):
            line = self._read_line(timeout_override=timeout)
            values = line.split(',')
            if len(values) != 3:
                raise AFMCommandError(
                    f"Malformed spectrum row {i}: expected 3 values, got {len(values)}"
                )
            freq[i] = float(values[0])
            mag[i] = float(values[1])
            phase[i] = float(values[2])

        return SpectrumData(freq_kHz=freq, magnitude=mag, phase_rad=phase)

    @staticmethod
    def _parse_spectrum_payload(payload: str, num_rows: int) -> SpectrumData:
        """Parse a CSV spectrum payload (N rows of freq,magnitude,phase)."""
        lines = payload.split('\n')
        # A trailing '\n' on the last row produces a final empty element.
        if lines and lines[-1] == '':
            lines.pop()

        if len(lines) != num_rows:
            raise AFMCommandError(
                f"Spectrum payload row count mismatch: header said {num_rows}, "
                f"got {len(lines)}"
            )

        freq = np.empty(num_rows, dtype=np.float64)
        mag = np.empty(num_rows, dtype=np.float64)
        phase = np.empty(num_rows, dtype=np.float64)

        for i, line in enumerate(lines):
            values = line.split(',')
            if len(values) != 3:
                raise AFMCommandError(
                    f"Malformed spectrum row {i}: expected 3 values, got {len(values)}"
                )
            freq[i] = float(values[0])
            mag[i] = float(values[1])
            phase[i] = float(values[2])

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
            num_samples: Number of samples (1-32768, default 8192)
            decimation: FPGA decimation factor (power of 2, 16-1024, default 64)
            amplitude: Signal amplitude 0-1 (default 1.0)

        Returns:
            SpectrumData with freq_kHz, magnitude, and phase_rad arrays

        Raises:
            AFMCommandError: If measurement fails
            AFMTimeoutError: If server doesn't respond in time
        """
        cmd = (f"MEASURE:SINC {center_kHz},{bandwidth_kHz},"
               f"{num_samples},{decimation},{amplitude}")
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
        """
        # Calculate dynamic timeout based on sweep parameters.
        # Each frequency step takes roughly 1-2 seconds (load + acquire + compute).
        num_steps = max(1, int(range_kHz / step_kHz) + 1) if step_kHz > 0 else 1
        estimated_time = num_steps * 2.0
        sweep_timeout = max(self.timeout, estimated_time + 30.0)
        logger.info(f"Sweep: {num_steps} steps, timeout set to {sweep_timeout:.0f}s")

        cmd = (f"MEASURE:SWEEP {center_kHz},{range_kHz},"
               f"{step_kHz},{decimation},{amplitude}")
        self._flush_buffer()
        self._send(cmd)
        return self._read_spectrum_response(timeout=sweep_timeout)

    # ------ Calibration Commands --------

    def calibrate(
        self,
        center_kHz: float,
        bandwidth_kHz: float,
        num_samples: int = 8192,
        decimation: int = 64,
        amplitude: float = 1.0,
    ) -> str:
        """
        Run a loopback calibration measurement.

        The DAC output must be physically connected through the electronic
        board back to the ADC input before calling this.  The server stores
        the resulting spectrum as the reference transfer function H_ref(f).
        Subsequent MEASURE:SINC calls will automatically divide by H_ref
        to remove system artifacts.

        Args:
            center_kHz: Center frequency in kHz
            bandwidth_kHz: Bandwidth in kHz
            num_samples: Number of samples (1-32768, default 8192)
            decimation: FPGA decimation factor (power of 2, 16-1024, default 64)
            amplitude: Signal amplitude 0-1 (default 1.0)

        Returns:
            Calibration status message (e.g. "CALIBRATED 167 points")

        Raises:
            AFMCommandError: If calibration measurement fails
        """
        cmd = (f"CALIBRATE:RUN {center_kHz},{bandwidth_kHz},"
               f"{num_samples},{decimation},{amplitude}")
        return self.send_command(cmd)

    def get_calibration_status(self) -> str:
        """
        Query the current calibration state.

        Returns:
            Status string, e.g. "CALIBRATED DEC=64 N=8192 CENTER=500.0
            BW=200.0 AMP=1.0 POINTS=167" or "NOT_CALIBRATED"
        """
        return self.send_command("CALIBRATE:STATUS?")

    def clear_calibration(self) -> None:
        """
        Clear stored calibration data.

        Subsequent measurements will return raw (uncalibrated) results.
        """
        self.send_command("CALIBRATE:CLEAR")

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
