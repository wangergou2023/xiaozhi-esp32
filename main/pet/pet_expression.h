#ifndef KITTY_PET_EXPRESSION_H_
#define KITTY_PET_EXPRESSION_H_

#include "pet/pet_behavior.h"
#include "pet/pet_types.h"

namespace kitty {

// How the body should look right now: which row of the sheet, how fast it runs,
// how much she breathes, and what the LED and the backlight do.
struct Look {
    Clip clip = Clip::kIdle;
    int frame_ms = 140;    // 0 means "hold a single frame"
    int frame = 0;         // used when frame_ms == 0
    float breath_px = 1.5f;
    uint8_t red = 255;
    uint8_t green = 208;
    uint8_t blue = 160;
    uint8_t screen_level = 0;  // 0 means "use the configured brightness"
};

// What her numbers look like.
Look LookForStatus(const Status& status);

// What she is doing on her own, ignoring the numbers. The intent wins only for
// its duration; the caller falls back to LookForStatus() afterwards.
Look LookForIntent(Intent intent, const Status& status);

}  // namespace kitty

#endif  // KITTY_PET_EXPRESSION_H_
