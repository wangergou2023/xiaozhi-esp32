#ifndef _GLM_REALTIME_H_
#define _GLM_REALTIME_H_

#include <memory>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class WebSocket;

/**
 * Minimal client for Zhipu GLM-Realtime (wss://open.bigmodel.cn/api/paas/v4/realtime).
 *
 * Push-to-talk: one call to RunTurn() records a phrase, streams it as 16 kHz
 * PCM16, and plays the 24 kHz PCM answer through the normal AudioService
 * playback queue (raw PCM, no Opus round trip).
 */
class GlmRealtime {
public:
    GlmRealtime();
    ~GlmRealtime();

    bool IsConfigured() const { return !api_key_.empty(); }
    bool IsBusy() const { return busy_; }

    /** Record one phrase and play the answer. Blocking; runs its own task. */
    bool StartTurn();

private:
    std::string api_key_;
    std::shared_ptr<WebSocket> websocket_;
    TaskHandle_t task_ = nullptr;
    volatile bool busy_ = false;
    volatile bool stop_requested_ = false;

    bool Connect();
    void Close();
    void TurnTask();
    static void TurnTaskEntry(void* arg);
};

#endif  // _GLM_REALTIME_H_
