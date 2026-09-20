---
name: smart-shipping-assistant
description: Guide, diagnose, and verify the openvela smart shipping terminal workflow. Use when working on parcel entry, voice STT fields, UART JSON exchange, item recognition, RS485 weighing, label printing, dry-run verification, or competition demonstrations for this repository.
---

# Smart Shipping Assistant

Use this repository's existing modules and preserve the two-node architecture. Treat `quanzhi_openvela/` as the R528S3 openvela side and `feiteng/mm/` as the D3000M edge-AI side.

## Workflow

1. Read the root `README.md` and the README nearest to the module being changed.
2. Identify whether the issue belongs to interaction/STT, UART transport, recognition, weighing, or printing.
3. Keep the serial contract as one UTF-8 JSON object per line at 115200 8N1.
4. For the openvela side, preserve the launch order `wifi_auto`, `control_center &`, `sound_app &`, `express_ui`.
5. For the D3000M side, verify with `--dry-run` before enabling physical printing.
6. Require sender, receiver, phone, address, a stable weight, and an item-recognition result before printing.
7. After a successful print, preserve order-ID deduplication and require weight return-to-zero before the next order.
8. Record meaningful AI-assisted changes in `logs/` and keep the log in the submitted Git repository.

## Safety and recovery

- Never invent hardware measurements, model accuracy, latency, power, or successful device tests.
- Never bypass the print gate merely to make a demonstration pass.
- Mask phone numbers on labels and avoid placing real personal data in committed samples.
- If UART data is rejected, check the newline terminator, UTF-8 encoding, required fields, TX/RX crossover, common ground, and device path.
- If recognition fails, keep the order pending, improve framing or lighting, and retry; do not guess the object.
- If the scale is unstable, wait for four valid samples within the configured 2 g range or recalibrate/tare the device.
- If printing fails, retain the unprinted state so the operator can fix the device and retry without creating a false success record.

## Verification checklist

- Python source parses successfully.
- openvela configuration enables LVGL, control center, sound app, Wi-Fi auto-connect, UART1, cJSON, curl, WebSocket, and Opus.
- A `parcel` message updates item, weight, and package fields on the openvela UI.
- A `submit` message includes sender, receiver, phone, address, item, weight, and package.
- The `--dry-run` path produces a complete label preview without writing to the printer.
- The final demo visibly shows interaction, AI recognition, weighing, confirmation, and output.
