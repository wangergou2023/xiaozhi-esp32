#ifndef KITTY_PET_EARS_H_
#define KITTY_PET_EARS_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <esp_mn_iface.h>

class AudioCodec;

namespace kitty {

// Her ears. A microphone task that feeds MultiNet (a Chinese command-word model)
// so "小咪" makes her look up, plus a plain energy detector so a clap or a loud
// noise startles her. docs/pet-manual.md §7 lists both.
//
// This is the only part of the pet that listens, and it is off while she sleeps
// at night.
class Ears {
public:
    enum class Event : uint8_t {
        kNone,
        kCalled,    // she heard her name
        kStartled,  // loud noise
    };

    static Ears& Get();

    bool Start();
    void Stop();
    bool active() const { return running_; }
    // True when a command-word model came up; otherwise only loud noises work.
    bool knows_her_name() const { return model_data_ != nullptr; }

    Event TakeEvent();

    // Feed a block of 16 kHz mono PCM straight into the command model instead of
    // the microphone. Used for self-tests: it separates "the model did not
    // recognise it" from "the room did not deliver it".
    std::string Recognize(const int16_t* samples, size_t count);

    // Copy the most recent microphone audio out of the ring the ear task
    // maintains. No second reader, so nothing competes for the codec.
    bool Latest(size_t samples, std::vector<int16_t>& out);

    // Swap the command phrase at runtime (does not touch NVS). Used while
    // tuning, so a phrase can be tried without a rebuild.
    bool UsePhrase(const std::string& phrase);

    // The command phrase in pinyin, e.g. "xiao mi". Stored in NVS.
    std::string phrase();
    void SetPhrase(const std::string& phrase);

    // 0 low, 1 medium, 2 high. Sets the detection threshold; stored in NVS.
    int sensitivity();
    void SetSensitivity(int level);

private:
    Ears() = default;
    static void TaskEntry(void* arg);
    void Run();
    void OnHeard(Event event);

    void* codec_ = nullptr;
    void* task_ = nullptr;
    void* models_ = nullptr;  // srmodel_list_t*, loaded once
    const esp_mn_iface_t* model_ = nullptr;
    model_iface_data_t* model_data_ = nullptr;
    int chunk_samples_ = 512;
    std::string phrase_;

    std::mutex ring_mutex_;
    std::vector<int16_t> mic_ring_;
    size_t ring_write_ = 0;

    std::mutex model_mutex_;  // the task and Recognize must not detect at once
    std::mutex codec_mutex_;  // only one reader at a time (I2S RX is not shared)
    bool commands_ready_ = false;
    volatile bool running_ = false;
    volatile int event_ = 0;
};

}  // namespace kitty

#endif  // KITTY_PET_EARS_H_
