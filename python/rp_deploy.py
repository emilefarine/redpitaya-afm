import argparse
import os
import shutil
import subprocess
import traceback
from datetime import datetime
from glob import glob
from pathlib import Path

import paramiko
from scp import SCPClient

# Repository layout (this script lives in <repo>/python)
REPO_ROOT = Path(__file__).resolve().parent.parent

# Configuration (populated from CLI args / environment in main())
LOCAL_PROJECT = str(REPO_ROOT / "cpp")
REMOTE_PROJECT = "/root/afm/cpp"
LOCAL_BITFILE = None        # a raw .bit needing bootgen conversion (local Vivado builds)
LOCAL_BITFILE_BIN = None    # the .bit.bin actually copied to the board
REMOTE_FPGA_DIR = "/root/afm/fpga"
BOOTGEN_PATH = None         # explicit bootgen path (--bootgen / BOOTGEN); only for .bit conversion
REDPITAYA_USER = "root"
REDPITAYA_PASSWORD = "root"

def run_remote(ssh, command):
    """Run a command on the Red Pitaya; return (exit_status, stdout, stderr)."""
    _, stdout, stderr = ssh.exec_command(command)
    exit_status = stdout.channel.recv_exit_status()
    return exit_status, stdout.read().decode().strip(), stderr.read().decode().strip()

def _is_build_artifact(rel_path):
    """Check if a remote file is a build artifact that should not be deleted during sync."""
    if rel_path.startswith("out/") or rel_path == "out":
        return True
    if rel_path.endswith(".o"):
        return True
    if rel_path.endswith(".csv"):
        return True
    return False

def sync_cpp_files(scp, ssh):
    """Mirror the local C++ project to the Red Pitaya, deleting obsolete remote files."""
    print("Synchronizing C++ project files...")

    print("Scanning local files...")
    local_files = set()
    local_dirs = set()

    for root, dirs, files in os.walk(LOCAL_PROJECT):
        rel_dir = os.path.relpath(root, LOCAL_PROJECT)
        if rel_dir != ".":
            local_dirs.add(rel_dir.replace("\\", "/"))

        for file in files:
            rel_path = os.path.relpath(os.path.join(root, file), LOCAL_PROJECT)
            local_files.add(rel_path.replace("\\", "/"))

    print(f"Found {len(local_files)} local files in {len(local_dirs)} directories")

    print("Scanning remote files...")
    _, remote_files_output, _ = run_remote(ssh, f"find {REMOTE_PROJECT} -type f 2>/dev/null")

    remote_files = set()
    if remote_files_output:
        for remote_path in remote_files_output.split('\n'):
            if remote_path.startswith(REMOTE_PROJECT):
                rel_path = remote_path[len(REMOTE_PROJECT):].lstrip('/')
                remote_files.add(rel_path)

    print(f"Found {len(remote_files)} remote files")

    # Delete files that exist remotely but not locally, keeping build artifacts
    files_to_delete = remote_files - local_files
    skipped_artifacts = set()
    actual_deletes = set()
    for rel_path in files_to_delete:
        if _is_build_artifact(rel_path):
            skipped_artifacts.add(rel_path)
        else:
            actual_deletes.add(rel_path)

    if actual_deletes:
        print(f"Deleting {len(actual_deletes)} obsolete remote files...")
        for rel_path in actual_deletes:
            run_remote(ssh, f"rm -f {REMOTE_PROJECT}/{rel_path}")
            print(f"  Deleted: {rel_path}")

    if skipped_artifacts:
        print(f"Kept {len(skipped_artifacts)} build artifacts (out/, .o, .csv)")

    print("Creating remote directory structure...")
    run_remote(ssh, f"mkdir -p {REMOTE_PROJECT}")
    for rel_dir in local_dirs:
        run_remote(ssh, f"mkdir -p {REMOTE_PROJECT}/{rel_dir}")

    print("Copying files...")
    file_count = 0
    for rel_path in local_files:
        local_path = os.path.join(LOCAL_PROJECT, rel_path.replace("/", os.sep))
        remote_path = f"{REMOTE_PROJECT}/{rel_path}"

        try:
            scp.put(local_path, remote_path)
            file_count += 1
            if file_count % 10 == 0:
                print(f"  Copied {file_count}/{len(local_files)} files...")
        except Exception as e:
            print(f"Warning: Could not copy {rel_path}: {e}")

    print(f"Synchronization complete: {file_count} files copied")
    if files_to_delete:
        print(f"Cleaned up: {len(files_to_delete)} obsolete files removed")

