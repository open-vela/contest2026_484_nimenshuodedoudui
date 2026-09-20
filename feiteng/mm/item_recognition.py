#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""USB 摄像头 + Ollama 模型的独立物品识别程序。"""

import argparse
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import cv2


BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = BASE_DIR / "camera_config.json"
LOCAL_MODELS = BASE_DIR / "models"
DEFAULT_MODEL = "minicpm-v4.6:latest"
PROMPT = (
    "识别图片中心的主要物品。只输出 JSON，不要解释，不要输出思考过程。"
    '格式必须是：{"item":"物品名"}。物品名使用1到8个中文字符。'
)


def find_models_dir(model):
    name, _, tag = model.partition(":")
    relative = Path("manifests/registry.ollama.ai/library") / name / (tag or "latest")
    global_models = Path.home() / ".ollama" / "models"
    if (global_models / relative).exists():
        return global_models
    if (LOCAL_MODELS / relative).exists():
        return LOCAL_MODELS
    raise RuntimeError(f"本机没有找到模型 {model}，请先执行：ollama pull {model}")


def load_camera_url(cli_value):
    if cli_value:
        return cli_value
    env_value = os.environ.get("ITEM_CAMERA_DEVICE", "").strip()
    if env_value:
        return env_value
    return "/dev/video0"


def is_usb_camera(source):
    text = str(source).strip()
    return text.isdigit() or text.startswith("/dev/video")


def load_camera_config():
    if not CONFIG_PATH.exists():
        return {}
    return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))


def camera_host(url):
    return urllib.parse.urlparse(url).hostname or ""


def resolve_camera_interface(config, host):
    configured = str(config.get("camera_interface", "")).strip()
    if configured and Path("/sys/class/net", configured).exists():
        return configured
    ip_tool = shutil.which("ip")
    if ip_tool and host:
        result = subprocess.run(
            [ip_tool, "route", "get", host], capture_output=True, text=True
        )
        match = re.search(r"\bdev\s+(\S+)", result.stdout)
        if result.returncode == 0 and match:
            return match.group(1)
    net_root = Path("/sys/class/net")
    if net_root.exists():
        names = [p.name for p in net_root.iterdir() if p.name != "lo"]
        wired = [n for n in names if not (net_root / n / "wireless").exists()]
        preferred = [n for n in wired if n.startswith("enx")]
        preferred += [n for n in wired if n.startswith(("enp", "eth")) and n not in preferred]
        if preferred:
            return preferred[0]
    return ""


def run_ip_command(arguments):
    ip_tool = shutil.which("ip")
    if not ip_tool:
        return False, "系统没有 ip 命令"
    result = subprocess.run([ip_tool, *arguments], capture_output=True, text=True)
    if result.returncode == 0:
        return True, ""
    sudo = shutil.which("sudo")
    if sudo:
        result = subprocess.run(
            [sudo, "-n", ip_tool, *arguments], capture_output=True, text=True
        )
        if result.returncode == 0:
            return True, ""
    return False, (result.stderr or "配置网卡失败").strip()


def ensure_camera_network(url):
    if is_usb_camera(url):
        return
    if not sys.platform.startswith("linux"):
        return
    config = load_camera_config()
    host = camera_host(url)
    interface = resolve_camera_interface(config, host)
    local_ip = str(config.get("camera_local_ip", "192.168.138.1/24")).strip()
    if not interface:
        raise RuntimeError("没有找到摄像头使用的有线网卡")
    ok, error = run_ip_command(["link", "set", interface, "up"])
    if not ok:
        raise RuntimeError(f"无法启用网卡 {interface}：{error}")
    result = subprocess.run(
        ["ip", "addr", "show", "dev", interface], capture_output=True, text=True
    )
    if local_ip.split("/")[0] not in result.stdout:
        ok, error = run_ip_command(["addr", "add", local_ip, "dev", interface])
        if not ok and "File exists" not in error:
            raise RuntimeError(
                f"无法给网卡 {interface} 配置 {local_ip}：{error}。"
                f"请先执行 sudo ip addr add {local_ip} dev {interface}"
            )
    print(f"摄像头网卡已就绪：{interface} = {local_ip}，目标 {host}")


def alternate_rtsp_url(url):
    match = re.search(r"(/Streaming/Channels/)(10[12])(?=\D*$|$)", url, re.I)
    if not match:
        return None
    channel = "101" if match.group(2) == "102" else "102"
    return url[: match.start(2)] + channel + url[match.end(2) :]


