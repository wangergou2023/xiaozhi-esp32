#include "pet/pet_action.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <lvgl.h>

#include "backlight.h"
#include "board.h"
#include "settings.h"
#include "display/display.h"
#include "pet/assets/pet_frames.h"

// The 57-frame blob is embedded by main/CMakeLists.txt (EMBED_FILES). The
// symbol name only depends on the file name, not on its directory.
extern "C" {
extern const uint8_t pet_frames_blob[] asm("_binary_pet_frames_bin_start");
}

namespace kitty {
namespace {

constexpr const char* TAG = "PetAction";
constexpr float kPi = 3.14159265358979f;

constexpr float kBreathPeriod = 4.5f;   // seconds per breath
constexpr float kLedPeriod = 3.0f;      // seconds per LED pulse
constexpr float kBlinkMin = 3.0f;
constexpr float kBlinkMax = 9.0f;
constexpr float kBlinkDuration = 0.12f;
constexpr int32_t kMaxClipFrames = 8;   // widest row of the sheet

uint8_t* g_blob = nullptr;
lv_image_dsc_t g_frames[PET_FRAME_COUNT];

// LVGL keeps the array pointer, not a copy (lv_animimage.c: animimg->dsc = dsc),
// so the array has to outlive the call. A stack buffer here crashed the board
// with a LoadProhibited inside the animation timer.
const void* g_srcs[kMaxClipFrames];

bool LoadFrames() {
    if (g_blob != nullptr) {
        return true;
    }
    constexpr size_t kBytes = static_cast<size_t>(PET_FRAME_COUNT) * PET_FRAME_BYTES;
    g_blob = static_cast<uint8_t*>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_blob == nullptr) {
        ESP_LOGE(TAG, "Not enough PSRAM for %u bytes of frames", static_cast<unsigned>(kBytes));
        return false;
    }
    memcpy(g_blob, pet_frames_blob, kBytes);

    for (int i = 0; i < PET_FRAME_COUNT; i++) {
        lv_image_dsc_t& dsc = g_frames[i];
        memset(&dsc, 0, sizeof(dsc));
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        dsc.header.w = PET_FRAME_W;
        dsc.header.h = PET_FRAME_H;
        dsc.header.stride = PET_FRAME_W * 2;
        dsc.data_size = PET_FRAME_BYTES;
        dsc.data = g_blob + static_cast<size_t>(i) * PET_FRAME_BYTES;
    }
    ESP_LOGI(TAG, "Loaded %d frames, %d bytes each", PET_FRAME_COUNT, PET_FRAME_BYTES);
    return true;
}

}  // namespace

Action& Action::Get() {
    static Action instance;
    return instance;
}

void Action::Attach(Display* display, Lamp* lamp, Backlight* backlight) {
    display_ = display;
    lamp_ = lamp;
    backlight_ = backlight;
    led_level_ = Settings("kitty", false).GetInt("led", 100);
    if (led_level_ < 0) {
        led_level_ = 0;
    } else if (led_level_ > 100) {
        led_level_ = 100;
    }

    if (display_ == nullptr) {
        ESP_LOGW(TAG, "No display: the pet will run blind");
        return;
    }
    if (!LoadFrames()) {
        return;
    }

    DisplayLockGuard lock(display_);
    if (!lock) {
        ESP_LOGE(TAG, "Could not lock the display to create the face");
        return;
    }
    image_ = lv_animimg_create(lv_layer_top());
    lv_obj_remove_style_all(image_);
    lv_obj_set_size(image_, PET_FRAME_W, PET_FRAME_H);
    lv_obj_align(image_, LV_ALIGN_CENTER, 0, 0);
    ESP_LOGI(TAG, "Face ready (%dx%d)", PET_FRAME_W, PET_FRAME_H);
}

