#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""直接打印一张填好内容的测试标签。"""

import os
from label_printer import build_label_lines, make_label_chunks, next_shipment_id, write_label


PRINTER_DEVICE = os.environ.get("LABEL_PRINTER_DEV", "/dev/ttyACM0")


def main():
    lines = build_label_lines(
        next_shipment_id(), "张三", "李四", "13812345678",
        "北京市朝阳区建国路88号", "水瓶",
        20.0, 8.0, 8.0, 350.0, "大箱",
    )

    print("正在打印以下测试内容：")
    print("-" * 36)
    print("\n".join(lines))
    print("-" * 36)
    total = write_label(PRINTER_DEVICE, make_label_chunks(lines))
    print(f"测试标签已发送到 {PRINTER_DEVICE}，共 {total} 字节。")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\n已取消。")
        raise SystemExit(130)
    except Exception as exc:
        print(f"打印失败：{exc}")
        raise SystemExit(1)
