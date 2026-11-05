#include "pet/pet_behavior.h"

#include <esp_random.h>

namespace kitty {
namespace {

struct WeightedIntent {
    Intent intent;
    int weight;
};

// The whole offline "intelligence": a weighted table, modulated by her numbers.
// docs/pet-design.md §10: no language model, deterministic enough to test.
Intent PickIntent(const Status& status) {
    WeightedIntent table[] = {
        {Intent::kIdle, 30},
        {Intent::kLookAround, 25},
        {Intent::kStretch, 12},
        {Intent::kYawn, 10},
        {Intent::kHop, 6},
        {Intent::kDoze, 4},
    };

    // Tired pets yawn and doze more; content pets fidget more.
    for (auto& entry : table) {
        switch (entry.intent) {
            case Intent::kYawn:
                entry.weight += (100 - status.energy) / 6;
                break;
            case Intent::kDoze:
                entry.weight += status.energy < 35 ? 30 : 0;
                break;
            case Intent::kHop:
                entry.weight += status.affection / 8 + (status.mood == Mood::kHappy ? 20 : 0);
                break;
            case Intent::kLookAround:
                entry.weight += (100 - status.affection) / 10;
                break;
            case Intent::kStretch:
                entry.weight += status.sleeping ? 0 : 5;
                break;
            default:
                break;
        }
    }

    int total = 0;
    for (const auto& entry : table) {
        total += entry.weight;
    }
    if (total <= 0) {
        return Intent::kIdle;
    }
    int roll = static_cast<int>(esp_random() % static_cast<uint32_t>(total));
    for (const auto& entry : table) {
        roll -= entry.weight;
        if (roll < 0) {
            return entry.intent;
        }
    }
    return Intent::kIdle;
}

}  // namespace

Intent Behavior::Tick(float dt, const Status& status) {
    if (dt <= 0.0f) {
        return Intent::kNone;
    }
    next_in_ -= dt;
    if (next_in_ > 0.0f) {
        return Intent::kNone;
    }

    // Restless when she wants something; quiet when she is asleep.
    float base = 18.0f;
    if (status.sleeping) {
        base = 45.0f;
    } else if (status.mood == Mood::kHungry || status.mood == Mood::kLonely) {
        base = 10.0f;
    } else if (status.mood == Mood::kHappy) {
        base = 12.0f;
    }
    next_in_ = base + static_cast<float>(esp_random() % 1000) / 1000.0f * base;
    return PickIntent(status);
}

void Behavior::NoteInteraction() {
    next_in_ = 12.0f + static_cast<float>(esp_random() % 1000) / 1000.0f * 10.0f;
}

}  // namespace kitty
