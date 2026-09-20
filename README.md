# 智慧物流寄件终端

面向校园驿站、社区服务点和小型直营网点的双节点 AI 寄件终端。openvela 全志 R528S3 终端负责触摸交互、语音录入和订单确认，飞腾 D3000M 边缘节点负责物品识别、稳定称重、订单汇聚与标签打印。

## 核心能力

- 320×480 LVGL 三页寄件界面，支持中文显示和 450 ms 防连点保护；
- 小智语音识别填写收件人、寄件人、电话和地址；
- USB 摄像头结合本地 Ollama `minicpm-v4.6:latest` 模型识别物品；
- SJ101CX RS485/Modbus 称重，使用连续稳定样本抑制抖动；
- 全志与飞腾通过 115200 8N1 TTL 串口交换单行 UTF-8 JSON；
- 信息齐备后驱动 GY-EP201L 标签打印机，带订单去重和回零门控；
- 支持 `--dry-run` 预览联调，避免误打印。

## 系统结构

```text
openvela / 全志 R528S3                         飞腾 D3000M
┌────────────────────────┐    TTL UART     ┌─────────────────────────┐
│ express_ui  触摸 UI     │◄──────────────►│ integrated_shipping.py  │
│ control_center 语音/STT │  JSON + 换行   │ 摄像头 + Ollama 识别    │
│ sound_app 采音/Opus     │  115200 8N1    │ RS485 称重 + 标签打印   │
│ wifi_auto 自动联网      │                │ 订单去重 + 回零门控      │
└────────────────────────┘                └─────────────────────────┘
```

## 目录说明

```text
openvela/
├── quanzhi_openvela/            openvela / NuttX 端源码与 defconfig
│   ├── demo1/
│   │   ├── control_center/       小智 OTA、WebSocket、STT 中转
│   │   ├── express_ui/           LVGL UI 与 UART1 订单通信
│   │   └── sound_app/            麦克风采集与 Opus 编码
│   └── wifi_auto/                CPS Wi-Fi 自动连接
├── feiteng/mm/                   D3000M 边缘 AI 与外设程序
├── skills/smart-shipping-assistant/SKILL.md
├── logs/AI_Coding_Log.md
└── docs/DEMO_SCRIPT.md
```

## 硬件连接

全志 R528S3 的 J3 扩展接口连接飞腾侧 TTL 串口：

| 全志引脚 | 信号 | 接到飞腾侧 |
|---|---|---|
| J3 Pin 5 / PE10 | UART1_TX | RX |
| J3 Pin 3 / PE11 | UART1_RX | TX |
| J3 Pin 9 | GND | GND |

飞腾侧默认设备：称重模块 `/dev/ttyUSB0`、订单串口 `/dev/ttyUSB1`、打印机 `/dev/ttyACM0`、摄像头 `/dev/video0`。

## 编译 openvela 端

将 `quanzhi_openvela/demo1` 放到 `~/vela-opensource/apps/demo1`，将 `quanzhi_openvela/wifi_auto` 放到 `~/vela-opensource/apps/example/wifi_auto`，并合入对应 `Kconfig`、`Make.defs` 和 `defconfig` 后，在全志 SDK 环境执行：

```sh
cd ~/vela-opensource/vendor/allwinnertech/lichee
m
```

板端推荐启动顺序：

```text
nsh> wifi_auto
nsh> control_center &
nsh> sound_app &
nsh> express_ui
```

## 运行飞腾端

首次运行前确保 D3000M 已安装 OpenCV、Ollama，且本机存在 `minicpm-v4.6:latest` 模型。先用预览模式联调：

```sh
cd feiteng/mm
python3 integrated_shipping.py \
  --scale-port /dev/ttyUSB0 \
  --order-port /dev/ttyUSB1 \
  --printer-device /dev/ttyACM0 \
  --dry-run
```

确认识别、称重、订单接收与标签预览正常后，去掉 `--dry-run` 才会写入打印机。摄像头窗口按 `R` 识别，按 `Q` 退出。

## UART 数据协议

飞腾向 openvela 发送包裹信息：

```json
{"type":"parcel","item":"雨伞","weight":"530g","package":"大箱"}
```

openvela 确认后向飞腾发送完整订单：

```json
{"type":"submit","item":"雨伞","weight":"530g","package":"大箱","receiver":"张三","sender":"李四","phone":"13800138000","address":"北京市海淀区"}
```

每条 JSON 必须使用 UTF-8 编码并以换行符结束。

## Skill 与 AI Coding 日志

- 参赛 Skill：`skills/smart-shipping-assistant/SKILL.md`；
- AI Coding 日志：`logs/AI_Coding_Log.md`；
- 提交前必须确认 `logs/` 已随代码一起 `git push`。

## 演示建议

按 `docs/DEMO_SCRIPT.md` 在 5 分钟内演示：启动与架构、触摸/语音交互、AI 物品识别、稳定称重、订单确认、标签输出和异常保护。

## 当前验证边界

仓库已包含两端源码和 openvela 构建产物痕迹。当前 Windows 整理环境可完成文档、Skill 和 Python 静态检查，但不能替代 R528S3、D3000M、摄像头、称重模块和打印机上的整机复测。提交前请在实物上重新编译并完整走通一次演示流程。
