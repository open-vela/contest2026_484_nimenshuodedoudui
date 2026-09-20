# 独立称重测试

该程序从原项目中拆出了 SJ101CX RS485/Modbus 称重读取逻辑，不依赖原项目及第三方 Python 包。

在 Linux 上运行：

```bash
cd mm
python3 weight_monitor.py
```

如果自动检测不到或存在多个串口：

```bash
python3 weight_monitor.py --port /dev/ttyUSB0
```

启动时去皮：

```bash
python3 weight_monitor.py --port /dev/ttyUSB0 --tare
```

如果提示没有串口权限：

```bash
sudo usermod -aG dialout "$USER"
```

重新登录后再运行。默认通信参数与旧项目一致：115200 baud、无校验、Modbus 地址 1、每 0.2 秒刷新一次。

## 独立物品识别

该程序使用 USB 摄像头，以及 demo7 当前使用的 Ollama `minicpm-v4.6:latest` 物品识别模型。

在 D3000M 上启动识别：

```bash
cd ~/Desktop/mm
python3 item_recognition.py
```

程序直接使用 demo7 已安装在 `~/.ollama/models` 中的模型，不再重复复制，避免占用数 GB 磁盘空间。默认使用 USB 摄像头 `/dev/video0`：未按键时持续实时预览，按 `R` 截取当前一帧并识别，按 `Q` 退出。若摄像头不是 `/dev/video0`，启动时增加 `--camera /dev/videoX`。

## 独立标签打印

打印机默认使用 `/dev/ttyACM0`。直接运行后按提示输入寄件信息：

```bash
python3 label_printer.py
```

可先只预览而不打印：

```bash
python3 label_printer.py --dry-run
```

也可一次传入所有字段：

```bash
python3 label_printer.py --sender 张三 --receiver 李四 --phone 13812345678 \
  --address 北京市朝阳区建国路88号 --item 水瓶 \
  --length 20 --width 8 --height 8 --weight 350 --yes
```

需要直接打一张已填好内容的测试标签时：

```bash
python3 test_label_print.py
```

## 称重、识别、板间通信和打印一体化

`integrated_shipping.py` 把 USB 摄像头物品识别、SJ101CX RS485 称重、另一块板子的订单信息和标签打印合并到一个程序。

先用预览模式联调（信息齐全后不会真正打印）：

```bash
sudo -v
python3 integrated_shipping.py --scale-port /dev/ttyUSB0 --dry-run
```

确认流程正常后去掉 `--dry-run`：

```bash
python3 integrated_shipping.py --scale-port /dev/ttyUSB0 --printer-device /dev/ttyACM0
```

主板默认在 TCP `8765` 端口接收一行 UTF-8 JSON。另一块板子发送：

```json
{"order_id":"WL001","sender":"张三","receiver":"李四","phone":"13812345678","address":"北京市朝阳区建国路88号","length_cm":20,"width_cm":8,"height_cm":8}
```

例如在另一块 Linux 板子上发送（把 `<主板IP>` 换成运行本程序的板子 IP）：

```bash
printf '%s\n' '{"order_id":"WL001","sender":"张三","receiver":"李四","phone":"13812345678","address":"北京市朝阳区建国路88号","length_cm":20,"width_cm":8,"height_cm":8}' | nc <主板IP> 8765
```

程序在收到完整寄收件信息、尺寸，并得到稳定重量和物品识别结果后自动打印。同一 `order_id` 只打印一次；打印后需先移走物品并等待称重回零。下一单应使用新的 `order_id`。

## 通过 TTL 串口连接两块板

接线为 TX 接对方 RX、RX 接对方 TX、GND 接 GND，两端都使用 115200 8N1。
D3000M 上 `/dev/ttyUSB0` 用于称重，`/dev/ttyUSB1` 用于订单数据。

```bash
python3 integrated_shipping.py \
  --scale-port /dev/ttyUSB0 \
  --order-port /dev/ttyUSB1 \
  --order-baud 115200 \
  --dry-run
```

另一块板的串口设备路径在 `demo1/control_center/cfg.h` 的
`ORDER_UART_DEVICE` 中配置。去掉 `--dry-run` 后才会真正打印。

`demo1` 当前没有尺寸传感器或尺寸输入页面，因此长、宽、高被调整为可选字段。
未收到尺寸时不阻塞打印，标签显示“尺寸:未测量”和“包装:待确认”。