def find_bootgen():
    """Locate the bootgen executable (Vitis tool). Only needed to convert a raw .bit locally."""
    # 1. Explicit --bootgen / BOOTGEN env
    if BOOTGEN_PATH:
        if os.path.exists(BOOTGEN_PATH):
            return BOOTGEN_PATH
        print(f"  Warning: bootgen not found at given path: {BOOTGEN_PATH}")

    # 2. On PATH
    bootgen_path = shutil.which("bootgen")
    if bootgen_path:
        return bootgen_path

    # 3. Scan common Vitis install locations
    for base in sorted(glob(r"C:\Xilinx\Vitis\*\bin"), reverse=True):
        candidate = os.path.join(base, "bootgen.exe")
        if os.path.exists(candidate):
            return candidate

    return None

def generate_bitfile_bin():
    """Convert the Vivado .bit file to .bit.bin using bootgen (Vitis tool)."""
    if not os.path.exists(LOCAL_BITFILE):
        print(f"Error: Bitfile not found at {LOCAL_BITFILE}")
        return False

    print(f"Found bitfile: {LOCAL_BITFILE}")

    print("Searching for bootgen...")
    bootgen_path = find_bootgen()

    if bootgen_path:
        print(f"  Using bootgen: {bootgen_path}")
    else:
        print("  Error: bootgen not found")
        print("  Solutions:")
        print("    1. Install Vitis (part of the AMD/Xilinx Unified Installer)")
        print("    2. Pass --bootgen <path> or set the BOOTGEN environment variable")
        print("    3. Make sure C:\\Xilinx\\Vitis\\<version>\\bin is in PATH")
        return False

    bitfile_dir = os.path.dirname(LOCAL_BITFILE)
    bitfile_name = os.path.basename(LOCAL_BITFILE)
    bif_file = os.path.join(bitfile_dir, "red_pitaya_top.bif")

    print("Generating .bit.bin file using bootgen...")

    bif_content = f"all:{{ {bitfile_name} }}"
    try:
        with open(bif_file, 'w') as f:
            f.write(bif_content)
        print(f"  Created .bif file: {os.path.basename(bif_file)}")
    except Exception as e:
        print(f"  Error creating .bif file: {e}")
        return False

    bootgen_cmd = [
        bootgen_path,
        "-image", bif_file,
        "-arch", "zynq",
        "-process_bitstream", "bin",
        "-o", LOCAL_BITFILE_BIN,
        "-w"
    ]

    try:
        print(f"  Running: {' '.join(bootgen_cmd)}")
        print(f"  Working directory: {bitfile_dir}")

        result = subprocess.run(
            bootgen_cmd,
            cwd=bitfile_dir,
            capture_output=True,
            text=True,
            timeout=60,
            shell=False
        )

        print(f"  bootgen exit code: {result.returncode}")

        if result.stdout:
            print(f"  stdout: {result.stdout}")
        if result.stderr:
            print(f"  stderr: {result.stderr}")

        if result.returncode == 0:
            print(f"  Generated: {LOCAL_BITFILE_BIN}")
            return True
        else:
            print(f"  bootgen failed (exit code {result.returncode})")
            return False

    except FileNotFoundError as e:
        print("  Error: bootgen not found")
        print(f"  Exception details: {e}")
        print("  Please run this script from the Vivado/Vitis command prompt")
        print("  Or ensure the Vitis tools are in your PATH")
        return False
    except subprocess.TimeoutExpired:
        print("  Error: bootgen timed out (>60s)")
        return False
    except Exception as e:
        print(f"  Error running bootgen: {e}")
        print(f"  Exception type: {type(e).__name__}")
        traceback.print_exc()
        return False

