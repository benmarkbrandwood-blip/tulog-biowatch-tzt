# IO Pin Plan — ESP32-2432S024 (resistive touch variant)

## Board context

MCU: ESP32-WROOM-32 (dual-core LX6, 520 KB SRAM, 4 MB flash, no PSRAM).
Display SPI2 and SD SPI3 are already committed and unchanged.
Touch: XPT2046 resistive, SPI2 shared with display, CS = GPIO33.

---

## Pins committed to existing firmware — do not reassign

| GPIO | Function |
|------|----------|
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

---

## Confirmed available pins

### Active set — used in sensor integration

| GPIO | Direction | Assigned function | Notes |
|------|-----------|-------------------|-------|
| **32** | Output | Sensor SPI MOSI | Formerly CST820 SCL — XPT2046 driver does not use it; fully free |
| **25** | In/Out | Sensor SPI MISO | Formerly CST820 RST — XPT2046 driver does not use it; no loading |
| **4** | Output | Sensor SPI SCLK | RGB LED G (1 kΩ R17 to Vcc) — remove R17 + LED1 to reclaim |
| **17** | Output | ADS1220 CS (active-low) | RGB LED R (1 kΩ R16 to Vcc) — remove R16 + LED1 to reclaim |
| **16** | Output | ADS1293 CS (active-low) | RGB LED B (1 kΩ R20 to Vcc) — remove R20 + LED1 to reclaim |
| **21** | Bidir | I2C SDA (MAX30102 / MPU-6050) | Board I2C header; no driver active |
| **22** | Bidir | I2C SCL (MAX30102 / MPU-6050) | Board I2C header; no driver active |
| **35** | Input only | ADS1220 DRDY | Expansion P3 header; input-only pin; no loading |

### Reserved — do not assign yet

| GPIO | Notes |
|------|-------|
| **34** | Input-only; currently photoresistor ADC. Reserve as second DRDY or spare MISO if needed later. |
| **26** | Audio path through R4 (4.7 kΩ). Remove R4 to clean. Reserve as ADS1293 DRDY/INT or extra CS if a ninth line is needed later. |

---

## Sensor bus architecture

### Dedicated biosensor SPI bus (SPI2 or SPI3 free slot, TBD in firmware)

| Signal | GPIO |
|--------|------|
| MOSI | 32 |
| MISO | 25 |
| SCLK | 4 |
| ADS1220 CS | 17 |
| ADS1293 CS | 16 |

Both CS lines idle HIGH, driven LOW only during a transaction.

### I2C bus (shared)

| Signal | GPIO |
|--------|------|
| SDA | 21 |
| SCL | 22 |

| Device | I2C address |
|--------|-------------|
| MAX30102 | 0x57 |
| MPU-6050 | 0x68 (AD0 low) or 0x69 (AD0 high) |

### Interrupt / data-ready

| Signal | GPIO |
|--------|------|
| ADS1220 DRDY | 35 |
| ADS1293 DRDY | *reserve io34 or io26 — assign when needed* |

---

## Required board modifications

### Mandatory — reclaim LED pins for SPI

Remove **LED1**, **R16**, **R17**, **R20**.

Use the GPIO side of each resistor pad (not the Vcc/LED side):
- GPIO4 pad (R17 GPIO side) → SPI SCLK
- GPIO17 pad (R16 GPIO side) → ADS1220 CS
- GPIO16 pad (R20 GPIO side) → ADS1293 CS

### Optional — clean io26 for future use

Remove **R4** (4.7 kΩ) to isolate GPIO26 from the audio amplifier input.
Not required until io26 is assigned.

---

## Cautions

- **ADC2 (includes GPIO25, 26, 32) is blocked during WiFi** on classic ESP32.
  GPIO25/32/4 are used here as **digital SPI signals only** — ADC on these pins
  is not attempted. No conflict.
- Keep ECG analog leads physically separated from SPI/TFT/SD wiring.
- Verify solder-tap access to GPIO25 and GPIO32 on the ESP32 module pads
  before wiring — these are not labelled connectors on the TZT board.
- SD card remains on its own SPI3 bus (GPIO18/19/23/5) — no bus sharing with sensors.
