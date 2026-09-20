# 智能快递终端开发 Skill

本项目运行在全志 R528S3 Vela EVB 开发板，使用 NuttX 和 LVGL。

## 编译

在工程根目录执行：

```sh
cd ~/vela-opensource/vendor/allwinnertech/lichee
m
不要使用 pack。
运行程序
NSH 中推荐启动顺序：
wifi_auto
control_center &
sound_app &
express_ui
如果 Wi-Fi 已手动连接，可以跳过 wifi_auto。
UART1
UART1 使用 J3 扩展口：
J3 Pin 5：PE10，UART1_TX
J3 Pin 3：PE11，UART1_RX
J3 Pin 9：GND
串口参数：
115200，8N1，无校验，无流控
全志与飞腾连接：
全志 TX -> 飞腾 RX
全志 RX <- 飞腾 TX
全志 GND <-> 飞腾 GND
飞腾设备为 /dev/ttyS1。
数据协议
飞腾向全志发送：
{"type":"parcel","item":"雨伞","weight":"530g","package":"大箱"}
全志点击“确认提交”后向飞腾发送：
{"type":"submit","item":"雨伞","weight":"530g","package":"大箱","receiver":"张三","sender":"李四","phone":"13800138000","address":"北京市海淀区"}
每条 JSON 结尾必须有换行符。
