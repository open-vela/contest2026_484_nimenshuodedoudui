#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GY-EP201L 标签打印机独立打印程序。"""

import argparse
import os
import secrets
import string
import struct
import subprocess
import time
from pathlib import Path


DEFAULT_DEVICE = "/dev/ttyACM0"
LABEL_START_Y = 32
LABEL_LINE_STEP = 42
BOX_TYPE_SPECS_CM = (
    ("小箱", (8.0, 8.0, 12.0)),
    ("中箱", (15.0, 15.0, 15.0)),
    ("大箱", (26.0, 16.0, 26.0)),
)


def clean_text(value):
    return " ".join(str(value or "").split()).strip()


def mask_phone(value):
    phone = clean_text(value).replace(" ", "").replace("-", "")
    if len(phone) >= 7:
        return f"{phone[:3]}****{phone[-4:]}"
    return phone


def wrap_address(value, width=20, max_lines=2):
    address = clean_text(value)
    parts = [address[i:i + width] for i in range(0, len(address), width)] or [""]
    if len(parts) > max_lines:
        parts = parts[:max_lines]
        parts[-1] = parts[-1][:-1] + "…"
    return parts


def build_label_lines(
    shipment_id,
    sender,
    receiver,
    phone,
    address,
    item,
    length_cm,
    width_cm,
    height_cm,
    weight_g,
    box_type,
    printed_at=None,
):
    if length_cm <= 0 or width_cm <= 0:
        size_text = "尺寸:未测量"
    elif height_cm > 0:
        size_text = f"尺寸:{length_cm:.1f}x{width_cm:.1f}x{height_cm:.1f}cm"
    else:
        size_text = f"尺寸:{length_cm:.1f}x{width_cm:.1f}cm"
    lines = [
        "智慧物流寄件凭证",
        f"编号:{clean_text(shipment_id)}",
        f"寄:{clean_text(sender)}  收:{clean_text(receiver)}",
        f"手机:{mask_phone(phone)}",
    ]
    lines.extend(
        (f"地址:{part}" if index == 0 else f"     {part}")
        for index, part in enumerate(wrap_address(address))
    )
    lines.extend([
        f"物品:{clean_text(item)}  包装:{clean_text(box_type)}",
        f"{size_text}  重量:{weight_g:.1f}g",
        f"时间:{printed_at or time.strftime('%Y-%m-%d %H:%M')}",
    ])
    return lines


def next_shipment_id():
    now = time.time()
    date_part = time.strftime("%y%m%d%H%M%S", time.localtime(now))
    millis = int((now - int(now)) * 1000)
    alphabet = string.digits + string.ascii_uppercase
    random_part = "".join(secrets.choice(alphabet) for _ in range(2))
    return f"WL{date_part}{millis:03d}{random_part}"


def recommend_box_type(length_cm, width_cm, height_cm):
    item_sides = sorted(
        (max(length_cm, 0.0), max(width_cm, 0.0), max(height_cm, 0.0)),
        reverse=True,
    )
    for box_type, dimensions in BOX_TYPE_SPECS_CM:
        if all(a <= b for a, b in zip(item_sides, sorted(dimensions, reverse=True))):
            return box_type
    return "大箱"


def label_text_command(x, y, text):
    x = max(0, min(383, int(x)))
    y = max(0, min(319, int(y)))
    payload = clean_text(text)[:24].encode("gbk", errors="replace")
    return b"\x1A\x54" + struct.pack(">HHB", x, y, 0) + payload + b"\x00"


