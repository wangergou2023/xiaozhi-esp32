#ifndef KITTY_PET_BEHAVIOR_H_
#define KITTY_PET_BEHAVIOR_H_

#include <cstdint>

#include "pet/pet_types.h"

namespace kitty {

// A self-generated little action. No language model, no network: the weights
// below are the whole "personality" of docs/pet-design.md §10.
enum class Intent : uint8_t {
    kNone,
    kIdle,       // stand there
    kLookAround, // 张望
    kYawn,       // 打哈欠
    kStretch,    // 伸懒腰
    kHop,        // 突然高兴一下
    kDoze,       // 打瞌睡
};

const char* ToName(Intent intent);

// Picks a new intent every so often, weighted by how she feels.
class Behavior {
public:
    // Returns kNone most of the time. dt is in seconds.
    Intent Tick(float dt, const Status& status);
    // A human or the agent just interacted: she is busy, postpone self-actions.
    void NoteInteraction();
    // Personality stretches or shrinks the time between self-actions.
    void SetTempo(float tempo) { tempo_ = tempo < 0.2f ? 0.2f : tempo; }
    void SetDozeWeight(int weight);

private:
    float next_in_ = 6.0f;
    float tempo_ = 1.0f;
};

}  // namespace kitty

#endif  // KITTY_PET_BEHAVIOR_H_
