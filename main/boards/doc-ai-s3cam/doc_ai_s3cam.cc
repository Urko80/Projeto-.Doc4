#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "esp32_camera.h"
#include "mcp_server.h"
#include "vl53l0x_driver.h"

#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include "esp_lcd_st7735.h"

#define TAG "doc-ai-s3cam"

// Minimal PCA9685 driver using the same ESP-IDF I2C master bus as VL53L0X.
class Pca9685 {
public:
    explicit Pca9685(i2c_master_bus_handle_t bus) {
        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = PCA9685_I2C_ADDRESS;
        config.scl_speed_hz = DOC_I2C_FREQUENCY_HZ;
        if (i2c_master_bus_add_device(bus, &config, &device_) != ESP_OK) {
            device_ = nullptr;
        }
    }

    ~Pca9685() {
        if (device_ != nullptr) {
            i2c_master_bus_rm_device(device_);
        }
    }

    bool Initialize() {
        if (device_ == nullptr) return false;

        // Set 50 Hz servo PWM. Do not move uncalibrated channels at startup.
        if (!WriteRegister(0x00, 0x10)) return false; // MODE1 sleep
        if (!WriteRegister(0xFE, 121)) return false;  // prescale for 50 Hz
        if (!WriteRegister(0x00, 0x20)) return false; // wake + auto-increment
        esp_rom_delay_us(500);
        if (!WriteRegister(0x00, 0xA0)) return false; // restart + auto-increment

        // Only channels 1 and 2 are mapped to calibrated continuous-rotation wheels.
        return SetPulseUs(1, PCA9685_WHEEL1_STOP_US) &&
               SetPulseUs(2, PCA9685_WHEEL2_STOP_US);
    }

    bool SetPulseUs(uint8_t channel, uint16_t pulse_us) {
        if (device_ == nullptr || channel > 15 || pulse_us < 500 || pulse_us > 2500) {
            return false;
        }

        // 4096 counts per 20 ms period (50 Hz).
        const uint16_t off_count =
            static_cast<uint16_t>((static_cast<uint32_t>(pulse_us) * 4096U) / 20000U);
        const uint8_t reg = static_cast<uint8_t>(0x06 + 4 * channel);
        const uint8_t data[] = {
            reg, 0x00, 0x00,
            static_cast<uint8_t>(off_count & 0xFF),
            static_cast<uint8_t>((off_count >> 8) & 0x0F)
        };
        return i2c_master_transmit(device_, data, sizeof(data), 100) == ESP_OK;
    }

private:
    i2c_master_dev_handle_t device_ = nullptr;

    bool WriteRegister(uint8_t reg, uint8_t value) {
        const uint8_t data[] = {reg, value};
        return i2c_master_transmit(device_, data, sizeof(data), 100) == ESP_OK;
    }
};

