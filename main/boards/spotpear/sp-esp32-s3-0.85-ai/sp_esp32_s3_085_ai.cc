#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "led/circular_strip.h"
#include "button.h"
#include "config.h"
#include "settings.h"

#include "pet/lamp.h"
#include "pet/pet.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include <lvgl.h>
#include "esp_lvgl_port.h"

#include <string>

#define TAG "SpotpearS3_085AI"

// Kitty is the whole UI of this device, so the built-in display is stripped
// down: no status bar, no noto emoji, no chat bubbles. Nothing above this layer
// draws the pet; main/pet/ does that on lv_layer_top().
class PetLcdDisplay : public SpiLcdDisplay {
public:
    PetLcdDisplay(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(io, panel, width, height, offset_x, offset_y,
                        mirror_x, mirror_y, swap_xy) {}

    // Emotions come from the pet now. Accept and ignore whatever the assistant
    // layer tries to set, so a stale server reply cannot overwrite her face.
    bool SetEmotion(const char* name) override {
        (void)name;
        return true;
    }

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

// Adapter between the one WS2812 and the pet's narrow Lamp interface.
class StripLamp : public kitty::Lamp {
public:
    explicit StripLamp(CircularStrip& strip) : strip_(strip) {}

    void SetColor(uint8_t red, uint8_t green, uint8_t blue) override {
        strip_.SetAllColor(StripColor{red, green, blue});
    }

private:
    CircularStrip& strip_;
};

class SpotpearS3_085AI : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    PetLcdDisplay* display_ = nullptr;
    CircularStrip led_{RGB_LED_GPIO, 1};
    StripLamp lamp_{led_};

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

    // Wiring only: every gesture is forwarded to the pet, which owns the logic.
    void InitializeButtons() {
        boot_button_.OnClick([]() {
            kitty::OnGesture(kitty::Gesture::kBootClick);
        });
        boot_button_.OnDoubleClick([]() {
            kitty::OnGesture(kitty::Gesture::kBootDoubleClick);
        });
        boot_button_.OnLongPress([]() {
            kitty::OnGesture(kitty::Gesture::kBootLongPress);
        });
    }

public:
    SpotpearS3_085AI() : boot_button_(BOOT_BUTTON_GPIO) {
        // The display reads its theme from NVS when it is constructed, so the
        // preference has to be written before InitializeLcdDisplay().
        {
            Settings settings("display", true);
            settings.SetString("theme", "dark");
        }
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        kitty::RegisterLamp(&lamp_);
        GetBacklight()->RestoreBrightness();
        ESP_LOGI(TAG, "Board initialised");
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

    // Single WS2812B on GP13. CircularStrip drives it through the RMT
    // peripheral; the pet talks to it through StripLamp above.
    virtual Led* GetLed() override {
        return &led_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(SpotpearS3_085AI);
