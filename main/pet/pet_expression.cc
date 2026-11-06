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
            look.red = 26;   // a faint ember, not a dead lamp
            look.green = 8;
            look.blue = 3;
            look.screen_level = 4;
            break;
        case Mood::kSleepy:
            look.clip = Clip::kFail;
            look.frame_ms = 220;
            look.breath_px = 2.2f;
            look.red = 40;
            look.green = 50;
            look.blue = 190;
            look.screen_level = 45;
            break;
        case Mood::kTired:
            look.clip = Clip::kWait;
            look.frame_ms = 190;
            look.breath_px = 2.0f;
            look.red = 60;
            look.green = 90;
            look.blue = 255;
            look.screen_level = 55;
            break;
        case Mood::kHungry:
            look.clip = Clip::kWait;
            look.frame_ms = 130;
            look.breath_px = 1.8f;
            look.red = 255;
            look.green = 60;
            look.blue = 10;
            break;
        case Mood::kDirty:
            look.clip = Clip::kIdle;
            look.frame_ms = 170;
            look.red = 150;
            look.green = 105;
            look.blue = 20;
            break;
        case Mood::kLonely:
            look.clip = Clip::kIdle;
            look.frame_ms = 190;
            look.breath_px = 1.0f;
            look.red = 30;
            look.green = 150;
            look.blue = 160;
            look.screen_level = 60;
            break;
        case Mood::kHappy:
            look.clip = Clip::kJump;
            look.frame_ms = 95;
            look.breath_px = 2.4f;
            look.red = 255;
            look.green = 200;
            look.blue = 20;
            break;
        case Mood::kContent:
        default:
            look.clip = Clip::kIdle;
            look.frame_ms = 140;
            look.red = 255;
            look.green = 140;
            look.blue = 40;
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
            // kRun is the head turning left and right; kWait just sways.
            look.clip = Clip::kRun;
            look.frame_ms = 190;
            break;
        case Intent::kYawn:
            // kFail is the clip that closes its eyes and drops its head: a yawn.
            // kReview is the one typing on a laptop, which is not a yawn.
            look.clip = Clip::kFail;
            look.frame_ms = 220;
            look.breath_px = 2.6f;
            break;
        case Intent::kStretch:
            // Arms up and a rise: that is what a stretch looks like here.
            look.clip = Clip::kJump;
            look.frame_ms = 190;
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
