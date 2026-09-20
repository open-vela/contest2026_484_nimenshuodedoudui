#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
智慧寄件一体化程序：称重、物品识别、接收订单信息、自动打印。

另一块板子通过 TTL 串口发送一行 UTF-8 JSON，例如：
{"order_id":"WL001","sender":"张三","receiver":"李四",
 "phone":"13812345678","address":"北京市朝阳区建国路88号",
 "length_cm":20,"width_cm":8,"height_cm":8}
"""

import argparse
import json
import math
import os
import select
import threading
import time
from collections import deque

import cv2

from item_recognition import (
    DEFAULT_MODEL,
    LatestFrameCamera,
    OllamaItemRecognizer,
    RecognitionWorker,
    ensure_camera_network,
    find_models_dir,
    load_camera_url,
)
from label_printer import (
    DEFAULT_DEVICE,
    build_label_lines,
    make_label_chunks,
    next_shipment_id,
    recommend_box_type,
    write_label,
)
from weight_monitor import SJ101CX, SerialPort, choose_port


EXTERNAL_REQUIRED_FIELDS = (
    "sender", "receiver", "phone", "address",
)


def clean_text(value):
    return " ".join(str(value or "").split()).strip()


def positive_number(value):
    try:
        number = float(value)
        return number if math.isfinite(number) and number > 0 else 0.0
    except (TypeError, ValueError):
        return 0.0


class OrderStore:
    """合并另一块板子分次发来的订单字段。"""

    TEXT_ALIASES = {
        "order_id": ("order_id", "shipment_id", "id"),
        "sender": ("sender", "sender_name", "sender_id"),
        "receiver": ("receiver", "receiver_name"),
        "phone": ("phone", "mobile", "receiver_phone"),
        "address": ("address", "receiver_address"),
        "box_type": ("box_type", "package_type"),
    }
    NUMBER_ALIASES = {
        "length_cm": ("length_cm", "length"),
        "width_cm": ("width_cm", "width"),
        "height_cm": ("height_cm", "height", "depth_cm", "depth"),
    }

    def __init__(self):
        self._lock = threading.Lock()
        self._order = {}
        self._version = 0

    @staticmethod
    def _first(data, names):
        for name in names:
            if name in data and data[name] is not None:
                return data[name]
        return None

    def update(self, payload):
        if not isinstance(payload, dict):
            raise ValueError("JSON 顶层必须是对象")
        flattened = dict(payload)
        dimensions = payload.get("dimensions_cm")
        if isinstance(dimensions, dict):
            for key in ("length", "width", "height"):
                flattened.setdefault(f"{key}_cm", dimensions.get(key))

        requested_id = clean_text(self._first(flattened, self.TEXT_ALIASES["order_id"]))
        action = clean_text(payload.get("action")).lower()
        with self._lock:
            current_id = clean_text(self._order.get("order_id"))
            start_new = action in ("new", "reset", "new_order")
            if requested_id and requested_id != current_id:
                start_new = True
            if start_new:
                self._order = {}
            if not self._order:
                self._order = {
                    "order_id": requested_id or next_shipment_id(),
                    "printed": False,
                    "received_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                }
            elif requested_id:
                self._order["order_id"] = requested_id

            for field, aliases in self.TEXT_ALIASES.items():
                if field == "order_id":
                    continue
                value = self._first(flattened, aliases)
                if value is not None:
                    self._order[field] = clean_text(value)
            for field, aliases in self.NUMBER_ALIASES.items():
                value = self._first(flattened, aliases)
                if value is not None:
                    self._order[field] = positive_number(value)

            self._version += 1
            return dict(self._order), self._version

    def snapshot(self):
        with self._lock:
            return dict(self._order), self._version

    def mark_printed(self, order_id):
        with self._lock:
            if clean_text(self._order.get("order_id")) != clean_text(order_id):
                return False
            self._order["printed"] = True
            self._order["printed_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
            self._version += 1
            return True

    @staticmethod
    def missing_fields(order):
        missing = []
        for field in EXTERNAL_REQUIRED_FIELDS:
            value = order.get(field)
            if field.endswith("_cm"):
                if positive_number(value) <= 0:
                    missing.append(field)
            elif not clean_text(value):
                missing.append(field)
        return missing


class OrderSerialReceiver:
    """通过 TTL/USB 转串口接收一行 UTF-8 JSON。"""

    def __init__(self, port, baud, store):
        self.port = port
        self.baud = baud
        self.store = store
        self._stop = threading.Event()
        self._thread = None
        self._write_lock = threading.Lock()
        self._fd = -1

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        print(f"TTL 订单串口已启动：{self.port}，{self.baud} baud")

    def send(self, payload):
        """向另一块板发送一行 JSON 状态；串口未连接时返回 False。"""
        data = (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        with self._write_lock:
            if self._fd < 0:
                return False
            try:
                os.write(self._fd, data)
                return True
            except OSError:
                return False

    def _run(self):
        while not self._stop.is_set():
            serial_port = None
            try:
                serial_port = SerialPort(self.port, self.baud, "N", 0.2)
                with self._write_lock:
                    self._fd = serial_port.fd
                buffer = bytearray()
                while not self._stop.is_set():
                    readable, _, _ = select.select([serial_port.fd], [], [], 0.2)
                    if not readable:
                        continue
                    chunk = os.read(serial_port.fd, 512)
                    if not chunk:
                        continue
                    buffer.extend(chunk)
                    if len(buffer) > 65536:
                        buffer.clear()
                        raise ValueError("串口 JSON 数据超过 64KB")

                    while b"\n" in buffer:
                        raw_line, _, remainder = buffer.partition(b"\n")
                        buffer = bytearray(remainder)
                        if not raw_line.strip():
                            continue
                        try:
                            payload = json.loads(raw_line.decode("utf-8"))
                            order, version = self.store.update(payload)
                            missing = self.store.missing_fields(order)
                            print(
                                f"\n已通过 TTL 接收订单：{order.get('order_id')}，"
                                f"待补充：{missing or '无'}",
                                flush=True,
                            )
                        except Exception as exc:
                            print(f"\nTTL 订单解析失败：{exc}", flush=True)
            except Exception as exc:
                if not self._stop.is_set():
                    print(f"\nTTL 订单串口异常：{exc}，2 秒后重连。", flush=True)
                    self._stop.wait(2.0)
            finally:
                if serial_port is not None:
                    with self._write_lock:
                        self._fd = -1
                        serial_port.close()

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)


class ScaleReader:
    def __init__(
        self,
        port,
        baud=115200,
        parity="N",
        address=1,
        timeout=0.35,
        retries=2,
        interval=0.2,
        stable_samples=4,
        stable_tolerance_g=2.0,
        min_weight_g=5.0,
        tare=False,
    ):
        self.port = port
        self.baud = baud
        self.parity = parity
        self.address = address
        self.timeout = timeout
        self.retries = retries
        self.interval = interval
        self.stable_samples = max(2, stable_samples)
        self.stable_tolerance_g = max(0.1, stable_tolerance_g)
        self.min_weight_g = max(0.0, min_weight_g)
        self.tare_on_connect = tare
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None
        self._reading = {}
        self._stable_weight_g = 0.0
        self._error = ""

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _set_error(self, message):
        message = clean_text(message)
        with self._lock:
            changed = message != self._error
            self._error = message
        if message and changed:
            print(f"\n称重模块：{message}", flush=True)

    def _run(self):
        samples = deque(maxlen=self.stable_samples)
        while not self._stop.is_set():
            serial_port = None
            try:
                selected = choose_port(self.port)
                serial_port = SerialPort(selected, self.baud, self.parity, self.timeout)
                scale = SJ101CX(serial_port, self.address, self.retries)
                if self.tare_on_connect:
                    scale.tare()
                    time.sleep(0.3)
                print(f"称重模块已连接：{selected}")
                self._set_error("")
                while not self._stop.is_set():
                    reading = scale.read_weight()
                    weight = float(reading.get("weight_g", 0.0) or 0.0)
                    if not math.isfinite(weight):
                        raise RuntimeError("称重模块返回非数字重量")
                    if reading.get("valid") and reading.get("stable"):
                        if abs(weight) < self.min_weight_g:
                            samples.clear()
                            stable_weight = 0.0
                        else:
                            samples.append(weight)
                            stable_weight = 0.0
                            if (
                                len(samples) == self.stable_samples
                                and max(samples) - min(samples) <= self.stable_tolerance_g
                            ):
                                stable_weight = sum(samples) / len(samples)
                        with self._lock:
                            self._stable_weight_g = stable_weight
                    else:
                        samples.clear()
                        with self._lock:
                            self._stable_weight_g = 0.0
                    with self._lock:
                        self._reading = dict(reading)
                        self._error = ""
                    self._stop.wait(self.interval)
            except Exception as exc:
                samples.clear()
                with self._lock:
                    self._stable_weight_g = 0.0
                self._set_error(str(exc))
                self._stop.wait(2.0)
            finally:
                if serial_port is not None:
                    serial_port.close()

    def snapshot(self):
        with self._lock:
            return dict(self._reading), float(self._stable_weight_g), self._error

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)


def parse_args():
    parser = argparse.ArgumentParser(description="称重、物品识别和标签打印一体化程序")
    parser.add_argument("--order-port", default="/dev/ttyUSB1", help="TTL 订单串口")
    parser.add_argument("--order-baud", type=int, default=115200, help="TTL 订单串口波特率")
    parser.add_argument("--scale-port", help="称重 RS485 串口，例如 /dev/ttyUSB0")
    parser.add_argument("--scale-baud", type=int, default=115200)
    parser.add_argument("--scale-address", type=int, default=1)
    parser.add_argument("--tare", action="store_true", help="称重模块连接后去皮")
    parser.add_argument("--min-weight", type=float, default=5.0, help="有效物品最小重量，g")
    parser.add_argument("--stable-samples", type=int, default=4)
    parser.add_argument("--stable-tolerance", type=float, default=2.0, help="稳定重量允许波动，g")
    parser.add_argument("--camera", default="/dev/video0", help="USB 摄像头设备，默认 /dev/video0")
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--recognition-retries", type=int, default=3)
    parser.add_argument("--manual-recognition", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--printer-device", default=DEFAULT_DEVICE)
    parser.add_argument("--dry-run", action="store_true", help="信息齐全后只预览，不写入打印机")
    parser.add_argument("--no-display", action="store_true", help="不显示摄像头窗口")
    return parser.parse_args()


def make_status(order, missing, weight_g, item_name, recognizing, waiting_clear):
    if waiting_clear:
        return "已打印，请移走物品并等待重量回零"
    if not order:
        return "等待另一块板子发送订单信息"
    pending = list(missing)
    if weight_g <= 0:
        pending.append("稳定重量")
    if not item_name:
        pending.append("物品识别" if not recognizing else "物品识别中")
    return "信息已齐，准备打印" if not pending else "等待：" + "、".join(pending)


def package_details(order):
    length_cm = positive_number(order.get("length_cm"))
    width_cm = positive_number(order.get("width_cm"))
    height_cm = positive_number(order.get("height_cm"))
    # 当前项目统一使用中等包裹，不再依据尺寸自动推荐。
    box_type = "中箱"
    return length_cm, width_cm, height_cm, box_type


def main():
    args = parse_args()
    store = OrderStore()
    order_serial = OrderSerialReceiver(args.order_port, args.order_baud, store)
    scale = ScaleReader(
        args.scale_port,
        baud=args.scale_baud,
        address=args.scale_address,
        stable_samples=args.stable_samples,
        stable_tolerance_g=args.stable_tolerance,
        min_weight_g=args.min_weight,
        tare=args.tare,
    )
    camera = None

    camera_url = load_camera_url(args.camera)
    models_dir = find_models_dir(args.model)
    recognizer = OllamaItemRecognizer(args.model, models_dir)
    print(f"正在准备模型服务：{args.model}")
    recognizer.ensure_running()
    ensure_camera_network(camera_url)
    print(f"正在连接 USB 摄像头：{camera_url}")
    camera = LatestFrameCamera(camera_url)
    camera.start()
    first_frame = camera.wait_for_frame(timeout=15.0)
    if first_frame is None:
        camera.stop()
        raise RuntimeError(f"USB 摄像头连接失败：{camera_url}")

    worker = RecognitionWorker(recognizer)
    order_serial.start()
    scale.start()
    print("一体化程序已启动：只有按 R 才识别，Q=退出。")

    item_name = ""
    active_order_id = ""
    recognition_order_id = ""
    recognition_pending = False
    recognition_attempts = 0
    next_recognition_at = 0.0
    next_print_at = 0.0
    waiting_clear = False
    last_frame = first_frame
    last_status = ""
    next_board_update_at = 0.0

    try:
        while True:
            order, _version = store.snapshot()
            order_id = clean_text(order.get("order_id"))
            if order_id != active_order_id:
                previous_order_id = active_order_id
                active_order_id = order_id
                # 允许先按 R 识别物品、随后再接收订单；只有订单之间切换才清空结果。
                if previous_order_id and order_id:
                    item_name = ""
                recognition_attempts = 0
                recognition_pending = False

            reading, stable_weight_g, scale_error = scale.snapshot()
            current_weight_g = float(reading.get("weight_g", 0.0) or 0.0)
            scale_zero = bool(
                reading.get("valid")
                and reading.get("stable")
                and abs(current_weight_g) < args.min_weight
            )
            if waiting_clear and scale_zero:
                waiting_clear = False
                item_name = ""
                recognition_attempts = 0
                print("\n重量已回零，可以处理下一单。", flush=True)

            if recognition_pending and not worker.busy:
                recognition_pending = False
                if not worker.error:
                    result = clean_text(worker.result)
                    if result and result != "未识别出物品":
                        item_name = result
                next_recognition_at = time.monotonic() + 3.0

            frame, frame_age, camera_connected = camera.latest()
            if frame is not None:
                last_frame = frame
            else:
                frame = last_frame
            frame_fresh = frame is not None and frame_age < 3.0 and camera_connected

            can_manual_recognize = bool(
                not worker.busy
                and not recognition_pending
                and frame_fresh
            )

            missing = store.missing_fields(order) if order else list(EXTERNAL_REQUIRED_FIELDS)
            length_cm, width_cm, height_cm, box_type = package_details(order)
            status = make_status(
                order,
                missing,
                stable_weight_g,
                item_name,
                worker.busy or recognition_pending,
                waiting_clear,
            )
            if status != last_status:
                weight_text = f"{stable_weight_g:.1f}g" if stable_weight_g > 0 else (scale_error or "未稳定")
                print(f"\n状态：{status}；重量={weight_text}；物品={item_name or '-'}", flush=True)
                last_status = status

            if time.monotonic() >= next_board_update_at:
                order_serial.send({
                    "type": "parcel",
                    "item": item_name or "识别中",
                    "weight": f"{current_weight_g:.0f}g",
                    "package": box_type,
                })
                next_board_update_at = time.monotonic() + 1.0

            ready_to_print = bool(
                order_id
                and not order.get("printed")
                and not waiting_clear
                and not missing
                and stable_weight_g >= args.min_weight
                and item_name
                and not worker.busy
                and time.monotonic() >= next_print_at
            )
            if ready_to_print:
                lines = build_label_lines(
                    order_id,
                    order.get("sender"),
                    order.get("receiver"),
                    order.get("phone"),
                    order.get("address"),
                    item_name,
                    length_cm,
                    width_cm,
                    height_cm,
                    stable_weight_g,
                    box_type,
                )
                print("\n信息已齐，标签内容：")
                print("-" * 36)
                print("\n".join(lines))
                print("-" * 36)
                try:
                    if args.dry_run:
                        print("预览模式：未向打印机写入数据。")
                    else:
                        total = write_label(args.printer_device, make_label_chunks(lines))
                        print(f"标签已打印，共发送 {total} 字节。")
                    store.mark_printed(order_id)
                    order_serial.send({
                        "type": "parcel",
                        "event": "previewed" if args.dry_run else "printed",
                        "item": item_name,
                        "weight": f"{stable_weight_g:.0f}g",
                        "package": box_type,
                    })
                    waiting_clear = True
                    last_status = ""
                except Exception as exc:
                    print(f"打印失败：{exc}", flush=True)
                    next_print_at = time.monotonic() + 5.0

            key = -1
            if not args.no_display and frame is not None:
                preview = frame.copy()
                if waiting_clear:
                    display_status = "printed - remove item"
                elif worker.busy or recognition_pending:
                    display_status = "recognizing..."
                elif not frame_fresh:
                    display_status = "camera reconnecting..."
                elif not order_id:
                    display_status = "waiting for order data"
                elif missing:
                    display_status = "waiting for board data"
                elif stable_weight_g < args.min_weight:
                    display_status = "waiting for stable weight"
                elif not item_name:
                    display_status = "waiting for item recognition"
                else:
                    display_status = "ready to print"
                cv2.putText(preview, display_status, (16, 34), cv2.FONT_HERSHEY_SIMPLEX, 0.58, (0, 255, 0), 2)
                cv2.putText(
                    preview,
                    f"weight:{stable_weight_g:.1f}g item:{item_name or '-'}",
                    (16, 66),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.58,
                    (0, 220, 255),
                    2,
                )
                cv2.putText(preview, "R: recognize  Q: quit", (16, 98), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 2)
                cv2.imshow("Integrated Shipping", preview)
                key = cv2.waitKey(1) & 0xFF

            if key in (ord("r"), ord("R")):
                if can_manual_recognize:
                    recognition_attempts += 1
                    recognition_order_id = active_order_id
                    recognition_pending = worker.submit(frame)
                    if recognition_pending:
                        print("\n已截取当前画面，开始物品识别……", flush=True)
                elif worker.busy or recognition_pending:
                    print("\n上一次识别尚未完成。", flush=True)
                elif not frame_fresh:
                    print("\n摄像头正在重连，暂时不能识别。", flush=True)
                else:
                    print("\n当前不能识别。", flush=True)
            if key in (ord("q"), ord("Q"), 27):
                break
            time.sleep(0.003)
    except KeyboardInterrupt:
        pass
    finally:
        scale.stop()
        order_serial.stop()
        if camera is not None:
            camera.stop()
        cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"错误：{exc}")
        raise SystemExit(1)
