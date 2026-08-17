#include "clipboard.h"

#include <SDL.h>

#ifdef Q_OS_LINUX
#include <QByteArray>

#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

namespace
{

// Run a clipboard tool with our text on its stdin, inheriting none of our
// file descriptors.
//
// The descriptor part is not defensive tidiness. wl-copy forks and stays alive
// to serve the selection, so anything it inherits is held for as long as the
// clipboard lives. Helios hit exactly this from the other side: a wl-copy
// started with popen() kept the host's RTSP socket open, and the next start
// failed with "Address already in use" -- a failure that looks nothing like a
// clipboard bug. A streaming client holds sockets too.
bool runClipboardTool(const char* const argv[], const QByteArray& input)
{
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }

    if (pid == 0) {
        // Child.
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
        closefrom(STDERR_FILENO + 1);
#else
        long maxFd = sysconf(_SC_OPEN_MAX);
        for (int fd = STDERR_FILENO + 1; fd < (int)maxFd; fd++) {
            close(fd);
        }
#endif

        execvp(argv[0], (char* const*)argv);
        _exit(127);
    }

    // Parent.
    close(fds[0]);

    bool ok = true;
    qsizetype remaining = input.size();
    const char* at = input.constData();
    while (remaining > 0) {
        ssize_t written = write(fds[1], at, (size_t)remaining);
        if (written <= 0) {
            ok = false;
            break;
        }
        at += written;
        remaining -= written;
    }
    close(fds[1]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    // 127 is the exec failing, which is the tool not being installed rather
    // than the clipboard refusing the text. Worth telling apart in the log.
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s is not installed, so the clipboard cannot be set",
                    argv[0]);
        return false;
    }

    return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace
#endif

namespace Clipboard
{

bool setText(const QString& text)
{
#ifdef Q_OS_LINUX
    QByteArray utf8 = text.toUtf8();

    // WAYLAND_DISPLAY first: a Wayland session usually has DISPLAY set too for
    // Xwayland, and setting the X selection there writes to a clipboard most
    // applications are not reading.
    if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
        const char* const argv[] = { "wl-copy", nullptr };
        if (runClipboardTool(argv, utf8)) {
            return true;
        }
    }
    else if (qEnvironmentVariableIsSet("DISPLAY")) {
        const char* const argv[] = { "xclip", "-selection", "clipboard", "-i", nullptr };
        if (runClipboardTool(argv, utf8)) {
            return true;
        }
    }

    // Fall through to SDL rather than giving up: on a session without those
    // tools, or one that is neither Wayland nor X11, SDL may still be right.
#endif

    if (SDL_SetClipboardText(text.toUtf8().constData()) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_SetClipboardText() failed: %s", SDL_GetError());
        return false;
    }

    return true;
}

} // namespace Clipboard
