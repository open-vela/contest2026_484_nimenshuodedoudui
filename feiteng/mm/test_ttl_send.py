#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Send one test line to the other board through the TTL serial link."""

import argparse
import json
import os
import select
import termios
import time


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
    parser = argparse.ArgumentParser(description="TTL serial send test")
    parser.add_argument("--port", default="/dev/ttyUSB1")
    parser.add_argument("--baud", type=int, default=115200, choices=BAUDS)
    parser.add_argument("--message", default="D3000M TTL test")
    parser.add_argument("--interval", type=float, default=1.0, help="send interval, seconds")
    parser.add_argument("--count", type=int, default=0, help="number of messages; 0 means forever")
    args = parser.parse_args()

    fd = open_serial(args.port, args.baud)
    try:
        sequence = 0
        while args.count <= 0 or sequence < args.count:
            sequence += 1
            payload = {
                "type": "ttl_test",
                "message": args.message,
                "sequence": sequence,
                "sent_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            }
            data = (json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8")
            os.write(fd, data)
            print(f"Sent #{sequence} to {args.port}: {data.decode('utf-8').strip()}")
            time.sleep(max(0.05, args.interval))
    except KeyboardInterrupt:
        print("TTL test stopped.")
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
