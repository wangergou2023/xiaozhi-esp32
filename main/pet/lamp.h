#ifndef KITTY_LAMP_H_
#define KITTY_LAMP_H_

#include <cstdint>

namespace kitty {

// The one RGB LED. The board owns the driver (WS2812 over RMT) and only exposes
// this narrow shape, so the pet never sees a board header.
class Lamp {
public:
    virtual ~Lamp() = default;
    // Raw 0-255 per channel. The caller already applied its own dimming.
    virtual void SetColor(uint8_t red, uint8_t green, uint8_t blue) = 0;
};

}  // namespace kitty

#endif  // KITTY_LAMP_H_
