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
    kBootLongPress,    // 配网热点开/关（配网中单击 = 关掉）
    // POWER is the power latch on this board, so it has no gestures. It cannot
    // be read by the firmware at all (docs/pet-design.md §8).
    // There is deliberately no triple click: the button library does not
    // distinguish three clicks from a double plus a single here, so a "triple"
    // gesture would fire as something else half the time.
};

// The board hands over its one RGB LED. Call this before Run().
void RegisterLamp(Lamp* lamp);

// Boot the pet and run its body loop. Never returns.
void Run();

// Queue a gesture. Safe from any task; the loop applies it on the next tick.
void OnGesture(Gesture gesture);

}  // namespace kitty

#endif  // KITTY_PET_H_
