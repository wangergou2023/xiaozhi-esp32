#include "pet/pet_types.h"

namespace kitty {

const char* ToName(Mood mood) {
    switch (mood) {
        case Mood::kContent: return "content";
        case Mood::kHappy: return "happy";
        case Mood::kHungry: return "hungry";
        case Mood::kTired: return "tired";
        case Mood::kSleepy: return "sleepy";
        case Mood::kDirty: return "dirty";
        case Mood::kLonely: return "lonely";
        case Mood::kAsleep: return "asleep";
    }
    return "unknown";
}

const char* ToName(Stage stage) {
    switch (stage) {
        case Stage::kBaby: return "baby";
        case Stage::kGrown: return "grown";
        case Stage::kMature: return "mature";
    }
    return "unknown";
}

const char* ToName(Mode mode) {
    switch (mode) {
        case Mode::kAutonomous: return "autonomous";
        case Mode::kDocked: return "docked";
        case Mode::kRemote: return "remote";
    }
    return "unknown";
}

const char* ToName(Clip clip) {
    switch (clip) {
        case Clip::kIdle: return "idle";
        case Clip::kRunRight: return "run_right";
        case Clip::kRunLeft: return "run_left";
        case Clip::kWave: return "wave";
        case Clip::kJump: return "jump";
        case Clip::kFail: return "fail";
        case Clip::kWait: return "wait";
        case Clip::kReview: return "review";
        case Clip::kRun: return "run";
    }
    return "unknown";
}

}  // namespace kitty
