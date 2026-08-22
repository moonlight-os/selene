#include "quictransport.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QSslCertificate>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryFile>
#include <QUdpSocket>
#include <QUrl>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifdef HAVE_MSQUIC
#include "MlosQuicWire.h"
#include "msquic.h"
#endif

bool QuicTransport::isAvailable()
{
#ifdef HAVE_MSQUIC
    return true;
#else
    return false;
#endif
}

QuicSessionInfo QuicTransport::parseSessionUrl(const QString& sessionUrl)
{
    const QUrl url(sessionUrl, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QStringLiteral("quic") ||
            url.host().isEmpty() || url.port() <= 0 || url.port() > 65535 ||
            !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
        return {};
    }

    const QString path = url.path();
    if (path.size() != 65 || path.at(0) != QLatin1Char('/')) return {};
    const QByteArray encoded = path.mid(1).toLatin1();
    for (const char c : encoded) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
    }
    const QByteArray token = QByteArray::fromHex(encoded);
    if (token.size() != 32 || token.toHex() != encoded) return {};
    return {url.host(), static_cast<quint16>(url.port()), token};
}

struct QuicTransport::Impl
{
#ifdef HAVE_MSQUIC
    struct SendContext {
        QUIC_BUFFER buffer;
        QByteArray bytes;
        explicit SendContext(QByteArray data) : bytes(std::move(data)) {
            buffer.Length = static_cast<uint32_t>(bytes.size());
            buffer.Buffer = reinterpret_cast<uint8_t*>(bytes.data());
        }
    };

    struct UdpBridge {
        Impl* owner;
        quint8 channel;
        std::thread worker;
        std::atomic_bool stopping {false};
        std::mutex mutex;
        std::condition_variable boundChanged;
        quint16 boundPort = 0;
        bool bindFailed = false;
        std::deque<QByteArray> inbound;

        UdpBridge(Impl* owner_, quint8 channel_) : owner(owner_), channel(channel_) {}
        ~UdpBridge() { stop(); }

        bool start() {
            worker = std::thread([this] { run(); });
            std::unique_lock lock(mutex);
            boundChanged.wait(lock, [this] { return boundPort != 0 || bindFailed; });
            return boundPort != 0;
        }

        void run() {
            QUdpSocket socket;
            if (!socket.bind(QHostAddress::LocalHost, 0)) {
                std::lock_guard lock(mutex);
                bindFailed = true;
                boundChanged.notify_all();
                return;
            }
            {
                std::lock_guard lock(mutex);
                boundPort = socket.localPort();
                boundChanged.notify_all();
            }
            QHostAddress peerAddress;
            quint16 peerPort = 0;
            while (!stopping) {
                if (socket.waitForReadyRead(5)) {
                    while (socket.hasPendingDatagrams()) {
                        QByteArray packet(static_cast<int>(socket.pendingDatagramSize()), Qt::Uninitialized);
                        if (socket.readDatagram(packet.data(), packet.size(), &peerAddress, &peerPort) == packet.size()) {
                            if (!owner->sendDatagram(channel, packet)) owner->fail();
                        }
                    }
                }
                std::deque<QByteArray> queued;
                {
                    std::lock_guard lock(mutex);
                    queued.swap(inbound);
                }
                if (!peerAddress.isNull() && peerPort != 0) {
                    for (const auto& packet : queued) {
                        if (socket.writeDatagram(packet, peerAddress, peerPort) != packet.size()) owner->fail();
                    }
                }
            }
        }

        void deliver(QByteArray packet) {
            std::lock_guard lock(mutex);
            if (inbound.size() >= 256) inbound.pop_front();
            inbound.push_back(std::move(packet));
        }

        void stop() {
            if (stopping.exchange(true)) return;
            if (worker.joinable()) worker.join();
        }
    };

    const QUIC_API_TABLE* api = nullptr;
    HQUIC registration = nullptr;
    HQUIC configuration = nullptr;
    HQUIC connection = nullptr;
    HQUIC authStream = nullptr;
    HQUIC rtspStream = nullptr;
    QTemporaryFile certificateFile;
    QTemporaryFile keyFile;
    QByteArray pinnedServerSha256;
    QByteArray clientCertificateSha256;
    QByteArray token;
    std::atomic_bool serverCertificateValid {false};
    std::atomic_bool authenticated {false};
    std::atomic_bool stopping {false};
    std::mutex stateMutex;
    std::condition_variable stateChanged;
    bool connectionFinished = false;
    bool failed = false;
    std::array<std::unique_ptr<UdpBridge>, MLOS_QUIC_DATAGRAM_CAMERA + 1> udp;
    std::thread rtspWorker;
    std::mutex rtspMutex;
    std::condition_variable rtspBoundChanged;
    quint16 rtspPort = 0;
    bool rtspBindFailed = false;
    bool rtspPeerSendClosed = false;
    std::deque<QByteArray> rtspInbound;

