# IO Pin Plan — ESP32-2432S024 (resistive touch variant)

## Board context

MCU: ESP32-WROOM-32 (dual-core LX6, 520 KB SRAM, 4 MB flash, no PSRAM). Display and touch are on SPI2 (HSPI). SD card is on SPI3 (VSPI). The ESP32 has no third hardware SPI host; biosensor SPI devices share SPI2. Touch: XPT2046 resistive, SPI2 shared with display, CS = GPIO33.


## Pins committed to existing firmware — do not reassign

| GPIO | Function |
| - | - |
| 2 | ILI9341 DC (SPI2) |
| 5 | SD CS (SPI3) |
| 12 | Display MISO (SPI2) |
| 13 | Display MOSI (SPI2) |
| 14 | Display SCLK (SPI2) |
| 15 | Display CS (SPI2) |
| 18 | SD SCLK (SPI3) |
| 19 | SD MISO (SPI3) |
| 23 | SD MOSI (SPI3) |
| 27 | Backlight LEDC PWM |
| 33 | XPT2046 touch CS (SPI2) |
| 0 | BOOT button |
| 1 | UART0 TX (console) |
| 3 | UART0 RX (console) |
| 6–11 | Internal SPI flash — **never touch** |



## Biosensor SPI bus — shares SPI2 (display bus)

The ESP32 has only two user SPI hosts (SPI2 and SPI3), both already claimed. Biosensor SPI devices (ADS1293, ADS1220) are added to **SPI2** as additional `spi\_bus\_add\_device()` entries alongside the display and touch. The ESP-IDF `spi\_master` driver arbitrates access via a per-bus mutex. Each device carries its own clock-speed setting; biosensor rates (ADS1293 ≤8 MHz, ADS1220 ≤2 MHz) are independent of the display's 40 MHz rate. At 1–2 kSPS the biosensor SPI transactions are tiny bursts that fit cleanly in the gaps between LVGL flushes.

| Signal | GPIO | Notes |
| - | - | - |
| SPI MOSI | 13 | Shared with ILI9341 + XPT2046 on SPI2 |
| SPI MISO | 12 | Shared with ILI9341 + XPT2046 on SPI2 |
| SPI SCLK | 14 | Shared with ILI9341 + XPT2046 on SPI2 |
| ADS1293 CS | 16 | Active-low — reclaim LED B (remove R20 + LED1) |
| ADS1220 CS | 17 | Active-low — reclaim LED R (remove R16 + LED1) |



## I2C bus

| Signal | GPIO | Notes |
| - | - | - |
| SDA | 21 | Board I2C header; no driver active |
| SCL | 22 | Board I2C header; no driver active |


| Device | I2C address |
| - | - |
| MAX30102 | 0x57 |
| MPU-6050 | 0x68 (AD0 low) or 0x69 (AD0 high) |



## Interrupt and data-ready lines

| Signal | GPIO | Direction | Notes |
| - | - | - | - |
| ADS1220 DRDY | 35 | Input-only | Expansion P3 header; no loading |
| ADS1293 DRDY | 04 | Io  | Formerly planned biosensor SPI SCLK. LED G (1 kΩ R17 to Vcc) — leave populated. Reserve for later if an additional signal line is needed. |
| MAX30102 INT | 32 | Input | Formerly planned biosensor SPI MOSI; freed by SPI2 sharing |
| MPU-6050 INT | 25 | Input | Formerly planned biosensor SPI MISO; freed by SPI2 sharing |



## Reserved — do not assign yet

| GPIO | Notes |
| - | - |
| **26** | Audio path through R4 (4.7 kΩ). Reserve — remove R4 to clean if needed later. |



## Required board modifications

### Mandatory — reclaim CS pins from LED footprint

Remove **R16** (GPIO17 path) and **R20** (GPIO16 path). LED1 can be removed at the same time or left in place (it will simply be dark once the resistors are gone).

Use the GPIO side of each resistor pad:

- GPIO17 pad (R16 GPIO side) → ADS1220 CS

- GPIO16 pad (R20 GPIO side) → ADS1293 CS

### Not required yet

- GPIO4 / R17 (LED G): leave in place. GPIO4 is in reserve; the LED causes no conflict at present.

- GPIO26 / R4 (audio): leave in place until GPIO26 is needed.

### Physical access for INT lines

GPIO32 and GPIO25 are not labelled connectors on the TZT board. Verify solder access to those pads on the ESP32-WROOM-32 module before wiring INT lines.


## Cautions

- **SPI2 is shared with the display DMA.** Never attempt a biosensor SPI transaction from an ISR — always use the `spi\_master` task-level API so the bus mutex is respected.

- **ADC2 (includes GPIO25, 32) is blocked during WiFi** on classic ESP32. GPIO25 and GPIO32 are used here as **digital INT inputs only** — no ADC.

- Keep ECG analog leads physically separated from SPI/TFT/SD wiring.

- SD card remains on its own SPI3 bus (GPIO18/19/23/5) — no bus sharing with biosensors or display.