void Action::ApplyLocked(int clip_index, int frame_ms) {
    if (clip_index < 0 || clip_index >= PET_STATE_COUNT) {
        return;
    }
    const int first = pet_state_first[clip_index];
    int count = pet_state_frames[clip_index];
    if (count > kMaxClipFrames) {
        count = kMaxClipFrames;
    }

    if (frame_ms <= 0) {
        g_srcs[0] = &g_frames[first];
        lv_animimg_set_src(image_, g_srcs, 1);
        lv_animimg_set_duration(image_, 1000);
        lv_animimg_set_repeat_count(image_, 1);
        lv_animimg_start(image_);
        frozen_frame_ = 0;
    } else {
        for (int i = 0; i < count; i++) {
            g_srcs[i] = &g_frames[first + i];
        }
        lv_animimg_set_src(image_, g_srcs, count);
        lv_animimg_set_duration(image_, static_cast<uint32_t>(count * frame_ms));
        lv_animimg_set_repeat_count(image_, LV_ANIM_REPEAT_INFINITE);
        lv_animimg_start(image_);
        frozen_frame_ = -1;
    }
    clip_ = clip_index;
    frame_ms_ = frame_ms;
}

void Action::Play(Clip clip, int frame_ms) {
    if (!ready()) {
        return;
    }
    const int index = static_cast<int>(clip);
    if (index == clip_ && frame_ms == frame_ms_ && frozen_frame_ < 0) {
        return;  // already running this clip
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return;
    }
    ApplyLocked(index, frame_ms);
}

void Action::Freeze(Clip clip, int frame) {
    if (!ready()) {
        return;
    }
    const int index = static_cast<int>(clip);
    if (index < 0 || index >= PET_STATE_COUNT) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return;
    }
    int count = pet_state_frames[index];
    if (frame < 0 || frame >= count) {
        frame = 0;
    }
    g_srcs[0] = &g_frames[pet_state_first[index] + frame];
    lv_animimg_set_src(image_, g_srcs, 1);
    lv_animimg_set_duration(image_, 1000);
    lv_animimg_set_repeat_count(image_, 1);
    lv_animimg_start(image_);
    clip_ = index;
    frame_ms_ = 0;
    frozen_frame_ = frame;
}

