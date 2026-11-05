#ifndef KITTY_PET_AUDIO_H_
#define KITTY_PET_AUDIO_H_

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace kitty {

// The few noises she can make without a voice model. Everything is generated
// on the fly: there is no audio file in the firmware (docs/pet-design.md §10).
enum class Voice : uint8_t {
    kNone = 0,
    kMeow,    // petted
    kHappy,   // two quick chirps
    kYawn,    // tired
    kPurr,    // content
    kChew,    // fed
    kSigh,    // annoyed / bored
};

// A tiny output-only audio path: the board codec, one task, a synthesizer.
// It deliberately does not start AudioService: no microphone, no AEC, no
// wake word, so nothing runs when she is quiet.
class PetAudio {
public:
    static PetAudio& Get();

    // Grabs the board codec and starts the playback task. Safe to call twice.
    void Start();
    bool ready() const { return codec_ != nullptr; }

    // Interrupt whatever she is saying with a new noise.
    void Play(Voice voice);

    // Push 16 kHz mono PCM for her to say. Used by the host (pet.say) and by
    // anything else that has real audio to give her.
    void PlayPcm(std::vector<int16_t>&& samples);

    // Night mode swaps the voice for an algorithmic bed: slow breathing hiss.
    // `noise` can be turned off so the night is silent except for her breathing.
    void SetNight(bool on);
    void SetNightNoise(bool noise);

    // 0-100, also written to NVS so it survives a reboot.
    void SetVolume(int volume);
    int volume() const { return volume_; }
    bool night_noise() const { return night_noise_; }

private:
    PetAudio() = default;
    static void TaskEntry(void* arg);
    void Run();
    void Fill(int16_t* out, int samples);
    float VoiceSample();
    float NextNoise();

    struct VoiceState {
        Voice kind = Voice::kNone;
        int position = 0;  // samples played
        int length = 0;    // samples total
    };

    void* codec_ = nullptr;   // AudioCodec*, kept opaque in the header
    void* queue_ = nullptr;   // QueueHandle_t
    void* task_ = nullptr;    // TaskHandle_t

    VoiceState voice_{};
    std::mutex pcm_mutex_;
    std::deque<std::vector<int16_t>> pcm_queue_;
    size_t pcm_offset_ = 0;
    int sample_rate_ = 16000;
    int volume_ = 60;

    volatile bool night_ = false;
    volatile bool night_noise_ = true;
    bool started_ = false;

    uint32_t rng_ = 0x9E3779B9u;
    float low_pass_ = 0.0f;
    float breath_phase_ = 0.0f;
};

}  // namespace kitty

#endif  // KITTY_PET_AUDIO_H_
