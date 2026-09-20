#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Receive and display newline-delimited UTF-8 data from the TTL serial link."""

import argparse
import json
import os
import select
import termios


BAUDS = {
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    57600: termios.B57600,
    115200: termios.B115200,
}


def open_serial(path, baud):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
        attrs[3] = 0
        attrs[4] = BAUDS[baud]
        attrs[5] = BAUDS[baud]
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        termios.tcflush(fd, termios.TCIOFLUSH)
        return fd
    except Exception:
        os.close(fd)
        raise


def main():
    parser = argparse.ArgumentParser(description="TTL serial receive test")
    parser.add_argument("--port", default="/dev/ttyUSB1")
    parser.add_argument("--baud", type=int, default=115200, choices=BAUDS)
    args = parser.parse_args()

    fd = open_serial(args.port, args.baud)
    buffer = bytearray()
    print(f"Listening on {args.port} at {args.baud} baud. Press Ctrl+C to stop.")
    try:
        while True:
            readable, _, _ = select.select([fd], [], [], 1.0)
            if not readable:
                continue
            chunk = os.read(fd, 512)
            if not chunk:
                continue
            buffer.extend(chunk)
            if len(buffer) > 65536:
                print("Discarded oversized data line.")
                buffer.clear()
                continue

            while b"\n" in buffer:
                raw_line, _, remainder = buffer.partition(b"\n")
                buffer = bytearray(remainder)
                text = raw_line.decode("utf-8", errors="replace").strip()
                if not text:
                    continue
                print(f"Received: {text}")
                try:
                    payload = json.loads(text)
                    reply = {"ok": True, "received_type": payload.get("type", "unknown")}
                except json.JSONDecodeError:
                    reply = {"ok": True, "received_type": "plain_text"}
                os.write(fd, (json.dumps(reply, ensure_ascii=False) + "\n").encode("utf-8"))
    except KeyboardInterrupt:
        print("TTL receive test stopped.")
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
