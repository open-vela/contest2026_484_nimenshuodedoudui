# 全志 R528S3 智能快递终端

## 1. 项目说明

wifi_auto存放路径：~/vela-opensource/apps/example
demo1存放路径：~/vela-opensource/apps/demo1

# 智能快递终端

基于 OpenVela、全志 R528S3 和飞腾平台的智能快递交互终端。

## 项目功能

- 320×480 触摸屏快递寄件 UI；
- 小智 AI 语音识别；
- 语音填写收件人、寄件人、电话、地址；
- 视觉端/飞腾板识别包裹物品、重量、包装；
- UART1 将包裹数据发送到全志 UI；
- 用户确认提交后，全志通过 UART1 返回完整订单；
- 自动连接 CPS Wi-Fi；
- 中文字体显示和按钮防连点保护。

## 界面流程

```text
首页
  ↓
包裹信息页：物品、重量、包装
  ↓
收件信息页：收件人、寄件人、电话、地址
  ↓
确认提交后回到首页
代码结构
demo1/
├── control_center/   小智控制中心、OTA、WebSocket、STT
├── express_ui/       LVGL UI、STT 字段显示、UART1 通信
├── sound_app/        麦克风录音、Opus 编码与音频发送
├── Kconfig
├── Make.defs
└── Makefile

wifi_auto/
└── wifi_auto.c       CPS Wi-Fi 自动连接程序

skills/
└── express-terminal/
    └── SKILL.md       项目开发 Skill

logs/
└── AI_Coding_Log.md   AI Coding 开发日志
编译
cd ~/vela-opensource/vendor/allwinnertech/lichee
m
全志端启动
nsh> wifi_auto
nsh> control_center &
nsh> sound_app &
nsh> express_ui
UART1 接线
全志 J3 扩展接口：
J3 引脚	信号	用途
Pin 5	PE10	UART1_TX
Pin 3	PE11	UART1_RX
Pin 9	GND	公共地


连接飞腾板：
全志 Pin 5 / UART1_TX -> 飞腾 RX
全志 Pin 3 / UART1_RX <- 飞腾 TX
全志 Pin 9 / GND      <-> 飞腾 GND
串口参数：
115200，8N1，无校验，无流控
UART 数据协议
飞腾发送包裹信息：
{"type":"parcel","item":"雨伞","weight":"530g","package":"大箱"}
全志 UI 显示物品、重量和包装。
全志点击“确认提交”后发送：
{"type":"submit","item":"雨伞","weight":"530g","package":"大箱","receiver":"张三","sender":"李四","phone":"13800138000","address":"北京市海淀区"}
每条 JSON 必须以换行符结尾。
飞腾测试命令
飞腾串口设备：
/dev/ttyS1
配置：
sudo stty -F /dev/ttyS1 115200 cs8 -cstopb -parenb \
  -ixon -ixoff -crtscts raw -echo
发送测试包裹：
printf '%s\n' \
'{"type":"parcel","item":"雨伞","weight":"530g","package":"大箱"}' \
| sudo tee /dev/ttyS1 >/dev/null
接收全志提交的信息：
sudo cat /dev/ttyS1
