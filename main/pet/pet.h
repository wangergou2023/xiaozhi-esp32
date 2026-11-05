#ifndef KITTY_PET_H_
#define KITTY_PET_H_

#include <cstdint>

namespace kitty {

class Lamp;

// Every input the body understands. The board wires its buttons to these; the
// pet never reads a GPIO itself (docs/pet-design.md §8).
enum class Gesture : uint8_t {
    kBootClick,        // 摸摸头
    kBootDoubleClick,  // 喂食
    kBootLongPress,    // 配网
    kPowerClick,       // 清洁 / 陪玩，自动挑最急的一条
    kPowerLongPress,   // 夜间模式
};

// The board hands over its one RGB LED. Call this before Run().
void RegisterLamp(Lamp* lamp);

// Boot the pet and run its body loop. Never returns.
void Run();

// Queue a gesture. Safe from any task; the loop applies it on the next tick.
void OnGesture(Gesture gesture);

}  // namespace kitty

#endif  // KITTY_PET_H_
