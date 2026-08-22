#include "usbtunnelclient.h"

#include <Limelight.h>

#include <QByteArray>
#include <QHostAddress>
#include <QTcpSocket>

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr qsizetype MaxQueuedBytes = 1024 * 1024;
}

struct UsbTunnelClient::Private
{
    struct Tunnel {
        uint32_t id = 0;
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<QByteArray> pending;
        qsizetype queuedBytes = 0;
        bool closing = false;
        bool finished = false;
        bool notifyPeer = true;
        uint16_t closeReason = ML_USB_TUNNEL_CLOSE_NORMAL;
        uint16_t targetPort = 0;
        uint16_t maxChunk = 0;
        UsbTunnelClient::SendData sendData = nullptr;
        UsbTunnelClient::SendClose sendClose = nullptr;
        std::thread worker;
    };

    std::mutex mutex;
    std::map<uint32_t, std::shared_ptr<Tunnel>> tunnels;
    uint16_t targetPort;
    uint16_t maxChunk;
    UsbTunnelClient::SendData sendData;
    UsbTunnelClient::SendClose sendClose;

    Private(uint16_t port, uint16_t chunk, UsbTunnelClient::SendData data,
            UsbTunnelClient::SendClose close)
        : targetPort(port), maxChunk(chunk), sendData(data), sendClose(close) {}

    static void run(const std::shared_ptr<Tunnel>& tunnel)
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, tunnel->targetPort);
        if (!socket.waitForConnected(1000)) {
            tunnel->sendClose(tunnel->id, ML_USB_TUNNEL_CLOSE_CONNECT_FAILED);
            std::lock_guard<std::mutex> lock(tunnel->mutex);
            tunnel->finished = true;
            return;
        }

        for (;;) {
            std::deque<QByteArray> outgoing;
            {
                std::unique_lock<std::mutex> lock(tunnel->mutex);
                if (tunnel->pending.empty() && !tunnel->closing) {
                    tunnel->wake.wait_for(lock, std::chrono::milliseconds(10));
                }
                outgoing.swap(tunnel->pending);
                tunnel->queuedBytes = 0;
                if (tunnel->closing && outgoing.empty()) {
                    break;
                }
            }

            bool ioFailed = false;
            for (const auto& chunk : outgoing) {
                qsizetype written = 0;
                while (written < chunk.size()) {
                    auto count = socket.write(chunk.constData() + written, chunk.size() - written);
                    if (count <= 0 || (!socket.waitForBytesWritten(1000) && socket.bytesToWrite() != 0)) {
                        ioFailed = true;
                        break;
                    }
                    written += count;
                }
                if (ioFailed) {
                    break;
                }
            }

            if (ioFailed) {
                std::lock_guard<std::mutex> lock(tunnel->mutex);
                tunnel->closeReason = ML_USB_TUNNEL_CLOSE_IO_ERROR;
                break;
            }

            socket.waitForReadyRead(10);
            while (socket.bytesAvailable() > 0) {
                auto chunk = socket.read(tunnel->maxChunk);
                if (chunk.isEmpty() || tunnel->sendData(tunnel->id,
                                                        chunk.constData(),
                                                        (uint16_t)chunk.size()) != 0) {
                    std::lock_guard<std::mutex> lock(tunnel->mutex);
                    tunnel->closeReason = ML_USB_TUNNEL_CLOSE_IO_ERROR;
                    ioFailed = true;
                    break;
                }
            }
            if (ioFailed || socket.state() == QAbstractSocket::UnconnectedState) {
                break;
            }
        }

        socket.abort();
        std::lock_guard<std::mutex> lock(tunnel->mutex);
        tunnel->finished = true;
        if (tunnel->notifyPeer) {
            tunnel->sendClose(tunnel->id, tunnel->closeReason);
        }
    }
};

UsbTunnelClient::UsbTunnelClient(uint16_t targetPort, uint16_t maxChunk,
                                 SendData sendData, SendClose sendClose)
    : d(std::make_unique<Private>(targetPort, maxChunk,
          sendData != nullptr ? sendData : LiSendUsbTunnelData,
          sendClose != nullptr ? sendClose : LiSendUsbTunnelClose)) {}

