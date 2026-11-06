#include "pet/pet.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <driver/usb_serial_jtag.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <nvs.h>
#include <esp_timer.h>

#include "board.h"
#include "display/display.h"
#include "pet/lamp.h"
#include "pet/agent_link.h"
#include "pet/pet_action.h"
#include "pet/pet_audio.h"
#include "pet/pet_ears.h"
#include "pet/pet_gateway.h"
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

Behavior g_behavior;
bool g_night = false;

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

// Night is a schedule state: she sleeps through it, keeps the screen at a
// candle and plays the breathing bed instead of her voice (docs §10).
void ApplyNight(bool on, const char* why) {
    if (on == g_night) {
        return;
    }
    g_night = on;
    auto& core = PetCore::Get();
    core.SetNight(on);
    if (on) {
        core.ForceSleep();
        PetAudio::Get().SetNight(true);
        // The microphone goes off with the lights: nothing to hear at night,
        // and the radio-free part of the power budget matters most here.
        Ears::Get().Stop();
        ESP_LOGI(TAG, "Night mode on (%s)", why);
    } else {
        core.ForceWake();
        PetAudio::Get().SetNight(false);
        PetAudio::Get().Play(Voice::kHappy);
        Ears::Get().Start();
        ESP_LOGI(TAG, "Night mode off (%s)", why);
    }
}

void HandleGesture(Gesture gesture) {
    auto& core = PetCore::Get();
    const Status& status = core.status();
    // The host hears about every gesture, whether or not it owns the face.
    AgentLink::Get().OnGesture(gesture, status);

    // A touch in the middle of the night is how you wake her up.
    if (g_night) {
        ApplyNight(false, "touched");
        return;
    }

    switch (gesture) {
        case Gesture::kBootClick:
            core.Poke();
            PetAudio::Get().Play(Voice::kMeow);
            ESP_LOGI(TAG, "Petted");
            if (!core.status().sleeping) {
                ReactionLook(Clip::kWave, 110, 1.2f, status);
            }
            break;
        case Gesture::kBootDoubleClick:
            core.Feed();
            PetAudio::Get().Play(Voice::kChew);
            ESP_LOGI(TAG, "Fed");
            if (!core.status().sleeping) {
                ReactionLook(Clip::kJump, 95, 1.4f, status);
            }
            break;
        case Gesture::kBootLongPress:
            // Long press opens the configuration hotspot (docs/pet-design.md §11).
            if (Gateway::Get().active()) {
                Gateway::Get().Stop();
                Action::Get().HideBanner();
            } else if (Gateway::Get().Start()) {
                Action::Get().ShowBanner(Gateway::Get().ssid().c_str(), Gateway::Get().password());
                PetAudio::Get().Play(Voice::kHappy);
            } else {
                PetAudio::Get().Play(Voice::kSigh);
            }
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
        Action::Get().Freeze(look.clip, look.frame);
    } else {
        Action::Get().Play(look.clip, look.frame_ms);
    }
    Action::Get().SetBreathing(look.breath_px);
    Action::Get().SetScreenLevel(look.screen_level);
    Action::Get().SetColor(look.red, look.green, look.blue);
}

// Personality only changes how often she does something on her own.
float TempoFor(int personality) {
    switch (personality) {
        case 0: return 0.7f;   // clingy
        case 2: return 1.7f;   // cold
        case 3: return 0.5f;   // lively
        default: return 1.0f;
    }
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
    Action::Get().Attach(display, g_lamp, board.GetBacklight());
    PetAudio::Get().Start();
    Ears::Get().Start();
    AgentLink::Get().Start(&Action::Get());

    nvs_stats_t nvs_stats = {};
    if (nvs_get_stats(nullptr, &nvs_stats) == ESP_OK) {
        ESP_LOGI(TAG, "NVS: %u used, %u free of %u entries", (unsigned)nvs_stats.used_entries,
                 (unsigned)nvs_stats.free_entries, (unsigned)nvs_stats.total_entries);
    }

    ESP_LOGI(TAG, "Free memory: internal %u KB, PSRAM %u KB",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

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

        AgentLink::Get().Tick(dt);

        // The host may take over or drop out at any tick, so the mode follows
        // it directly instead of waiting for the slow USB poll.
        if (AgentLink::Get().remote()) {
            if (core.status().mode != Mode::kRemote) {
                core.SetMode(Mode::kRemote);
            }
        } else if (core.status().mode == Mode::kRemote) {
            core.SetMode(UsbHostAttached() ? Mode::kDocked : Mode::kAutonomous);
        }

        switch (Ears::Get().TakeEvent()) {
            case Ears::Event::kCalled: {
                // She heard her name: look up, chirp, tell the host.
                core.Poke();
                core.NoteCalled();
                PetAudio::Get().Play(Voice::kHappy);
                ReactionLook(Clip::kWave, 110, 1.6f, core.status());
                Action::Get().SetLook(0, -3);
                AgentLink::Get().OnCalled();
                break;
            }
            case Ears::Event::kStartled: {
                PetAudio::Get().Play(Voice::kSigh);
                ReactionLook(Clip::kReview, 160, 1.2f, core.status());
                break;
            }
            case Ears::Event::kNone:
            default:
                break;
        }

        Gateway::Get().Tick(dt);
        if (!Gateway::Get().active()) {
            Action::Get().HideBanner();
        }
        g_behavior.SetTempo(TempoFor(core.personality()));

        // The host may also send her to bed.
        if (core.night() != g_night) {
            ApplyNight(core.night(), "host");
        }

        usb_poll_left -= dt;
        if (usb_poll_left <= 0.0f) {
            usb_poll_left = kUsbPollSeconds;
            // With a clock the schedule runs itself; without one the night only
            // starts when somebody asks for it.
            if (core.HasClock() && core.InSleepWindow() != g_night) {
                ApplyNight(core.InSleepWindow(), "schedule");
            }
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
            AgentLink::Get().OnStatusChanged(status);
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

        Action::Get().SetLedPattern(AgentLink::Get().remote() ? Action::LedPattern::kFastBlink
                                                         : Action::LedPattern::kSteady);

        // While a host owns the face it also owns the look; she keeps breathing.
        if (!AgentLink::Get().OwnsFace()) {
            ApplyLook(look);
        }
        Action::Get().Tick(dt);
    }
}

}  // namespace kitty
