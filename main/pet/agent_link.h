#ifndef KITTY_AGENT_LINK_H_
#define KITTY_AGENT_LINK_H_

#include <deque>
#include <mutex>
#include <string>

#include <cJSON.h>

#include "pet/pet.h"
#include "pet/pet_types.h"

namespace kitty {

class Action;

// The second brain: a line-oriented JSON-RPC 2.0 channel over the USB serial
// port (docs/pet-design.md §9). A host that says `pet.hello` takes the body
// over; three seconds without a line and she goes back to her own life.
//
// The port is shared with the log output, so the host has to ignore any line
// that is not JSON. That is the price of using the cable that is already there.
class AgentLink {
public:
    static AgentLink& Get();

    void Start(Action* action);

    // Called from the body loop: reads the port, dispatches, enforces the
    // heartbeat. dt is in seconds.
    void Tick(float dt);

    // True while a host owns the face. The body loop leaves the face alone.
    bool OwnsFace() const;
    // True between pet.hello and the heartbeat timeout.
    bool remote() const { return remote_; }

    // The host just said hello (consumed once).
    bool TakeHello();

    // Report a human interaction and a change of her numbers to the host.
    void OnGesture(Gesture gesture, const Status& status);
    // She heard her name through the microphone.
    void OnCalled();
    void OnStatusChanged(const Status& status);

private:
    AgentLink() = default;

    void HandleLine(char* line);
    // The port is drained by its own task; complete lines wait here for the body
    // loop, so every handler still runs on one thread.
    static void ReaderEntry(void* arg);
    void ReadLoop();
    void Dispatch(char* line);
    void SendEvent(const char* method, cJSON* params);
    void SendDocument(cJSON* document);
    void ApplyFace(const char* emotion, float intensity);
    void ExitRemote(const char* reason);

    Action* action_ = nullptr;
    bool remote_ = false;
    bool hello_pending_ = false;
    bool tired_reported_ = false;

    float since_rx_ = 0.0f;    // seconds since the last line from the host
    float face_hold_ = 0.0f;   // seconds the host's face stays on screen

    // Big enough for one pet.say line (base64 PCM); allocated in PSRAM.
    char* rx_ = nullptr;
    size_t rx_capacity_ = 0;
    size_t rx_len_ = 0;

    // pet.say is streamed in chunks; they are collected here and handed to the
    // audio path in one piece when the host marks the last one. Playing each
    // chunk as it arrives stutters, and stuttered speech is useless to both the
    // speaker and her ears.
    int16_t* say_buffer_ = nullptr;
    size_t say_capacity_ = 0;
    size_t say_length_ = 0;

    std::mutex lines_mutex_;
    std::deque<std::string> lines_;
    void* reader_ = nullptr;  // TaskHandle_t

    int look_dx_ = 0;
    int look_dy_ = 0;
};

}  // namespace kitty

#endif  // KITTY_AGENT_LINK_H_
