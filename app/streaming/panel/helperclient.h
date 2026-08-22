#pragma once

#include <QJsonObject>
#include <QMutex>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QThread>

#include <atomic>

// Talks to moonlight-helper, the appliance's privileged daemon.
//
// Selene runs as an unprivileged user and most of what the settings panel does
// -- joining a Wi-Fi network, logging into Tailscale, writing a partition
// table -- is not something it should be able to do directly. The helper owns
// that, exposes a fixed set of named operations over a unix socket, and this
// is the other end of it.
//
// Why hand-rolled sockets rather than QLocalSocket: this runs during a stream,
// and Qt's event loop is suspended for the whole of it -- Session::exec() owns
// the thread. Anything Qt-signal-driven would simply never fire. So a worker
// thread does blocking reads and the answers are handed back through a queue
// that the SDL loop drains, which is the same shape the clipboard callbacks
// use for the same reason.
class HelperClient : public QThread
{
    Q_OBJECT

public:
    explicit HelperClient(QObject* parent = nullptr);
    ~HelperClient() override;

    // Queue an operation. Returns the request id its reply will carry, or 0 if
    // the helper is unreachable -- an image without it is a normal state, not
    // an error to report.
    int request(const QString& op, const QJsonObject& args = {});

    // Take one finished reply, or false when there is nothing waiting. Called
    // from the SDL thread, so it never blocks.
    bool takeReply(QJsonObject& reply);

    bool isAvailable() const { return m_Available.load(); }

    static constexpr const char* k_SocketPath = "/run/moonlight-os/helper.sock";
    static constexpr qsizetype k_MaxReplyBytes = 1024 * 1024;

protected:
    void run() override;

private:
    bool connectToHelper(bool reportMissing = true);
    void connectionLost();
    bool sendLine(const QByteArray& line);
    bool readLine(QByteArray& line);

    int m_Socket;
    std::atomic_bool m_Available;

    QMutex m_Lock;
    QQueue<QByteArray> m_Outbound;
    QQueue<QJsonObject> m_Inbound;
    QSet<int> m_Outstanding;
    int m_NextId;
    bool m_Stopping;

    // Wakes the worker when a request is queued, without a busy loop.
    int m_WakePipe[2];
};
