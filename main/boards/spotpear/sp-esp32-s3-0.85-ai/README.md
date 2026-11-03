# Spotpear ESP32-S3-0.85-AI

0.85" 128x128 IPS panel, ES8311 codec + NS4150B amplifier, single WS2812B, no touch.

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

## Pet animation

`pet_frames.bin` holds 57 RGB565 frames (9 animation rows x 6/8/8/4/5/8/6/6/6),
88x128 each, embedded through `EMBED_FILES`. Animation row is chosen from the
server emotion first (4 s window) and the device state second.

## GLM Realtime (optional)

`glm_realtime.cc` talks to `wss://open.bigmodel.cn/api/paas/v4/realtime` with
PCM16 16 kHz up / PCM 24 kHz down, so no Opus round trip is involved.

- One BOOT click in the idle state records one phrase and plays the answer.
- The API key is read from NVS (`Settings("glm")`, key `api_key`) and falls back
  to `GLM_API_KEY` from a local `glm_key.h` that is **not** committed.
- The audio service is stopped for the duration of a turn: two tasks reading the
  same I2S channel makes the driver abort.

### Local patch to a managed component

Zhipu sends single audio frames well above 256 KB (observed 411 KB), which the
bundled websocket client rejects. `managed_components/78__esp-ml307/include/web_socket.h`
has `MAX_MESSAGE_SIZE` raised from 256 KB to 1 MB. That directory is not tracked
by git, so the patch is lost if the component is reinstalled.
