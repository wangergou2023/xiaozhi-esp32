#ifndef KITTY_PET_EARS_H_
#define KITTY_PET_EARS_H_

#include <cstdint>
#include <string>

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

    // The command phrase in pinyin, e.g. "xiao mi". Stored in NVS.
    std::string phrase();
    void SetPhrase(const std::string& phrase);

private:
    Ears() = default;
    static void TaskEntry(void* arg);
    void Run();
    void OnHeard(Event event);

    void* codec_ = nullptr;
    void* task_ = nullptr;
    const esp_mn_iface_t* model_ = nullptr;
    model_iface_data_t* model_data_ = nullptr;
    int chunk_samples_ = 512;
    std::string phrase_;

    volatile bool running_ = false;
    volatile int event_ = 0;
};

}  // namespace kitty

#endif  // KITTY_PET_EARS_H_
