#include "pet/pet_core.h"

#include <algorithm>
#include <cmath>

#include <esp_log.h>
#include <esp_timer.h>

#include "settings.h"

namespace kitty {
namespace {

constexpr const char* TAG = "PetCore";
constexpr const char* kNamespace = "kitty";

// Decay in points per hour, before the difficulty multiplier. These are first
// guesses; docs/pet-design.md §13 item 6 says they have to be tuned on hardware.
constexpr float kHungerPerHour = 4.0f;    // empty after ~25 h
constexpr float kEnergyPerHour = 8.0f;    // awake for ~12 h on a full charge
constexpr float kCleanPerHour = 2.0f;     // dirty after ~2 days
constexpr float kAffectionPerHour = 1.5f; // cold after ~2 days of neglect
constexpr float kEnergyRecoverPerHour = 70.0f;
constexpr float kDockedChargePerHour = 20.0f;  // on a cable she tops up slowly

// She does not notice you are gone for a while; then she does.
constexpr float kNeglectGrace = 20.0f * 60.0f;

// The host can keep her alive but not thriving: care given by the agent only
// counts half and stops working below 100.
constexpr float kAgentCareCap = 85.0f;
constexpr float kSaveInterval = 60.0f;

// Energy thresholds. Below kSleepAt she drops off by herself, below kWakeAt she
// cannot be woken by a poke (docs: "精力见底会强制睡着，叫不醒").
constexpr float kSleepAt = 3.0f;
constexpr float kWakeAt = 96.0f;
constexpr float kCannotWakeBelow = 30.0f;

// Storage: the numbers keep one decimal, so they are stored as fixed point.
int ToStored(float value) { return static_cast<int>(std::lround(value * 10.0f)); }
float FromStored(int value, float fallback) {
    return value < 0 ? fallback : static_cast<float>(value) / 10.0f;
}

}  // namespace

PetCore& PetCore::Get() {
    static PetCore instance;
    return instance;
}

void PetCore::Load() {
    if (loaded_) {
        return;
    }
    Settings settings(kNamespace, false);
    hunger_ = FromStored(settings.GetInt("hunger", -1), 70.0f);
    energy_ = FromStored(settings.GetInt("energy", -1), 70.0f);
    clean_ = FromStored(settings.GetInt("clean", -1), 80.0f);
    affection_ = FromStored(settings.GetInt("affection", -1), 20.0f);
    interactions_ = settings.GetInt("interactions", 0);
    difficulty_ = settings.GetInt("difficulty", 1);
    name_ = settings.GetString("name", "Kitty");
    asleep_ = settings.GetBool("asleep", false);
    tz_offset_minute_ = settings.GetInt("tz_offset", 8 * 60);
    personality_ = settings.GetInt("personality", 1);
    sleep_start_minute_ = settings.GetInt("sleep_start", 23 * 60);
    sleep_end_minute_ = settings.GetInt("sleep_end", 7 * 60);
    ClampAll();
    loaded_ = true;
    Refresh();
    ESP_LOGI(TAG, "Loaded %s: hunger=%.1f energy=%.1f clean=%.1f affection=%.1f interactions=%d%s",
             name_.c_str(), hunger_, energy_, clean_, affection_, interactions_,
             asleep_ ? " (asleep)" : "");
}

void PetCore::Save() {
    if (!loaded_) {
        return;
    }
    Settings settings(kNamespace, true);
    settings.SetInt("hunger", ToStored(hunger_));
    settings.SetInt("energy", ToStored(energy_));
    settings.SetInt("clean", ToStored(clean_));
    settings.SetInt("affection", ToStored(affection_));
    settings.SetInt("interactions", interactions_);
    settings.SetInt("difficulty", difficulty_);
    settings.SetString("name", name_);
    settings.SetBool("asleep", asleep_);
    settings.SetInt("tz_offset", tz_offset_minute_);
    settings.SetInt("personality", personality_);
    settings.SetInt("sleep_start", sleep_start_minute_);
    settings.SetInt("sleep_end", sleep_end_minute_);
    save_left_ = kSaveInterval;
}

void PetCore::Reset() {
    hunger_ = 70.0f;
    energy_ = 70.0f;
    clean_ = 80.0f;
    affection_ = 20.0f;
    interactions_ = 0;
    asleep_ = false;
    neglect_ = 0.0f;
    happy_left_ = 0.0f;
    mode_ = Mode::kAutonomous;
    ClampAll();
    loaded_ = true;
    Save();
    Refresh();
    ESP_LOGI(TAG, "Reset to a newborn");
}

void PetCore::SetMode(Mode mode) {
    if (mode_ == mode) {
        return;
    }
    ESP_LOGI(TAG, "Mode %s -> %s", ToName(mode_), ToName(mode));
    mode_ = mode;
    status_.mode = mode;
    changed_ = true;
}

void PetCore::Tick(float dt) {
    if (!loaded_) {
        Load();
    }
    if (dt <= 0.0f) {
        return;
    }

    const float rate = RateScale();
    if (asleep_) {
        energy_ += kEnergyRecoverPerHour * dt / 3600.0f;
        if (energy_ >= kWakeAt) {
            asleep_ = false;
            ESP_LOGI(TAG, "Woke up on her own (energy %.1f)", energy_);
        }
    } else if (mode_ == Mode::kDocked) {
        // Docked is the charging state: plugged in but nobody has taken over.
        energy_ += kDockedChargePerHour * dt / 3600.0f;
    } else {
        energy_ -= kEnergyPerHour * rate * dt / 3600.0f;
        if (energy_ <= kSleepAt) {
            asleep_ = true;
            ESP_LOGI(TAG, "Too tired, fell asleep");
        }
    }

    // Charging slows everything else down: she is dozing on the cable.
    const float decay = (mode_ == Mode::kDocked) ? rate * 0.75f : rate;
    hunger_ -= kHungerPerHour * decay * dt / 3600.0f;
    clean_ -= kCleanPerHour * decay * dt / 3600.0f;

    neglect_ += dt;
    if (neglect_ > kNeglectGrace && !asleep_) {
        affection_ -= kAffectionPerHour * decay * dt / 3600.0f;
    }

    if (happy_left_ > 0.0f) {
        happy_left_ = std::max(0.0f, happy_left_ - dt);
    }

    ClampAll();

    save_left_ -= dt;
    if (save_left_ <= 0.0f) {
        Save();
    }
    Refresh();
}

void PetCore::Poke() {
    if (!loaded_) {
        Load();
    }
    if (asleep_) {
        if (energy_ < kCannotWakeBelow) {
            ESP_LOGI(TAG, "Poked while exhausted: she will not wake (energy %.1f)", energy_);
            return;
        }
        ForceWake();
        return;
    }
    affection_ += 2.0f;
    interactions_++;
    happy_left_ = 6.0f;
    Touch();
    ClampAll();
    Refresh();
}

void PetCore::Feed(bool by_agent) {
    if (asleep_) {
        return;
    }
    const float gain = by_agent ? 12.0f : 25.0f;
    if (by_agent && hunger_ >= kAgentCareCap) {
        ESP_LOGI(TAG, "Agent fed her but she is already full enough");
        return;
    }
    hunger_ += gain;
    affection_ += by_agent ? 0.5f : 1.0f;
    interactions_++;
    happy_left_ = 5.0f;
    Touch();
    ClampAll();
    Refresh();
    ESP_LOGI(TAG, "Fed%s: hunger %.0f", by_agent ? " by agent" : "", hunger_);
}

void PetCore::Clean(bool by_agent) {
    if (by_agent && clean_ >= kAgentCareCap) {
        ESP_LOGI(TAG, "Agent cleaned her but she is already clean enough");
        return;
    }
    clean_ += by_agent ? 18.0f : 35.0f;
    affection_ += 1.0f;
    interactions_++;
    happy_left_ = 4.0f;
    Touch();
    ClampAll();
    Refresh();
    ESP_LOGI(TAG, "Cleaned: clean %.0f", clean_);
}

void PetCore::Play(bool by_agent) {
    if (asleep_) {
        return;
    }
    if (by_agent && affection_ >= kAgentCareCap) {
        ESP_LOGI(TAG, "Agent played with her but she wants a human");
        return;
    }
    affection_ += by_agent ? 1.0f : 3.0f;
    energy_ -= 2.0f;
    hunger_ -= 2.0f;
    interactions_++;
    happy_left_ = 8.0f;
    Touch();
    ClampAll();
    Refresh();
    ESP_LOGI(TAG, "Played: affection %.0f energy %.0f", affection_, energy_);
}

void PetCore::AgentTaskDone() {
    energy_ -= 5.0f;
    affection_ += 2.0f;
    happy_left_ = 4.0f;
    interactions_++;
    ClampAll();
    Refresh();
    ESP_LOGI(TAG, "Agent finished a task: energy %.0f affection %.0f", energy_, affection_);
}

void PetCore::AgentTaskFailed() {
    energy_ -= 8.0f;
    affection_ -= 1.0f;
    ClampAll();
    Refresh();
    ESP_LOGI(TAG, "Agent failed: energy %.0f affection %.0f", energy_, affection_);
}

void PetCore::ForceSleep() {
    asleep_ = true;
    happy_left_ = 0.0f;
    Refresh();
}

void PetCore::ForceWake() {
    asleep_ = false;
    neglect_ = 0.0f;
    Refresh();
    ESP_LOGI(TAG, "Awake");
}

int PetCore::difficulty() {
    if (!loaded_) {
        Load();
    }
    return difficulty_;
}

void PetCore::SetDifficulty(int difficulty) {
    difficulty_ = std::clamp(difficulty, 0, 2);
    Save();
}

std::string PetCore::name() {
    if (!loaded_) {
        Load();
    }
    return name_;
}

void PetCore::SetName(const std::string& name) {
    name_ = name.empty() ? "Kitty" : name;
    Save();
}

void PetCore::SetEpoch(int64_t epoch_seconds) {
    epoch_base_ = epoch_seconds;
    epoch_us_ = esp_timer_get_time();
    has_clock_ = true;
    const int minute = MinuteOfDay();
    ESP_LOGI(TAG, "Clock set to %02d:%02d", (minute / 60) % 24, minute % 60);
}

void PetCore::SetTimezone(int minutes_from_utc) {
    tz_offset_minute_ = std::clamp(minutes_from_utc, -14 * 60, 14 * 60);
    Save();
    ESP_LOGI(TAG, "Time zone UTC%+d:%02d", tz_offset_minute_ / 60,
             std::abs(tz_offset_minute_ % 60));
}

int PetCore::MinuteOfDay() const {
    if (!has_clock_) {
        return -1;
    }
    const int64_t elapsed = (esp_timer_get_time() - epoch_us_) / 1000000;
    const int64_t seconds = epoch_base_ + elapsed + tz_offset_minute_ * 60;
    int64_t minute_of_day = (seconds % 86400 + 86400) % 86400 / 60;
    return static_cast<int>(minute_of_day);
}

bool PetCore::InSleepWindow() const {
    const int minute = MinuteOfDay();
    if (minute < 0 || sleep_start_minute_ == sleep_end_minute_) {
        return false;
    }
    if (sleep_start_minute_ < sleep_end_minute_) {
        return minute >= sleep_start_minute_ && minute < sleep_end_minute_;
    }
    // A window that crosses midnight, like the default 23:00-07:00.
    return minute >= sleep_start_minute_ || minute < sleep_end_minute_;
}

void PetCore::SetNight(bool on) {
    if (night_ == on) {
        return;
    }
    night_ = on;
    status_.night = on;
    changed_ = true;
    ESP_LOGI(TAG, "Night mode %s", on ? "on" : "off");
}

int PetCore::personality() {
    if (!loaded_) {
        Load();
    }
    return personality_;
}

void PetCore::SetPersonality(int personality) {
    personality_ = std::clamp(personality, 0, 3);
    Save();
    ESP_LOGI(TAG, "Personality %d", personality_);
}

int PetCore::sleep_start_minute() {
    if (!loaded_) {
        Load();
    }
    return sleep_start_minute_;
}

int PetCore::sleep_end_minute() {
    if (!loaded_) {
        Load();
    }
    return sleep_end_minute_;
}

void PetCore::SetSleepWindow(int start_minute, int end_minute) {
    sleep_start_minute_ = std::clamp(start_minute, 0, 24 * 60 - 1);
    sleep_end_minute_ = std::clamp(end_minute, 0, 24 * 60 - 1);
    Save();
    ESP_LOGI(TAG, "Sleep window %02d:%02d - %02d:%02d", sleep_start_minute_ / 60,
             sleep_start_minute_ % 60, sleep_end_minute_ / 60, sleep_end_minute_ % 60);
}

bool PetCore::TakeChanged() {
    bool changed = changed_;
    changed_ = false;
    return changed;
}

float PetCore::RateScale() const {
    switch (difficulty_) {
        case 0: return 0.6f;
        case 2: return 1.6f;
        default: return 1.0f;
    }
}

Mood PetCore::ComputeMood() const {
    if (asleep_) {
        return Mood::kAsleep;
    }
    if (energy_ < 20.0f) {
        return Mood::kSleepy;
    }
    if (hunger_ < 25.0f) {
        return Mood::kHungry;
    }
    if (clean_ < 30.0f) {
        return Mood::kDirty;
    }
    if (affection_ < 20.0f) {
        return Mood::kLonely;
    }
    if (happy_left_ > 0.0f) {
        return Mood::kHappy;
    }
    if (energy_ < 40.0f) {
        return Mood::kTired;
    }
    return Mood::kContent;
}

Stage PetCore::ComputeStage() const {
    if (interactions_ >= 120 && affection_ >= 70.0f) {
        return Stage::kMature;
    }
    if (interactions_ >= 30 || affection_ >= 40.0f) {
        return Stage::kGrown;
    }
    return Stage::kBaby;
}

void PetCore::Refresh() {
    Mood mood = ComputeMood();
    Stage stage = ComputeStage();
    if (mood != status_.mood || stage != status_.stage || asleep_ != status_.sleeping ||
        night_ != status_.night) {
        changed_ = true;
    }
    status_.hunger = static_cast<int>(std::lround(hunger_));
    status_.energy = static_cast<int>(std::lround(energy_));
    status_.clean = static_cast<int>(std::lround(clean_));
    status_.affection = static_cast<int>(std::lround(affection_));
    status_.interactions = interactions_;
    status_.mode = mode_;
    status_.stage = stage;
    status_.mood = mood;
    status_.sleeping = asleep_;
    status_.night = night_;
}

void PetCore::ClampAll() {
    hunger_ = std::clamp(hunger_, 0.0f, 100.0f);
    energy_ = std::clamp(energy_, 0.0f, 100.0f);
    clean_ = std::clamp(clean_, 0.0f, 100.0f);
    affection_ = std::clamp(affection_, 0.0f, 100.0f);
}

void PetCore::Touch() {
    neglect_ = 0.0f;
}

}  // namespace kitty
