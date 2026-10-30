#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "web_servo_api.h"
#include "audio/audio_codec.h"
#include "protocols/protocol.h"

#include "esp32_camera.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_timer.h>
#include <esp_log.h>
#include "esp_lcd_st7735.h"

#define TAG "doc-ai-s3cam"

class DocAiS3Cam : public WifiBoard {
private:
    Button boot_button_;

    // TTP223: entrada digital ativa em nível alto durante o toque.
    // Se o sensor não responder, revisar o nível lógico e a ligação OUT/GND/3V3.
    Button touch_button_;

    // DOC TTP223: estado do interpretador de gestos e do menu físico.
    std::atomic<bool> touch_interrupted_speech_{false};
    std::atomic<bool> touch_menu_open_{false};
    esp_timer_handle_t touch_menu_timer_ = nullptr;
    int touch_menu_index_ = 0;
    int touch_personality_index_ = 0;

    Display* display_ = nullptr;
    bool servo_api_started_ = false;

    // DOC INÍCIO — Estado da câmera OV3660, aberta somente quando solicitada.
    Camera* camera_ = nullptr;
    std::string camera_vision_url_;
    std::string camera_vision_token_;
    TaskHandle_t camera_preview_task_ = nullptr;
    std::atomic<bool> camera_preview_running_{false};
    std::atomic<bool> camera_preview_stop_requested_{false};

    void InitializeCamera() {
        camera_config_t camera_config = {};
        camera_config.ledc_channel = LEDC_CHANNEL_0;
        camera_config.ledc_timer = LEDC_TIMER_0;
        camera_config.pin_d0 = CAMERA_PIN_D0;
        camera_config.pin_d1 = CAMERA_PIN_D1;
        camera_config.pin_d2 = CAMERA_PIN_D2;
        camera_config.pin_d3 = CAMERA_PIN_D3;
        camera_config.pin_d4 = CAMERA_PIN_D4;
        camera_config.pin_d5 = CAMERA_PIN_D5;
        camera_config.pin_d6 = CAMERA_PIN_D6;
        camera_config.pin_d7 = CAMERA_PIN_D7;
        camera_config.pin_xclk = CAMERA_PIN_XCLK;
        camera_config.pin_pclk = CAMERA_PIN_PCLK;
        camera_config.pin_vsync = CAMERA_PIN_VSYNC;
        camera_config.pin_href = CAMERA_PIN_HREF;
        camera_config.pin_sccb_sda = CAMERA_PIN_SIOD;
        camera_config.pin_sccb_scl = CAMERA_PIN_SIOC;
        camera_config.pin_pwdn = CAMERA_PIN_PWDN;
        camera_config.pin_reset = CAMERA_PIN_RESET;
        camera_config.xclk_freq_hz = XCLK_FREQ_HZ;
        camera_config.pixel_format = PIXFORMAT_RGB565;
        camera_config.frame_size = FRAMESIZE_QQVGA;
        camera_config.jpeg_quality = 12;
        camera_config.fb_count = 2;
        camera_config.fb_location = CAMERA_FB_IN_PSRAM;
        camera_config.grab_mode = CAMERA_GRAB_LATEST;

        camera_ = new Esp32Camera(camera_config);
        camera_->SetExplainUrl(camera_vision_url_, camera_vision_token_);
        camera_->SetVFlip(true);
        ESP_LOGI(TAG, "OV3660 camera initialization requested");
    }

