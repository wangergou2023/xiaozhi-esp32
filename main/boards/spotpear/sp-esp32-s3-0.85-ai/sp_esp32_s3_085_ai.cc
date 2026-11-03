#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "pet_frames.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include <lvgl.h>
#include "esp_lvgl_port.h"

#include <cstring>
#include <string>

#define TAG "SpotpearS3_085AI"

// Animation rows inside the spritesheet (see pet_frames.h).
enum PetAnim {
    kPetIdle = 0,
    kPetRunRight = 1,
    kPetRunLeft = 2,
    kPetWave = 3,
    kPetJump = 4,
    kPetFail = 5,
    kPetWait = 6,
    kPetReview = 7,
    kPetRun = 8,
};

// The RGB565 frame blob is embedded by main/CMakeLists.txt (EMBED_FILES).
extern "C" {
extern const uint8_t pet_frames_blob[] asm("_binary_pet_frames_bin_start");
}

// Kitty is the whole UI of this device, so the built-in display is stripped down:
// no yellow noto emoji, no chat bubbles. Emotion updates are recorded here and
// turned into animations by the board.
class PetLcdDisplay : public SpiLcdDisplay {
public:
    std::string emotion;
    int64_t emotion_at_us = 0;

    PetLcdDisplay(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(io, panel, width, height, offset_x, offset_y,
                        mirror_x, mirror_y, swap_xy) {}

    bool SetEmotion(const char* name) override {
        if (name != nullptr) {
            emotion = name;
            emotion_at_us = esp_timer_get_time();
        }
        return true;  // handled by Kitty
    }

    // Full-screen pet: the chat text would only cover her.
    void SetChatMessage(const char* role, const char* content) override {
        (void)role;
        (void)content;
    }

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_bar_ != nullptr) {
            lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (chat_message_label_ != nullptr) {
            lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
};

class SpotpearS3_085AI : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    PetLcdDisplay* display_ = nullptr;

    // --- pet animation state ---
    uint8_t* pet_data_ = nullptr;
    lv_obj_t* pet_img_ = nullptr;
    static lv_image_dsc_t pet_dsc_[PET_FRAME_COUNT];
    int pet_anim_ = -1;
    int64_t wave_until_us_ = 0;
    DeviceState last_state_ = kDeviceStateUnknown;
    esp_timer_handle_t pet_timer_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = (i2c_port_t)I2C_NUM_0;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        // The 0.85" 128x128 panel is a GC9107, which the ST7789 driver drives fine.
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new PetLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // Copy the frame blob into PSRAM and build one LVGL image descriptor per frame.
    bool InitializePetFrames() {
        pet_data_ = (uint8_t*)heap_caps_malloc((size_t)PET_FRAME_COUNT * PET_FRAME_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pet_data_ == nullptr) {
            ESP_LOGE(TAG, "Not enough PSRAM for pet frames");
            return false;
        }
        memcpy(pet_data_, pet_frames_blob, (size_t)PET_FRAME_COUNT * PET_FRAME_BYTES);

        for (int i = 0; i < PET_FRAME_COUNT; i++) {
            lv_image_dsc_t& d = pet_dsc_[i];
            memset(&d, 0, sizeof(d));
            d.header.magic = LV_IMAGE_HEADER_MAGIC;
            d.header.cf = LV_COLOR_FORMAT_RGB565;
            d.header.w = PET_FRAME_W;
            d.header.h = PET_FRAME_H;
            d.header.stride = PET_FRAME_W * 2;
            d.data_size = PET_FRAME_BYTES;
            d.data = pet_data_ + (size_t)i * PET_FRAME_BYTES;
        }
        ESP_LOGI(TAG, "Pet frames loaded: %d frames, %d bytes each", PET_FRAME_COUNT, PET_FRAME_BYTES);
        return true;
    }

    void PlayAnim(int anim, int frame_ms) {
        if (anim == pet_anim_ || pet_img_ == nullptr) {
            return;
        }
        pet_anim_ = anim;
        int first = pet_state_first[anim];
        int count = pet_state_frames[anim];
        static const void* srcs[8];
        for (int i = 0; i < count && i < 8; i++) {
            srcs[i] = &pet_dsc_[first + i];
        }
        lv_animimg_set_src(pet_img_, srcs, count);
        lv_animimg_set_duration(pet_img_, count * frame_ms);
        lv_animimg_set_repeat_count(pet_img_, LV_ANIM_REPEAT_INFINITE);
        lv_animimg_start(pet_img_);
        ESP_LOGI(TAG, "Pet -> %s (%d frames)", pet_state_names[anim], count);
    }

    // Server emotions take priority; they fade out after a few seconds.
    static int AnimForEmotion(const std::string& e) {
        if (e.empty()) return -1;
        if (e == "happy" || e == "laughing" || e == "funny" || e == "loving" ||
            e == "kissy" || e == "delicious" || e == "confident" || e == "cool" ||
            e == "relaxed" || e == "surprised" || e == "shocked") {
            return kPetJump;
        }
        if (e == "sad" || e == "crying" || e == "sleepy" || e == "embarrassed" ||
            e == "angry") {
            return kPetFail;
        }
        if (e == "thinking" || e == "confused") {
            return kPetReview;
        }
        return -1;  // winking / silly / neutral: fall through to the device state
    }

    static int AnimForState(DeviceState s) {
        switch (s) {
            case kDeviceStateSpeaking:
                return kPetRun;
            case kDeviceStateListening:
            case kDeviceStateConnecting:
            case kDeviceStateActivating:
            case kDeviceStateStarting:
            case kDeviceStateWifiConfiguring:
                return kPetWait;
            case kDeviceStateUpgrading:
                return kPetReview;
            case kDeviceStateFatalError:
                return kPetFail;
            default:
                return kPetIdle;
        }
    }

    void UpdatePet() {
        auto& app = Application::GetInstance();
        DeviceState st = app.GetDeviceState();
        int64_t now = esp_timer_get_time();

        // Wave once when the conversation starts.
        if (st == kDeviceStateListening && last_state_ != kDeviceStateListening) {
            wave_until_us_ = now + 900 * 1000;
        }
        last_state_ = st;

        int anim;
        if (now < wave_until_us_) {
            anim = kPetWave;
        } else {
            int by_emotion = -1;
            if (now - display_->emotion_at_us < 4 * 1000 * 1000) {
                by_emotion = AnimForEmotion(display_->emotion);
            }
            anim = by_emotion >= 0 ? by_emotion : AnimForState(st);
        }

        // Speaking should feel livelier than idle.
        int frame_ms = (anim == kPetRun) ? 70 : 110;

        if (!lvgl_port_lock(100)) {
            return;
        }
        PlayAnim(anim, frame_ms);
        lvgl_port_unlock();
    }

    static void PetTimerCallback(void* arg) {
        static_cast<SpotpearS3_085AI*>(arg)->UpdatePet();
    }

    void InitializePet() {
        if (!InitializePetFrames()) {
            return;
        }
        if (lvgl_port_lock(1000)) {
            pet_img_ = lv_animimg_create(lv_layer_top());
            lv_obj_remove_style_all(pet_img_);
            lv_obj_set_size(pet_img_, PET_FRAME_W, PET_FRAME_H);
            lv_obj_align(pet_img_, LV_ALIGN_CENTER, 0, 0);
            lvgl_port_unlock();
        }

        esp_timer_create_args_t args = {};
        args.callback = &PetTimerCallback;
        args.arg = this;
        args.name = "pet_tick";
        ESP_ERROR_CHECK(esp_timer_create(&args, &pet_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(pet_timer_, 150 * 1000));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    SpotpearS3_085AI() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializePet();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            true);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

lv_image_dsc_t SpotpearS3_085AI::pet_dsc_[PET_FRAME_COUNT];

DECLARE_BOARD(SpotpearS3_085AI);
