#ifndef KITTY_PET_BATTERY_H_
#define KITTY_PET_BATTERY_H_

#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>

namespace kitty {

// Battery reading for the Spotpear 0.85 AI board.
//
// The schematic puts the battery on a 100K/100K divider (R24/R26) with a 0.1 uF
// cap (C60) at the midpoint, and that midpoint is GPIO1 (schematic pin 6,
// net VBAT_ADC). So the pin sits at half the pack voltage: 4.2 V -> 2.1 V.
//
// The divider is a 50K source, so the reading must not be hammered: this class
// keeps its own cadence (one conversion per second at most) and lets the cap
// recover in between.
class Battery {
public:
    static Battery& Get();

    void Start();
    // Battery voltage in millivolts, or 0 when the pin reads nothing useful.
    int Millivolts();
    // 0-100, linear between 3.3 V and 4.2 V.
    int Percent();
    bool ready() const { return unit_ != nullptr; }

private:
    adc_oneshot_unit_handle_t unit_ = nullptr;
    adc_cali_handle_t cali_ = nullptr;
    int last_mv_ = 0;
    int64_t last_read_us_ = 0;
};

}  // namespace kitty

#endif  // KITTY_PET_BATTERY_H_
