# ESP32-C3 SuperMini Wiring — Dasai Mochi V2

## Power
- 5V -> MAX98357A VCC
- 3.3V -> INMP441 VCC + all TTP223 VCC
- GND -> MAX98357A GND + OLED GND + INMP441 GND + TTP223 GND

## OLED
- SDA -> GPIO21
- SCL -> GPIO20
- Address -> 0x3C

## INMP441
- SCK -> GPIO1
- WS -> GPIO2
- SD -> GPIO8
- L/R -> GND

## MAX98357A
- BCLK -> GPIO1
- LRC -> GPIO2
- DIN -> GPIO5
- GAIN -> leave floating unless your module requires another setting

## TTP223
- TALK SIG -> GPIO3
- NEXT SIG -> GPIO6
- MODE SIG -> GPIO7
- VCC -> 3.3V
- GND -> GND

TTP223 inputs are active-HIGH in the firmware.
