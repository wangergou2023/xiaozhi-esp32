#ifndef KITTY_PET_CORE_H_
#define KITTY_PET_CORE_H_

#include <cstdint>
#include <string>

#include "pet/pet_types.h"

namespace kitty {

// The body. Four numbers that decay with time, a growth stage derived from care,
// and the save file. It knows nothing about the screen, the LED or the network.
// All times are real seconds; the caller decides how often Tick() runs.
class PetCore {
public:
    static PetCore& Get();

    // Restore from NVS. Safe to call twice; the second call is a no-op.
    void Load();
    // Force a save now. Normally the core saves itself every minute.
    void Save();
    // Factory reset: back to a newborn with default numbers.
    void Reset();

    // Advance the simulation. dt is in seconds.
    void Tick(float dt);

    const Status& status() const { return status_; }

    void SetMode(Mode mode);

    // Interactions (docs/pet-design.md §7). Each one is a gesture the human or
    // the agent can trigger; the numbers move the same way either way.
    // `by_agent` is the design's leash (§10.3): when the host does the caring it
    // only counts half, and it cannot top her up past a cap. Only a human can
    // bring her all the way back.
    void Poke();                              // 摸摸头
    void Feed(bool by_agent = false);         // 喂食
    void Clean(bool by_agent = false);        // 清洁
    void Play(bool by_agent = false);         // 陪玩
    void AgentTaskDone();   // agent 完成一次任务
    void AgentTaskFailed(); // agent 报错 / 被打断
    // She heard her name. Counted in NVS so a test does not depend on being
    // plugged in at the right moment.
    void NoteCalled();
    void ForceSleep();
    void ForceWake();

    // Time. The board has no RTC and no network, so the clock only exists once
    // a host hands it over (docs/pet-design.md §13 item 3). Until then every
    // schedule is disabled and the night can only be entered by hand.
    void SetEpoch(int64_t epoch_seconds);
    void SetTimezone(int minutes_from_utc);
    int timezone() const { return tz_offset_minute_; }
    bool HasClock() const { return has_clock_; }
    int MinuteOfDay() const;  // -1 without a clock
    bool InSleepWindow() const;

    // Night mode is a body state, not a mood: it outranks everything else.
    bool night() const { return night_; }
    void SetNight(bool on);

    // Configuration (docs/pet-design.md §11). Kept here so there is one owner
    // of the save file; the HTTP configuration page is only a front end.
    int difficulty();  // 0 easy, 1 normal, 2 hard
    void SetDifficulty(int difficulty);
    std::string name();
    void SetName(const std::string& name);
    int personality();  // 0 clingy, 1 normal, 2 cold, 3 lively
    void SetPersonality(int personality);
    int sleep_start_minute();
    int sleep_end_minute();
    void SetSleepWindow(int start_minute, int end_minute);

    // True when the mood or the stage changed since the last call. Used to tell
    // the agent about a change once instead of on every tick.
    bool TakeChanged();

private:
    PetCore() = default;
    PetCore(const PetCore&) = delete;
    PetCore& operator=(const PetCore&) = delete;

    void Refresh();
    void ClampAll();
    void Touch();  // a human interacted: reset the neglect clock
    Mood ComputeMood() const;
    Stage ComputeStage() const;
    float RateScale() const;

    float hunger_ = 70.0f;
    float energy_ = 70.0f;
    float clean_ = 80.0f;
    float affection_ = 20.0f;
    int interactions_ = 0;
    int difficulty_ = 1;
    std::string name_ = "Kitty";

    Mode mode_ = Mode::kAutonomous;
    bool asleep_ = false;
    float neglect_ = 0.0f;     // seconds since the last human interaction
    float happy_left_ = 0.0f;  // seconds of the "just been petted" glow
    float save_left_ = 60.0f;
    bool loaded_ = false;
    bool changed_ = true;

    bool has_clock_ = false;
    int64_t epoch_base_ = 0;
    int64_t epoch_us_ = 0;
    int calls_ = 0;
    int personality_ = 1;
    int tz_offset_minute_ = 8 * 60;  // UTC+8 until a host says otherwise
    int sleep_start_minute_ = 23 * 60;
    int sleep_end_minute_ = 7 * 60;
    bool night_ = false;

    Status status_{};
};

}  // namespace kitty

#endif  // KITTY_PET_CORE_H_
