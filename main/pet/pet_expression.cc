#include "pet/pet_expression.h"

#include "settings.h"

namespace kitty {
namespace {

// Read once: the backlight level the user configured, so she does not fight the
// settings page. 0 in Look means "keep this".
uint8_t ConfiguredBrightness() {
    static uint8_t cached = 0;
    if (cached == 0) {
        Settings settings("display", false);
        int level = settings.GetInt("brightness", 75);
        if (level < 5) {
            level = 5;
        }
        cached = static_cast<uint8_t>(level);
    }
    return cached;
}

}  // namespace

const char* ToName(Intent intent) {
    switch (intent) {
        case Intent::kNone: return "none";
        case Intent::kIdle: return "idle";
        case Intent::kLookAround: return "look_around";
        case Intent::kYawn: return "yawn";
        case Intent::kStretch: return "stretch";
        case Intent::kHop: return "hop";
        case Intent::kDoze: return "doze";
    }
    return "unknown";
}

Look LookForStatus(const Status& status) {
    Look look;
    look.screen_level = ConfiguredBrightness();

    switch (status.mood) {
        case Mood::kAsleep:
            look.clip = Clip::kFail;
            look.frame_ms = 0;
            look.frame = 2;
            look.breath_px = 0.7f;
            look.red = 0;
            look.green = 0;
            look.blue = 0;
            look.screen_level = 4;
            break;
        case Mood::kSleepy:
            look.clip = Clip::kFail;
            look.frame_ms = 220;
            look.breath_px = 2.2f;
            look.red = 90;
            look.green = 110;
            look.blue = 200;
            look.screen_level = 45;
            break;
        case Mood::kTired:
            look.clip = Clip::kWait;
            look.frame_ms = 190;
            look.breath_px = 2.0f;
            look.red = 120;
            look.green = 140;
            look.blue = 210;
            look.screen_level = 55;
            break;
        case Mood::kHungry:
            look.clip = Clip::kWait;
            look.frame_ms = 130;
            look.breath_px = 1.8f;
            look.red = 255;
            look.green = 110;
            look.blue = 30;
            break;
        case Mood::kDirty:
            look.clip = Clip::kIdle;
            look.frame_ms = 170;
            look.red = 150;
            look.green = 120;
            look.blue = 70;
            break;
        case Mood::kLonely:
            look.clip = Clip::kIdle;
            look.frame_ms = 190;
            look.breath_px = 1.0f;
            look.red = 90;
            look.green = 90;
            look.blue = 150;
            look.screen_level = 60;
            break;
        case Mood::kHappy:
            look.clip = Clip::kJump;
            look.frame_ms = 95;
            look.breath_px = 2.4f;
            look.red = 255;
            look.green = 175;
            look.blue = 90;
            break;
        case Mood::kContent:
        default:
            look.clip = Clip::kIdle;
            look.frame_ms = 140;
            look.red = 255;
            look.green = 208;
            look.blue = 160;
            break;
    }
    return look;
}

Look LookForIntent(Intent intent, const Status& status) {
    Look look = LookForStatus(status);
    switch (intent) {
        case Intent::kIdle:
            look.clip = Clip::kIdle;
            look.frame_ms = 140;
            break;
        case Intent::kLookAround:
            look.clip = Clip::kWait;
            look.frame_ms = 150;
            break;
        case Intent::kYawn:
            look.clip = Clip::kReview;
            look.frame_ms = 240;
            look.breath_px = 2.6f;
            break;
        case Intent::kStretch:
            look.clip = Clip::kReview;
            look.frame_ms = 200;
            look.breath_px = 2.2f;
            break;
        case Intent::kHop:
            look.clip = Clip::kJump;
            look.frame_ms = 95;
            look.breath_px = 2.4f;
            break;
        case Intent::kDoze:
            look.clip = Clip::kFail;
            look.frame_ms = 0;
            look.frame = 2;
            look.breath_px = 0.8f;
            break;
        case Intent::kNone:
        default:
            break;
    }
    // A sleeping pet does not get up for a yawn: the numbers win.
    if (status.sleeping) {
        return LookForStatus(status);
    }
    return look;
}

}  // namespace kitty