def open_camera(url, read_timeout_ms=60000):
    if is_usb_camera(url):
        text = str(url).strip()
        source = int(text) if text.isdigit() else text
        backend = cv2.CAP_V4L2 if sys.platform.startswith("linux") else cv2.CAP_ANY
        cap = cv2.VideoCapture(source, backend)
        if not cap.isOpened() and backend != cv2.CAP_ANY:
            cap.release()
            cap = cv2.VideoCapture(source)
        if not cap.isOpened():
            cap.release()
            return None
        # 640x480 足够用于模型识别，也能降低 USB 带宽，保证实时预览流畅。
        cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        cap.set(cv2.CAP_PROP_FPS, 30)
        cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        return cap

    os.environ.setdefault(
        "OPENCV_FFMPEG_CAPTURE_OPTIONS",
        "rtsp_transport;tcp|stimeout;3000000|timeout;3000000|max_delay;500000",
    )
    urls = [url]
    alternative = alternate_rtsp_url(url)
    if alternative:
        urls.append(alternative)
    for source in urls:
        params = []
        if hasattr(cv2, "CAP_PROP_OPEN_TIMEOUT_MSEC"):
            params += [cv2.CAP_PROP_OPEN_TIMEOUT_MSEC, 4000]
        if hasattr(cv2, "CAP_PROP_READ_TIMEOUT_MSEC"):
            # 板端执行视觉大模型时视频解码可能暂停数秒。
            # 保留原 RTSP 会话，不要过早释放后反复新建连接。
            params += [cv2.CAP_PROP_READ_TIMEOUT_MSEC, max(1000, int(read_timeout_ms))]
        try:
            cap = cv2.VideoCapture(source, cv2.CAP_FFMPEG, params)
        except TypeError:
            cap = cv2.VideoCapture(source, cv2.CAP_FFMPEG)
        if cap.isOpened():
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
            return cap
        cap.release()
    return None


