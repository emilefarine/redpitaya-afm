#!/usr/bin/env python3
"""
AFM CLI - Command Line Interface for AFM SCPI measurement server

The server uses an SCPI-inspired protocol. Commands are case-insensitive,
use colon-separated keywords, and comma-separated arguments.

Usage:
    # Interactive mode
    python afm_cli.py 192.168.1.100

    # Single command mode
    python afm_cli.py 192.168.1.100 --command "SYSTEM:PING"

    # Script mode (read commands from file)
    python afm_cli.py 192.168.1.100 --script commands.txt

"""

import argparse
import cmd
import sys
import time
from typing import Optional

import numpy as np

from afm_client import (
    AFMClient, AFMError, AFMConnectionError, AFMCommandError, AFMTimeoutError,
    GainSetting, SystemStatus, SpectrumData
)


class AFMShell(cmd.Cmd):
    """Interactive command shell for AFM measurement system"""

    intro = """
 ===================================================================
|       AFM SCPI Measurement System - Interactive Shell             |
|                                                                   |
|       Type 'help' for available commands, 'quit' to exit          |
|       Commands are SCPI-style (case-insensitive, colon-separated) |
 ===================================================================
"""
    prompt = "AFM> "

    def __init__(self, client: AFMClient):
        super().__init__()
        self.client = client
        self._last_spectrum: Optional[SpectrumData] = None

    def default(self, line: str):
        """Send raw command to server"""
        if not line.strip():
            return
        try:
            response = self.client.send_command(line)
            print(f"OK: {response}" if response else "OK")
        except AFMCommandError as e:
            print(f"Error: {e}")

    def emptyline(self):
        """Do nothing on empty line"""
        pass

    # ------- System Commands -------

    def do_ping(self, arg):
        """Test connection to server"""
        try:
            if self.client.ping():
                print("PONG - Server is responding")
            else:
                print("No response from server")
        except AFMError as e:
            print(f"Error: {e}")

    def do_version(self, arg):
        """Get server version"""
        try:
            print(self.client.get_version())
        except AFMError as e:
            print(f"Error: {e}")

    def do_status(self, arg):
        """Get system status"""
        try:
            status = self.client.get_status()
            print(f"  Hardware Initialized: {status.hardware_initialized}")
            print(f"  Board Connected:      {status.board_connected}")
            print(f"  Measurement Active:   {status.measurement_in_progress}")
            print(f"  Decimation:           {status.decimation}")
        except AFMError as e:
            print(f"Error: {e}")

    def do_init(self, arg):
        """Initialize hardware"""
        try:
            print("Initializing hardware...")
            result = self.client.init()
            print(f"OK: {result}")
        except AFMError as e:
            print(f"Error: {e}")

    def do_deinit(self, arg):
        """Deinitialize hardware"""
        try:
            result = self.client.deinit()
            print(f"OK: {result}")
        except AFMError as e:
            print(f"Error: {e}")

    def do_shutdown(self, arg):
        """Shutdown the server (careful!)"""
        confirm = input("Are you sure you want to shutdown the server? (yes/no): ")
        if confirm.lower() == 'yes':
            try:
                self.client.shutdown()
                print("Server shutdown initiated")
                return True  # Exit shell
            except AFMError as e:
                print(f"Error: {e}")
        else:
            print("Shutdown cancelled")

    # ------ Electronic Board Commands ------

    def do_mux(self, arg):
        """Set MUX routing: mux <output> <input> or mux disconnect <output>  (channels 1-4)"""
        args = arg.split()
        try:
            if len(args) == 2:
                if args[0] == 'disconnect':
                    output = int(args[1])
                    self.client.disconnect_mux(output)
                    print(f"OK: Output {output} disconnected")
                else:
                    output, input_ch = int(args[0]), int(args[1])
                    self.client.set_mux(output, input_ch)
                    print(f"OK: Input {input_ch} -> Output {output}")
            else:
                print("Usage: mux <output> <input>  or  mux disconnect <output>  (channels 1-4)")
        except ValueError:
            print("Error: Arguments must be integers")
        except AFMError as e:
            print(f"Error: {e}")

    def do_gain(self, arg):
        """Set gain: gain <channel> <index>  channel=1-4, index 0-7: 1/8, 1/4, 1/2, 1, 2, 4, 8, 16"""
        args = arg.split()
        try:
            if len(args) == 2:
                channel, gain_idx = int(args[0]), int(args[1])
                result = self.client.set_gain(channel, gain_idx)
                print(f"OK: Channel {channel} gain set to {result}")
            else:
                print("Usage: gain <channel> <index>  (channel 1-4)")
                print("  Gain indices: 0=1/8, 1=1/4, 2=1/2, 3=1, 4=2, 5=4, 6=8, 7=16")
        except ValueError:
            print("Error: Arguments must be integers")
        except AFMError as e:
            print(f"Error: {e}")

    def do_board_reset(self, arg):
        """Reset electronic board to defaults"""
        try:
            result = self.client.reset_board()
            print(f"OK: {result}")
        except AFMError as e:
            print(f"Error: {e}")

    def do_board_status(self, arg):
        """Get electronic board status"""
        try:
            result = self.client.get_board_status()
            print(result)
        except AFMError as e:
            print(f"Error: {e}")

    # ------ Measurement Commands ------

    def do_sinc(self, arg):
        """Broadband sinc measurement: sinc <center_kHz> <bandwidth_kHz> [samples] [decimation] [amplitude]"""
        args = arg.split()
        try:
            if len(args) < 2:
                print("Usage: sinc <center_kHz> <bandwidth_kHz> [num_samples] [decimation] [amplitude]")
                print("  Defaults: num_samples=8192, decimation=64, amplitude=1.0")
                print("  Example: sinc 1000 200")
                print("  Example: sinc 1000 200 16384 32 0.5")
                return

            center = float(args[0])
            bw = float(args[1])
            samples = int(args[2]) if len(args) > 2 else 8192
            dec = int(args[3]) if len(args) > 3 else 64
            amp = float(args[4]) if len(args) > 4 else 1.0

            print(f"Sinc measurement: center={center} kHz, BW={bw} kHz, "
                  f"samples={samples}, dec={dec}, amp={amp}")
            print("Measuring...", end="", flush=True)

            start = time.time()
            self._last_spectrum = self.client.measure_sinc(
                center, bw, samples, dec, amp
            )
            elapsed = time.time() - start

            print(f" done! ({elapsed:.2f}s)")
            self._print_spectrum_summary(self._last_spectrum)

        except ValueError:
            print("Error: Invalid arguments")
        except AFMError as e:
            print(f"\nError: {e}")

    def do_sweep(self, arg):
        """Frequency sweep measurement: sweep <center_kHz> <range_kHz> [step_kHz] [decimation] [amplitude]"""
        args = arg.split()
        try:
            if len(args) < 2:
                print("Usage: sweep <center_kHz> <range_kHz> [step_kHz] [decimation] [amplitude]")
                print("  Defaults: step_kHz=1.0, decimation=64, amplitude=1.0")
                print("  Example: sweep 1000 100")
                print("  Example: sweep 1000 100 0.5 64 0.8")
                return

            center = float(args[0])
            range_kHz = float(args[1])
            step = float(args[2]) if len(args) > 2 else 1.0
            dec = int(args[3]) if len(args) > 3 else 64
            amp = float(args[4]) if len(args) > 4 else 1.0

            num_steps = int(range_kHz / step) + 1
            print(f"Sweep measurement: center={center} kHz, range={range_kHz} kHz, "
                  f"step={step} kHz ({num_steps} points)")
            print(f"  decimation={dec}, amplitude={amp}")
            print("Sweeping...", end="", flush=True)

            start = time.time()
            self._last_spectrum = self.client.measure_sweep(
                center, range_kHz, step, dec, amp
            )
            elapsed = time.time() - start

            print(f" done! ({elapsed:.2f}s)")
            self._print_spectrum_summary(self._last_spectrum)

        except ValueError:
            print("Error: Invalid arguments")
        except AFMError as e:
            print(f"\nError: {e}")

    @staticmethod
    def _print_spectrum_summary(spectrum: SpectrumData):
        """Print summary of spectrum data"""
        print(f"  Points:         {spectrum.num_points}")
        print(f"  Freq range:     {spectrum.freq_kHz[0]:.3f} - {spectrum.freq_kHz[-1]:.3f} kHz")
        peak_idx = int(np.argmax(spectrum.magnitude))
        print(f"  Peak frequency: {spectrum.freq_kHz[peak_idx]:.3f} kHz")
        print(f"  Peak magnitude: {spectrum.magnitude[peak_idx]:.6e}")
        print(f"  Peak phase:     {spectrum.phase_rad[peak_idx]:.4f} rad")
        print(f"  Mag range:      [{spectrum.magnitude.min():.6e}, {spectrum.magnitude.max():.6e}]")

    # ------ Data Commands ------

    def do_data_show(self, arg):
        """Show last spectrum data (first N points): data_show [count]"""
        if self._last_spectrum is None or self._last_spectrum.num_points == 0:
            print("No data available. Use sinc or sweep first.")
            return

        args = arg.split()
        count = int(args[0]) if args else 10
        count = min(count, self._last_spectrum.num_points)

        print(f"First {count} of {self._last_spectrum.num_points} points:")
        print(f"  {'freq_kHz':>12s}  {'magnitude':>14s}  {'phase_rad':>12s}")
        print(f"  {'─'*12}  {'─'*14}  {'─'*12}")
        for i in range(count):
            print(f"  {self._last_spectrum.freq_kHz[i]:12.3f}  "
                  f"{self._last_spectrum.magnitude[i]:14.6e}  "
                  f"{self._last_spectrum.phase_rad[i]:12.6f}")

    def do_data_save(self, arg):
        """Save last spectrum data to CSV file: data_save <filename>"""
        if self._last_spectrum is None or self._last_spectrum.num_points == 0:
            print("No data available. Use sinc or sweep first.")
            return

        if not arg:
            print("Usage: data_save <filename>")
            return

        filename = arg.strip()
        if not filename.endswith('.csv'):
            filename += '.csv'

        try:
            with open(filename, 'w') as f:
                f.write("freq_kHz,magnitude,phase_rad\n")
                for i in range(self._last_spectrum.num_points):
                    f.write(f"{self._last_spectrum.freq_kHz[i]:.3f},"
                            f"{self._last_spectrum.magnitude[i]:.6e},"
                            f"{self._last_spectrum.phase_rad[i]:.6f}\n")
            print(f"Saved {self._last_spectrum.num_points} points to {filename}")
        except IOError as e:
            print(f"Error writing file: {e}")

    # ------ Shell Commands ------

    def do_help(self, arg):
        """Show help for commands"""
        if arg:
            super().do_help(arg)
        else:
            print("""
Available Commands:
-------------------------------------------------------------
System:
  ping            Test connection (SYSTEM:PING)
  version         Show server version (SYSTEM:VERSION)
  status          Show system status (SYSTEM:STATUS?)
  init            Initialize hardware (SYSTEM:INIT)
  deinit          Deinitialize hardware (SYSTEM:DEINIT)
  shutdown        Shutdown server (SYSTEM:SHUTDOWN)

IEEE 488.2:
  *IDN?           Identify instrument
  *RST            Reset to power-on state
  *OPC?           Operation complete query

Electronic Board:
  mux <out> <in>          Route input to output (BOARD:MUX:ROUTE)
                          out and in: 1-4 (matching board connector labels)
  mux disconnect <out>    Disconnect output (BOARD:MUX:DISCONNECT)
  gain <ch> <idx>         Set gain (BOARD:GAIN)
                          ch: 1-4, idx: 0=1/8, 1=1/4, 2=1/2, 3=1, 4=2, 5=4, 6=8, 7=16
  board_reset             Reset board (BOARD:RESET)
  board_status            Show board status (BOARD:STATUS?)

Measurement:
  sinc <center_kHz> <bw_kHz> [samples] [dec] [amp]
                          Broadband sinc + FFT (MEASURE:SINC)
  sweep <center_kHz> <range_kHz> [step] [dec] [amp]
                          Frequency sweep (MEASURE:SWEEP)

Data:
  data_show [n]      Show first N spectrum points
  data_save <file>   Save spectrum to CSV

Other:
  <any SCPI command>    Send raw SCPI command to server
  quit / exit           Exit shell
-------------------------------------------------------------
""")

    def do_quit(self, arg):
        """Exit the shell"""
        print("Goodbye!")
        return True

    def do_exit(self, arg):
        """Exit the shell"""
        return self.do_quit(arg)

    def do_EOF(self, arg):
        """Exit on Ctrl+D"""
        print()
        return self.do_quit(arg)


