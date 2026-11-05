#ifndef KITTY_PET_TYPES_H_
#define KITTY_PET_TYPES_H_

#include <cstdint>

namespace kitty {

// The three identities of docs/pet-design.md §4. The device is always in one
// of them; the state vector below is continuous across the switch.
enum class Mode : uint8_t {
    kAutonomous,
    kDocked,
    kRemote,
};

// Growth stage (docs/pet-design.md §6). Derived from care, never stored.
enum class Stage : uint8_t {
    kBaby,
    kGrown,
    kMature,
};

// What she feels right now. Several of these are not stored: they are derived
// from the four numbers on every tick.
enum class Mood : uint8_t {
    kContent,
    kHappy,
    kHungry,
    kTired,
    kSleepy,
    kDirty,
    kLonely,
    kAsleep,
};

// Animation rows of the sprite sheet (main/pet/assets/pet_frames.h).
// The names come from the codex-pet sheet and describe the motion, not an
// emotion: the art has no separate face per emotion.
enum class Clip : uint8_t {
    kIdle = 0,
    kRunRight,
    kRunLeft,
    kWave,
    kJump,
    kFail,
    kWait,
    kReview,
    kRun,
};

// The four numbers (0-100) plus everything derived from them.
struct Status {
    int hunger = 70;
    int energy = 70;
    int clean = 80;
    int affection = 20;
    int interactions = 0;
    Mode mode = Mode::kAutonomous;
    Stage stage = Stage::kBaby;
    Mood mood = Mood::kContent;
    bool sleeping = false;
    bool night = false;
    int calls = 0;  // how many times she heard her name, saved across boots
};

const char* ToName(Mood mood);
const char* ToName(Stage stage);
const char* ToName(Mode mode);
const char* ToName(Clip clip);

}  // namespace kitty

#endif  // KITTY_PET_TYPES_H_