def copy_bitfile(scp, ssh):
    """Copy the FPGA bitfile to the Red Pitaya, converting a raw .bit with bootgen if needed."""
    # A .bit.bin can be copied as-is; a raw .bit must first be converted with bootgen.
    if LOCAL_BITFILE is not None:
        if not generate_bitfile_bin():
            print("Failed to generate .bit.bin file")
            return False

    if not LOCAL_BITFILE_BIN or not os.path.exists(LOCAL_BITFILE_BIN):
        print(f"Error: Bitfile not found at {LOCAL_BITFILE_BIN}")
        return False

    print(f"\nCopying {os.path.basename(LOCAL_BITFILE_BIN)} to RedPitaya...")
    run_remote(ssh, f"mkdir -p {REMOTE_FPGA_DIR}")

    remote_bitfile_path = f"{REMOTE_FPGA_DIR}/red_pitaya_top.bit.bin"
    try:
        scp.put(LOCAL_BITFILE_BIN, remote_bitfile_path)
        print(f"Copied to: {remote_bitfile_path}")
        return True
    except Exception as e:
        print(f"Error copying file: {e}")
        return False

def flash_bitfile(ssh):
    """Flash the .bit.bin on the Red Pitaya to the FPGA using fpgautil."""
    remote_bitfile_path = f"{REMOTE_FPGA_DIR}/red_pitaya_top.bit.bin"

    print("Checking if bitfile exists on Red Pitaya...")
    exit_status, file_info, _ = run_remote(ssh, f"ls -la {remote_bitfile_path}")

    if exit_status != 0:
        print(f"Error: Bitfile not found at {remote_bitfile_path}")
        print("Please copy the bitfile first using --bitfile or --all")
        return False

    print(f"Found bitfile: {file_info}")

    print("Locating fpgautil...")
    # Known Red Pitaya location first (fastest), then PATH, then other common spots
    candidate_checks = [
        "test -x /opt/redpitaya/bin/fpgautil && echo /opt/redpitaya/bin/fpgautil",
        "which fpgautil 2>/dev/null",
        "test -x /usr/bin/fpgautil && echo /usr/bin/fpgautil",
        "test -x /usr/local/bin/fpgautil && echo /usr/local/bin/fpgautil",
    ]
    fpgautil_path = None
    for check in candidate_checks:
        _, result, _ = run_remote(ssh, check)
        if result:
            fpgautil_path = result
            break

    if fpgautil_path:
        print(f"Found fpgautil at: {fpgautil_path}")
        command = f"cd {REMOTE_FPGA_DIR} && {fpgautil_path} -b red_pitaya_top.bit.bin"
    else:
        print("Could not locate fpgautil. Trying with full environment...")
        command = f"cd {REMOTE_FPGA_DIR} && bash -l -c 'fpgautil -b red_pitaya_top.bit.bin'"

    print("Flashing bitfile to FPGA using fpgautil...")
    exit_status, output, error_output = run_remote(ssh, command)

    if exit_status == 0:
        print("Bitfile flashed successfully to FPGA!")
        if output:
            print(f"fpgautil output: {output}")
        return True
    else:
        print(f"Error flashing bitfile (exit code: {exit_status})")
        if error_output:
            print(f"Error output: {error_output}")
        if output:
            print(f"Standard output: {output}")
        return False

