# AI Coding 开发日志

## 项目

- 项目名称：智能快递终端
- 硬件：全志 R528S3 Vela EVB、触摸屏、麦克风、飞腾板
- 系统：OpenVela / NuttX
- AI 协作工具：Codex

## 主要开发过程

1. 创建三页 LVGL 快递 UI：首页、包裹信息页、收件信息页。
2. 增加收件人、寄件人、电话、地址四个可语音录入字段。
3. 增加四个独立清除按钮、清空全部按钮、确认提交按钮和防连点保护。
4. 扩充中文字库，并启用 LVGL 大字体支持。
5. 集成 sound_app、control_center 和小智 STT。
6. 修复小智连接所需的网络、随机数、cURL、c-ares 和 WebSocket 初始化问题。
7. 增加 Wi-Fi 自动连接程序 wifi_auto。
8. 调整 UART1 到 J3 扩展接口：
   - PE10：UART1_TX
   - PE11：UART1_RX
   - Function3
9. 完成 UART1 回环测试。
10. 在 express_ui 中加入 UART1 JSON 通信：
    - 接收物品、重量、包装；
    - 第二页实时显示包裹数据；
    - 确认提交后发送完整订单 JSON 给飞腾板。

## 当前功能

- 触摸屏 UI 可用；
- 中文显示可用；
- 小智语音识别可用；
- 收寄件字段可通过语音填写；
- UART1 可与飞腾板传递包裹信息和订单信息。
