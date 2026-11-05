#include "pet/pet_audio.h"

#include <cmath>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_log.h>

#include "audio/audio_codec.h"
#include "board.h"
#include "settings.h"

// The night bed is a real purr (public domain, Wikimedia Commons
// File:Purring_cat.oga by Mysid), cut on whole purr cycles by make_purr.py.
extern "C" {
extern const uint8_t purr_pcm[] asm("_binary_purr_pcm_start");
extern const uint8_t purr_pcm_end[] asm("_binary_purr_pcm_end");
}

namespace kitty {
namespace {

constexpr const char* TAG = "PetAudio";
constexpr int kSamplesPerWrite = 320;  // 20 ms at 16 kHz
constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kBreathPeriod = 12.0f;  // 5 breaths per minute

int LengthOf(Voice voice, int sample_rate) {
    switch (voice) {
        case Voice::kMeow: return sample_rate * 35 / 100;
        case Voice::kHappy: return sample_rate * 30 / 100;
        case Voice::kYawn: return sample_rate * 90 / 100;
        case Voice::kPurr: return sample_rate * 12 / 10;
        case Voice::kChew: return sample_rate * 60 / 100;
        case Voice::kSigh: return sample_rate * 70 / 100;
        case Voice::kNone:
        default: return 0;
    }
}

// A tiny envelope: quick attack, smooth release, never clicks.
float Envelope(float progress) {
    if (progress <= 0.0f || progress >= 1.0f) {
        return 0.0f;
    }
    const float attack = 0.12f;
    if (progress < attack) {
        return progress / attack;
    }
    return sinf(kPi * (progress - attack) / (1.0f - attack) * 0.5f + kPi * 0.5f);
}

}  // namespace

PetAudio& PetAudio::Get() {
    static PetAudio instance;
    return instance;
}

void PetAudio::Start() {
    if (started_) {
        return;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGW(TAG, "No audio codec: she stays mute");
        return;
    }
    codec_ = codec;
    // A hot microphone: her own speaker is quiet at conversational gain, and
    // the command-word model wants a normal level. This has to be set before
    // the codec device is opened (which happens on the first EnableOutput).
    codec->SetInputGain(42);
    codec->Start();
    codec->EnableOutput(true);

    if (codec->output_sample_rate() > 0) {
        sample_rate_ = codec->output_sample_rate();
    }
    Settings settings("audio", false);
    volume_ = settings.GetInt("output_volume", 60);
    {
        Settings pet("pet", false);
        night_noise_ = pet.GetBool("night_noise", true);
    }
    if (volume_ > 0) {
        codec->SetOutputVolume(volume_);
    } else {
        volume_ = 60;
    }

    queue_ = xQueueCreate(4, sizeof(int));
    if (queue_ == nullptr) {
        ESP_LOGE(TAG, "No queue for voices");
        codec_ = nullptr;
        return;
    }
    if (xTaskCreate(TaskEntry, "pet_audio", 4096, this, 6, (TaskHandle_t*)&task_) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the audio task");
        vQueueDelete((QueueHandle_t)queue_);
        queue_ = nullptr;
        codec_ = nullptr;
        return;
    }
    started_ = true;
    ESP_LOGI(TAG, "Voice ready: %d Hz, volume %d", sample_rate_, volume_);
}

void PetAudio::TaskEntry(void* arg) {
    static_cast<PetAudio*>(arg)->Run();
}

void PetAudio::Run() {
    auto* codec = static_cast<AudioCodec*>(codec_);
    std::vector<int16_t> buffer(kSamplesPerWrite);
    int requested = 0;
    while (true) {
        while (xQueueReceive((QueueHandle_t)queue_, &requested, 0) == pdTRUE) {
            voice_.kind = static_cast<Voice>(requested);
            voice_.position = 0;
            voice_.length = LengthOf(voice_.kind, sample_rate_);
        }
        Fill(buffer.data(), static_cast<int>(buffer.size()));
        codec->OutputData(buffer);
    }
}

float PetAudio::purr_sample() {
    const size_t count = (purr_pcm_end - purr_pcm) / sizeof(int16_t);
    if (count == 0) {
        return 0.0f;
    }
    const auto* samples = reinterpret_cast<const int16_t*>(purr_pcm);
    const float value = static_cast<float>(samples[purr_position_]) / 32768.0f;
    purr_position_ = (purr_position_ + 1) % count;
    return value;
}

float PetAudio::NextNoise() {
    // xorshift32: cheap, good enough for hiss and clicks.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(static_cast<int32_t>(rng_)) / 2147483648.0f;
}

float PetAudio::VoiceSample() {
    if (voice_.kind == Voice::kNone || voice_.length <= 0) {
        return 0.0f;
    }
    if (voice_.position >= voice_.length) {
        voice_.kind = Voice::kNone;
        return 0.0f;
    }
    const float t = static_cast<float>(voice_.position) / static_cast<float>(sample_rate_);
    const float duration = static_cast<float>(voice_.length) / static_cast<float>(sample_rate_);
    const float progress = (duration > 0.0f) ? t / duration : 0.0f;
    voice_.position++;

    switch (voice_.kind) {
        case Voice::kMeow: {
            const float frequency = 470.0f + 330.0f * sinf(kPi * progress) + 25.0f * sinf(kTwoPi * 7.0f * t);
            // A single sine with a wobble reads as a short "mew".
            return sinf(kTwoPi * frequency * t) * Envelope(progress) * 0.55f;
        }
        case Voice::kHappy: {
            // Two rising chirps with a gap between them.
            float sample = 0.0f;
            if (t < 0.06f) {
                sample = sinf(kTwoPi * 900.0f * t) * Envelope(t / 0.06f) * 0.45f;
            } else if (t >= 0.12f && t < 0.19f) {
                sample = sinf(kTwoPi * 1250.0f * t) * Envelope((t - 0.12f) / 0.07f) * 0.45f;
            }
            return sample;
        }
        case Voice::kYawn: {
            const float frequency = 330.0f - 130.0f * progress;
            const float tone = sinf(kTwoPi * frequency * t) * 0.35f;
            const float breath = NextNoise() * 0.12f;
            return (tone + breath) * Envelope(progress);
        }
        case Voice::kPurr: {
            const float rumble = sinf(kTwoPi * 55.0f * t);
            const float pulse = 0.5f + 0.5f * sinf(kTwoPi * 24.0f * t);
            return rumble * pulse * Envelope(progress) * 0.30f;
        }
        case Voice::kChew: {
            // Three soft, filtered clicks.
            const float window = fmodf(t, 0.20f);
            if (window > 0.09f) {
                return 0.0f;
            }
            const float envelope = Envelope(window / 0.09f);
            return NextNoise() * envelope * 0.22f;
        }
        case Voice::kSigh: {
            const float frequency = 420.0f - 200.0f * progress;
            return sinf(kTwoPi * frequency * t) * Envelope(progress) * 0.30f;
        }
        case Voice::kNone:
        default:
            return 0.0f;
    }
}

void PetAudio::Fill(int16_t* out, int samples) {
    const float gain = (static_cast<float>(volume_) / 100.0f) * 0.6f;
    for (int i = 0; i < samples; i++) {
        float mixed = 0.0f;

        if (night_ && night_noise_) {
            // Night bed: her own purr, swelling and fading with slow breathing.
            breath_phase_ += 1.0f / static_cast<float>(sample_rate_);
            if (breath_phase_ >= kBreathPeriod) {
                breath_phase_ -= kBreathPeriod;
            }
            const float swell = 0.55f + 0.45f * (0.5f + 0.5f * sinf(kTwoPi * breath_phase_ / kBreathPeriod - kPi * 0.5f));
            mixed += purr_sample() * 0.42f * swell;
        }

        // Host-pushed PCM plays on top of everything else.
        {
            std::lock_guard<std::mutex> lock(pcm_mutex_);
            while (!pcm_queue_.empty() && pcm_offset_ >= pcm_queue_.front().size()) {
                pcm_queue_.pop_front();
                pcm_offset_ = 0;
            }
            if (!pcm_queue_.empty()) {
                mixed += static_cast<float>(pcm_queue_.front()[pcm_offset_++]) / 32768.0f;
            }
        }

        mixed += VoiceSample();

        if (mixed > 1.0f) {
            mixed = 1.0f;
        } else if (mixed < -1.0f) {
            mixed = -1.0f;
        }
        out[i] = static_cast<int16_t>(mixed * gain * 32767.0f);
    }
}

void PetAudio::Play(Voice voice) {
    if (!started_ || queue_ == nullptr || voice == Voice::kNone) {
        return;
    }
    const int requested = static_cast<int>(voice);
    xQueueSend((QueueHandle_t)queue_, &requested, 0);
}

void PetAudio::PlayPcm(std::vector<int16_t>&& samples) {
    if (!started_ || samples.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    constexpr size_t kMaxQueued = 6;
    if (pcm_queue_.size() >= kMaxQueued) {
        pcm_queue_.pop_front();
    }
    pcm_queue_.push_back(std::move(samples));
}

void PetAudio::SetNightNoise(bool noise) {
    night_noise_ = noise;
    Settings settings("pet", true);
    settings.SetBool("night_noise", noise);
}

void PetAudio::SetNight(bool on) {
    if (night_ == on) {
        return;
    }
    night_ = on;
    if (!on) {
        // Start the purr from its beginning next time.
        purr_position_ = 0;
    }
}

void PetAudio::SetVolume(int volume) {
    volume_ = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    if (codec_ != nullptr) {
        static_cast<AudioCodec*>(codec_)->SetOutputVolume(volume_);
    }
}

}  // namespace kitty
