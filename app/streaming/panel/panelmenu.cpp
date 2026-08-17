#include "panelmenu.h"
#include "helperclient.h"

#include "streaming/session.h"
#include "streaming/video/overlaymanager.h"
#include "streaming/streamutils.h"

#include <QJsonArray>

#include <sys/wait.h>
#include <unistd.h>

PanelMenu::PanelMenu()
    : m_Helper(new HelperClient()),
      m_Open(false),
      m_Screen(Screen::Main),
      m_Selected(0),
      m_PendingRequest(0),
      m_Hovered(-1),
      m_Window(nullptr),
      m_StreamWidth(0),
      m_StreamHeight(0)
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

    QPoint mapped((int)((x - originX) / scale), (int)((y - originY) / scale));

    return mapped;
}

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
    if (!m_Helper->isAvailable()) {
        return;
    }

    const char* const argv[] = { "moonlight-hotkey", "release", nullptr };
    runApplianceCommand(argv);
}

// And afterwards it has to go back, or there is no way to open a panel at the
// launcher -- which is exactly where someone sets up Wi-Fi for the first time.
void PanelMenu::releaseHotkey()
{
    if (!m_Helper->isAvailable()) {
        return;
    }

    const char* const argv[] = { "moonlight-hotkey", "grab", nullptr };
    runApplianceCommand(argv);
}

PanelMenu::~PanelMenu()
{
    delete m_Helper;
}

bool PanelMenu::open()
{
    if (!m_Helper->isAvailable()) {
        return false;
    }

    m_Open = true;
    m_Screen = Screen::Main;
    m_Selected = 0;
    m_Message.clear();

    // Ask immediately rather than on entering the status screen: it is the
    // first thing anyone opening this wants to know, and it costs one line.
    m_PendingRequest = m_Helper->request("status");

    Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayPanel, true);
    redraw();
    return true;
}

void PanelMenu::close()
{
    // Leaving it on would send every later keypress to the panel's idea of a
    // text field instead of to the host.
    SDL_StopTextInput();

    m_Open = false;
    Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayPanel, false);
}

QStringList PanelMenu::currentItems() const
{
    switch (m_Screen) {
    case Screen::Main:
        return { "Status", "Wi-Fi networks", "Close" };
    case Screen::Status:
        return { "Back" };
    case Screen::Networks:
        return m_Networks.isEmpty() ? QStringList { "Back" } : m_Networks + QStringList { "Back" };
    case Screen::Password:
        return { "Join", "Cancel" };
    }
    return {};
}

bool PanelMenu::handleKey(const SDL_KeyboardEvent* event)
{
    if (!m_Open || event->state != SDL_PRESSED) {
        // Still ours to swallow while open, so a key release does not reach
        // the host after its press was taken here.
        return m_Open;
    }

    auto items = currentItems();

    switch (event->keysym.sym) {
    case SDLK_UP:
        if (!items.isEmpty()) {
            m_Selected = (m_Selected + items.size() - 1) % items.size();
        }
        break;
    case SDLK_DOWN:
        if (!items.isEmpty()) {
            m_Selected = (m_Selected + 1) % items.size();
        }
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        activateSelection();
        break;
    case SDLK_BACKSPACE:
        if (m_Screen == Screen::Password) {
            m_Password.chop(1);
        }
        break;
    case SDLK_ESCAPE:
        if (m_Screen == Screen::Password) {
            SDL_StopTextInput();
            m_Password.clear();
        }
        if (m_Screen == Screen::Main) {
            close();
            return true;
        }
        m_Screen = Screen::Main;
        m_Selected = 0;
        break;
    default:
        // Swallowed deliberately: everything belongs to the panel while it is
        // up, so no keystroke leaks into the game behind it.
        return true;
    }

    redraw();
    return true;
}

void PanelMenu::activateSelection()
{
    auto items = currentItems();
    if (m_Selected < 0 || m_Selected >= items.size()) {
        return;
    }

    QString choice = items.at(m_Selected);

    if (choice == "Close") {
        close();
        return;
    }

    if (choice == "Back" || choice == "Cancel") {
        if (m_Screen == Screen::Password) {
            SDL_StopTextInput();
            m_Password.clear();
        }
        m_Screen = Screen::Main;
        m_Selected = 0;
        return;
    }

    if (choice == "Join") {
        SDL_StopTextInput();

        QJsonObject args;
        args["ssid"] = m_PendingSsid;
        if (!m_Password.isEmpty()) {
            args["psk"] = m_Password;
        }

        m_PendingRequest = m_Helper->request("wifi.connect", args);
        m_Password.clear();
        m_Screen = Screen::Status;
        m_Selected = 0;
        m_Message = QStringLiteral("Joining %1...").arg(m_PendingSsid);
        return;
    }

    if (choice == "Status") {
        m_Screen = Screen::Status;
        m_Selected = 0;
        m_PendingRequest = m_Helper->request("status");
        m_Message = "Checking...";
        return;
    }

    if (choice == "Wi-Fi networks") {
        m_Screen = Screen::Networks;
        m_Selected = 0;
        m_Networks.clear();
        m_PendingRequest = m_Helper->request("wifi.list");
        m_Message = "Scanning...";
        return;
    }

    // A network was chosen. The label carries the signal and lock state
    // after a tab, so the SSID is the part before it.
    m_PendingSsid = choice.split(QChar('\t')).value(0);
    m_Password.clear();
    m_Screen = Screen::Password;
    m_Selected = 0;
    m_Message.clear();

    // Only now, and only here: text input is off for the rest of the session
    // so that typing goes to the host as scancodes, which is the whole point
    // of a streaming client.
    SDL_StartTextInput();
}

