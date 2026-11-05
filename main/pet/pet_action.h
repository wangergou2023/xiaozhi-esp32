#ifndef KITTY_PET_ACTION_H_
#define KITTY_PET_ACTION_H_

#include <cstdint>

#include "pet/lamp.h"
#include "pet/pet_types.h"

class Display;
class Backlight;
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace kitty {

// The only layer allowed to touch hardware: the LVGL face, the RGB LED and the
// backlight. Everything above it speaks in clips, colours and levels.
class Action {
public:
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
        kSteady,  // slow breathing, the idle look
        kFastBlink,  // a host has taken over (docs/pet-manual.md §10)
    };
    void SetLedPattern(LedPattern pattern) { led_pattern_ = pattern; }
    // LED colour, already dimmed by the caller.
    void SetColor(uint8_t red, uint8_t green, uint8_t blue);
    // Advance the idle motion (breathing, blink, LED pulse). dt in seconds.
    void Tick(float dt);

private:
    void ApplyLocked(int clip_index, int frame_ms);
    void PushColor(float factor);

    Display* display_ = nullptr;
    Lamp* lamp_ = nullptr;
    Backlight* backlight_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_obj_t* banner_ = nullptr;
    lv_obj_t* banner_label_ = nullptr;

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
    float led_phase_ = 0.0f;
    bool led_dirty_ = false;
    LedPattern led_pattern_ = LedPattern::kSteady;
};

}  // namespace kitty

#endif  // KITTY_PET_ACTION_H_
