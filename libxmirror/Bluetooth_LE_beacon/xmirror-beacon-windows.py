#!/usr/bin/env python3
# Slim Windows-only AirPlay BLE beacon for XMirror
# No GLib dependency — uses a plain polling loop instead.

import os
import sys
import struct
import time
import socket
import argparse
import psutil

try:
    import winrt.windows.foundation.collections
    import winrt.windows.devices.bluetooth.advertisement as ble_adv
    import winrt.windows.storage.streams as streams
except ImportError as e:
    print(f"Missing winrt package: {e}")
    print("Run: pip install winrt-Windows.Foundation winrt-Windows.Devices.Bluetooth.Advertisement winrt-Windows.Storage.Streams")
    sys.exit(1)

# ── BLE publisher state ────────────────────────────────────────────────────────

publisher = None
advertised_port = None
advertised_address = None

def on_status_changed(sender, args):
    global publisher
    if args.status.name == "STOPPED":
        publisher = None

def start_advertising(ipv4_str: str, port: int):
    global publisher, advertised_port, advertised_address

    mfg_data = bytearray([0x09, 0x08, 0x13, 0x30])
    import ipaddress
    mfg_data.extend(bytearray(ipaddress.ip_address(ipv4_str).packed))
    mfg_data.extend(port.to_bytes(2, 'big'))

    writer = streams.DataWriter()
    writer.write_bytes(mfg_data)

    mfg = ble_adv.BluetoothLEManufacturerData()
    mfg.company_id = 0x004C
    mfg.data = writer.detach_buffer()

    advertisement = ble_adv.BluetoothLEAdvertisement()
    advertisement.manufacturer_data.append(mfg)

    publisher = ble_adv.BluetoothLEAdvertisementPublisher(advertisement)
    publisher.add_status_changed(on_status_changed)

    try:
        publisher.start()
        advertised_port = port
        advertised_address = ipv4_str
        print(f"[beacon] Advertising started: {ipv4_str}:{port}", flush=True)
    except Exception as e:
        print(f"[beacon] Failed to start: {e}", flush=True)
        publisher = None

def stop_advertising():
    global publisher, advertised_port, advertised_address
    if publisher is not None:
        try:
            publisher.stop()
        except Exception:
            pass
        publisher = None
    advertised_port = None
    advertised_address = None
    print("[beacon] Advertising stopped", flush=True)

# ── BLE data file parsing ──────────────────────────────────────────────────────

def read_ble_file(path: str):
    """
    Returns (port, pid) if the file exists and the owning process is alive,
    otherwise returns (None, None).
    """
    if not os.path.isfile(path):
        return None, None
    try:
        with open(path, 'rb') as f:
            port = struct.unpack('<H', f.read(2))[0]
            pid  = struct.unpack('<I', f.read(4))[0]
            name = f.read().split(b'\0', 1)[0].decode('utf-8')
        if not psutil.pid_exists(pid):
            return None, None
        proc = psutil.Process(pid)
        if not proc.name().startswith(os.path.basename(name)):
            return None, None
        return port, pid
    except Exception:
        return None, None

# ── Main polling loop ──────────────────────────────────────────────────────────

def get_local_ipv4() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return socket.gethostbyname(socket.gethostname())

def main():
    parser = argparse.ArgumentParser(description="AirPlay BLE beacon for XMirror")
    parser.add_argument('--path',  default=os.path.expanduser("~/.xmirror.ble"),
                        help="Path to XMirror BLE data file (default: ~/.xmirror.ble)")
    parser.add_argument('--ipv4',  default=None,
                        help="Override IPv4 address advertised to clients")
    parser.add_argument('--interval', type=float, default=1.0,
                        help="Polling interval in seconds (default: 1.0)")
    args = parser.parse_args()

    ipv4 = args.ipv4 or get_local_ipv4()
    print(f"[beacon] Starting. Watching: {args.path}", flush=True)
    print(f"[beacon] Advertising IP: {ipv4}", flush=True)
    print(f"[beacon] Press Ctrl+C to exit", flush=True)

    is_running = False

    try:
        while True:
            port, _ = read_ble_file(args.path)

            if port is not None:
                if not is_running:
                    start_advertising(ipv4, port)
                    is_running = publisher is not None
                elif port != advertised_port:
                    # XMirror restarted on a different port
                    stop_advertising()
                    start_advertising(ipv4, port)
                    is_running = publisher is not None
            else:
                if is_running:
                    stop_advertising()
                    is_running = False

            time.sleep(args.interval)

    except KeyboardInterrupt:
        if is_running:
            stop_advertising()
        print("[beacon] Exiting.", flush=True)
        sys.exit(0)

if __name__ == '__main__':
    main()
