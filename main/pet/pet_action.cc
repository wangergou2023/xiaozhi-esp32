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

void Action::Attach(Display* display, Lamp* lamp, Backlight* backlight) {
    display_ = display;
    lamp_ = lamp;
    backlight_ = backlight;

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

void Action::ShowBanner(const char* line1, const char* line2) {
    if (!ready()) {
        return;
    }
    char text[64] = {};
    snprintf(text, sizeof(text), "%s\n%s", line1 != nullptr ? line1 : "",
             line2 != nullptr ? line2 : "");
    DisplayLockGuard lock(display_);
    if (!lock) {
        return;
    }
    if (banner_ == nullptr) {
        banner_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(banner_);
        lv_obj_set_size(banner_, PET_FRAME_W, 52);
        lv_obj_align(banner_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(banner_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(banner_, LV_OPA_80, 0);
        banner_label_ = lv_label_create(banner_);
        lv_obj_set_style_text_color(banner_label_, lv_color_white(), 0);
        lv_obj_set_style_text_font(banner_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(banner_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(banner_label_, LV_ALIGN_CENTER, 0, 0);
    }
    lv_label_set_text(banner_label_, text);
    lv_obj_remove_flag(banner_, LV_OBJ_FLAG_HIDDEN);
}

void Action::HideBanner() {
    if (banner_ == nullptr || display_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (lock) {
        lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
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

void Action::PushColor(float factor) {
    if (lamp_ == nullptr) {
        return;
    }
    const auto scale = [factor](uint8_t value) {
        float scaled = static_cast<float>(value) * factor;
        return static_cast<uint8_t>(scaled > 255.0f ? 255.0f : scaled);
    };
    const uint8_t r = scale(red_);
    const uint8_t g = scale(green_);
    const uint8_t b = scale(blue_);
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
    float pulse = 0.78f + 0.22f * (0.5f + 0.5f * sinf(led_phase_));
    if (led_pattern_ == LedPattern::kFastBlink) {
        // 4 Hz on/off: unmistakable "somebody is driving me" without a screen.
        pulse = fmodf(led_phase_ / (2.0f * kPi) * kLedPeriod, 0.25f) < 0.125f ? 1.0f : 0.0f;
    }
    PushColor(pulse);
}

}  // namespace kitty