def main():
    parser = argparse.ArgumentParser(
        description='AFM SCPI Measurement System CLI',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s 192.168.1.100                    Interactive mode
  %(prog)s 192.168.1.100 -c "SYSTEM:PING"   Single command
  %(prog)s 192.168.1.100 -s commands.txt    Script mode
  %(prog)s 192.168.1.100 -s script.txt --continue-on-error
        """
    )
    parser.add_argument('host', help='Red Pitaya IP address or hostname')
    parser.add_argument('-p', '--port', type=int, default=5025,
                        help='Server port (default: 5025)')
    parser.add_argument('-c', '--command', help='Execute single SCPI command and exit')
    parser.add_argument('-s', '--script', help='Execute commands from file')
    parser.add_argument('--continue-on-error', action='store_true',
                        help='Continue script execution on error (default: stop)')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Verbose output')

    args = parser.parse_args()

    # Connect to server
    print(f"Connecting to {args.host}:{args.port}...")
    try:
        client = AFMClient(args.host, args.port)
        client.connect()
    except AFMConnectionError as e:
        print(f"Connection failed: {e}")
        sys.exit(1)

    try:
        if args.command:
            # Single command mode
            try:
                response = client.send_command(args.command)
                print(f"OK: {response}" if response else "OK")
            except AFMCommandError as e:
                print(f"Error: {e}")
                sys.exit(1)

        elif args.script:
            # Script mode - stop on error by default for safety
            error_count = 0
            try:
                with open(args.script, 'r') as f:
                    for line_num, line in enumerate(f, 1):
                        line = line.strip()
                        if not line or line.startswith('#'):
                            continue
                        print(f"[{line_num}] {line}")
                        try:
                            response = client.send_command(line)
                            print(f"  OK: {response}" if response else "  OK")
                        except AFMCommandError as e:
                            print(f"  Error: {e}")
                            error_count += 1
                            if not args.continue_on_error:
                                print("\nScript aborted due to error. Use --continue-on-error to ignore errors.")
                                sys.exit(1)
            except IOError as e:
                print(f"Error reading script: {e}")
                sys.exit(1)
            
            if error_count > 0:
                print(f"\nScript completed with {error_count} error(s)")
                sys.exit(1)

        else:
            # Interactive mode
            shell = AFMShell(client)
            shell.cmdloop()

    finally:
        client.disconnect()


if __name__ == '__main__':
    main()