    ~Impl() { stop(); }

    static void freeSend(void* context) { delete static_cast<SendContext*>(context); }

    bool sendStream(HQUIC stream, QByteArray bytes, QUIC_SEND_FLAGS flags = QUIC_SEND_FLAG_NONE) {
        auto context = new (std::nothrow) SendContext(std::move(bytes));
        if (!context) return false;
        const QUIC_STATUS status = api->StreamSend(stream, &context->buffer, 1, flags, context);
        if (QUIC_FAILED(status)) delete context;
        return QUIC_SUCCEEDED(status);
    }

    bool sendDatagram(quint8 channel, const QByteArray& payload) {
        if (!authenticated || payload.isEmpty() || payload.size() > 1442) return false;
        QByteArray packet(MLOS_QUIC_DATAGRAM_HEADER_SIZE + payload.size(), Qt::Uninitialized);
        if (!MlosQuicEncodeDatagramHeader(reinterpret_cast<uint8_t*>(packet.data()),
                                          MLOS_QUIC_DATAGRAM_HEADER_SIZE, channel, 0,
                                          static_cast<uint16_t>(payload.size()))) return false;
        std::copy(payload.begin(), payload.end(), packet.begin() + MLOS_QUIC_DATAGRAM_HEADER_SIZE);
        auto context = new (std::nothrow) SendContext(std::move(packet));
        if (!context) return false;
        const QUIC_STATUS status = api->DatagramSend(connection, &context->buffer, 1,
                                                     QUIC_SEND_FLAG_NONE, context);
        if (QUIC_FAILED(status)) delete context;
        return QUIC_SUCCEEDED(status);
    }

