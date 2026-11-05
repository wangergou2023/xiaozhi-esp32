#include "pet/pet_ears.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_mn_iface.h>
#include <model_path.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>

#include "assets.h"
#include "audio/audio_codec.h"
#include "board.h"
#include "settings.h"

namespace kitty {
namespace {

constexpr const char* TAG = "PetEars";
constexpr const char* kDefaultPhrase = "ni hao xiao mi";  // 你好小咪
constexpr int kSampleRate = 16000;
constexpr int kFallbackChunk = 512;          // 32 ms, when there is no model
constexpr float kLoudRms = 4200.0f;          // a clap lands well above this
constexpr float kLoudRefractory = 1.6f;      // seconds between two startles
constexpr float kNoiseFloorAlpha = 0.02f;    // slow average of the room
constexpr int kModelTimeoutMs = 6000;

// Reads index.json, finds the srmodels bundle and registers it. Returns false
// when the firmware was built without a command-word model.
srmodel_list_t* LoadModelBundle() {
    auto& assets = Assets::GetInstance();
    void* index_ptr = nullptr;
    size_t index_size = 0;
    if (!assets.GetAssetData("index.json", index_ptr, index_size)) {
        ESP_LOGW("PetEars", "No asset index: no command word model");
        return nullptr;
    }
    cJSON* root = cJSON_ParseWithLength(static_cast<char*>(index_ptr), index_size);
    if (root == nullptr) {
        ESP_LOGW("PetEars", "Broken index.json");
        return nullptr;
    }
    srmodel_list_t* models = nullptr;
    cJSON* srmodels = cJSON_GetObjectItem(root, "srmodels");
    if (cJSON_IsString(srmodels)) {
        void* model_ptr = nullptr;
        size_t model_size = 0;
        if (assets.GetAssetData(srmodels->valuestring, model_ptr, model_size)) {
            models = srmodel_load(model_ptr);
            ESP_LOGI("PetEars", "Loaded %d models from %s", models != nullptr ? models->num : -1,
                     srmodels->valuestring);
        }
    }
    cJSON_Delete(root);
    return models;
}

}  // namespace

Ears& Ears::Get() {
    static Ears instance;
    return instance;
}

std::string Ears::phrase() {
    Settings settings("pet", false);
    return settings.GetString("wake_phrase", kDefaultPhrase);
}

void Ears::SetPhrase(const std::string& phrase) {
    Settings settings("pet", true);
    settings.SetString("wake_phrase", phrase.empty() ? kDefaultPhrase : phrase);
    phrase_ = phrase.empty() ? kDefaultPhrase : phrase;
}

bool Ears::Start() {
    if (running_) {
        return true;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGW(TAG, "No codec: she is deaf");
        return false;
    }
    codec_ = codec;

    // MultiNet runs from the srmodels bundle in the assets partition, not from
    // .rodata: esp_mn_handle_from_name() without a loaded bundle sits in a
    // model-loading loop forever. The bundle is only packed when the variant
    // selects the Multinet wake word (see config.json "wake_word": "custom").
    srmodel_list_t* models = LoadModelBundle();
    if (models != nullptr) {
        char* mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, "cn");
        if (mn_name != nullptr) {
            const esp_mn_iface_t* iface = esp_mn_handle_from_name(mn_name);
            if (iface != nullptr) {
                model_ = iface;
                model_data_ = iface->create(mn_name, kModelTimeoutMs);
                ESP_LOGI(TAG, "Command model %s %s", mn_name,
                         model_data_ != nullptr ? "ready" : "failed to start");
            }
        } else {
            ESP_LOGW(TAG, "The bundle has no Chinese command model");
        }
    }

    if (model_data_ != nullptr) {
        const esp_mn_iface_t* iface = model_;
        chunk_samples_ = iface->get_samp_chunksize(model_data_);
        iface->set_det_threshold(model_data_, 0.15f);
        phrase_ = phrase();
        // The command list needs a root node before anything can be added; the
        // "clear" API silently refuses to work without it.
        esp_mn_commands_alloc(model_, model_data_);
        esp_mn_commands_clear();
        esp_mn_commands_add(1, phrase_.c_str());
        esp_mn_error_t* errors = esp_mn_commands_update();
        if (errors != nullptr && errors->num > 0) {
            ESP_LOGW(TAG, "%d command(s) rejected by the model", errors->num);
        }
        iface->print_active_speech_commands(model_data_);
        ESP_LOGI(TAG, "Listening for '%s' (%d samples per chunk)", phrase_.c_str(), chunk_samples_);
    } else {
        chunk_samples_ = kFallbackChunk;
        ESP_LOGW(TAG, "No command model: she will only notice loud noises");
    }

