#ifndef KITTY_PET_ACTION_H_
#define KITTY_PET_ACTION_H_

#include <cstdint>

#include "pet/lamp.h"
#include "pet/pet_types.h"

#include <lvgl.h>

class Display;
class Backlight;

namespace kitty {

// The only layer allowed to touch hardware: the LVGL face, the RGB LED and the
// backlight. Everything above it speaks in clips, colours and levels.
class Action {
public:
    static Action& Get();

    // Shrink the face vertically by ~10% to read as a blink. The art has no
    // eyelid frames, so this is the closest honest option.
    static constexpr int32_t kEyesOpen = 256;
    static constexpr int32_t kEyesClosed = 230;

    void Attach(Display* display, Lamp* lamp, Backlight* backlight);
    bool ready() const { return image_ != nullptr; }

    // Play a clip in a loop. frame_ms == 0 freezes the clip on its first frame.
    void Play(Clip clip, int frame_ms);
    // Show one frame and hold it.
    void Freeze(Clip clip, int frame = 0);
    // Vertical breathing amplitude in pixels (0 disables the sway).
    void SetBreathing(float amplitude_px);
    // Nudge the whole face by a few pixels. The eyes are part of the frame, so
    // this is the only way she can "look" somewhere.
    void SetLook(int dx, int dy);
    // Backlight level 0-100.
    void SetScreenLevel(uint8_t level);
    // A small dark panel over the face, used while the configuration hotspot is
    // up so the phone can read the SSID off the screen.
    void ShowBanner(const char* line1, const char* line2);
    void HideBanner();
    // How the LED behaves on top of its colour.
    enum class LedPattern : uint8_t {
        kBreath,      // 3 s breathing, the idle look
        kSlowBreath,  // 6 s deep breathing: drowsy, lonely, asleep
        kFastBlink,   // 4 Hz, a host has taken over (docs/pet-manual.md §10)
        kDoubleBlink, // two short pulses every 2 s: hungry, wants attention
        kSolid,       // constant
    };
    void SetLedPattern(LedPattern pattern) {
        if (led_pattern_ != pattern) {
            led_pattern_ = pattern;
            led_phase_ = 0.0f;
            led_slow_phase_ = 0.0f;
        }
    }
    // A short colour burst over the base colour: white for "heard my name",
    // green for a finished task, red for a failed one.
    void Flash(uint8_t red, uint8_t green, uint8_t blue, int pulses = 1,
               float seconds = 0.9f);
    // LED colour, already dimmed by the caller.
    void SetColor(uint8_t red, uint8_t green, uint8_t blue);
    // 0 turns the lamp off, 100 is full brightness. Saved in NVS unless told not to.
    void SetLedLevel(int percent, bool save = true);
    int led_level() const { return led_level_; }
    // Advance the idle motion (breathing, blink, LED pulse). dt in seconds.
    void Tick(float dt);

private:
    void ApplyLocked(int clip_index, int frame_ms);
    void PushColor(uint8_t red, uint8_t green, uint8_t blue, float factor);

    Display* display_ = nullptr;
    Lamp* lamp_ = nullptr;
    Backlight* backlight_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_obj_t* banner_canvas_ = nullptr;   // offscreen: text is drawn here
    lv_obj_t* banner_image_ = nullptr;    // rotated + mirrored copy on screen
    uint8_t* banner_canvas_buf_ = nullptr;
    uint8_t* banner_image_buf_ = nullptr;
    lv_image_dsc_t banner_dsc_ = {};

    int clip_ = -1;
    int frame_ms_ = 0;
    int frozen_frame_ = -1;

    float breath_px_ = 1.5f;
    float breath_phase_ = 0.0f;
    int look_dx_ = 0;
    int look_dy_ = 0;

    float blink_in_ = 4.0f;
    float blink_left_ = 0.0f;

    uint8_t level_ = 0;       // cached backlight level
    uint8_t red_ = 0, green_ = 0, blue_ = 0;
    uint8_t led_r_ = 0, led_g_ = 0, led_b_ = 0;
    int led_level_ = 100;
    float led_phase_ = 0.0f;
    float led_slow_phase_ = 0.0f;
    float flash_left_ = 0.0f;
    float flash_seconds_ = 0.9f;
    int flash_pulses_ = 1;
    uint8_t flash_r_ = 0, flash_g_ = 0, flash_b_ = 0;
    bool led_dirty_ = false;
    LedPattern led_pattern_ = LedPattern::kBreath;
};

}  // namespace kitty

#endif  // KITTY_PET_ACTION_H_
