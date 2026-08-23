#pragma once

#include <QByteArray>
#include <QString>

#include <memory>

struct QuicSessionInfo
{
    QString host;
    quint16 port = 0;
    QByteArray token;

    bool isValid() const { return !host.isEmpty() && port != 0 && token.size() == 32; }
};

class QuicTransport
{
public:
    struct ProxyPorts {
        quint16 rtsp = 0;
        quint16 video = 0;
        quint16 control = 0;
        quint16 audio = 0;
        quint16 microphone = 0;
        quint16 camera = 0;
    };

    QuicTransport();
    ~QuicTransport();
    QuicTransport(const QuicTransport&) = delete;
    QuicTransport& operator=(const QuicTransport&) = delete;

    static bool isAvailable();
    static QuicSessionInfo parseSessionUrl(const QString& sessionUrl);
    bool start(const QuicSessionInfo& session, const QByteArray& pinnedServerCertificate,
               const QByteArray& clientCertificate, const QByteArray& clientPrivateKey,
               ProxyPorts& ports);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
