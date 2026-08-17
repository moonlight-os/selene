#include "helperclient.h"

#include <SDL.h>

#include <QJsonDocument>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

HelperClient::HelperClient(QObject* parent)
    : QThread(parent),
      m_Socket(-1),
      m_Available(false),
      m_NextId(1),
      m_Stopping(false)
{
    m_WakePipe[0] = m_WakePipe[1] = -1;
    if (pipe(m_WakePipe) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Could not create the helper wake pipe: %s", strerror(errno));
        return;
    }

    m_Available = connectToHelper();
    if (m_Available) {
        start();
    }
}

HelperClient::~HelperClient()
{
    {
        QMutexLocker locker(&m_Lock);
        m_Stopping = true;
    }

    if (m_WakePipe[1] >= 0) {
        char wake = 1;
        ssize_t ignored = write(m_WakePipe[1], &wake, 1);
        (void)ignored;
    }

    if (isRunning()) {
        wait(2000);
    }

    if (m_Socket >= 0) {
        close(m_Socket);
    }
    if (m_WakePipe[0] >= 0) {
        close(m_WakePipe[0]);
    }
    if (m_WakePipe[1] >= 0) {
        close(m_WakePipe[1]);
    }
}

bool HelperClient::connectToHelper()
{
    // Close-on-exec because this process spawns wl-copy, which forks and stays
    // alive to serve the selection -- an inherited descriptor would be held
    // for as long as the clipboard lives. The same mistake with a listening
    // socket is what once kept Helios from restarting.
    //
    // SOCK_CLOEXEC is Linux; macOS has no such flag and needs the fcntl.
#ifdef SOCK_CLOEXEC
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#endif
    if (fd < 0) {
        return false;
    }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, k_SocketPath, sizeof(addr.sun_path) - 1);

    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        // Not an error worth shouting about: a Selene running anywhere other
        // than a Moonlight OS appliance has no helper and never will.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "No appliance helper at %s (%s), so the settings panel is unavailable",
                    k_SocketPath, strerror(errno));
        close(fd);
        return false;
    }

    m_Socket = fd;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Connected to the appliance helper");
    return true;
}

int HelperClient::request(const QString& op, const QJsonObject& args)
{
    if (!m_Available) {
        return 0;
    }

    QMutexLocker locker(&m_Lock);

    int id = m_NextId++;
    QJsonObject request;
    request["id"] = id;
    request["op"] = op;
    if (!args.isEmpty()) {
        request["args"] = args;
    }

    m_Outbound.enqueue(QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n");
    locker.unlock();

    char wake = 1;
    ssize_t ignored = write(m_WakePipe[1], &wake, 1);
    (void)ignored;

    return id;
}

bool HelperClient::takeReply(QJsonObject& reply)
{
    QMutexLocker locker(&m_Lock);
    if (m_Inbound.isEmpty()) {
        return false;
    }
    reply = m_Inbound.dequeue();
    return true;
}

bool HelperClient::sendLine(const QByteArray& line)
{
    qsizetype remaining = line.size();
    const char* at = line.constData();

    while (remaining > 0) {
        ssize_t written = write(m_Socket, at, (size_t)remaining);
        if (written <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        at += written;
        remaining -= written;
    }

    return true;
}

bool HelperClient::readLine(QByteArray& line)
{
    line.clear();

    // One byte at a time is fine here: these are short replies arriving at
    // human speed, and it keeps the reader from having to hold a buffer
    // across calls.
    for (;;) {
        char c;
        ssize_t got = read(m_Socket, &c, 1);
        if (got == 0) {
            return false; // helper closed the connection
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (c == '\n') {
            return true;
        }
        line.append(c);
    }
}

void HelperClient::run()
{
    for (;;) {
        struct pollfd fds[2];
        fds[0].fd = m_Socket;
        fds[0].events = POLLIN;
        fds[1].fd = m_WakePipe[0];
        fds[1].events = POLLIN;

        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        {
            QMutexLocker locker(&m_Lock);
            if (m_Stopping) {
                break;
            }
        }

        if (fds[1].revents & POLLIN) {
            char drain[64];
            ssize_t ignored = read(m_WakePipe[0], drain, sizeof(drain));
            (void)ignored;

            for (;;) {
                QByteArray line;
                {
                    QMutexLocker locker(&m_Lock);
                    if (m_Outbound.isEmpty()) {
                        break;
                    }
                    line = m_Outbound.dequeue();
                }

                if (!sendLine(line)) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Lost the helper connection while sending");
                    m_Available = false;
                    return;
                }
            }
        }

        if (fds[0].revents & (POLLIN | POLLHUP)) {
            QByteArray line;
            if (!readLine(line)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The helper closed the connection");
                m_Available = false;
                return;
            }

            QJsonParseError error;
            auto document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Ignoring an unparseable helper reply");
                continue;
            }

            QMutexLocker locker(&m_Lock);
            m_Inbound.enqueue(document.object());
        }
    }
}
