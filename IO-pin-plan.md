# IO Pin Plan for ESP32-2.4TFT Sensor Expansion

This pin plan assigns available and reclaimable GPIOs on the ESP32-2.4TFT board to four external sensor modules while **keeping the sensor SPI bus separate from the onboard TF-card SPI bus**.

Target external modules:

- ADS1220 ADC

- MAX30102 PPG sensor

- MPU-6050 IMU

- ADS1293 3-channel ECG AFE

The plan is based on the provided schematic excerpts and prioritizes the least risky board changes while avoiding runtime contention between sensor acquisition and SD-card writes.

## Design goals

- Keep the onboard **TF-card SPI bus independent** from the external biosensor SPI bus.

- Use the existing **I2C bus on IO21/IO22** for the MAX30102 and MPU-6050.

- Reclaim **IO17, IO4, IO16** by removing the RGB LED loading parts.

- Use **IO35** as an interrupt/data-ready input because it is already exposed and input-only.

- Optionally reclaim **IO26** from the audio path if one more digital output or interrupt line is needed.

## Required board modifications

### 1. Reclaim RGB LED GPIOs

The RGB LED block ties these GPIOs to 3.3 V through 1 kΩ resistors:

- IO17 via **R16**

- IO4 via **R17**

- IO16 via **R20**

To make these pins clean general-purpose digital IO:

- Remove **LED1**

- Remove **R16**

- Remove **R17**

- Remove **R20**

After this, the following become free GPIOs suitable for chip-select, reset, and interrupt use:

- IO17

- IO4

- IO16

### 2. Optional: reclaim IO26 from audio

The audio path uses **IO26** through **R4 (4.7 kΩ)** into the amplifier input network.

If one more digital signal is required:

- Remove **R4** to isolate IO26 from the audio circuit

This sacrifices the onboard audio path and frees:

- IO26

This step is optional and is not required for the base plan.

## Bus plan

## I2C bus for MAX30102 and MPU-6050

Use the existing board I2C lines:

- **IO21** → SDA

- **IO22** → SCL

These two devices can share the same I2C bus.

### I2C addresses

- **MAX30102:** default address **0x57**

- **MPU-6050:**

  - **0x68** when AD0 is tied low (GND)

  - **0x69** when AD0 is tied high (VCC)

## Separate external SPI bus for biosensors

Do **not** share the TF-card SPI bus with the external biosignal modules.

Instead, create a dedicated SPI bus for the external modules using free or free-able GPIOs:

- **IO32** → sensor SPI MOSI

- **IO33** → sensor SPI MISO

- **IO25** → sensor SPI SCLK

These pins appear available from the ESP32 module schematic and are preferable to sharing the onboard TF-card bus used by the SD card.

### Devices on this dedicated sensor SPI bus

- **ADS1220**

- **ADS1293**

## Onboard TF-card SPI bus

Leave the TF-card on its original bus:

- IO23 → TF-card MOSI

- IO19 → TF-card MISO

- IO18 → TF-card CLK

- IO5 → TF-card CS

This avoids contention between sensor sampling and SD-card activity.

## Proposed signal assignment