class DocAiS3Cam : public WifiBoard {
private:
    Button boot_button_;
    Button touch_button_;
    Display* display_ = nullptr;
    Esp32Camera* camera_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Pca9685* pca9685_ = nullptr;
    vl53l0x_driver_t* distance_sensor_ = nullptr;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // TTP223 OUT is active-high with the documented 3.3 V connection.
        touch_button_.OnClick([]() {
            Application::GetInstance().ToggleChatState();
        });
    }

    void InitializeDisplay() {
        spi_bus_config_t bus_config = {};
        bus_config.sclk_io_num = DISPLAY_SPI_CLK_PIN;
        bus_config.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        bus_config.miso_io_num = GPIO_NUM_NC;
        bus_config.quadwp_io_num = GPIO_NUM_NC;
        bus_config.quadhd_io_num = GPIO_NUM_NC;
        bus_config.max_transfer_sz =
            DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);

        ESP_ERROR_CHECK(
            spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_CLOCK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;

        ESP_ERROR_CHECK(
            esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(
            esp_lcd_new_panel_st7735(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(
            esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(
            esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(
            esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(
            esp_lcd_panel_set_gap(panel, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new SpiLcdDisplay(
            panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
            DISPLAY_SWAP_XY);

        ESP_LOGI(TAG, "ST7735 initialized: %dx%d, SPI=%d Hz",
                 DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_SPI_CLOCK_HZ);
    }

    void InitializeI2cAndSensors() {
        // Shared I2C bus for PCA9685 (0x40) and VL53L0X (0x29).
        i2c_master_bus_config_t bus_config = {};
        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = DOC_I2C_SDA_GPIO;
        bus_config.scl_io_num = DOC_I2C_SCL_GPIO;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = true;

        esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
            return;
        }

        pca9685_ = new Pca9685(i2c_bus_);
        if (!pca9685_->Initialize()) {
            ESP_LOGE(TAG, "PCA9685 init failed at 0x%02X", PCA9685_I2C_ADDRESS);
            delete pca9685_;
            pca9685_ = nullptr;
        } else {
            ESP_LOGI(TAG, "PCA9685 ready at 0x%02X, 50 Hz", PCA9685_I2C_ADDRESS);
        }

        err = vl53l0x_driver_create_on_bus(
            i2c_bus_, VL53L0X_I2C_ADDRESS, DOC_I2C_FREQUENCY_HZ, 1000,
            &distance_sensor_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "VL53L0X init failed: %s", esp_err_to_name(err));
            distance_sensor_ = nullptr;
        } else {
            ESP_LOGI(TAG, "VL53L0X ready at 0x%02X", VL53L0X_I2C_ADDRESS);
        }
    }

    void InitializeCamera() {
        // OV3660 SCCB is separate from the external GPIO 19/20 I2C bus.
        camera_config_t config = {};
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_sccb_sda = CAMERA_PIN_SIOD;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.ledc_timer = LEDC_TIMER_0;
        config.ledc_channel = LEDC_CHANNEL_0;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_QQVGA; // 160x120
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
        config.sccb_i2c_port = 1;
        config.jpeg_buffer_size = 0;

        camera_ = new Esp32Camera(config);
        camera_->SetVFlip(CAMERA_VFLIP);
        camera_->SetHMirror(CAMERA_HMIRROR);
        ESP_LOGI(TAG, "OV3660 initialization requested at XCLK=%d Hz", XCLK_FREQ_HZ);
    }

    void InitializeMcpTools() {
        auto& mcp = McpServer::GetInstance();

        if (distance_sensor_ != nullptr) {
            mcp.AddTool(
                "self.doc.distance.read",
                "Read one VL53L0X distance measurement. Returns millimeters.",
                PropertyList(),
                [this](const PropertyList&) -> ToolResult {
                    uint16_t distance_mm = 0;
                    esp_err_t err =
                        vl53l0x_driver_read_mm(distance_sensor_, &distance_mm);
                    if (err != ESP_OK) {
                        return std::unexpected(
                            "VL53L0X measurement failed: " +
                            std::string(esp_err_to_name(err)));
                    }
                    return std::string("{\"distance_mm\":") +
                           std::to_string(distance_mm) + "}";
                });
        }

        if (pca9685_ != nullptr) {
            mcp.AddTool(
                "self.doc.wheels.set",
                "Set both continuous-rotation wheels. Use cw, ccw, or stop for each wheel. "
                "Only calibrated PCA9685 channels 1 and 2 are enabled.",
                PropertyList({
                    Property("wheel1", kPropertyTypeString),
                    Property("wheel2", kPropertyTypeString)
                }),
                [this](const PropertyList& properties) -> ToolResult {
                    auto pulse_for = [](const std::string& direction,
                                        uint16_t cw, uint16_t stop,
                                        uint16_t ccw) -> uint16_t {
                        if (direction == "cw") return cw;
                        if (direction == "ccw") return ccw;
                        if (direction == "stop") return stop;
                        return 0;
                    };

                    const auto wheel1 = properties["wheel1"].value<std::string>();
                    const auto wheel2 = properties["wheel2"].value<std::string>();
                    const uint16_t pulse1 = pulse_for(
                        wheel1, PCA9685_WHEEL1_CW_US,
                        PCA9685_WHEEL1_STOP_US, PCA9685_WHEEL1_CCW_US);
                    const uint16_t pulse2 = pulse_for(
                        wheel2, PCA9685_WHEEL2_CW_US,
                        PCA9685_WHEEL2_STOP_US, PCA9685_WHEEL2_CCW_US);
                    if (pulse1 == 0 || pulse2 == 0) {
                        return std::unexpected(
                            "Invalid direction. Use cw, ccw, or stop.");
                    }
                    if (!pca9685_->SetPulseUs(1, pulse1) ||
                        !pca9685_->SetPulseUs(2, pulse2)) {
                        return std::unexpected("PCA9685 write failed");
                    }
                    return std::string("{\"success\":true,\"wheel1_us\":") +
                           std::to_string(pulse1) + ",\"wheel2_us\":" +
                           std::to_string(pulse2) + "}";
                });
        }
    }

public:
    DocAiS3Cam()
        : boot_button_(BOOT_BUTTON_GPIO),
          touch_button_(TOUCH_BUTTON_GPIO, true) {
        ESP_LOGI(TAG, "DOC AI S3CAM startup");
        InitializeButtons();
        InitializeDisplay();
        InitializeI2cAndSensors();
        InitializeCamera();
        InitializeMcpTools();
    }

    ~DocAiS3Cam() override {
        if (distance_sensor_ != nullptr) {
            vl53l0x_driver_delete(distance_sensor_);
        }
        delete pca9685_;
        if (i2c_bus_ != nullptr) {
            i2c_del_master_bus(i2c_bus_);
        }
        delete camera_;
        delete display_;
    }

    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
            AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }
    Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(DocAiS3Cam);
