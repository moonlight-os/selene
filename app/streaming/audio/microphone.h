#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include <SDL.h>
#include <opus.h>

class MicrophoneCapturer
{
public:
    MicrophoneCapturer();
    ~MicrophoneCapturer();

    bool start();

private:
    void run();

    SDL_AudioDeviceID m_Device = 0;
    OpusEncoder* m_Encoder = nullptr;
    std::atomic<bool> m_Stopping{false};
    std::thread m_Worker;
    std::uint32_t m_Timestamp = 0;
};
