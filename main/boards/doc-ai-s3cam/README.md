# DOC AI S3CAM — XiaoZhi board

## Hardware and firmware status

- Target: ESP32-S3 N16R8.
- XiaoZhi network/protocol path: `WifiBoard` + shared `Application`.
- I2S simplex audio: MS3625 microphone and MAX98357A amplifier.
- BOOT button: enters Wi-Fi configuration while starting; otherwise toggles chat.
- Display: ST7735 1.77" initialized by `DocAiS3Cam::InitializeDisplay()`.
- Camera: OV3660 pin map is present, but camera initialization is not implemented in this board class.
- PCA9685 and VL53L0X: shared I2C pin map is declared; sensor drivers and runtime initialization are not yet implemented.

## Confirmed pin map

| Signal | GPIO |
|---|---:|
| Microphone BCLK/SCK | 1 |
| Microphone WS/LRCL | 2 |
| Microphone DOUT | 3 |
| OV3660 SCCB SDA | 4 |
| OV3660 SCCB SCL | 5 |
| ST7735 SPI CLK | 14 |
| OV3660 XCLK | 15 |
| ST7735 SPI MOSI | 21 |
| PCA9685 + VL53L0X SDA | 19 |
| PCA9685 + VL53L0X SCL | 20 |
| ST7735 CS | 38 |
| ST7735 DC | 39 |
| ST7735 RESET | 40 |
| MAX98357A BCLK | 41 |
| MAX98357A LRC | 42 |
| MAX98357A DIN | 47 |
| TTP223 OUT | 48 |

## I2C and USB warning

The PCA9685 and VL53L0X share the external I2C bus:

- SDA: GPIO 19
- SCL: GPIO 20
- PCA9685 default address: 0x40
- VL53L0X default address: 0x29

On ESP32-S3, GPIO 19 and GPIO 20 are also native USB D- and D+.
Using them for I2C prevents using the native USB interface at the same time.
A USB-to-UART bridge on separate pins is not the same interface and may remain usable.

The OV3660 SCCB bus remains separate on GPIO 4/5.

## Build

Run from the repository root with the ESP-IDF environment initialized:

```powershell
python scripts/build.py doc-ai-s3cam --name doc-ai-s3cam
```

This selects the board configuration from `main/boards/doc-ai-s3cam/config.json`.
A successful build and hardware validation are still required before treating the firmware as tested.