| Function | GPIO | Direction | Access point on board | Notes |
| - | -: | - | - | - |
| I2C SDA | IO21 | bidirectional | Available on the board I/O header / connector shown in the I/O-out schematic; already routed as a free external signal | Shared by MAX30102 and MPU-6050 |
| I2C SCL | IO22 | output/open-drain | Available on the board sensor/header connector shown in the temperature/humidity sensor schematic; already routed as a free external signal | Shared by MAX30102 and MPU-6050 |
| Sensor SPI MOSI | IO32 | output | Appears free at the ESP32 module level; use a direct solder tap to the ESP32 net or any exposed pad/test point carrying IO32 | Dedicated external biosensor SPI bus |
| Sensor SPI MISO | IO33 | input | Appears free at the ESP32 module level; use a direct solder tap to the ESP32 net or any exposed pad/test point carrying IO33 | Dedicated external biosensor SPI bus |
| Sensor SPI SCLK | IO25 | output | Appears free at the ESP32 module level; use a direct solder tap to the ESP32 net or any exposed pad/test point carrying IO25 | Dedicated external biosensor SPI bus |
| ADS1220 CS | IO17 | output | Reclaim from RGB LED net by removing LED1 and R16; use the GPIO side of R16 / IO17 net | Reclaimed from RGB LED, active-low |
| ADS1293 CS | IO4 | output | Reclaim from RGB LED net by removing LED1 and R17; use the GPIO side of R17 / IO4 net | Reclaimed from RGB LED, active-low |
| ADS1293 RESET | IO16 | output | Reclaim from RGB LED net by removing LED1 and R20; use the GPIO side of R20 / IO16 net | Reclaimed from RGB LED |
| ADS1220 DRDY | IO35 | input | Available on the board I/O header / connector shown in the I/O-out schematic; already routed as a free external signal | Good fit because IO35 is input-only |
| ADS1293 DRDY/INT | IO26 | input or output | Optional reclaim from the audio path by removing R4 and using the IO26 side of R4 | Optional; requires removing R4 |


## Recommended minimum viable wiring

### I2C

- IO21 → SDA → MAX30102 + MPU-6050

- IO22 → SCL → MAX30102 + MPU-6050

### Dedicated sensor SPI bus

- IO32 → SPI MOSI → ADS1220 + ADS1293

- IO33 → SPI MISO → ADS1220 + ADS1293

- IO25 → SPI CLK → ADS1220 + ADS1293

- IO17 → ADS1220 CS

- IO4 → ADS1293 CS

- IO16 → ADS1293 RESET

- IO35 → ADS1220 DRDY

### Optional extra control line

- IO26 → ADS1293 DRDY/INT or MAX30102 INT if needed, after removing R4

## Active level notes

### SPI chip select polarity

For the intended SPI modules:

- **ADS1220 CS is active-low**

- **ADS1293 CS is active-low**

Both CS lines should idle high and be driven low only during a transaction.

### Interrupt/data-ready lines

- **ADS1220 DRDY** should be treated as an input to the ESP32

- **ADS1293 DRDY/INT** should be treated as an input if used

- **MAX30102 INT** is optional

- **MPU-6050 INT** is optional

## Priority order for freeable pins

Best reclaim candidates:

1. **IO17** — remove R16 and LED1

2. **IO4** — remove R17 and LED1

3. **IO16** — remove R20 and LED1

4. **IO26** — remove R4 only if an extra line is required

## Notes and cautions

- Use the **GPIO side** of the reclaimed RGB nets, not the 3.3 V resistor side.

- IO35 is input-only, so it is suitable for DRDY/INT but not CS.

- Keep the dedicated sensor SPI wiring short because the ADS1293 ECG front-end is sensitive to digital noise.

- Keep ECG analog leads physically separated from SPI, TFT, and SD-card wiring.

- Leaving the SD card on its own SPI bus is the preferred architecture if you want to sample and log continuously without bus-sharing complexity.

- Verify physical access to IO32, IO33, and IO25 on the actual board before wiring.

## Final recommended plan

### Base version

- Remove **LED1, R16, R17, R20**

- Use:

  - IO21 = I2C SDA for MAX30102 + MPU-6050

  - IO22 = I2C SCL for MAX30102 + MPU-6050

  - IO32 = sensor SPI MOSI

  - IO33 = sensor SPI MISO

  - IO25 = sensor SPI SCLK

  - IO17 = ADS1220 CS

  - IO4 = ADS1293 CS

  - IO16 = ADS1293 RESET

  - IO35 = ADS1220 DRDY

### Expanded-control version

If a dedicated ADS1293 interrupt or extra control line is needed:

- Also remove **R4**

- Use **IO26** for ADS1293 DRDY/INT or MAX30102 INT

This version gives the biosignal modules their own SPI bus and keeps TF-card traffic independent.