def make_label_chunks(lines):
    chunks = [
        b"\x1B\x40",
        b"\x1A\x5B\x01\x00\x00\x00\x00\x80\x01\x40\x01\x00",
    ]
    # 字段增加后自动紧凑行距，保证内容不超出 320 点高度。
    line_step = min(
        LABEL_LINE_STEP,
        max(28, (300 - LABEL_START_Y) // max(1, len(lines) - 1)),
    )
    y = LABEL_START_Y
    for line in lines:
        chunks.append(label_text_command(8, y, line))
        y += line_step
    chunks.extend((b"\x1A\x5D\x00", b"\x1A\x4F\x00"))
    return chunks


def configure_printer(device):
    try:
        subprocess.run(
            [
                "stty", "-F", device, "115200", "cs8", "-cstopb", "-parenb",
                "clocal", "-crtscts",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
    except FileNotFoundError as exc:
        raise RuntimeError("系统没有 stty 命令") from exc
    except subprocess.CalledProcessError as exc:
        detail = clean_text(exc.stderr) or "串口参数设置失败"
        raise RuntimeError(detail) from exc


def write_label(device, chunks, delay=0.12):
    path = Path(device)
    if not path.exists():
        raise RuntimeError(f"找不到打印机设备 {device}")
    configure_printer(device)
    total = 0
    try:
        with path.open("wb", buffering=0) as handle:
            for chunk in chunks:
                handle.write(chunk)
                handle.flush()
                total += len(chunk)
                time.sleep(delay)
    except PermissionError as exc:
        raise RuntimeError(
            f"没有写入 {device} 的权限，请把当前用户加入 dialout 组"
        ) from exc
    return total


def ask_text(label, current="", optional=False):
    if clean_text(current):
        return clean_text(current)
    while True:
        value = clean_text(input(f"{label}{'(可留空)' if optional else ''}："))
        if value or optional:
            return value
        print(f"{label}不能为空。")


def ask_number(label, current, optional=False):
    if current is not None:
        value = float(current)
    else:
        while True:
            raw = clean_text(input(f"{label}{'(可留空)' if optional else ''}："))
            if optional and not raw:
                return 0.0
            try:
                value = float(raw)
                break
            except ValueError:
                print("请输入数字。")
    if value < 0 or (not optional and value <= 0):
        raise ValueError(f"{label}必须大于 0")
    return value


def parse_args():
    parser = argparse.ArgumentParser(description="打印智慧物流寄件标签")
    parser.add_argument("--device", default=os.environ.get("LABEL_PRINTER_DEV", DEFAULT_DEVICE))
    parser.add_argument("--shipment-id", help="订单编号；默认自动生成")
    parser.add_argument("--sender", help="寄件人")
    parser.add_argument("--receiver", help="收件人")
    parser.add_argument("--phone", help="收件人手机号；打印时自动脱敏")
    parser.add_argument("--address", help="收件地址")
    parser.add_argument("--item", help="物品名称")
    parser.add_argument("--length", type=float, help="长度，cm")
    parser.add_argument("--width", type=float, help="宽度，cm")
    parser.add_argument("--height", type=float, help="高度，cm；可不填")
    parser.add_argument("--weight", type=float, help="重量，g")
    parser.add_argument("--box-type", help="箱型；默认按尺寸自动推荐")
    parser.add_argument("--yes", action="store_true", help="不询问确认，直接打印")
    parser.add_argument("--dry-run", action="store_true", help="只预览内容，不写入打印机")
    return parser.parse_args()


def main():
    args = parse_args()
    fully_interactive = all(
        value is None
        for value in (
            args.sender, args.receiver, args.phone, args.address, args.item,
            args.length, args.width, args.weight,
        )
    )
    sender = ask_text("寄件人", args.sender)
    receiver = ask_text("收件人", args.receiver)
    phone = ask_text("收件人手机号", args.phone)
    address = ask_text("收件地址", args.address)
    item = ask_text("物品", args.item)
    length = ask_number("长度(cm)", args.length)
    width = ask_number("宽度(cm)", args.width)
    height_arg = args.height if args.height is not None else (None if fully_interactive else 0.0)
    height = ask_number("高度(cm)", height_arg, optional=True)
    weight = ask_number("重量(g)", args.weight)
    shipment_id = clean_text(args.shipment_id) or next_shipment_id()
    box_type = clean_text(args.box_type) or recommend_box_type(length, width, height)

    lines = build_label_lines(
        shipment_id, sender, receiver, phone, address, item,
        length, width, height, weight, box_type,
    )

    print("\n将要打印的内容：")
    print("-" * 36)
    print("\n".join(lines))
    print("-" * 36)
    chunks = make_label_chunks(lines)
    if args.dry_run:
        print(f"预览完成，指令共 {sum(map(len, chunks))} 字节，未访问打印机。")
        return 0
    if not args.yes:
        answer = clean_text(input("确认打印？[Y/n]：")).lower()
        if answer not in ("", "y", "yes", "是"):
            print("已取消。")
            return 0
    total = write_label(args.device, chunks)
    print(f"打印指令已发送：{args.device}，共 {total} 字节。")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (EOFError, KeyboardInterrupt):
        print("\n已取消。")
        raise SystemExit(130)
    except Exception as exc:
        print(f"打印失败：{exc}")
        raise SystemExit(1)
