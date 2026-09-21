# Dasai Mochi — ESP32-C3 SuperMini Port V3

V3 is the SuperMini hardware port with a RAM fix and Wi-Fi RF transmit power limited to 10 dBm.

## Hardware mapping

- OLED SDA -> GPIO21
- OLED SCL -> GPIO20
- INMP441 SCK/BCLK -> GPIO1
- INMP441 WS/LRCLK -> GPIO2
- INMP441 SD -> GPIO8
- MAX98357A BCLK -> GPIO1
- MAX98357A LRC -> GPIO2
- MAX98357A DIN -> GPIO5
- TTP223 TALK -> GPIO3
- TTP223 NEXT -> GPIO6
- TTP223 MODE -> GPIO7

TTP223 modules are expected to be active-HIGH and are configured as ordinary `INPUT` pins.

## V3 RAM fix

The previous build had a global static PCM array for 8 seconds at 16 kHz:
`16000 * 8 * 2 = 256000 bytes`.

V3 changes the maximum recording time to 4 seconds and allocates the PCM buffer only while recording/STT is in progress. The buffer is freed immediately after the STT call.

This removes the large static `.dram0.bss` allocation that caused the linker error:
`region 'dram0_0_seg' overflowed by 33936 bytes`.

## Wi-Fi RF power

V3 applies:
`WiFi.setTxPower(WIFI_POWER_10dBm);`

The setting is applied after entering both STA and AP mode. This targets 10 dBm transmit power.

## Build

Push this repository to GitHub and run:
**Actions -> Build Dasai Mochi - ESP32-C3 SuperMini**.

The workflow creates a `DasaiMochi-SuperMini-firmware` artifact containing bootloader, partitions, firmware, and a merged binary.

For phone flashing, use the merged binary when your flasher supports a single image at offset `0x0`.

## Important

The `docs/firmware` directory in this V3 source intentionally does not contain the old prebuilt original firmware binaries. Do not flash binaries from an older build as the SuperMini V3 firmware.


## V3 fixes
- Fixed `Btn` initialization for Arduino/C++: uses aggregate initialization.
- Fixed exact 10 dBm RF setting using `esp_wifi_set_max_tx_power(40)` because this Arduino core exposes 11 dBm but not a `WIFI_POWER_10dBm` enum.
