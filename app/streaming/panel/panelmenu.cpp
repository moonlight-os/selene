#include "panelmenu.h"
#include "helperclient.h"

#include "streaming/session.h"
#include "streaming/video/overlaymanager.h"
#include "streaming/streamutils.h"

#include <QJsonArray>

#include <sys/wait.h>
#include <unistd.h>

namespace
{
// Run a short appliance command, inheriting none of our descriptors.
//
// The descriptor part is not politeness: this process has already been bitten
// once by a forked child holding a socket open for the lifetime of a
// clipboard selection.
void runApplianceCommand(const char* const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }

    if (pid == 0) {
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

    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}
} // namespace

// While a stream runs, the key has to reach this client: the panel is drawn
// into our own surface, and sway holding the binding means we never see it.
void PanelMenu::claimHotkey()
{
    if (m_HotkeyClaimed || !m_Model.isAvailable()) {
        return;
    }

    const char* const argv[] = { "moonlight-hotkey", "release", nullptr };
    runApplianceCommand(argv);
    m_HotkeyClaimed = true;
}

// And afterwards it has to go back, or there is no way to open a panel at the
// launcher -- which is exactly where someone sets up Wi-Fi for the first time.
void PanelMenu::releaseHotkey()
{
    if (!m_HotkeyClaimed) {
        return;
    }

    const char* const argv[] = { "moonlight-hotkey", "grab", nullptr };
    runApplianceCommand(argv);
    m_HotkeyClaimed = false;
}

PanelMenu::PanelMenu()
    : m_Open(false),
      m_HotkeyClaimed(false),
      m_Window(nullptr),
      m_StreamWidth(0),
      m_StreamHeight(0)
{
}

PanelMenu::~PanelMenu()
{
}

// Turn a pointer position into panel-image coordinates.
//
// The panel is composited into the video frame and then scaled and
// letterboxed onto the window along with the picture, so a click has to be
// pushed back through both. Getting this wrong does not look like a
// coordinate bug -- it looks like the hover highlighting the wrong row.
QPoint PanelMenu::mapToPanel(int x, int y) const
{
    if (m_Window == nullptr || m_StreamWidth <= 0 || m_StreamHeight <= 0) {
        return QPoint(x, y);
    }

    int windowWidth, windowHeight;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);

    SDL_Rect src = { 0, 0, m_StreamWidth, m_StreamHeight };
    SDL_Rect dst = { 0, 0, windowWidth, windowHeight };
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    if (dst.w <= 0 || dst.h <= 0) {
        return QPoint(x, y);
    }

    double scale = (double)dst.w / m_StreamWidth;

    auto size = m_Painter.size();
    double originX = dst.x + ((m_StreamWidth - size.width()) / 2) * scale;
    double originY = dst.y + ((m_StreamHeight - size.height()) / 2) * scale;

    return QPoint((int)((x - originX) / scale), (int)((y - originY) / scale));
}

bool PanelMenu::open()
{
    if (!m_Model.isAvailable()) {
        return false;
    }

    m_Open = true;
    m_Model.reset();

    Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayPanel, true);
    redraw();
    return true;
}

void PanelMenu::close()
{
    // Leaving text input on would send every later keypress to the panel's
    // idea of a field instead of to the host.
    SDL_StopTextInput();

    m_Open = false;
    m_Model.clearCloseRequest();
    Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayPanel, false);
}

bool PanelMenu::handleKey(const SDL_KeyboardEvent* event)
{
    if (!m_Open || event->state != SDL_PRESSED) {
        // Still ours to swallow while open, so a key release does not reach
        // the host after its press was taken here.
        return m_Open;
    }

    bool wantedText = m_Model.wantsTextInput();

    switch (event->keysym.sym) {
    case SDLK_UP:
        m_Model.handleKey(PanelModel::Key::Up);
        break;
    case SDLK_DOWN:
    case SDLK_TAB:
        m_Model.handleKey(PanelModel::Key::Down);
        break;
    case SDLK_PAGEUP:
        m_Model.handleKey(PanelModel::Key::PageUp);
        break;
    case SDLK_PAGEDOWN:
        m_Model.handleKey(PanelModel::Key::PageDown);
        break;
    case SDLK_HOME:
        m_Model.handleKey(PanelModel::Key::Home);
        break;
    case SDLK_END:
        m_Model.handleKey(PanelModel::Key::End);
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        m_Model.handleKey(PanelModel::Key::Activate);
        break;
    case SDLK_BACKSPACE:
        m_Model.handleKey(PanelModel::Key::Backspace);
        break;
    case SDLK_ESCAPE:
        m_Model.handleKey(PanelModel::Key::Back);
        break;
    default:
        // Swallowed deliberately: everything belongs to the panel while it is
        // up, so no keystroke leaks into the game behind it.
        return true;
    }

    // The model decides when a field is up; this side owns SDL's text input,
    // which must not be left on when it is not.
    if (m_Model.wantsTextInput() != wantedText) {
        if (m_Model.wantsTextInput()) {
            SDL_StartTextInput();
        }
        else {
            SDL_StopTextInput();
        }
    }

    if (m_Model.closeRequested()) {
        close();
        return true;
    }

    redraw();
    return true;
}

bool PanelMenu::handleTextInput(const char* text)
{
    if (!m_Open) {
        return false;
    }

    m_Model.textEntered(QString::fromUtf8(text));
    redraw();
    return true;
}

bool PanelMenu::handleMouseMotion(int x, int y)
{
    if (!m_Open) {
        return false;
    }

    if (m_Model.setHovered(m_Painter.rowAt(mapToPanel(x, y)))) {
        redraw();
    }

    // Swallowed even when the pointer is outside the panel: letting motion
    // through would have the game turn while the menu is up.
    return true;
}

bool PanelMenu::handleMouseButton(const SDL_MouseButtonEvent* event)
{
    if (!m_Open) {
        return false;
    }

    if (event->state == SDL_PRESSED && event->button == SDL_BUTTON_LEFT) {
        bool wantedText = m_Model.wantsTextInput();

        m_Model.activateRow(m_Painter.rowAt(mapToPanel(event->x, event->y)));

        if (m_Model.wantsTextInput() != wantedText) {
            if (m_Model.wantsTextInput()) {
                SDL_StartTextInput();
            }
            else {
                SDL_StopTextInput();
            }
        }

        if (m_Model.closeRequested()) {
            close();
            return true;
        }

        redraw();
    }

    return true;
}

bool PanelMenu::handleMouseWheel(const SDL_MouseWheelEvent* event)
{
    if (!m_Open) {
        return false;
    }
    if (event->y != 0) {
        m_Model.handleKey(event->y > 0 ? PanelModel::Key::Up : PanelModel::Key::Down);
        redraw();
    }
    return true;
}

void PanelMenu::poll()
{
    // If the appliance helper started after the stream, claim the in-client
    // shortcut as soon as it becomes available. Without this, the reconnect
    // succeeds but Ctrl+Alt+M remains owned by Sway until the next stream.
    if (!m_HotkeyClaimed && m_Model.isAvailable()) {
        claimHotkey();
    }

    if (m_Open && m_Model.poll()) {
        redraw();
    }
}

void PanelMenu::redraw()
{
    SDL_Surface* surface = m_Painter.paint(
        m_Model.model(), QSize(qMax(1, m_StreamWidth - 24), qMax(1, m_StreamHeight - 24)));
    if (surface != nullptr) {
        Session::get()->getOverlayManager().setOverlaySurface(Overlay::OverlayPanel, surface);
    }
}