class LatestFrameCamera:
    """持续读取摄像头，只向界面和识别线程提供最新画面。"""

    def __init__(self, url, read_timeout_ms=60000):
        self.url = url
        self.read_timeout_ms = read_timeout_ms
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None
        self._cap = None
        self._frame = None
        self._captured_at = 0.0
        self._connected = False

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _set_connection(self, connected):
        with self._lock:
            changed = self._connected != connected
            self._connected = connected
        return changed

    def _run(self):
        connected_once = False
        while not self._stop.is_set():
            cap = open_camera(self.url, self.read_timeout_ms)
            with self._lock:
                self._cap = cap
            if cap is None:
                self._set_connection(False)
                self._stop.wait(0.8)
                continue

            if self._set_connection(True) and connected_once:
                print("摄像头已自动恢复。", flush=True)
            connected_once = True
            while not self._stop.is_set():
                ok, frame = cap.read()
                if not ok or frame is None:
                    break
                with self._lock:
                    self._frame = frame
                    self._captured_at = time.monotonic()

            cap.release()
            with self._lock:
                self._cap = None
            if not self._stop.is_set() and self._set_connection(False):
                print("摄像头流中断，正在自动重连……", flush=True)
            self._stop.wait(0.5)

    def latest(self):
        with self._lock:
            frame = None if self._frame is None else self._frame.copy()
            age = time.monotonic() - self._captured_at if self._captured_at else float("inf")
            connected = self._connected
        return frame, age, connected

    def wait_for_frame(self, timeout=12.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame, age, connected = self.latest()
            if frame is not None and age < 2.0:
                return frame
            if self._stop.wait(0.05):
                break
        return None

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            # VideoCapture 由采集线程自行释放；不在主线程强制 release。
            self._thread.join(timeout=1.0)


def clean_answer(text):
    text = str(text or "").strip()
    match = re.search(r"\{.*?\}", text, re.S)
    if match:
        try:
            data = json.loads(match.group(0))
            for key in ("item", "物品", "name", "object"):
                if data.get(key):
                    text = str(data[key]).strip()
                    break
        except json.JSONDecodeError:
            pass
    text = re.sub(r"<think>.*?</think>", "", text, flags=re.S | re.I).strip()
    item_match = re.search(r'["\']?(?:item|物品)["\']?\s*[:：]\s*["\']([^"\'{}\r\n]+)', text, re.I)
    if item_match:
        text = item_match.group(1)
    text = text.splitlines()[0].strip(" 。,，:：;；\"'`[]{}()（）") if text else ""
    for marker in ("注意", "检查", "输出", "格式", "主要物品", "需要", "确保", "JSON", "fmt"):
        if marker.lower() in text.lower():
            return ""
    if not re.search(r"[\u4e00-\u9fff]", text) or len(text) > 12:
        return ""
    return text


class OllamaItemRecognizer:
    def __init__(self, model, models_dir, timeout=120):
        self.model = model
        self.models_dir = Path(models_dir)
        self.timeout = timeout
        self.url = "http://127.0.0.1:11434/api/generate"
        self.health_url = "http://127.0.0.1:11434/api/tags"
        self.process = None

    def ready(self):
        try:
            with urllib.request.urlopen(self.health_url, timeout=1.5) as response:
                return 200 <= response.status < 300
        except (urllib.error.URLError, TimeoutError):
            return False

    def ensure_running(self):
        if self.ready():
            return
        env = os.environ.copy()
        env["OLLAMA_MODELS"] = str(self.models_dir)
        try:
            self.process = subprocess.Popen(
                ["ollama", "serve"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env=env,
            )
        except FileNotFoundError as exc:
            raise RuntimeError("没有安装 Ollama") from exc
        deadline = time.time() + 20
        while time.time() < deadline:
            if self.ready():
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.5)
        raise RuntimeError("Ollama 服务启动失败")

    def recognize(self, frame):
        height, width = frame.shape[:2]
        longest = max(height, width)
        if longest > 448:
            scale = 448.0 / longest
            frame = cv2.resize(frame, (int(width * scale), int(height * scale)))
        ok, encoded = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 92])
        if not ok:
            raise RuntimeError("摄像头画面编码失败")
        payload = {
            "model": self.model,
            "prompt": PROMPT,
            "images": [base64.b64encode(encoded).decode("ascii")],
            "stream": False,
            # 每次按 R 时才加载模型，识别后立即从内存卸载。
            "keep_alive": 0,
            "format": "json",
            "options": {"temperature": 0, "top_p": 0.1, "top_k": 1, "num_predict": 24},
        }
        request = urllib.request.Request(
            self.url,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                data = json.loads(response.read().decode("utf-8"))
        except urllib.error.URLError as exc:
            raise RuntimeError(f"Ollama 请求失败：{exc.reason}") from exc
        if data.get("error"):
            raise RuntimeError(str(data["error"]))
        return clean_answer(data.get("response", ""))


class RecognitionWorker:
    def __init__(self, recognizer):
        self.recognizer = recognizer
        self.busy = False
        self.result = "等待识别"
        self.error = ""
        self.elapsed = 0.0

    def submit(self, frame):
        if self.busy:
            return False
        self.busy = True
        self.error = ""
        threading.Thread(target=self._run, args=(frame.copy(),), daemon=True).start()
        return True

    def _run(self, frame):
        started = time.time()
        try:
            result = self.recognizer.recognize(frame)
            self.result = result or "未识别出物品"
            print(f"\n识别结果：{self.result}", flush=True)
        except Exception as exc:
            self.error = str(exc)
            print(f"\n识别失败：{exc}", flush=True)
        finally:
            self.elapsed = time.time() - started
            self.busy = False


def parse_args():
    parser = argparse.ArgumentParser(description="USB 摄像头实时物品识别")
    parser.add_argument("--camera", default="/dev/video0", help="USB 摄像头设备，默认 /dev/video0")
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--interval", type=float, default=0.0, help="自动识别间隔；默认0表示仅按R识别")
    parser.add_argument("--no-display", action="store_true", help="不显示画面，只在终端输出")
    return parser.parse_args()


def main():
    args = parse_args()
    camera_url = load_camera_url(args.camera)
    models_dir = find_models_dir(args.model)
    recognizer = OllamaItemRecognizer(args.model, models_dir)
    print(f"正在准备模型服务：{args.model}")
    recognizer.ensure_running()
    print(f"模型将在按 R 后按需运行，正在连接 USB 摄像头：{camera_url}")
    ensure_camera_network(camera_url)
    camera = LatestFrameCamera(camera_url)
    camera.start()
    first_frame = camera.wait_for_frame()
    if first_frame is None:
        camera.stop()
        raise RuntimeError(f"USB 摄像头连接失败：{camera_url}")

    worker = RecognitionWorker(recognizer)
    last_submit = 0.0
    last_frame = first_frame

    print("开始运行：按 R 立即识别，按 Q 退出。")
    try:
        while True:
            frame, frame_age, camera_connected = camera.latest()
            if frame is not None:
                last_frame = frame
            else:
                frame = last_frame
            now = time.time()
            frame_is_fresh = frame is not None and frame_age < 2.0 and camera_connected
            if (
                args.interval > 0
                and frame_is_fresh
                and not worker.busy
                and now - last_submit >= args.interval
            ):
                print("\n开始本次识别……", flush=True)
                if worker.submit(frame):
                    last_submit = now

            key = -1
            if not args.no_display and frame is not None:
                preview = frame.copy()
                if worker.busy:
                    status = "recognizing..."
                    color = (0, 220, 255)
                elif not frame_is_fresh:
                    status = "camera reconnecting..."
                    color = (0, 0, 255)
                else:
                    status = f"result: {worker.result}"
                    color = (0, 255, 0)
                cv2.putText(preview, status, (20, 38), cv2.FONT_HERSHEY_SIMPLEX, 0.8, color, 2)
                cv2.putText(preview, "R: recognize   Q: quit", (20, 72), cv2.FONT_HERSHEY_SIMPLEX, 0.65, (0, 220, 255), 2)
                cv2.imshow("Item Recognition", preview)
                key = cv2.waitKey(1) & 0xFF
            if key in (ord("r"), ord("R")):
                if not frame_is_fresh:
                    print("摄像头正在重连，暂时不能识别。", flush=True)
                elif worker.busy:
                    print("上一次识别尚未完成。", flush=True)
                else:
                    print("\n已截取当前画面，开始本次识别……", flush=True)
                    if worker.submit(frame):
                        last_submit = now
            if key in (ord("q"), ord("Q"), 27):
                break
            time.sleep(0.01 if args.no_display else 0.003)
    except KeyboardInterrupt:
        pass
    finally:
        camera.stop()
        cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"错误：{exc}")
        raise SystemExit(1)