def check_fpga_status(ssh):
    """Report FPGA programming status via /sys/class/fpga_manager (fpgautil has no -l option)."""
    print("Checking FPGA status...")
    print()

    print("FPGA Manager State:")
    _, state, _ = run_remote(ssh, "cat /sys/class/fpga_manager/fpga0/state 2>/dev/null")

    if state:
        print(f"  State: {state}")

        if state == "operating":
            print("  Status: FPGA is programmed and operating")
        elif state == "unknown":
            print("  Status: FPGA is NOT programmed")
        elif state == "write complete":
            print("  Status: FPGA programming complete")
        else:
            print(f"  Status: {state}")
    else:
        print("  Could not read FPGA state")

    print()

    print("Bitfile Location:")
    remote_bitfile = f"{REMOTE_FPGA_DIR}/red_pitaya_top.bit.bin"
    _, bitfile_info, _ = run_remote(ssh, f"ls -lh {remote_bitfile} 2>/dev/null")

    if bitfile_info:
        print(f"  {bitfile_info}")
    else:
        print(f"  No bitfile found at {remote_bitfile}")

    print()

    print("FPGA Firmware Info:")
    _, name, _ = run_remote(ssh, "cat /sys/class/fpga_manager/fpga0/name 2>/dev/null")
    if name:
        print(f"  FPGA Manager: {name}")

    _, firmware, _ = run_remote(ssh, "cat /sys/class/fpga_manager/fpga0/firmware 2>/dev/null")
    if firmware:
        print(f"  Loaded Firmware: {firmware}")

    print()

    print("Summary:")
    if state == "operating" or state == "write complete":
        print("  FPGA is ready for use")
    elif state == "unknown":
        print("  FPGA needs to be programmed")
        print("  Run: python rp_deploy.py --flash")
    else:
        print(f"  FPGA state: {state}")

    print()

    print("Available Commands:")
    print("  Check state:  cat /sys/class/fpga_manager/fpga0/state")
    print("  Load bitfile: fpgautil -b <bitfile.bit.bin>")
    print("  Flash FPGA:   python rp_deploy.py --flash")

def sync_remote_clock(ssh):
    """
    Set the Red Pitaya's system clock to match the local PC's time.
    The board has no battery-backed RTC, so its clock resets on every
    boot and drifts to a stale build-image date without this.
    """
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    exit_status, _, err = run_remote(ssh, f'date -s "{now}"')
    if exit_status == 0:
        print(f"Synced Red Pitaya clock to {now}")
    else:
        print(f"Warning: failed to sync clock: {err}")

def install_oled_service(scp, ssh):
    """
    Copy the oled-ip.service file to /etc/systemd/system/ on the Red Pitaya
    and enable + start it so the OLED displays the IP at every boot.
    """
    local_service = os.path.join(LOCAL_PROJECT, "Tools", "oled-ip.service")
    remote_service = "/etc/systemd/system/oled-ip.service"

    if not os.path.exists(local_service):
        print(f"Error: service file not found at {local_service}")
        return False

    print("Installing oled-ip systemd service...")

    try:
        scp.put(local_service, remote_service)
        print(f"  Copied {os.path.basename(local_service)} to {remote_service}")
    except Exception as e:
        print(f"  Failed to copy service file: {e}")
        return False

    commands = [
        ("systemctl daemon-reload",           "daemon-reload"),
        ("systemctl enable oled-ip.service",  "enable"),
        ("systemctl restart oled-ip.service", "start"),
    ]
    for cmd, label in commands:
        exit_status, _, err = run_remote(ssh, cmd)
        if exit_status == 0:
            print(f"  systemctl {label} OK")
        else:
            print(f"  systemctl {label} failed: {err}")
            return False

    _, status_output, _ = run_remote(ssh, "systemctl status oled-ip.service --no-pager")
    print(status_output)

    print("Oled-ip service installed and enabled at boot.")
    return True

def resolve_bitfile(bitfile_arg):
    """
    Work out which local files back the deploy.

    Returns (local_bit, local_bit_bin):
      - a .bit.bin is copied as-is (local_bit is None)
      - a raw .bit is converted with bootgen to a sibling .bit.bin
      - when no bitfile is given, the newest fpga/bitfiles/*.bit.bin is used
    """
    if bitfile_arg is None:
        candidates = sorted(
            glob(str(REPO_ROOT / "fpga" / "bitfiles" / "*.bit.bin")),
            key=os.path.getmtime,
            reverse=True,
        )
        bitfile_arg = candidates[0] if candidates else None

    if not bitfile_arg:
        return None, None

    if bitfile_arg.endswith(".bit.bin"):
        return None, bitfile_arg

    # A raw .bit: bootgen produces a sibling .bit.bin
    return bitfile_arg, bitfile_arg + ".bin"

