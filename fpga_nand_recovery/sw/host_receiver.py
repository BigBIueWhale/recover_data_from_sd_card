#!/usr/bin/env python3
"""
host_receiver.py
Host-side UART receiver for NAND flash dump

Connects to the Arty Z7's USB-UART and captures the raw NAND dump stream
to a binary file. The dump file contains every physical NAND page in
sequential order, including spare/OOB areas.

Usage:
    python3 host_receiver.py [OPTIONS]

Options:
    --port PORT       Serial port (default: /dev/ttyUSB1 on Linux)
    --baud BAUD       Baud rate (default: 921600)
    --output FILE     Output file (default: nand_raw_dump.bin)
    --interactive     Interactive mode (terminal, no dump capture)

The dump protocol from the ARM firmware:
    "DUMP_START\r\n"
    For each page:
        4 bytes: page_byte_count (uint32 LE)
        3 bytes: row_address (uint24 LE)
        N bytes: page data (N = page_byte_count)
    "DUMP_END\r\n"
"""

import argparse
import serial
import struct
import sys
import time
import os


def find_serial_port():
    """Auto-detect the Arty Z7 serial port."""
    candidates = [
        "/dev/ttyUSB1",   # Linux: second FTDI interface (UART)
        "/dev/ttyUSB0",   # Linux: first FTDI interface
        "/dev/ttyACM0",   # Linux: CDC-ACM
        "COM4",           # Windows: typical
        "COM3",
    ]
    for port in candidates:
        try:
            s = serial.Serial(port, 921600, timeout=0.1)
            s.close()
            return port
        except (serial.SerialException, OSError):
            continue
    return None


def interactive_mode(ser):
    """Simple terminal for sending commands and viewing responses."""
    print("Interactive mode. Type commands (R, I, S, G, D, V). Ctrl+C to exit.")
    import threading

    def reader():
        while True:
            try:
                data = ser.read(256)
                if data:
                    sys.stdout.write(data.decode("ascii", errors="replace"))
                    sys.stdout.flush()
            except Exception:
                break

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    try:
        while True:
            line = input()
            ser.write((line + "\r").encode("ascii"))
    except (KeyboardInterrupt, EOFError):
        print("\nExiting interactive mode.")


def wait_for_line(ser, target, timeout=30):
    """Read lines until one starts with 'target'. Return the full line."""
    start = time.time()
    buf = b""
    while time.time() - start < timeout:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line_str = line.decode("ascii", errors="replace").strip()
                print(f"  < {line_str}")
                if line_str.startswith(target):
                    return line_str
    return None


def capture_dump(ser, output_path):
    """Capture the full NAND dump stream to a binary file."""

    print(f"Sending 'D' command to start dump...")
    ser.write(b"D")

    # Wait for DUMP_START
    line = wait_for_line(ser, "DUMP_START", timeout=10)
    if line is None:
        print("ERROR: Did not receive DUMP_START. Is the NAND connected?")
        return

    # Read geometry info line
    wait_for_line(ser, "  page_data=", timeout=5)

    pages_captured = 0
    bytes_captured = 0
    start_time = time.time()

    with open(output_path, "wb") as f:
        print(f"Capturing dump to: {output_path}")

        while True:
            # Check for DUMP_END (text lines mixed into binary stream)
            # The ARM firmware sends progress lines every 64 blocks
            # We need to handle the mix of binary page data and text lines

            # Read the 7-byte header: page_bytes(4) + row_addr(3)
            hdr = ser.read(7)
            if len(hdr) < 7:
                # Check for text (DUMP_END or progress)
                hdr += ser.read(64)
                text = hdr.decode("ascii", errors="replace")
                if "DUMP_END" in text:
                    print("\nDUMP_END received.")
                    break
                if "BLK=" in text:
                    # Progress update — extract and display
                    for part in text.split("\n"):
                        part = part.strip()
                        if part.startswith("BLK="):
                            blk = int(part[4:], 16)
                            elapsed = time.time() - start_time
                            rate = bytes_captured / elapsed if elapsed > 0 else 0
                            print(f"\r  Block {blk} | "
                                  f"{pages_captured} pages | "
                                  f"{bytes_captured / (1024*1024):.1f} MB | "
                                  f"{rate / 1024:.1f} KB/s",
                                  end="", flush=True)
                    continue
                print(f"\nUnexpected data: {text[:80]}")
                continue

            # Check for error marker
            if hdr[4:7] == b"ERR":
                page_bytes = struct.unpack_from("<I", hdr, 0)[0]
                print(f"\n  Page read error (row data = 0x{page_bytes:08X})")
                continue

            page_bytes = struct.unpack_from("<I", hdr, 0)[0]
            row_addr = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16)

            if page_bytes == 0 or page_bytes > 32768:
                print(f"\n  Invalid page_bytes={page_bytes}, skipping...")
                continue

            # Read the page data
            page_data = b""
            remaining = page_bytes
            while remaining > 0:
                chunk = ser.read(min(remaining, 4096))
                if not chunk:
                    time.sleep(0.001)
                    continue
                page_data += chunk
                remaining -= len(chunk)

            # Write to output file
            f.write(page_data)
            pages_captured += 1
            bytes_captured += len(page_data)

            # Print progress every 100 pages
            if pages_captured % 100 == 0:
                elapsed = time.time() - start_time
                rate = bytes_captured / elapsed if elapsed > 0 else 0
                print(f"\r  Row 0x{row_addr:06X} | "
                      f"{pages_captured} pages | "
                      f"{bytes_captured / (1024*1024):.1f} MB | "
                      f"{rate / 1024:.1f} KB/s",
                      end="", flush=True)

    elapsed = time.time() - start_time
    print(f"\n\nDump complete:")
    print(f"  Pages:    {pages_captured}")
    print(f"  Size:     {bytes_captured / (1024*1024):.1f} MB")
    print(f"  Time:     {elapsed:.1f} seconds")
    print(f"  Rate:     {bytes_captured / elapsed / 1024:.1f} KB/s")
    print(f"  Output:   {os.path.abspath(output_path)}")


def main():
    parser = argparse.ArgumentParser(
        description="Host receiver for Arty Z7 NAND flash dump"
    )
    parser.add_argument("--port", default=None,
                        help="Serial port (auto-detect if not specified)")
    parser.add_argument("--baud", type=int, default=921600,
                        help="Baud rate (default: 921600)")
    parser.add_argument("--output", default="nand_raw_dump.bin",
                        help="Output file for dump (default: nand_raw_dump.bin)")
    parser.add_argument("--interactive", action="store_true",
                        help="Interactive terminal mode")
    args = parser.parse_args()

    port = args.port
    if port is None:
        port = find_serial_port()
        if port is None:
            print("ERROR: Could not auto-detect serial port. Use --port.")
            sys.exit(1)
        print(f"Auto-detected serial port: {port}")

    print(f"Opening {port} at {args.baud} baud...")
    ser = serial.Serial(port, args.baud, timeout=1)

    if args.interactive:
        interactive_mode(ser)
    else:
        # Send version check first
        ser.write(b"V")
        time.sleep(0.5)
        resp = ser.read(256).decode("ascii", errors="replace")
        print(f"  Version response: {resp.strip()}")

        capture_dump(ser, args.output)

    ser.close()


if __name__ == "__main__":
    main()