// The panel is viewed through a beam splitter: everything the eye sees is
// rotated 90 degrees and mirrored. The animation frames are pre-transformed by
// scripts/gen_pet_frames.py; anything LVGL draws has to get the same treatment,
// which is why the banner goes through a canvas and a pixel transform instead of
// straight onto the screen.
void Action::ShowBanner(const char* line1, const char* line2) {
    if (!ready()) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return;
    }

    constexpr int kW = PET_FRAME_W;  // 128, as seen by the user
    constexpr int kH = 52;

    if (banner_canvas_ == nullptr) {
        banner_canvas_buf_ = static_cast<uint8_t*>(
            heap_caps_malloc(kW * kH * 2, MALLOC_CAP_SPIRAM));
        banner_image_buf_ = static_cast<uint8_t*>(
            heap_caps_malloc(kW * kH * 2, MALLOC_CAP_SPIRAM));
        if (banner_canvas_buf_ == nullptr || banner_image_buf_ == nullptr) {
            ESP_LOGE(TAG, "No PSRAM for the banner");
            return;
        }
        banner_canvas_ = lv_canvas_create(lv_layer_top());
        lv_canvas_set_buffer(banner_canvas_, banner_canvas_buf_, kW, kH,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_add_flag(banner_canvas_, LV_OBJ_FLAG_HIDDEN);

        memset(&banner_dsc_, 0, sizeof(banner_dsc_));
        banner_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        banner_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        banner_dsc_.header.w = kH;  // rotated: 52 wide
        banner_dsc_.header.h = kW;  // 128 tall
        banner_dsc_.header.stride = kH * 2;
        banner_dsc_.data_size = kW * kH * 2;
        banner_dsc_.data = banner_image_buf_;
        banner_image_ = lv_image_create(lv_layer_top());
        lv_image_set_src(banner_image_, &banner_dsc_);
        lv_obj_align(banner_image_, LV_ALIGN_CENTER, 0, 0);
    }

    lv_canvas_fill_bg(banner_canvas_, lv_color_black(), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(banner_canvas_, &layer);

    const lv_font_t* font = &lv_font_montserrat_14;
    int y = 7;
    for (const char* text : {line1, line2}) {
        if (text == nullptr || text[0] == '\0') {
            continue;
        }
        lv_point_t size = {};
        lv_text_get_size(&size, text, font, 0, 0, kW, LV_TEXT_FLAG_NONE);
        lv_draw_label_dsc_t dsc;
        lv_draw_label_dsc_init(&dsc);
        dsc.color = lv_color_white();
        dsc.font = font;
        dsc.text = text;
        lv_area_t area = {};
        area.x1 = (kW - size.x) / 2;
        area.y1 = y;
        area.x2 = area.x1 + size.x;
        area.y2 = y + size.y;
        lv_draw_label(&layer, &dsc, &area);
        y += size.y + 2;
    }
    lv_canvas_finish_layer(banner_canvas_, &layer);

    // Same transform the frame generator applies: rotate counter-clockwise 90
    // degrees, then mirror left-right.
    const auto* src = reinterpret_cast<const uint16_t*>(banner_canvas_buf_);
    auto* dst = reinterpret_cast<uint16_t*>(banner_image_buf_);
    for (int sy = 0; sy < kH; sy++) {
        for (int sx = 0; sx < kW; sx++) {
            const int dx = kH - 1 - sy;
            const int dy = kW - 1 - sx;
            dst[dy * kH + dx] = src[sy * kW + sx];
        }
    }

    lv_obj_invalidate(banner_image_);
    lv_obj_remove_flag(banner_image_, LV_OBJ_FLAG_HIDDEN);
}

void Action::HideBanner() {
    if (banner_image_ == nullptr || display_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (lock) {
        lv_obj_add_flag(banner_image_, LV_OBJ_FLAG_HIDDEN);
    }
}

void Action::SetBreathing(float amplitude_px) {
    breath_px_ = amplitude_px < 0.0f ? 0.0f : amplitude_px;
}

void Action::SetLook(int dx, int dy) {
    constexpr int kMaxLook = 8;
    look_dx_ = std::clamp(dx, -kMaxLook, kMaxLook);
    look_dy_ = std::clamp(dy, -kMaxLook, kMaxLook);
}

void Action::SetScreenLevel(uint8_t level) {
    if (level > 100) {
        level = 100;
    }
    if (level == level_) {
        return;
    }
    level_ = level;
    if (backlight_ != nullptr) {
        backlight_->SetBrightness(level);
    }
}

void Action::SetColor(uint8_t red, uint8_t green, uint8_t blue) {
    if (red == red_ && green == green_ && blue == blue_) {
        return;
    }
    red_ = red;
    green_ = green;
    blue_ = blue;
    led_dirty_ = true;
}

void Action::SetLedLevel(int percent, bool save) {
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    led_level_ = percent;
    if (save) {
        Settings("kitty", true).SetInt("led", led_level_);
    }
    led_dirty_ = true;
    PushColor(red_, green_, blue_, 1.0f);
}

void Action::Flash(uint8_t red, uint8_t green, uint8_t blue, int pulses, float seconds) {
    flash_r_ = red;
    flash_g_ = green;
    flash_b_ = blue;
    flash_pulses_ = pulses < 1 ? 1 : pulses;
    flash_seconds_ = seconds < 0.1f ? 0.1f : seconds;
    flash_left_ = flash_seconds_;
}

void Action::PushColor(uint8_t red, uint8_t green, uint8_t blue, float factor) {
    if (lamp_ == nullptr) {
        return;
    }
    const float dim = static_cast<float>(led_level_) / 100.0f;
    const auto scale = [factor, dim](uint8_t value) {
        float scaled = static_cast<float>(value) * factor * dim;
        return static_cast<uint8_t>(scaled > 255.0f ? 255.0f : scaled);
    };
    const uint8_t r = scale(red);
    const uint8_t g = scale(green);
    const uint8_t b = scale(blue);
    if (r == led_r_ && g == led_g_ && b == led_b_ && !led_dirty_) {
        return;
    }
    led_r_ = r;
    led_g_ = g;
    led_b_ = b;
    led_dirty_ = false;
    lamp_->SetColor(r, g, b);
}

void Action::Tick(float dt) {
    if (!ready() || dt <= 0.0f) {
        return;
    }

    breath_phase_ += dt * 2.0f * kPi / kBreathPeriod;
    if (breath_phase_ > 2.0f * kPi) {
        breath_phase_ -= 2.0f * kPi;
    }

    if (blink_left_ > 0.0f) {
        blink_left_ -= dt;
    } else {
        blink_in_ -= dt;
        if (blink_in_ <= 0.0f) {
            blink_left_ = kBlinkDuration;
            blink_in_ = kBlinkMin + (esp_random() % 6001) / 1000.0f * (kBlinkMax - kBlinkMin) / 6.0f;
        }
    }

    const int offset = static_cast<int>(lroundf(sinf(breath_phase_) * breath_px_)) + look_dy_;
    const int32_t scale_y = blink_left_ > 0.0f ? kEyesClosed : kEyesOpen;

    DisplayLockGuard lock(display_);
    if (lock) {
        lv_obj_set_style_translate_x(image_, look_dx_, 0);
        lv_obj_set_style_translate_y(image_, offset, 0);
        lv_obj_set_style_transform_scale_y(image_, scale_y, 0);
    }

    led_phase_ += dt * 2.0f * kPi / kLedPeriod;
    if (led_phase_ > 2.0f * kPi) {
        led_phase_ -= 2.0f * kPi;
    }
    led_slow_phase_ += dt * 2.0f * kPi / (kLedPeriod * 2.0f);
    if (led_slow_phase_ > 2.0f * kPi) {
        led_slow_phase_ -= 2.0f * kPi;
    }

    // A flash sits on top of the pattern: it borrows the lamp for a second, then
    // the mood colour and breathing come back on their own.
    if (flash_left_ > 0.0f) {
        flash_left_ -= dt;
        const float elapsed = flash_seconds_ - (flash_left_ > 0.0f ? flash_left_ : 0.0f);
        const float window = flash_seconds_ / static_cast<float>(flash_pulses_);
        const float inside = fmodf(elapsed, window);
        const float pulse = inside < window * 0.55f ? 1.0f : 0.0f;
        PushColor(flash_r_, flash_g_, flash_b_, pulse);
        return;
    }

    const float cycle = led_phase_ / (2.0f * kPi) * kLedPeriod;
    float pulse = 1.0f;
    switch (led_pattern_) {
        case LedPattern::kBreath:
            pulse = 0.72f + 0.28f * (0.5f + 0.5f * sinf(led_phase_));
            break;
        case LedPattern::kSlowBreath:
            pulse = 0.40f + 0.60f * (0.5f + 0.5f * sinf(led_slow_phase_));
            break;
        case LedPattern::kFastBlink:
            // 4 Hz on/off: unmistakable "somebody is driving me" without a screen.
            pulse = fmodf(cycle, 0.25f) < 0.125f ? 1.0f : 0.0f;
            break;
        case LedPattern::kDoubleBlink:
            // Two quick pulses, then a pause: reads as "hey, over here".
            pulse = (fmodf(cycle, 2.0f) < 0.09f || (fmodf(cycle, 2.0f) >= 0.18f &&
                                                      fmodf(cycle, 2.0f) < 0.27f))
                        ? 1.0f
                        : 0.0f;
            break;
        case LedPattern::kSolid:
        default:
            pulse = 1.0f;
            break;
    }
    PushColor(red_, green_, blue_, pulse);
}

}  // namespace kitty