    void fail() {
        {
            std::lock_guard lock(stateMutex);
            failed = true;
        }
        stateChanged.notify_all();
        if (connection) api->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0x100);
    }

    bool openAuthStream() {
        if (QUIC_FAILED(api->StreamOpen(connection, QUIC_STREAM_OPEN_FLAG_NONE,
                                        streamCallback, this, &authStream)) ||
                QUIC_FAILED(api->StreamStart(authStream, QUIC_STREAM_START_FLAG_IMMEDIATE))) return false;
        QByteArray packet(MLOS_QUIC_STREAM_PREFACE_SIZE + MLOS_QUIC_AUTH_SIZE, Qt::Uninitialized);
        if (!MlosQuicEncodeStreamPreface(reinterpret_cast<uint8_t*>(packet.data()),
                                         MLOS_QUIC_STREAM_PREFACE_SIZE,
                                         MLOS_QUIC_STREAM_AUTH, 0) ||
                !MlosQuicEncodeAuth(reinterpret_cast<uint8_t*>(packet.data()) + MLOS_QUIC_STREAM_PREFACE_SIZE,
                                    MLOS_QUIC_AUTH_SIZE,
                                    reinterpret_cast<const uint8_t*>(token.constData()),
                                    reinterpret_cast<const uint8_t*>(clientCertificateSha256.constData()))) {
            return false;
        }
        return sendStream(authStream, std::move(packet));
    }

    static QUIC_STATUS QUIC_API streamCallback(HQUIC stream, void* context,
                                                QUIC_STREAM_EVENT* event) {
        auto self = static_cast<Impl*>(context);
        switch (event->Type) {
            case QUIC_STREAM_EVENT_RECEIVE: {
                QByteArray bytes;
                bytes.reserve(static_cast<int>(event->RECEIVE.TotalBufferLength));
                for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                    bytes.append(reinterpret_cast<const char*>(event->RECEIVE.Buffers[i].Buffer),
                                 static_cast<int>(event->RECEIVE.Buffers[i].Length));
                }
                if (stream == self->authStream) {
                    if (bytes.size() != 1 || bytes.at(0) != 1 || !self->serverCertificateValid) {
                        self->fail();
                    } else {
                        self->authenticated = true;
                        self->stateChanged.notify_all();
                    }
                } else {
                    std::lock_guard lock(self->rtspMutex);
                    if (stream == self->rtspStream) {
                        self->rtspInbound.push_back(std::move(bytes));
                        self->rtspBoundChanged.notify_all();
                    }
                }
                break;
            }
            case QUIC_STREAM_EVENT_SEND_COMPLETE:
                freeSend(event->SEND_COMPLETE.ClientContext);
                break;
            case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
                std::lock_guard lock(self->rtspMutex);
                if (stream == self->rtspStream) {
                    self->rtspPeerSendClosed = true;
                    self->rtspBoundChanged.notify_all();
                }
                break;
            }
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
                self->fail();
                break;
            case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
                if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress) self->api->StreamClose(stream);
                if (stream == self->authStream) self->authStream = nullptr;
                {
                    std::lock_guard lock(self->rtspMutex);
                    if (stream == self->rtspStream) {
                        self->rtspStream = nullptr;
                        self->rtspBoundChanged.notify_all();
                    }
                }
                break;
            default:
                break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    static QUIC_STATUS QUIC_API connectionCallback(HQUIC connection_, void* context,
                                                    QUIC_CONNECTION_EVENT* event) {
        auto self = static_cast<Impl*>(context);
        switch (event->Type) {
            case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED: {
                auto certificate = reinterpret_cast<const QUIC_BUFFER*>(
                    event->PEER_CERTIFICATE_RECEIVED.Certificate);
                if (certificate && certificate->Buffer && certificate->Length) {
                    const QByteArray der(reinterpret_cast<const char*>(certificate->Buffer),
                                         static_cast<int>(certificate->Length));
                    self->serverCertificateValid =
                        QCryptographicHash::hash(der, QCryptographicHash::Sha256) == self->pinnedServerSha256;
                }
                if (!self->serverCertificateValid) self->fail();
                break;
            }
            case QUIC_CONNECTION_EVENT_CONNECTED:
                if (!self->serverCertificateValid || !self->openAuthStream()) self->fail();
                break;
            case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
                const QUIC_BUFFER* buffer = event->DATAGRAM_RECEIVED.Buffer;
                MLOS_QUIC_DATAGRAM_HEADER header {};
                if (!buffer ||
                        !MlosQuicDecodeDatagramHeader(buffer->Buffer, buffer->Length, &header) ||
                        header.flags != 0 ||
                        header.channel >= self->udp.size()) {
                    self->fail();
                } else if (!self->authenticated) {
                    // A resumed host may already be producing media when it
                    // accepts our ticket. QUIC datagrams are not ordered with
                    // the authentication stream, so valid media can arrive
                    // just before the acknowledgement. Drop that data until
                    // authentication completes instead of tearing down the
                    // otherwise healthy connection.
                } else if (!self->udp[header.channel]) {
                    self->fail();
                } else {
                    self->udp[header.channel]->deliver(QByteArray(
                        reinterpret_cast<const char*>(buffer->Buffer + MLOS_QUIC_DATAGRAM_HEADER_SIZE),
                        header.payloadLength));
                }
                break;
            }
            case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
                if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(event->DATAGRAM_SEND_STATE_CHANGED.State)) {
                    freeSend(event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
                }
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
                if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress) self->api->ConnectionClose(connection_);
                self->connection = nullptr;
                {
                    std::lock_guard lock(self->stateMutex);
                    self->connectionFinished = true;
                }
                self->stateChanged.notify_all();
                break;
            default:
                break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    bool writeTemporaryCredentials(const QByteArray& certificate, const QByteArray& key) {
        certificateFile.setAutoRemove(true);
        keyFile.setAutoRemove(true);
        if (!certificateFile.open() || certificateFile.write(certificate) != certificate.size() ||
                !certificateFile.flush() || !keyFile.open() || keyFile.write(key) != key.size() ||
                !keyFile.flush()) return false;
        return true;
    }

    bool startQuic(const QuicSessionInfo& session, const QByteArray& serverCertificate,
                   const QByteArray& clientCertificate, const QByteArray& clientKey) {
        token = session.token;
        const auto serverCerts = QSslCertificate::fromData(serverCertificate);
        const auto clientCerts = QSslCertificate::fromData(clientCertificate);
        if (serverCerts.isEmpty() || clientCerts.isEmpty() ||
                !writeTemporaryCredentials(clientCertificate, clientKey)) return false;
        pinnedServerSha256 = QCryptographicHash::hash(serverCerts.first().toDer(),
                                                      QCryptographicHash::Sha256);
        clientCertificateSha256 = QCryptographicHash::hash(clientCerts.first().toDer(),
                                                            QCryptographicHash::Sha256);
        if (QUIC_FAILED(MsQuicOpen2(&api))) return false;
        const QUIC_REGISTRATION_CONFIG registrationConfig {
            "Selene Moonlight OS", QUIC_EXECUTION_PROFILE_LOW_LATENCY
        };
        if (QUIC_FAILED(api->RegistrationOpen(&registrationConfig, &registration))) return false;
        QUIC_SETTINGS settings {};
        settings.IdleTimeoutMs = 30000;
        settings.IsSet.IdleTimeoutMs = TRUE;
        settings.KeepAliveIntervalMs = 1000;
        settings.IsSet.KeepAliveIntervalMs = TRUE;
        settings.PeerBidiStreamCount = 1;
        settings.IsSet.PeerBidiStreamCount = TRUE;
        settings.DatagramReceiveEnabled = TRUE;
        settings.IsSet.DatagramReceiveEnabled = TRUE;
        settings.MigrationEnabled = TRUE;
        settings.IsSet.MigrationEnabled = TRUE;
        settings.MinimumMtu = 1280;
        settings.MaximumMtu = 1500;
        settings.IsSet.MinimumMtu = settings.IsSet.MaximumMtu = TRUE;
        QUIC_BUFFER alpn {sizeof(MLOS_QUIC_ALPN) - 1,
                          reinterpret_cast<uint8_t*>(const_cast<char*>(MLOS_QUIC_ALPN))};
        if (QUIC_FAILED(api->ConfigurationOpen(registration, &alpn, 1, &settings,
                                               sizeof(settings), nullptr, &configuration))) return false;
        const QByteArray certificatePath = certificateFile.fileName().toLocal8Bit();
        const QByteArray keyPath = keyFile.fileName().toLocal8Bit();
        QUIC_CERTIFICATE_FILE certificateConfig {keyPath.constData(), certificatePath.constData()};
        QUIC_CREDENTIAL_CONFIG credential {};
        credential.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        credential.CertificateFile = &certificateConfig;
        credential.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
            QUIC_CREDENTIAL_FLAG_CLIENT |
            QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION |
            QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED |
            QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES);
        if (QUIC_FAILED(api->ConfigurationLoadCredential(configuration, &credential)) ||
                QUIC_FAILED(api->ConnectionOpen(registration, connectionCallback, this, &connection))) return false;
        const QByteArray host = session.host.toUtf8();
        if (QUIC_FAILED(api->ConnectionStart(connection, configuration, QUIC_ADDRESS_FAMILY_UNSPEC,
                                             host.constData(), session.port))) return false;
        std::unique_lock lock(stateMutex);
        if (!stateChanged.wait_for(lock, std::chrono::seconds(10), [this] {
                return authenticated.load() || failed || connectionFinished;
            }) || !authenticated) return false;
        return true;
    }

    void runRtsp() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            std::lock_guard lock(rtspMutex);
            rtspBindFailed = true;
            rtspBoundChanged.notify_all();
            return;
        }
        {
            std::lock_guard lock(rtspMutex);
            rtspPort = server.serverPort();
            rtspBoundChanged.notify_all();
        }
        while (!stopping) {
            while (!stopping && !server.waitForNewConnection(10)) {}
            if (stopping) return;

            while (!stopping && server.hasPendingConnections()) {
                std::unique_ptr<QTcpSocket> socket(server.nextPendingConnection());
                if (!socket) { fail(); return; }

                HQUIC stream = nullptr;
                if (QUIC_FAILED(api->StreamOpen(connection, QUIC_STREAM_OPEN_FLAG_NONE,
                                                streamCallback, this, &stream))) {
                    fail(); return;
                }
                {
                    std::lock_guard lock(rtspMutex);
                    rtspStream = stream;
                    rtspPeerSendClosed = false;
                    rtspInbound.clear();
                }
                if (QUIC_FAILED(api->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
                    fail(); return;
                }
                QByteArray preface(MLOS_QUIC_STREAM_PREFACE_SIZE, Qt::Uninitialized);
                if (!MlosQuicEncodeStreamPreface(reinterpret_cast<uint8_t*>(preface.data()), preface.size(),
                                                 MLOS_QUIC_STREAM_RTSP, 0) ||
                        !sendStream(stream, preface)) {
                    fail(); return;
                }

                bool peerSendClosed = false;
                while (!stopping && socket->state() == QAbstractSocket::ConnectedState) {
                    if (socket->waitForReadyRead(5)) {
                        const QByteArray bytes = socket->readAll();
                        if (!bytes.isEmpty() && !sendStream(stream, bytes)) {
                            fail(); return;
                        }
                    }

                    std::deque<QByteArray> queued;
                    {
                        std::lock_guard lock(rtspMutex);
                        queued.swap(rtspInbound);
                        peerSendClosed = rtspPeerSendClosed;
                    }
                    for (const auto& bytes : queued) {
                        if (socket->write(bytes) != bytes.size() ||
                                (socket->bytesToWrite() > 0 && !socket->waitForBytesWritten(5000))) {
                            fail(); return;
                        }
                    }
                    if (peerSendClosed) {
                        socket->disconnectFromHost();
                        if (socket->state() != QAbstractSocket::UnconnectedState) {
                            socket->waitForDisconnected(1000);
                        }
                        break;
                    }
                }

                api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
                std::unique_lock lock(rtspMutex);
                if (!rtspBoundChanged.wait_for(lock, std::chrono::seconds(5), [this, stream] {
                        return stopping.load() || rtspStream != stream;
                    })) {
                    lock.unlock();
                    fail();
                    return;
                }
            }
        }
    }

    bool startProxies(QuicTransport::ProxyPorts& ports) {
        const std::array<quint8, 5> channels {
            MLOS_QUIC_DATAGRAM_VIDEO, MLOS_QUIC_DATAGRAM_CONTROL,
            MLOS_QUIC_DATAGRAM_AUDIO, MLOS_QUIC_DATAGRAM_MICROPHONE,
            MLOS_QUIC_DATAGRAM_CAMERA
        };
        for (const quint8 channel : channels) {
            udp[channel] = std::make_unique<UdpBridge>(this, channel);
            if (!udp[channel]->start()) return false;
        }
        ports.video = udp[MLOS_QUIC_DATAGRAM_VIDEO]->boundPort;
        ports.control = udp[MLOS_QUIC_DATAGRAM_CONTROL]->boundPort;
        ports.audio = udp[MLOS_QUIC_DATAGRAM_AUDIO]->boundPort;
        ports.microphone = udp[MLOS_QUIC_DATAGRAM_MICROPHONE]->boundPort;
        ports.camera = udp[MLOS_QUIC_DATAGRAM_CAMERA]->boundPort;
        rtspWorker = std::thread([this] { runRtsp(); });
        std::unique_lock lock(rtspMutex);
        rtspBoundChanged.wait(lock, [this] { return rtspPort != 0 || rtspBindFailed; });
        ports.rtsp = rtspPort;
        return ports.rtsp != 0;
    }

    void stop() {
        if (stopping.exchange(true)) return;
        for (auto& bridge : udp) bridge.reset();
        if (rtspWorker.joinable()) rtspWorker.join();
        if (connection && api) api->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
        if (connection && api) {
            std::unique_lock lock(stateMutex);
            stateChanged.wait_for(lock, std::chrono::seconds(5), [this] { return connectionFinished; });
        }
        if (connection && api) api->ConnectionClose(connection);
        connection = nullptr;
        if (configuration && api) api->ConfigurationClose(configuration);
        configuration = nullptr;
        if (registration && api) api->RegistrationClose(registration);
        registration = nullptr;
        if (api) MsQuicClose(api);
        api = nullptr;
    }
#else
    void stop() {}
#endif
};

QuicTransport::QuicTransport() : m_Impl(std::make_unique<Impl>())
{
}

QuicTransport::~QuicTransport() = default;

bool QuicTransport::start(const QuicSessionInfo& session,
                          const QByteArray& pinnedServerCertificate,
                          const QByteArray& clientCertificate,
                          const QByteArray& clientPrivateKey,
                          ProxyPorts& ports)
{
#ifdef HAVE_MSQUIC
    // Bind every loopback proxy before authentication. On a resumed session,
    // the host can send media immediately after accepting the ticket and
    // before the authentication acknowledgement reaches this client.
    if (!session.isValid() || !m_Impl->startProxies(ports) ||
            !m_Impl->startQuic(session, pinnedServerCertificate,
                               clientCertificate, clientPrivateKey)) {
        m_Impl->stop();
        return false;
    }
    return true;
#else
    Q_UNUSED(session)
    Q_UNUSED(pinnedServerCertificate)
    Q_UNUSED(clientCertificate)
    Q_UNUSED(clientPrivateKey)
    Q_UNUSED(ports)
    return false;
#endif
}

void QuicTransport::stop()
{
    m_Impl->stop();
}
