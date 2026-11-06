#include "pet/pet_battery.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>

namespace kitty {
namespace {

constexpr const char* TAG = "PetBattery";

// VBAT_ADC: 100K (R24) over 100K (R26), so the pin sees half the pack.
constexpr int kUpperK = 100;
constexpr int kLowerK = 100;
constexpr int kDividerScale = (kUpperK + kLowerK) / kLowerK;  // 2

constexpr int kFullMillivolts = 4200;
constexpr int kEmptyMillivolts = 3300;

// One conversion per second at most: the divider is a 50K source with a 0.1 uF
// cap, and hammering it drags the reading down.
constexpr int64_t kMinIntervalUs = 1000 * 1000;

}  // namespace

Battery& Battery::Get() {
    static Battery instance;
    return instance;
}

void Battery::Start() {
    if (unit_ != nullptr) {
        return;
    }
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unit_cfg, &unit_) != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 unit failed");
        unit_ = nullptr;
        return;
    }

    // Put the pad into a plain floating input first. If a pull-down is left on
    // the pin it loads the divider and every reading comes out far too low.
    gpio_config_t pad = {};
    pad.pin_bit_mask = 1ULL << 1;
    pad.mode = GPIO_MODE_INPUT;
    pad.pull_up_en = GPIO_PULLUP_DISABLE;
    pad.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pad.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&pad);

    // GPIO1 = ADC1 channel 0. adc_oneshot_config_channel also puts the pad into
    // analog mode, which is what makes the divider readable at all.
    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    chan_cfg.atten = ADC_ATTEN_DB_12;
    if (adc_oneshot_config_channel(unit_, ADC_CHANNEL_0, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 channel 0 failed");
        adc_oneshot_del_unit(unit_);
        unit_ = nullptr;
        return;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_) != ESP_OK) {
        ESP_LOGW(TAG, "No ADC calibration; readings will be rough");
        cali_ = nullptr;
    }
    // Let the 0.1 uF at the pin charge through the divider before the first read.
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Battery sense on GPIO1 (VBAT_ADC), divider 1:%d", kDividerScale);
}

int Battery::Millivolts() {
    if (unit_ == nullptr) {
        return 0;
    }
    const int64_t now = esp_timer_get_time();
    if (last_read_us_ != 0 && now - last_read_us_ < kMinIntervalUs) {
        return last_mv_;
    }
    last_read_us_ = now;

    int raw = 0;
    if (adc_oneshot_read(unit_, ADC_CHANNEL_0, &raw) != ESP_OK) {
        return last_mv_;
    }
    int pin_mv = 0;
    if (cali_ != nullptr) {
        if (adc_cali_raw_to_voltage(cali_, raw, &pin_mv) != ESP_OK) {
            pin_mv = raw * 3300 / 4095;
        }
    } else {
        pin_mv = raw * 3300 / 4095;
    }

    // A pin that reads essentially nothing is not a battery voltage; treat it as
    // "no reading" rather than reporting a bogus 0 %. Do not cache that for a
    // full second: retry soon, a divider that just came up may still settle.
    if (pin_mv < 300) {
        last_mv_ = 0;
        // Retry on the normal cadence: no voltage on the pin is a steady state,
        // not something to probe ten times a second.
        ESP_LOGD(TAG, "Battery pin low: raw %d, %d mV", raw, pin_mv);
        return 0;
    }
    ESP_LOGD(TAG, "Battery pin %d mV -> pack %d mV", pin_mv, pin_mv * kDividerScale);
    last_mv_ = pin_mv * kDividerScale;
    return last_mv_;
}

int Battery::Percent() {
    const int millivolts = Millivolts();
    if (millivolts == 0) {
        return -1;
    }
    int percent = (millivolts - kEmptyMillivolts) * 100 / (kFullMillivolts - kEmptyMillivolts);
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    return percent;
}

}  // namespace kitty
