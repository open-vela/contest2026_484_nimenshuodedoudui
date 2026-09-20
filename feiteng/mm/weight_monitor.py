#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""独立读取 SJ101CX RS485 称重模块，并在终端持续显示实时重量。"""

import argparse
import glob
import math
import os
import select
import struct
import sys
import termios
import time


BAUDS = {
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    115200: termios.B115200,
}

PORT_PATTERNS = (
    "/dev/serial/by-id/*",
    "/dev/serial/by-path/*",
    "/dev/ttyUSB*",
    "/dev/ttyACM*",
    "/dev/ttyCH*",
)


class ModbusError(RuntimeError):
    pass


def crc16_modbus(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def add_crc(frame):
    crc = crc16_modbus(frame)
    return frame + bytes((crc & 0xFF, crc >> 8))


def check_crc(frame):
    if len(frame) < 4:
        raise ModbusError("返回数据太短")
    received = frame[-2] | (frame[-1] << 8)
    expected = crc16_modbus(frame[:-2])
    if received != expected:
        raise ModbusError(
            f"CRC错误：收到0x{received:04x}，应为0x{expected:04x}"
        )


def find_ports():
    ports = []
    seen = set()
    for pattern in PORT_PATTERNS:
        for port in sorted(glob.glob(pattern)):
            real = os.path.realpath(port)
            if real not in seen:
                seen.add(real)
                ports.append(port)
    return ports


def choose_port(configured_port):
    if configured_port:
        return configured_port
    ports = find_ports()
    for port in ports:
        if os.path.basename(port).startswith("ttyUSB"):
            return port
    if len(ports) == 1:
        return ports[0]
    if not ports:
        raise RuntimeError("没有找到RS485串口，请检查USB转RS485模块")
    raise RuntimeError("检测到多个串口，请使用 --port 指定：" + ", ".join(ports))


class SerialPort:
    def __init__(self, path, baud, parity, timeout):
        if baud not in BAUDS:
            raise ValueError(f"不支持波特率 {baud}")
        parity = parity.upper()
        if parity not in {"N", "E", "O"}:
            raise ValueError("校验位必须是 N、E 或 O")

        self.path = path
        self.timeout = timeout
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            attrs = termios.tcgetattr(self.fd)
            attrs[0] = 0
            attrs[1] = 0
            attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
            if parity != "N":
                attrs[2] |= termios.PARENB
                if parity == "O":
                    attrs[2] |= termios.PARODD
            attrs[3] = 0
            attrs[4] = BAUDS[baud]
            attrs[5] = BAUDS[baud]
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            termios.tcflush(self.fd, termios.TCIOFLUSH)
        except Exception:
            os.close(self.fd)
            raise

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def transact(self, request, expected_length):
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        os.write(self.fd, request)
        deadline = time.monotonic() + self.timeout
        data = bytearray()
        while time.monotonic() < deadline:
            readable, _, _ = select.select(
                [self.fd], [], [], max(0.0, min(0.05, deadline - time.monotonic()))
            )
            if readable:
                chunk = os.read(self.fd, 256)
                if chunk:
                    data.extend(chunk)
                    if len(data) >= expected_length:
                        break
        if not data:
            raise ModbusError("等待称重模块响应超时")
        check_crc(data)
        return bytes(data)


class SJ101CX:
    def __init__(self, serial_port, address=1, retries=2):
        self.serial = serial_port
        self.address = address
        self.retries = retries

    def read_holding(self, start, count):
        request = add_crc(struct.pack(">BBHH", self.address, 0x03, start, count))
        last_error = None
        for _ in range(self.retries + 1):
            try:
                response = self.serial.transact(request, 5 + count * 2)
                break
            except ModbusError as exc:
                last_error = exc
                time.sleep(0.05)
        else:
            raise last_error or ModbusError("读取失败")

        if response[0] != self.address or response[1] != 0x03:
            raise ModbusError("模块返回了非预期数据")
        byte_count = response[2]
        if byte_count != count * 2:
            raise ModbusError(f"返回字节数异常：{byte_count}")
        return struct.unpack(">" + "H" * count, response[3 : 3 + byte_count])

    def read_weight(self):
        registers = self.read_holding(0x0000, 4)
        raw = struct.unpack(">i", struct.pack(">HH", registers[0], registers[1]))[0]
        precision = int(registers[2])
        status = int(registers[3])
        weight_kg = raw / (10 ** precision)
        return {
            "raw": raw,
            "weight_g": weight_kg * 1000.0,
            "stable": bool(status & 0x01),
            "zero": bool(status & 0x02),
            "overload": bool(status & 0x04),
            "valid": bool(status & 0x20),
            "status": status,
        }

    def tare(self):
        request = add_crc(struct.pack(">BBHH", self.address, 0x06, 0x0004, 1))
        response = self.serial.transact(request, 8)
        if response != request:
            raise ModbusError("去皮命令返回异常")


def print_reading(reading, new_line=False):
    weight = reading["weight_g"]
    if not math.isfinite(weight):
        raise RuntimeError("称重模块返回非数字重量")
    flags = []
    flags.append("稳定" if reading["stable"] else "变化中")
    if reading["zero"]:
        flags.append("零点")
    if reading["overload"]:
        flags.append("超载")
    if not reading["valid"]:
        flags.append("状态无效")
    text = f"重量: {weight:10.2f} g  状态: {'/'.join(flags):<16} 原始值: {reading['raw']}"
    if new_line:
        print(text, flush=True)
    else:
        print("\r" + text.ljust(90), end="", flush=True)


def parse_args():
    parser = argparse.ArgumentParser(description="实时显示SJ101CX RS485称重模块重量")
    parser.add_argument("--port", help="串口，例如 /dev/ttyUSB0；不填则自动检测")
    parser.add_argument("--baud", type=int, default=115200, choices=BAUDS)
    parser.add_argument("--parity", default="N", choices=("N", "E", "O"))
    parser.add_argument("--address", type=int, default=1, help="Modbus设备地址")
    parser.add_argument("--interval", type=float, default=0.2, help="刷新间隔，单位秒")
    parser.add_argument("--timeout", type=float, default=0.35, help="串口响应超时")
    parser.add_argument("--retries", type=int, default=2, help="单次读取重试次数")
    parser.add_argument("--tare", action="store_true", help="启动后先执行去皮清零")
    parser.add_argument("--lines", action="store_true", help="每次读数输出一行")
    return parser.parse_args()


def main():
    args = parse_args()
    serial_port = None
    try:
        port = choose_port(args.port)
        print(f"正在连接称重模块：{port}，{args.baud} baud，地址 {args.address}")
        serial_port = SerialPort(port, args.baud, args.parity, args.timeout)
        scale = SJ101CX(serial_port, args.address, args.retries)
        if args.tare:
            scale.tare()
            print("去皮成功")
            time.sleep(0.3)
        print("开始读取，按 Ctrl+C 停止。")
        while True:
            print_reading(scale.read_weight(), args.lines)
            time.sleep(max(0.02, args.interval))
    except KeyboardInterrupt:
        if not args.lines:
            print()
        print("已停止。")
        return 0
    except (OSError, RuntimeError, ValueError) as exc:
        if not args.lines:
            print()
        print(f"错误：{exc}", file=sys.stderr)
        return 1
    finally:
        if serial_port is not None:
            serial_port.close()


if __name__ == "__main__":
    raise SystemExit(main())
