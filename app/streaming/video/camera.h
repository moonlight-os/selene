#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

class CameraCapturer
{
public:
    CameraCapturer();
    ~CameraCapturer();

    bool start();

private:
    struct Buffer {
        void* data = nullptr;
        std::size_t size = 0;
    };

    void run();

    int m_Fd = -1;
    std::uint16_t m_Width = 0;
    std::uint16_t m_Height = 0;
    std::vector<Buffer> m_Buffers;
    std::vector<std::uint8_t> m_SyntheticFrame;
    std::atomic<bool> m_Stopping{false};
    std::thread m_Worker;
    std::uint32_t m_FrameId = 0;
};