    codec->EnableInput(true);
    running_ = true;
    if (xTaskCreate(TaskEntry, "pet_ears", 4096, this, 5, (TaskHandle_t*)&task_) != pdPASS) {
        running_ = false;
        ESP_LOGE(TAG, "Could not start the ear task");
        return false;
    }
    return true;
}

void Ears::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    // Let the task notice and exit on its own; it blocks on the codec read for
    // at most one chunk.
    task_ = nullptr;
    if (codec_ != nullptr) {
        static_cast<AudioCodec*>(codec_)->EnableInput(false);
    }
    ESP_LOGI(TAG, "Stopped listening");
}

Ears::Event Ears::TakeEvent() {
    const int event = event_;
    event_ = 0;
    return static_cast<Event>(event);
}

void Ears::OnHeard(Event event) {
    if (event_ == 0) {
        event_ = static_cast<int>(event);
    }
}

void Ears::TaskEntry(void* arg) {
    static_cast<Ears*>(arg)->Run();
}

void Ears::Run() {
    auto* codec = static_cast<AudioCodec*>(codec_);
    const esp_mn_iface_t* iface = model_;
    std::vector<int16_t> buffer(chunk_samples_);
    // The command model was trained on speech at a normal level; the raw mic
    // here is quiet (rms ~700 when she talks an inch from it), so the samples
    // handed to the model are lifted.
    std::vector<int16_t> boosted(chunk_samples_);
    float refractory = 0.0f;
    float floor = 300.0f;  // running estimate of the quiet room
    float calibration_ = 0.0f;
    const float chunk_seconds = static_cast<float>(chunk_samples_) / kSampleRate;

    float heartbeat = 0.0f;
    while (running_) {
        if (!codec->InputData(buffer)) {
            ESP_LOGW(TAG, "No input samples");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Loud noise: still available when the command model is missing.
        double sum = 0.0;
        for (int16_t sample : buffer) {
            sum += static_cast<double>(sample) * sample;
        }
        const float rms = static_cast<float>(sqrt(sum / buffer.size()));
        floor += (rms - floor) * kNoiseFloorAlpha;
        const float threshold = kLoudRms > floor * 5.0f ? kLoudRms : floor * 5.0f;
        // Calibration aid: any audible sound is reported, quietly and rarely,
        // so the threshold can be tuned from the serial log.
        if (rms > 700.0f) {
            calibration_ -= chunk_seconds;
            if (calibration_ <= 0.0f) {
                calibration_ = 2.0f;
                ESP_LOGI(TAG, "Sound: rms %.0f (floor %.0f, trigger %.0f)", rms, floor, threshold);
            }
        }
        heartbeat -= chunk_seconds;
        if (heartbeat <= 0.0f) {
            heartbeat = 30.0f;
            ESP_LOGI(TAG, "ear: rms %.0f floor %.0f", rms, floor);
        }
        refractory -= chunk_seconds;
        if (rms > threshold && refractory <= 0.0f) {
            refractory = kLoudRefractory;
            ESP_LOGI(TAG, "Loud noise: rms %.0f (floor %.0f)", rms, floor);
            OnHeard(Event::kStartled);
        }

        if (model_data_ != nullptr) {
            constexpr int kModelGain = 3;
            for (size_t i = 0; i < buffer.size(); i++) {
                int32_t value = static_cast<int32_t>(buffer[i]) * kModelGain;
                if (value > 32767) {
                    value = 32767;
                } else if (value < -32768) {
                    value = -32768;
                }
                boosted[i] = static_cast<int16_t>(value);
            }
            const esp_mn_state_t state = iface->detect(model_data_, boosted.data());
            if (state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t* result = iface->get_results(model_data_);
                ESP_LOGI(TAG, "Heard her name: %s (prob %.2f)", result->string, result->prob[0]);
                OnHeard(Event::kCalled);
                iface->clean(model_data_);
            } else if (state == ESP_MN_STATE_TIMEOUT) {
                iface->clean(model_data_);
            }
        }
    }
    ESP_LOGI(TAG, "Ear task finished");
    vTaskDelete(nullptr);
}

}  // namespace kitty