    static void CameraPreviewTask(void* arg) {
        auto* self = static_cast<DocAiS3Cam*>(arg);
        while (!self->camera_preview_stop_requested_.load()) {
            Camera* camera = self->camera_;
            if (camera != nullptr) {
                camera->Capture();
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        self->camera_preview_running_.store(false);
        self->camera_preview_task_ = nullptr;
        vTaskDelete(nullptr);
    }

    void StartCameraPreviewTask() {
        if (camera_ == nullptr || camera_preview_running_.load()) {
            return;
        }

        camera_preview_stop_requested_.store(false);
        camera_preview_running_.store(true);
        BaseType_t result = xTaskCreate(
            CameraPreviewTask, "doc_cam_preview", 4096, this, 2, &camera_preview_task_);
        if (result != pdPASS) {
            camera_preview_running_.store(false);
            camera_preview_task_ = nullptr;
            ESP_LOGE(TAG, "Failed to start camera preview task");
        }
    }

    void StopCameraPreviewTask() {
        if (!camera_preview_running_.load()) {
            camera_preview_task_ = nullptr;
            return;
        }

        camera_preview_stop_requested_.store(true);
        while (camera_preview_running_.load()) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        camera_preview_task_ = nullptr;
        camera_preview_stop_requested_.store(false);
    }
    // DOC FIM — Estado e ciclo de vida da câmera.

    // ============================================================
    // DOC TTP223 — MENU, PERSONALIDADES E AÇÕES DO SENSOR
    // Alterações dos gestos e das opções do menu ficam nesta seção.
    // ============================================================

    // Exibe o item atualmente selecionado no ST7735.
    void ShowTouchMenu() {
        static const char* const menu_items[] = {
            "Aumentar volume",
            "Diminuir volume",
            "Brilho (indisponivel)",
            "Estado Wi-Fi",
            "Configuracoes DOC"
        };
        constexpr int menu_count = sizeof(menu_items) / sizeof(menu_items[0]);
        char message[128];
        snprintf(message, sizeof(message),
                 "MENU DOC\n> %s\nToque: proxima\nDuplo: selecionar\nLongo: voltar",
                 menu_items[touch_menu_index_ % menu_count]);
        if (display_ != nullptr) {
            display_->ShowNotification(message, 30000);
        }
        ESP_LOGI(TAG, "TTP223 menu: %s (%d/%d)",
                 menu_items[touch_menu_index_], touch_menu_index_ + 1, menu_count);
    }

    // Fecha automaticamente o menu após 30 s sem interação.
    static void TouchMenuTimeout(void* arg) {
        auto* self = static_cast<DocAiS3Cam*>(arg);
        if (self->touch_menu_open_.exchange(false)) {
            if (self->display_ != nullptr) {
                self->display_->SetStatus("DOC");
            }
            ESP_LOGI(TAG, "TTP223 menu fechado por inatividade");
        }
    }

    void RestartTouchMenuTimer() {
        if (touch_menu_timer_ != nullptr) {
            esp_timer_stop(touch_menu_timer_);
            ESP_ERROR_CHECK(esp_timer_start_once(touch_menu_timer_, 30000000));
        }
    }

    void OpenTouchMenu() {
        touch_menu_open_.store(true);
        touch_menu_index_ = 0;
        ShowTouchMenu();
        RestartTouchMenuTimer();
    }

    void CloseTouchMenu() {
        touch_menu_open_.store(false);
        if (touch_menu_timer_ != nullptr) {
            esp_timer_stop(touch_menu_timer_);
        }
        if (display_ != nullptr) {
            display_->SetStatus("DOC");
        }
        ESP_LOGI(TAG, "TTP223 menu fechado");
    }

    void MoveTouchMenuNext() {
        constexpr int menu_count = 5;
        touch_menu_index_ = (touch_menu_index_ + 1) % menu_count;
        ShowTouchMenu();
        RestartTouchMenuTimer();
    }

    // Consulta o estado atual da aplicação para apresentar no menu.
    const char* GetTouchDeviceStateLabel() const {
        switch (Application::GetInstance().GetDeviceState()) {
            case kDeviceStateStarting: return "Iniciando";
            case kDeviceStateWifiConfiguring: return "Configurando Wi-Fi";
            case kDeviceStateIdle: return "Repouso";
            case kDeviceStateConnecting: return "Conectando";
            case kDeviceStateListening: return "Escutando";
            case kDeviceStateSpeaking: return "Falando";
            case kDeviceStateNotifying: return "Notificacao";
            case kDeviceStateUpgrading: return "Atualizando";
            case kDeviceStateActivating: return "Ativando";
            case kDeviceStateAudioTesting: return "Teste de audio";
            case kDeviceStateFatalError: return "Erro";
            default: return "Desconhecido";
        }
    }

    // Executa a opção confirmada com toque duplo dentro do menu.
    void SelectTouchMenuOption() {
        auto& board = Board::GetInstance();
        switch (touch_menu_index_) {
            case 0: {
                AudioCodec* codec = board.GetAudioCodec();
                if (codec != nullptr) {
                    const int new_volume = std::min(100, codec->output_volume() + 5);
                    codec->SetOutputVolume(new_volume);
                    char message[48];
                    snprintf(message, sizeof(message), "Volume: %d%%", new_volume);
                    display_->ShowNotification(message, 3000);
                    ESP_LOGI(TAG, "TTP223: volume aumentado para %d", new_volume);
                }
                break;
            }
            case 1: {
                AudioCodec* codec = board.GetAudioCodec();
                if (codec != nullptr) {
                    const int new_volume = std::max(0, codec->output_volume() - 5);
                    codec->SetOutputVolume(new_volume);
                    char message[48];
                    snprintf(message, sizeof(message), "Volume: %d%%", new_volume);
                    display_->ShowNotification(message, 3000);
                    ESP_LOGI(TAG, "TTP223: volume reduzido para %d", new_volume);
                }
                break;
            }
            case 2:
                // O backlight está configurado como GPIO_NUM_NC nesta placa.
                display_->ShowNotification(
                    "Brilho indisponivel: backlight sem GPIO de controle", 4000);
                ESP_LOGW(TAG, "TTP223: brilho nao ajustavel (DISPLAY_BACKLIGHT_PIN = NC)");
                break;
            case 3: {
                const char* network_icon = board.GetNetworkStateIcon();
                char message[64];
                snprintf(message, sizeof(message), "Estado Wi-Fi: %s",
                         network_icon != nullptr ? network_icon : "indisponivel");
                display_->ShowNotification(message, 4000);
                ESP_LOGI(TAG, "%s", message);
                break;
            }
            case 4: {
                char message[96];
                snprintf(message, sizeof(message), "DOC: %s\nEscuta: toque curto\nPersonalidade: toque duplo",
                         GetTouchDeviceStateLabel());
                display_->ShowNotification(message, 5000);
                ESP_LOGI(TAG, "TTP223: exibindo configuracoes/status do DOC");
                break;
            }
            default:
                break;
        }
    }

    // Fora do menu, o toque duplo percorre as três personalidades visuais.
    void CycleTouchPersonality() {
        static const char* const emotions[] = {"neutral", "listening", "happy"};
        static const char* const labels[] = {"Neutro", "Atento", "Divertido"};
        constexpr int personality_count = sizeof(emotions) / sizeof(emotions[0]);
        touch_personality_index_ = (touch_personality_index_ + 1) % personality_count;
        if (display_ != nullptr) {
            display_->SetEmotion(emotions[touch_personality_index_]);
            display_->ShowNotification(labels[touch_personality_index_], 1800);
        }
        ESP_LOGI(TAG, "TTP223 personalidade: %s", labels[touch_personality_index_]);
    }

    // Toque curto: alterna o estado de escuta; no menu, avança a seleção.
    void HandleTouchShort() {
        if (touch_menu_open_.load()) {
            MoveTouchMenuNext();
            return;
        }

        auto& app = Application::GetInstance();
        const auto state = app.GetDeviceState();
        app.ToggleChatState();
        if (display_ != nullptr) {
            display_->ShowNotification(
                state == kDeviceStateListening ? "Solicitando: escuta desativada"
                                               : "Solicitando: escuta ativada",
                1800);
        }
        ESP_LOGI(TAG, "TTP223: alternancia de escuta solicitada");
    }

    // Toque duplo: seleciona no menu ou troca a personalidade fora dele.
    void HandleTouchDouble() {
        if (touch_menu_open_.load()) {
            SelectTouchMenuOption();
            // A opção selecionada apresenta o resultado por alguns segundos.
            // O menu fecha após a seleção para não sobrescrever essa confirmação.
            touch_menu_open_.store(false);
            if (touch_menu_timer_ != nullptr) {
                esp_timer_stop(touch_menu_timer_);
            }
            return;
        }
        CycleTouchPersonality();
    }

    // Toque longo: abre o menu ou volta/fecha o menu atual.
    void HandleTouchLong() {
        if (touch_menu_open_.load()) {
            CloseTouchMenu();
        } else {
            OpenTouchMenu();
        }
    }

    void InitializeButtons() {
        // Botão BOOT original: comportamento preservado.
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // DOC TTP223: qualquer toque durante a fala interrompe imediatamente
        // a resposta. A flag impede que a soltura seja interpretada como outro gesto.
        touch_button_.OnPressDown([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateSpeaking) {
                touch_interrupted_speech_.store(true);
                app.Schedule([&app]() {
                    app.AbortSpeaking(kAbortReasonNone);
                });
                ESP_LOGI(TAG, "TTP223: interrupcao da resposta solicitada");
            }
        });

        // Toque simples: o evento só é entregue após a janela de detecção de duplo toque.
        touch_button_.OnClick([this]() {
            if (touch_interrupted_speech_.exchange(false)) {
                return;
            }
            HandleTouchShort();
        });

        // Toque duplo: não dispara dois comandos de toque curto.
        touch_button_.OnDoubleClick([this]() {
            if (touch_interrupted_speech_.exchange(false)) {
                return;
            }
            HandleTouchDouble();
        });

        // Toque longo: abre o menu; dentro dele, volta/fecha.
        touch_button_.OnLongPress([this]() {
            if (touch_interrupted_speech_.exchange(false)) {
                return;
            }
            HandleTouchLong();
        });
    }

    // DOC INÍCIO — API web inicia somente após a rede estar conectada
    void HandleNetworkEvent(NetworkEvent event, const std::string& data) {
        if (event == NetworkEvent::Connected && !servo_api_started_) {
            ESP_LOGI(TAG, "Wi-Fi conectado; iniciando API web de servos");
            DocWebServoApiStart();
            servo_api_started_ = true;
        } else if (event == NetworkEvent::Disconnected && servo_api_started_) {
            // Em desconexão, tenta parar os motores e encerra o servidor.
            DocWebServoApiStop();
            servo_api_started_ = false;
        }
    }
    // DOC FIM

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
        io_config.pclk_hz = 20 * 1000 * 1000;
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
            esp_lcd_panel_mirror(
                panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(
            esp_lcd_panel_set_gap(
                panel, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new SpiLcdDisplay(
            panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
            DISPLAY_SWAP_XY);

        ESP_LOGI(TAG, "ST7735 display initialized");
    }

public:
    // DOC INÍCIO — preservar o callback da aplicação e tratar eventos da placa
    // Application::Initialize() chama este método depois do construtor.
    // Encadeamos o callback da API web com o callback principal, evitando que
    // o registro posterior da aplicação substitua o tratamento de rede do DOC.
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback(
            [this, callback](NetworkEvent event, const std::string& data) {
                HandleNetworkEvent(event, data);
                if (callback) {
                    callback(event, data);
                }
            });
    }
    // DOC FIM

    DocAiS3Cam()
        : boot_button_(BOOT_BUTTON_GPIO),
          touch_button_(TOUCH_BUTTON_GPIO, true, DOC_TTP223_LONG_PRESS_MS, 0) {
        ESP_LOGI(TAG, "DOC AI S3CAM board startup");
        InitializeButtons();
        InitializeDisplay();

        // Timer de inatividade do menu físico do TTP223.
        esp_timer_create_args_t touch_menu_timer_args = {
            .callback = TouchMenuTimeout,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "doc_touch_menu",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&touch_menu_timer_args, &touch_menu_timer_));

        // A Application registra seu callback durante Initialize().
        // O encadeamento do callback é implementado em SetNetworkEventCallback().

        ESP_LOGI(TAG,
                 "I2S mic BCLK=%d WS=%d DIN=%d; speaker BCLK=%d WS=%d DOUT=%d",
                 AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS,
                 AUDIO_I2S_MIC_GPIO_DIN,
                 AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                 AUDIO_I2S_SPK_GPIO_DOUT);
    }

    ~DocAiS3Cam() override {
        if (touch_menu_timer_ != nullptr) {
            esp_timer_stop(touch_menu_timer_);
            esp_timer_delete(touch_menu_timer_);
            touch_menu_timer_ = nullptr;
        }
        CloseCamera();
        if (servo_api_started_) {
            DocWebServoApiStop();
        }
    }

    // DOC INÍCIO — API pública da câmera OV3660.
    bool HasCamera() override {
        return true;
    }

    bool SupportsCameraLifecycle() override {
        return true;
    }

    Camera* GetCamera() override {
        return camera_;
    }

    Camera* OpenCamera() override {
        if (camera_ == nullptr) {
            InitializeCamera();
            if (camera_ == nullptr || !camera_->Capture()) {
                ESP_LOGE(TAG, "Failed to initialize or capture from OV3660");
                delete camera_;
                camera_ = nullptr;
                return nullptr;
            }
        }
        camera_->SetExplainUrl(camera_vision_url_, camera_vision_token_);
        StartCameraPreviewTask();
        return camera_;
    }

    bool CloseCamera() override {
        StopCameraPreviewTask();
        delete camera_;
        camera_ = nullptr;
        if (display_ != nullptr) {
            display_->SetPreviewImage(nullptr);
        }
        ESP_LOGI(TAG, "OV3660 camera closed");
        return true;
    }

    void SetCameraVisionEndpoint(const std::string& url, const std::string& token) override {
        camera_vision_url_ = url;
        camera_vision_token_ = token;
        if (camera_ != nullptr) {
            camera_->SetExplainUrl(camera_vision_url_, camera_vision_token_);
        }
    }

    std::expected<std::string, std::string> CaptureAndExplain(
        const std::string& question) override {
        const bool camera_was_open = camera_ != nullptr;
        if (!camera_was_open && OpenCamera() == nullptr) {
            return std::unexpected("Failed to open OV3660 camera");
        }

        // Pause preview during photo analysis so two tasks do not capture at once.
        StopCameraPreviewTask();
        if (camera_ == nullptr || !camera_->Capture()) {
            if (camera_was_open) {
                StartCameraPreviewTask();
            } else {
                CloseCamera();
            }
            return std::unexpected("Failed to capture photo");
        }

        auto result = camera_->Explain(question);
        if (camera_was_open) {
            StartCameraPreviewTask();
        } else {
            CloseCamera();
        }
        return result;
    }
    // DOC FIM — API pública da câmera OV3660.

    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK,
            AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS,
            AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(DocAiS3Cam);
