# Spotpear ESP32-S3-0.85-AI

0.85" 128x128 IPS panel, ES8311 codec + NS4150B amplifier, single WS2812B, no touch.
Kitty uses this board as her body: the whole screen is her face.

Pin assignment comes from the vendor schematic (`ESP32-S3-0.85-AI.pdf`), see `config.h`.

## Display notes

- Panel controller is a **GC9107**; the ST7789 driver drives it correctly.
- The visible area starts at GRID `(2,1)`, so `DISPLAY_OFFSET_X/Y` are `2/1`. With
  `0/0` a column and a row of untouched GRAM show up as coloured stripes on the
  right and bottom edges.
- Rotation is done when the pet frames are generated (`gen_pet_frames.py` rotates
  them on the host). Panel-level `swap_xy` produced edge artefacts on this glass.
- `PetLcdDisplay` suppresses the built-in emoji/status bar/chat text: the pet owns
  the whole screen.

## Pet mode

`CONFIG_KITTY_PET_MODE` is on by default for this board. It skips the assistant
application entirely (`main.cc` calls `kitty::Run()` instead of
`Application::Initialize()`): no WiFi, no protocol, no activation, no OTA.

Everything about the pet lives in [`main/pet/`](../../../../pet) — this board only
supplies pins, the display, the LED and the buttons:

- `InitializeLcdDisplay()` → `PetLcdDisplay`, which hides the assistant UI;
- `StripLamp` adapts the single WS2812B to `kitty::Lamp`;
- the BOOT button forwards `kBootClick` / `kBootDoubleClick` / `kBootLongPress` to
  `kitty::OnGesture()`;
- nothing else. No behaviour belongs in this file.

POWER (GP2) is **not usable from firmware**: it is the power latch. Pressing it
cuts board power, pressing it again restores it, so the pin never reads as a button
(confirmed on hardware, 2026-11-06). Every gesture has to live on BOOT; see
`docs/pet-design.md` §8 for the single-button scheme.

The 57 frames live in `main/pet/assets/pet_frames.bin` and are embedded with
`EMBED_FILES`; see `docs/pet-design.md` for what the design does with them.
`scripts/gen_pet_frames.py` regenerates the blob from the codex-pet spritesheet.
The shipped frames are **mirrored** (`MIRROR_X = True`): the picture the user sees
is a reflection, so the frames are flipped to compensate.

## Console

`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`: the USB port is the only console. It has
to be, because the host agent talks to the pet over that same port
(`main/pet/agent_link.cc`) and the USB-Serial-JTAG driver only accepts writes from
the host when somebody reads them. With the default UART0 console the USB port is
just a secondary output: no driver is installed, the 64 byte RX FIFO fills up and
the host's `write()` blocks forever.

Logs and protocol lines share the stream. A host must ignore anything that does
not parse as JSON.
