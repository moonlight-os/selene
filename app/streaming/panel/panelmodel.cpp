#include "panelmodel.h"
#include "helperclient.h"

#include <QJsonArray>

PanelModel::PanelModel()
    : m_Helper(new HelperClient()),
      m_Screen(Screen::Main),
      m_Selected(0),
      m_Hovered(-1),
      m_CloseRequested(false)
{
}

PanelModel::~PanelModel()
{
    delete m_Helper;
}

bool PanelModel::isAvailable() const
{
    return m_Helper->isAvailable();
}

void PanelModel::reset()
{
    m_Screen = Screen::Main;
    m_Selected = 0;
    m_Hovered = -1;
    m_CloseRequested = false;
    m_Message.clear();
    m_Password.clear();

    // Asked on open rather than on entering the status screen: it is the
    // first thing anyone opening this wants to know, and it costs one line.
    m_Helper->request("status");
}

QStringList PanelModel::currentItems() const
{
    switch (m_Screen) {
    case Screen::Main:
        return { "Status", "Wi-Fi networks", "Power", "Close" };
    case Screen::Status:
        return { "Back" };
    case Screen::Networks:
        return m_Networks.isEmpty() ? QStringList { "Back" } : m_Networks + QStringList { "Back" };
    case Screen::Password:
        return { "Join", "Cancel" };
    case Screen::Power:
        return { "Reboot", "Shut down", "Back" };
    case Screen::ConfirmPower:
        // "No" first, and selected by default, because the two outcomes here
        // are not equally recoverable.
        return { "No, go back", m_PendingPower == QLatin1String("reboot")
                                    ? QStringLiteral("Yes, reboot now")
                                    : QStringLiteral("Yes, shut down now") };
    }
    return {};
}

bool PanelModel::wantsTextInput() const
{
    return m_Screen == Screen::Password;
}

PanelPainter::Model PanelModel::model() const
{
    PanelPainter::Model out;
    out.title = QStringLiteral("Moonlight OS");
    out.selected = m_Selected;
    out.hovered = m_Hovered;
    out.message = m_Message;
    out.hint = QStringLiteral("Arrows or mouse  ·  Enter to choose  ·  Esc to go back");

    if (m_Screen == Screen::Status) {
        out.lines = m_StatusLines;
    }

    if (m_Screen == Screen::Password) {
        out.inputActive = true;
        out.inputMasked = true;
        out.inputLabel = QStringLiteral("Password for %1").arg(m_PendingSsid);
        out.inputValue = m_Password;
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
        out.rows.append(row);
    }

    return out;
}

bool PanelModel::handleKey(Key key)
{
    auto items = currentItems();

    switch (key) {
    case Key::Up:
        if (!items.isEmpty()) {
            m_Selected = (m_Selected + items.size() - 1) % items.size();
        }
        break;
    case Key::Down:
        if (!items.isEmpty()) {
            m_Selected = (m_Selected + 1) % items.size();
        }
        break;
    case Key::Activate:
        activateSelection();
        break;
    case Key::Backspace:
        if (m_Screen == Screen::Password) {
            m_Password.chop(1);
        }
        break;
    case Key::Back:
        if (m_Screen == Screen::Password) {
            m_Password.clear();
        }
        if (m_Screen == Screen::Main) {
            m_CloseRequested = true;
            break;
        }
        m_Screen = Screen::Main;
        m_Selected = 0;
        m_Message.clear();
        break;
    }

    return true;
}

void PanelModel::textEntered(const QString& text)
{
    if (m_Screen == Screen::Password) {
        m_Password.append(text);
    }
}

bool PanelModel::setHovered(int row)
{
    if (m_Hovered == row) {
        return false;
    }

    m_Hovered = row;
    return true;
}

void PanelModel::activateRow(int row)
{
    auto items = currentItems();
    if (row < 0 || row >= items.size()) {
        return;
    }

    m_Selected = row;
    activateSelection();
}

void PanelModel::activateSelection()
{
    auto items = currentItems();
    if (m_Selected < 0 || m_Selected >= items.size()) {
        return;
    }

    QString choice = items.at(m_Selected);

    if (choice == "Close") {
        m_CloseRequested = true;
        return;
    }

    if (choice == "Back" || choice == "Cancel") {
        if (m_Screen == Screen::Password) {
            m_Password.clear();
        }
        m_Screen = Screen::Main;
        m_Selected = 0;
        m_Message.clear();
        return;
    }

    if (choice == "Status") {
        m_Screen = Screen::Status;
        m_Selected = 0;
        m_Helper->request("status");
        m_Message = QStringLiteral("Checking...");
        return;
    }

    if (choice == "Wi-Fi networks") {
        m_Screen = Screen::Networks;
        m_Selected = 0;
        m_Networks.clear();
        m_Helper->request("wifi.list");
        m_Message = QStringLiteral("Scanning...");
        return;
    }

    if (choice == "Power") {
        m_Screen = Screen::Power;
        m_Selected = 0;
        m_Message.clear();
        return;
    }

    if (choice == "Reboot" || choice == "Shut down") {
        m_PendingPower = choice == "Reboot" ? QStringLiteral("reboot") : QStringLiteral("poweroff");
        m_Screen = Screen::ConfirmPower;
        m_Selected = 0;  // on "No"
        m_Message = QStringLiteral("The stream will end.");
        return;
    }

    if (choice == "No, go back") {
        m_Screen = Screen::Power;
        m_Selected = 0;
        m_Message.clear();
        return;
    }

    if (choice.startsWith(QLatin1String("Yes, "))) {
        QJsonObject args;
        args["action"] = m_PendingPower;
        m_Helper->request("system.power", args);
        m_Message = QStringLiteral("Asking the system...");
        return;
    }

    if (choice == "Join") {
        QJsonObject args;
        args["ssid"] = m_PendingSsid;
        if (!m_Password.isEmpty()) {
            args["psk"] = m_Password;
        }

        m_Helper->request("wifi.connect", args);
        m_Password.clear();
        m_Screen = Screen::Status;
        m_Selected = 0;
        m_Message = QStringLiteral("Joining %1...").arg(m_PendingSsid);
        return;
    }

    // A network was chosen. The label carries the signal and lock state after
    // a tab, so the SSID is the part before it.
    if (m_Screen == Screen::Networks) {
        m_PendingSsid = choice.split(QChar('\t')).value(0);
        m_Password.clear();
        m_Screen = Screen::Password;
        m_Selected = 0;
        m_Message.clear();
    }
}

void PanelModel::applyReply(const QJsonObject& reply)
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
            m_Networks.append(QStringLiteral("%1\t%2%\t%3")
                                  .arg(network.value("ssid").toString())
                                  .arg(network.value("signal").toInt())
                                  .arg(network.value("secure").toBool() ? "locked" : ""));
        }
        if (m_Networks.isEmpty()) {
            m_Message = QStringLiteral("No networks in range");
        }
        return;
    }

    if (result.contains("hostname")) {
        // Built by appending rather than from a braced list: the Steam Link
        // toolchain cannot decide which QStringList assignment that means.
        m_StatusLines.clear();
        m_StatusLines.append(QStringLiteral("Name     %1").arg(result.value("hostname").toString()));
        m_StatusLines.append(QStringLiteral("Address  %1").arg(result.value("address").toString(QStringLiteral("none"))));
        m_StatusLines.append(QStringLiteral("Wi-Fi    %1").arg(result.value("wifi").toString(QStringLiteral("not connected"))));
    }
}

bool PanelModel::poll()
{
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

    return changed;
}
