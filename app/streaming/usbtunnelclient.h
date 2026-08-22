#pragma once

#include <cstdint>
#include <memory>

class UsbTunnelClient
{
public:
    using SendData = int(*)(uint32_t, const void*, uint16_t);
    using SendClose = int(*)(uint32_t, uint16_t);

    UsbTunnelClient(uint16_t targetPort = 3240,
                    uint16_t maxChunk = 16384,
                    SendData sendData = nullptr,
                    SendClose sendClose = nullptr);
    ~UsbTunnelClient();

    void open(uint32_t tunnelId);
    void write(uint32_t tunnelId, const void* data, uint16_t length);
    void close(uint32_t tunnelId, uint16_t reason);
    void closeAll();

private:
    struct Private;
    std::unique_ptr<Private> d;
};
