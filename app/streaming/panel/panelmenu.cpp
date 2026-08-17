#include "panelmenu.h"
#include "helperclient.h"

#include "streaming/session.h"
#include "streaming/video/overlaymanager.h"
#include "streaming/streamutils.h"

#include <QJsonArray>

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
    case SDLK_ESCAPE:
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

    if (choice == "Back") {
        m_Screen = Screen::Main;
        m_Selected = 0;
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

    // A network was chosen. Joining one needs a password this panel cannot
    // yet collect, so it is deliberately not attempted -- offering it and
    // then failing would be worse than not offering it.
    m_Message = "Joining a network from here is not built yet";
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