void PanelMenu::applyReply(const QJsonObject& reply)
{
    if (!reply.value("ok").toBool()) {
        auto error = reply.value("error").toObject();
        auto message = error.value("message").toString();
        m_Message = message.isEmpty() ? QStringLiteral("That did not work") : message;
        return;
    }

    m_Message.clear();
    auto result = reply.value("result").toObject();

    if (result.contains("networks")) {
        m_Networks.clear();
        for (auto value : result.value("networks").toArray()) {
            auto network = value.toObject();
            m_Networks.append(QStringLiteral("%1  %2%%3")
                                  .arg(network.value("ssid").toString())
                                  .arg(network.value("signal").toInt())
                                  .arg(network.value("secure").toBool() ? "  locked" : ""));
        }
        if (m_Networks.isEmpty()) {
            m_Message = "No networks in range";
        }
        return;
    }

    if (result.contains("hostname")) {
        m_StatusLines = {
            QStringLiteral("Name     %1").arg(result.value("hostname").toString()),
            QStringLiteral("Address  %1").arg(result.value("address").toString(QStringLiteral("none"))),
            QStringLiteral("Wi-Fi    %1").arg(result.value("wifi").toString(QStringLiteral("not connected"))),
        };
    }
}

void PanelMenu::poll()
{
    if (!m_Open) {
        return;
    }

    bool changed = false;
    QJsonObject reply;
    while (m_Helper->takeReply(reply)) {
        // Progress events carry a message and no verdict; showing them is the
        // difference between "scanning" and an apparently frozen panel.
        if (reply.value("event").toString() == QLatin1String("progress")) {
            m_Message = reply.value("message").toString();
        }
        else {
            applyReply(reply);
        }
        changed = true;
    }

    if (changed) {
        redraw();
    }
}

void PanelMenu::redraw()
{
    PanelPainter::Model model;
    model.title = QStringLiteral("Moonlight OS");
    model.selected = m_Selected;
    model.hovered = m_Hovered;
    model.message = m_Message;
    model.hint = QStringLiteral("Arrows or mouse  \u00b7  Enter to choose  \u00b7  Esc to go back");

    if (m_Screen == Screen::Status) {
        model.lines = m_StatusLines;
    }

    if (m_Screen == Screen::Password) {
        model.inputActive = true;
        model.inputMasked = true;
        model.inputLabel = QStringLiteral("Password for %1").arg(m_PendingSsid);
        model.inputValue = m_Password;
    }

    for (const auto& item : currentItems()) {
        PanelPainter::Row row;
        // Networks arrive tab-separated so the signal can sit at the right
        // edge instead of being run into the name.
        auto parts = item.split(QChar('\t'));
        row.text = parts.value(0);
        if (parts.size() > 1) {
            row.detail = parts.mid(1).join(QChar(' ')).trimmed();
        }
        model.rows.append(row);
    }

    SDL_Surface* surface = m_Painter.paint(model);
    if (surface != nullptr) {
        Session::get()->getOverlayManager().setOverlaySurface(Overlay::OverlayPanel, surface);
    }
}

bool PanelMenu::handleTextInput(const char* text)
{
    if (!m_Open || m_Screen != Screen::Password) {
        // Swallowed anyway while the panel is open: a character that reached
        // the host from behind an open menu would be a keystroke nobody
        // aimed at the game.
        return m_Open;
    }

    m_Password.append(QString::fromUtf8(text));
    redraw();
    return true;
}

bool PanelMenu::handleMouseMotion(int x, int y)
{
    if (!m_Open) {
        return false;
    }

    int hovered = m_Painter.rowAt(mapToPanel(x, y));
    if (hovered != m_Hovered) {
        m_Hovered = hovered;
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
        int row = m_Painter.rowAt(mapToPanel(event->x, event->y));
        if (row >= 0) {
            m_Selected = row;
            activateSelection();
            redraw();
        }
    }

    return true;
}
