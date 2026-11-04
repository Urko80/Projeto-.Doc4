# DOC AI S3CAM — ESP32-S3 N16R8

## Integrated hardware

- MS3625 I2S microphone: GPIO 1/2/3.
- MAX98357A I2S amplifier: GPIO 41/42/47.
- ST7735 1.77" GMT177-10 BLACKTAB: SPI GPIO 14/21/38/39/40, portrait 128x160.
- OV3660 camera: DVP data/sync GPIOs from `config.h`, SCCB GPIO 4/5, XCLK 20 MHz.
- TTP223 touch output: GPIO 48, active-high.
- PCA9685 + VL53L0X: shared I2C SDA GPIO 19, SCL GPIO 20.
- BOOT button: GPIO 0.

## Display orientation

The ST7735 is configured in its native portrait orientation (128x160). X/Y swapping and both mirror transforms are disabled in the panel and LVGL configuration so text and numbers render in their normal reading direction.

## MCP tools

When the corresponding device initializes successfully, the firmware registers:

- `self.doc.distance.read`: one VL53L0X measurement in millimeters.
- `self.doc.wheels.set`: controls the two continuous-rotation wheels on PCA9685 channels 1 and 2. Each wheel accepts `cw`, `ccw`, or `stop`.

Only the wheel channels with recorded pulse calibration are enabled:
- Wheel 1 / channel 1: CW 1665 µs, stop 1705 µs, CCW 1750 µs.
- Wheel 2 / channel 2: CW 1665 µs, stop 1705 µs, CCW 1755 µs.

Other PCA9685 channels are left untouched because their mechanical limits are not yet calibrated.

## I2C / USB note

The PCA9685 (default address 0x40) and VL53L0X (default address 0x29) share the I2C master bus on GPIO 19/20. These GPIOs are also native USB D-/D+ on ESP32-S3; native USB cannot be used at the same time as this I2C assignment. USB-to-UART connections on separate pins are unaffected.

The OV3660 SCCB bus remains separate on GPIO 4/5 and I2C1 (the `esp32-camera` component's default SCCB controller). PCA9685/VL53L0X use the ESP-IDF I2C master bus I2C0 on GPIO 19/20.

## Build

From the repository root with the ESP-IDF 6.0.1+ environment activated:

```powershell
python scripts/build.py doc-ai-s3cam --name doc-ai-s3cam
```

The VL53L0X driver is pinned to a specific Git commit through the ESP-IDF Component Manager. The first configure/build downloads that dependency.

## Validation status

This integration is source-level only until the board is built and tested physically. Confirm I2C pull-ups and common ground, and verify the camera's 20 MHz XCLK against the specific OV3660 module. The board has 8 MB PSRAM; the camera is configured for RGB565 QQVGA (160x120) with one PSRAM framebuffer.
