#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// =============================================================================
//  Spotpear ESP32-S3-0.85-AI  (ESP32-S3R8, 0.85" 128x128 LCD, ES8311, NS4150B)
//
//  Pin numbers taken from the vendor schematic:
//    https://cdn.static.spotpear.com/uploads/picture/learn/ESP32/ESP32-S3-LCD-0.85-AI/ESP32-S3-0.85-AI.pdf
// =============================================================================

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// ---- Audio: ES8311 codec + NS4150B power amplifier ----
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_8   // IIS_MCLK
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_4   // IIS_SCLK
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_6   // IIS_LRCK
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_7   // IIS_DSDIN  (ESP -> codec)
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_5   // IIS_ASDOUT (codec -> ESP)
#define AUDIO_CODEC_PA_PIN  GPIO_NUM_3   // PA_CTRL
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_11  // SDA
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_10  // SCL
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR

// ---- Display: 0.85" 128x128 SPI panel (GC9107), BL driven through a transistor ----
#define DISPLAY_CLK_PIN       GPIO_NUM_40   // LCD-CLK
#define DISPLAY_MOSI_PIN      GPIO_NUM_39   // LCD-MOSI
#define DISPLAY_CS_PIN        GPIO_NUM_41   // LCD-CS
#define DISPLAY_DC_PIN        GPIO_NUM_42   // LCD-DC
#define DISPLAY_RST_PIN       GPIO_NUM_38   // LCD-RST
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_14   // BL_PWM
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  128
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY false
#define DISPLAY_INVERT_COLOR  true
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_BGR
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#define DISPLAY_SPI_MODE 0

// ---- WS2812 RGB LED on GP13 ----
#define RGB_LED_GPIO GPIO_NUM_13

// ---- Buttons ----
#define BOOT_BUTTON_GPIO      GPIO_NUM_0
#define POWER_BUTTON_GPIO     GPIO_NUM_2
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC
#define BUILTIN_LED_GPIO      GPIO_NUM_NC

#endif  // _BOARD_CONFIG_H_