def main():
    parser = argparse.ArgumentParser(
        description="Copy files to Red Pitaya via SSH/SCP (OS 2.0+)",
        epilog="Note: converting a raw .bit requires Vitis tools (bootgen); a prebuilt .bit.bin is copied as-is."
    )
    parser.add_argument("--cpp", action="store_true", help="Sync C++ project files (default) - creates dirs, copies files, deletes obsolete files")
    parser.add_argument("--sync", action="store_true", help="Alias for --cpp (sync C++ files)")
    parser.add_argument("--flash", action="store_true", help="Flash bitfile to FPGA using fpgautil")
    parser.add_argument("--all", action="store_true", help="Sync C++ files, copy bitfile and flash")
    parser.add_argument("--deploy", action="store_true", help="Copy bitfile, flash to FPGA and check status")
    parser.add_argument("--status", action="store_true", help="Check FPGA programming status (reads /sys/class/fpga_manager)")
    parser.add_argument("--install-oled", action="store_true", dest="install_oled",
                        help="Install oled-ip as a systemd service (runs at boot to show IP on OLED)")
    # Connection
    parser.add_argument("--host", default=os.environ.get("RP_HOST"),
                        help="Red Pitaya IP address (or set RP_HOST)")
    parser.add_argument("--user", default=os.environ.get("RP_USER", "root"),
                        help="SSH username (or set RP_USER; default: root)")
    parser.add_argument("--password", default=os.environ.get("RP_PASSWORD", "root"),
                        help="SSH password (or set RP_PASSWORD; default: root)")
    # Paths
    parser.add_argument("--bitfile", default=None,
                        help="Bitfile to deploy: a .bit.bin is copied as-is, a raw .bit is converted with bootgen. "
                             "Passing this also triggers the copy. Default: newest fpga/bitfiles/*.bit.bin")
    parser.add_argument("--bootgen", default=os.environ.get("BOOTGEN"),
                        help="Path to the bootgen executable (or set BOOTGEN); only needed to convert a raw .bit")
    parser.add_argument("--remote-cpp", default="/root/afm/cpp", help="Remote C++ project directory")
    parser.add_argument("--remote-fpga", default="/root/afm/fpga", help="Remote FPGA directory")

    args = parser.parse_args()

    if args.sync:
        args.cpp = True

    # Default behavior: sync C++ files if no specific option is provided
    if not (args.cpp or args.bitfile or args.all or args.flash or args.deploy or args.status or args.install_oled):
        args.cpp = True

    if not args.host:
        parser.error("no host given: pass --host <ip> or set the RP_HOST environment variable")

    # Apply configuration from arguments / environment
    global REMOTE_PROJECT, REMOTE_FPGA_DIR, LOCAL_BITFILE, LOCAL_BITFILE_BIN
    global BOOTGEN_PATH, REDPITAYA_USER, REDPITAYA_PASSWORD
    REMOTE_PROJECT = args.remote_cpp
    REMOTE_FPGA_DIR = args.remote_fpga
    BOOTGEN_PATH = args.bootgen
    REDPITAYA_USER = args.user
    REDPITAYA_PASSWORD = args.password
    LOCAL_BITFILE, LOCAL_BITFILE_BIN = resolve_bitfile(args.bitfile)

    print(f"Connecting to Red Pitaya at {args.host}...")
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    try:
        ssh.connect(args.host, username=REDPITAYA_USER, password=REDPITAYA_PASSWORD)
        print("Connected successfully.")

        sync_remote_clock(ssh)

        with SCPClient(ssh.get_transport()) as scp:
            if args.cpp or args.all:
                sync_cpp_files(scp, ssh)

            if args.bitfile or args.all or args.deploy:
                copy_bitfile(scp, ssh)

            if args.install_oled:
                install_oled_service(scp, ssh)

        if args.flash or args.deploy or args.all:
            flash_bitfile(ssh)

        if args.status or args.deploy or args.all:
            check_fpga_status(ssh)

        print("All operations done.")

    except Exception as e:
        print(f"Error: {e}")
    finally:
        ssh.close()

if __name__ == "__main__":
    main()
