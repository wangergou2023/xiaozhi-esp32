#include "pet/pet.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <driver/usb_serial_jtag.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "display/display.h"
#include "pet/lamp.h"
#include "pet/pet_action.h"
#include "pet/pet_behavior.h"
#include "pet/pet_core.h"
#include "pet/pet_expression.h"

namespace kitty {
namespace {

constexpr const char* TAG = "Pet";
constexpr int kTickMs = 100;
constexpr float kUsbPollSeconds = 5.0f;

Lamp* g_lamp = nullptr;
QueueHandle_t g_gestures = nullptr;

Action g_action;
Behavior g_behavior;

// A short reaction that overrides the idle look for a moment.
struct Reaction {
    Look look{};
    float left = 0.0f;
} g_reaction;

QueueHandle_t GestureQueue() {
    if (g_gestures == nullptr) {
        g_gestures = xQueueCreate(8, sizeof(uint8_t));
    }
    return g_gestures;
}

// Docked means "on a cable and not taken over", which is what the manual calls
// 充电态. The USB host being attached is the only signal this board has.
bool UsbHostAttached() {
    return usb_serial_jtag_is_connected();
}

void React(const Look& look, float seconds) {
    g_reaction.look = look;
    g_reaction.left = seconds;
}

Look ReactionLook(Clip clip, int frame_ms, float seconds, const Status& status) {
    Look look = LookForStatus(status);
    look.clip = clip;
    look.frame_ms = frame_ms;
    look.frame = 0;
    React(look, seconds);
    return look;
}

void HandleGesture(Gesture gesture) {
    auto& core = PetCore::Get();
    const Status& status = core.status();

    switch (gesture) {
        case Gesture::kBootClick:
            core.Poke();
            ESP_LOGI(TAG, "Petted");
            if (!core.status().sleeping) {
                ReactionLook(Clip::kWave, 110, 1.2f, status);
            }
            break;
        case Gesture::kBootDoubleClick:
            core.Feed();
            ESP_LOGI(TAG, "Fed");
            if (!core.status().sleeping) {
                ReactionLook(Clip::kJump, 95, 1.4f, status);
            }
            break;
        case Gesture::kBootLongPress:
            // The SoftAP configuration page is V3 in the design; until then the
            // gesture is acknowledged with a look so it does not feel dead.
            ESP_LOGW(TAG, "Configuration mode is not implemented yet");
            ReactionLook(Clip::kReview, 200, 2.0f, status);
            break;
        case Gesture::kPowerClick: {
            // Clean or play, whichever is more urgent right now.
            if (status.clean <= status.affection) {
                core.Clean();
                ESP_LOGI(TAG, "Cleaned");
            } else {
                core.Play();
                ESP_LOGI(TAG, "Played");
            }
            if (!core.status().sleeping) {
                ReactionLook(Clip::kJump, 95, 1.0f, status);
            }
            break;
        }
        case Gesture::kPowerLongPress:
            ESP_LOGW(TAG, "Night mode is not implemented yet");
            ReactionLook(Clip::kFail, 220, 1.5f, status);
            break;
    }
    g_behavior.NoteInteraction();
}

void DrainGestures() {
    QueueHandle_t queue = GestureQueue();
    if (queue == nullptr) {
        return;
    }
    uint8_t raw = 0;
    while (xQueueReceive(queue, &raw, 0) == pdTRUE) {
        HandleGesture(static_cast<Gesture>(raw));
    }
}

void ApplyLook(const Look& look) {
    if (look.frame_ms == 0) {
        g_action.Freeze(look.clip, look.frame);
    } else {
        g_action.Play(look.clip, look.frame_ms);
    }
    g_action.SetBreathing(look.breath_px);
    g_action.SetScreenLevel(look.screen_level);
    g_action.SetColor(look.red, look.green, look.blue);
}

void Greet() {
    auto& core = PetCore::Get();
    const Status& status = core.status();
    ESP_LOGI(TAG, "Hello: %s, stage %s, mood %s", core.name().c_str(),
             ToName(status.stage), ToName(status.mood));
    ReactionLook(Clip::kWave, 110, 2.0f, status);
}

}  // namespace

void RegisterLamp(Lamp* lamp) {
    g_lamp = lamp;
}

void OnGesture(Gesture gesture) {
    QueueHandle_t queue = GestureQueue();
    if (queue == nullptr) {
        return;
    }
    const uint8_t raw = static_cast<uint8_t>(gesture);
    // Drop the gesture rather than block a button task.
    xQueueSend(queue, &raw, 0);
}

void Run() {
    auto& board = Board::GetInstance();
    auto* display = board.GetDisplay();
    if (display != nullptr) {
        display->SetupUI();
    }
    g_action.Attach(display, g_lamp, board.GetBacklight());

    auto& core = PetCore::Get();
    core.Load();
    Greet();

    int64_t last_us = esp_timer_get_time();
    float usb_poll_left = 0.0f;
    float alive_seconds = 0.0f;
    float heartbeat_left = 60.0f;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kTickMs));

        const int64_t now_us = esp_timer_get_time();
        const float dt = static_cast<float>(now_us - last_us) / 1000000.0f;
        last_us = now_us;

        DrainGestures();

        usb_poll_left -= dt;
        if (usb_poll_left <= 0.0f) {
            usb_poll_left = kUsbPollSeconds;
            const bool attached = UsbHostAttached();
            if (attached && core.status().mode == Mode::kAutonomous) {
                core.SetMode(Mode::kDocked);
            } else if (!attached && core.status().mode == Mode::kDocked) {
                core.SetMode(Mode::kAutonomous);
            }
        }

        core.Tick(dt);
        const Status& status = core.status();

        alive_seconds += dt;
        heartbeat_left -= dt;
        if (heartbeat_left <= 0.0f) {
            heartbeat_left = 60.0f;
            ESP_LOGI(TAG, "alive %.0fs | %s | h %d e %d c %d a %d | %s", alive_seconds,
                     ToName(status.mood), status.hunger, status.energy, status.clean,
                     status.affection, ToName(status.mode));
        }

        if (core.TakeChanged()) {
            ESP_LOGI(TAG, "%s | hunger %d energy %d clean %d affection %d | stage %s | mode %s%s",
                     ToName(status.mood), status.hunger, status.energy, status.clean,
                     status.affection, ToName(status.stage), ToName(status.mode),
                     status.sleeping ? " | asleep" : "");
        }

        if (g_reaction.left > 0.0f) {
            g_reaction.left -= dt;
        }

        Look look;
        if (g_reaction.left > 0.0f) {
            look = g_reaction.look;
        } else {
            const Intent intent = g_behavior.Tick(dt, status);
            if (intent != Intent::kNone) {
                ESP_LOGD(TAG, "herself: %s", ToName(intent));
                look = LookForIntent(intent, status);
            } else {
                look = LookForStatus(status);
            }
        }

        ApplyLook(look);
        g_action.Tick(dt);
    }
}

}  // namespace kitty