UsbTunnelClient::~UsbTunnelClient()
{
    closeAll();
}

void UsbTunnelClient::open(uint32_t tunnelId)
{
    if (tunnelId == 0) {
        d->sendClose(tunnelId, ML_USB_TUNNEL_CLOSE_PROTOCOL_ERROR);
        return;
    }

    std::vector<std::shared_ptr<Private::Tunnel>> finished;
    auto tunnel = std::make_shared<Private::Tunnel>();
    tunnel->id = tunnelId;
    tunnel->targetPort = d->targetPort;
    tunnel->maxChunk = d->maxChunk;
    tunnel->sendData = d->sendData;
    tunnel->sendClose = d->sendClose;
    bool rejected = false;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        for (auto it = d->tunnels.begin(); it != d->tunnels.end();) {
            std::lock_guard<std::mutex> tunnelLock(it->second->mutex);
            if (it->second->finished) {
                finished.push_back(it->second);
                it = d->tunnels.erase(it);
            } else {
                ++it;
            }
        }
        if (d->tunnels.count(tunnelId) != 0) {
            rejected = true;
        } else if (d->tunnels.size() >= 16) {
            rejected = true;
        } else {
            d->tunnels.emplace(tunnelId, tunnel);
        }
    }
    for (const auto& old : finished) {
        if (old->worker.joinable()) {
            old->worker.join();
        }
    }
    if (rejected) {
        d->sendClose(tunnelId, ML_USB_TUNNEL_CLOSE_PROTOCOL_ERROR);
        return;
    }
    tunnel->worker = std::thread(&Private::run, tunnel);
}

void UsbTunnelClient::write(uint32_t tunnelId, const void* data, uint16_t length)
{
    std::shared_ptr<Private::Tunnel> tunnel;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto it = d->tunnels.find(tunnelId);
        if (it == d->tunnels.end()) {
            d->sendClose(tunnelId, ML_USB_TUNNEL_CLOSE_PROTOCOL_ERROR);
            return;
        }
        tunnel = it->second;
    }

    std::lock_guard<std::mutex> lock(tunnel->mutex);
    if (tunnel->closing) {
        return;
    }
    if (length == 0 || length > tunnel->maxChunk ||
            tunnel->queuedBytes + length > MaxQueuedBytes) {
        tunnel->closing = true;
        tunnel->closeReason = ML_USB_TUNNEL_CLOSE_PROTOCOL_ERROR;
        tunnel->wake.notify_one();
        return;
    }
    tunnel->pending.emplace_back(static_cast<const char*>(data), length);
    tunnel->queuedBytes += length;
    tunnel->wake.notify_one();
}

void UsbTunnelClient::close(uint32_t tunnelId, uint16_t)
{
    std::shared_ptr<Private::Tunnel> tunnel;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto it = d->tunnels.find(tunnelId);
        if (it == d->tunnels.end()) {
            return;
        }
        tunnel = it->second;
    }
    std::lock_guard<std::mutex> lock(tunnel->mutex);
    tunnel->notifyPeer = false;
    tunnel->closing = true;
    tunnel->pending.clear();
    tunnel->queuedBytes = 0;
    tunnel->wake.notify_one();
}

void UsbTunnelClient::closeAll()
{
    std::vector<std::shared_ptr<Private::Tunnel>> tunnels;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        for (const auto& item : d->tunnels) {
            tunnels.push_back(item.second);
        }
        d->tunnels.clear();
    }
    for (const auto& tunnel : tunnels) {
        {
            std::lock_guard<std::mutex> lock(tunnel->mutex);
            tunnel->notifyPeer = false;
            tunnel->closing = true;
            tunnel->pending.clear();
            tunnel->queuedBytes = 0;
            tunnel->wake.notify_one();
        }
        if (tunnel->worker.joinable()) {
            tunnel->worker.join();
        }
    }
}
