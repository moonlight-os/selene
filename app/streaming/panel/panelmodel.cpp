#include "panelmodel.h"
#include "helperclient.h"

#include <QJsonArray>

PanelModel::PanelModel()
    : m_Helper(new HelperClient()),
      m_Screen(Screen::Main),
      m_Selected(0),
      m_Hovered(-1),
      m_Top(0),
      m_CloseRequested(false),
      m_BtPresent(false),
      m_BtPowered(false),
      m_UsbPaired(false)
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
    goTo(Screen::Main);
    m_Hovered = -1;
    m_CloseRequested = false;
    m_Password.clear();

    // Asked on open rather than on entering the status screen: it is the
    // first thing anyone opening this wants to know, and it costs one line.
    ask(QStringLiteral("status"));
}

int PanelModel::ask(const QString& op, const QJsonObject& args)
{
    int id = m_Helper->request(op, args);
    if (id != 0) {
        m_Pending.insert(id, op);
    }
    return id;
}

void PanelModel::goTo(Screen screen)
{
    m_Screen = screen;
    m_Selected = 0;
    m_Top = 0;
    m_Message.clear();
}

void PanelModel::ensureVisible()
{
    int count = currentItems().size();
    int last = qMax(0, count - k_MaxVisibleRows);

    if (m_Selected < m_Top) {
        m_Top = m_Selected;
    }
    else if (m_Selected >= m_Top + k_MaxVisibleRows) {
        m_Top = m_Selected - k_MaxVisibleRows + 1;
    }

    m_Top = qBound(0, m_Top, last);
}

int PanelModel::itemAt(int drawnRow) const
{
    return drawnRow < 0 ? -1 : m_Top + drawnRow;
}

const PanelModel::UsbDevice* PanelModel::selectedUsb() const
{
    for (const auto& device : m_UsbDevices) {
        if (device.busid == m_PendingBusid) {
            return &device;
        }
    }
    return nullptr;
}

const PanelModel::Device* PanelModel::selectedDevice() const
{
    for (const auto& device : m_Devices) {
        if (device.mac == m_PendingMac) {
            return &device;
        }
    }
    return nullptr;
}

QStringList PanelModel::currentItems() const
{
    switch (m_Screen) {
    case Screen::Main:
        return { "Status", "Wi-Fi networks", "Bluetooth", "USB devices", "Power", "Close" };
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

    case Screen::Bluetooth: {
        if (!m_BtPresent) {
            return { "Back" };
        }
        if (!m_BtPowered) {
            return { "Turn Bluetooth on", "Back" };
        }

        QStringList items;
        for (const auto& device : m_Devices) {
            // The address is never shown. It is 17 characters of nothing
            // anyone recognises, and on a list of "Xbox Wireless Controller"
            // and a headset it would be the only thing that did not fit.
            items.append(QStringLiteral("%1\t%2").arg(device.name,
                device.connected ? QStringLiteral("connected")
                                 : device.paired ? QStringLiteral("paired")
                                                 : QString()));
        }
        items.append(QStringLiteral("Look for new devices"));
        items.append(QStringLiteral("Turn Bluetooth off"));
        items.append(QStringLiteral("Back"));
        return items;
    }

    case Screen::BluetoothDevice: {
        const Device* device = selectedDevice();
        if (device == nullptr) {
            return { "Back" };
        }
        if (device->connected) {
            return { "Disconnect", "Forget this device", "Back" };
        }
        if (device->paired) {
            return { "Connect", "Forget this device", "Back" };
        }
        return { "Pair and connect", "Back" };
    }

    case Screen::ConfirmForget:
        // Keeping it is the recoverable answer, so it goes first and starts
        // selected -- the same shape as the power confirmation.
        return { "No, keep it", QStringLiteral("Yes, forget %1").arg(m_PendingName) };

    case Screen::Usb: {
        QStringList items;
        bool anyShared = false;
        for (const auto& device : m_UsbDevices) {
            anyShared = anyShared || device.shared;
            // The reason itself is not put in the right-hand column: it is a
            // sentence, and a sentence there would set the panel's width to
            // the longest explanation on the machine. It gets the device
            // screen, where there is a line to put it on.
            items.append(QStringLiteral("%1\t%2").arg(device.label,
                device.shared ? QStringLiteral("shared")
                              : device.reason.isEmpty() ? QString()
                                                        : QStringLiteral("not offered")));
        }
        if (anyShared && m_UsbPaired) {
            items.append(QStringLiteral("Hand everything back"));
        }
        items.append(QStringLiteral("Back"));
        return items;
    }

    case Screen::UsbDevice: {
        const UsbDevice* device = selectedUsb();
        if (device == nullptr || device->blocked) {
            return { "Back" };
        }
        if (device->shared) {
            return { "Stop sharing", "Back" };
        }
        // A device the policy declined can still be handed over -- it is
        // advice, not a rule -- but the button says so, because the reason
        // for declining is on screen right above it.
        return { device->reason.isEmpty() ? QStringLiteral("Share with the host PC")
                                          : QStringLiteral("Share it anyway"),
                 QStringLiteral("Back") };
    }

    case Screen::ConfirmShare:
        return { "No, leave it alone", QStringLiteral("Yes, share %1").arg(m_PendingName) };
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
    out.message = m_Message;
    out.hint = QStringLiteral("Arrows or mouse  ·  Enter to choose  ·  Esc to go back");

    if (m_Screen == Screen::Status) {
        out.lines = m_StatusLines;
    }

    if (m_Screen == Screen::Bluetooth) {
        if (!m_BtPresent) {
            out.lines = { QStringLiteral("This machine has no Bluetooth adapter.") };
        }
        else if (!m_BtPowered) {
            out.lines = { QStringLiteral("Bluetooth is off.") };
        }
        else if (m_Devices.isEmpty()) {
            out.lines = { QStringLiteral("Nothing paired yet.") };
        }
    }

    if (m_Screen == Screen::BluetoothDevice) {
        out.lines = { m_PendingName };
    }

    if (m_Screen == Screen::Usb) {
        bool shareable = false;
        for (const auto& device : m_UsbDevices) {
            shareable = shareable || device.reason.isEmpty();
        }
        if (m_UsbDevices.isEmpty()) {
            out.lines = { QStringLiteral("Nothing is plugged in.") };
        }
        else if (!shareable) {
            // Otherwise this screen is a list of things saying "not offered"
            // and no indication of what would be.
            out.lines = { QStringLiteral("Nothing plugged in needs sharing."),
                          QStringLiteral("Wheels, pedals and dongles do; controllers already work.") };
        }
    }

    if (m_Screen == Screen::ConfirmShare) {
        out.lines = { m_PendingName };
        const UsbDevice* device = selectedUsb();
        if (device != nullptr) {
            out.lines.append(device->reason);
        }
    }

    if (m_Screen == Screen::UsbDevice) {
        const UsbDevice* device = selectedUsb();
        out.lines = { m_PendingName };
        if (device != nullptr && !device->reason.isEmpty()) {
            // Why it is not offered, in the words the shell menu has always
            // used: "why is my wheel not being shared" wants an answer on
            // screen, not in a log.
            out.lines.append(device->reason);
        }
        else if (device != nullptr && !device->shared && !m_UsbPaired) {
            out.lines.append(QStringLiteral("No host PC is paired, so it must be attached there by hand."));
        }
    }

    if (m_Screen == Screen::Password) {
        out.inputActive = true;
        out.inputMasked = true;
        out.inputLabel = QStringLiteral("Password for %1").arg(m_PendingSsid);
        out.inputValue = m_Password;
    }

    // Only the visible window is handed to the painter, which sizes the card
    // to what it is given -- so a scan that turns up two dozen radios makes a
    // panel that scrolls rather than one taller than the display.
    auto items = currentItems();
    int first = qBound(0, m_Top, qMax(0, items.size() - 1));
    int last = qMin(items.size(), first + k_MaxVisibleRows);

    for (int i = first; i < last; i++) {
        PanelPainter::Row row;
        // Rows arrive tab-separated so a detail -- a signal strength, whether
        // a device is connected -- can sit at the right edge instead of being
        // run into the name.
        auto parts = items.at(i).split(QChar('\t'));
        row.text = parts.value(0);
        if (parts.size() > 1) {
            row.detail = parts.mid(1).join(QChar(' ')).trimmed();
        }
        out.rows.append(row);
    }

    out.selected = m_Selected - first;
    out.hovered = m_Hovered < 0 ? -1 : m_Hovered - first;
    out.scrollAbove = first;
    out.scrollBelow = items.size() - last;

    return out;
}

bool PanelModel::handleKey(Key key)
{
    auto items = currentItems();

    switch (key) {
    case Key::Up:
        if (!items.isEmpty()) {
            m_Selected = (m_Selected + items.size() - 1) % items.size();
            ensureVisible();
        }
        break;
    case Key::Down:
        if (!items.isEmpty()) {
            m_Selected = (m_Selected + 1) % items.size();
            ensureVisible();
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
        // A device screen and the confirmation behind it belong to the
        // Bluetooth list, not to the top: Escape from "forget this?" landing
        // on the main menu would lose the list you were working through.
        goTo(m_Screen == Screen::BluetoothDevice || m_Screen == Screen::ConfirmForget
                 ? Screen::Bluetooth
                 : m_Screen == Screen::UsbDevice ? Screen::Usb
                 : m_Screen == Screen::ConfirmShare ? Screen::UsbDevice
                 : Screen::Main);
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
    int item = itemAt(row);
    if (m_Hovered == item) {
        return false;
    }

    m_Hovered = item;
    return true;
}

void PanelModel::activateRow(int row)
{
    int item = itemAt(row);
    if (item < 0 || item >= currentItems().size()) {
        return;
    }

    m_Selected = item;
    activateSelection();
}

void PanelModel::activateSelection()
{
    auto items = currentItems();
    if (m_Selected < 0 || m_Selected >= items.size()) {
        return;
    }

    QString choice = items.at(m_Selected);

    // Device rows are matched by position, before any label is compared: the
    // names come off other people's hardware, and one of them being called
    // "Back" must not make it act like the Back button.
    if (m_Screen == Screen::Bluetooth && m_BtPowered && m_Selected < m_Devices.size()) {
        const auto& device = m_Devices.at(m_Selected);
        m_PendingMac = device.mac;
        m_PendingName = device.name;
        goTo(Screen::BluetoothDevice);
        return;
    }

    if (m_Screen == Screen::Usb && m_Selected < m_UsbDevices.size()) {
        const auto& device = m_UsbDevices.at(m_Selected);
        m_PendingBusid = device.busid;
        m_PendingName = device.label;
        goTo(Screen::UsbDevice);
        return;
    }

    if (m_Screen == Screen::ConfirmForget) {
        if (choice.startsWith(QLatin1String("Yes, "))) {
            QJsonObject args;
            args["mac"] = m_PendingMac;
            ask(QStringLiteral("bluetooth.forget"), args);
            goTo(Screen::Bluetooth);
            m_Message = QStringLiteral("Forgetting %1...").arg(m_PendingName);
        }
        else {
            goTo(Screen::Bluetooth);
        }
        return;
    }

    if (choice == "Close") {
        m_CloseRequested = true;
        return;
    }

    if (choice == "Back" || choice == "Cancel") {
        if (m_Screen == Screen::Password) {
            m_Password.clear();
        }
        goTo(m_Screen == Screen::BluetoothDevice ? Screen::Bluetooth
                                                 : m_Screen == Screen::UsbDevice ? Screen::Usb
                                                 : m_Screen == Screen::ConfirmShare ? Screen::UsbDevice
                                                                                 : Screen::Main);
        return;
    }

    if (choice == "Status") {
        goTo(Screen::Status);
        ask(QStringLiteral("status"));
        m_Message = QStringLiteral("Checking...");
        return;
    }

    if (choice == "Wi-Fi networks") {
        goTo(Screen::Networks);
        m_Networks.clear();
        ask(QStringLiteral("wifi.list"));
        m_Message = QStringLiteral("Scanning...");
        return;
    }

    if (choice == "Bluetooth") {
        goTo(Screen::Bluetooth);
        ask(QStringLiteral("bluetooth.status"));
        m_Message = QStringLiteral("Checking...");
        return;
    }

    if (choice == "Look for new devices") {
        ask(QStringLiteral("bluetooth.scan"));
        m_Message = QStringLiteral("Looking for devices...");
        return;
    }

    if (choice == "Turn Bluetooth on" || choice == "Turn Bluetooth off") {
        QJsonObject args;
        args["on"] = choice.endsWith(QLatin1String("on"));
        ask(QStringLiteral("bluetooth.power"), args);
        m_Message = QStringLiteral("Just a moment...");
        return;
    }

    if (choice == "Connect" || choice == "Disconnect" || choice == "Pair and connect") {
        QJsonObject args;
        args["mac"] = m_PendingMac;
        ask(choice == "Disconnect" ? QStringLiteral("bluetooth.disconnect")
                                   : choice == "Connect" ? QStringLiteral("bluetooth.connect")
                                                         : QStringLiteral("bluetooth.pair"),
            args);

        // Straight back to the list, where the progress events land and the
        // refreshed state shows up. Waiting on the device screen would mean
        // watching a row whose label is about to stop being true.
        goTo(Screen::Bluetooth);
        m_Message = QStringLiteral("%1 %2...").arg(
            choice == "Disconnect" ? QStringLiteral("Disconnecting") : QStringLiteral("Connecting to"),
            m_PendingName);
        return;
    }

    if (choice == "Forget this device") {
        goTo(Screen::ConfirmForget);
        m_Message = QStringLiteral("You will have to pair it again.");
        return;
    }

    if (choice == "USB devices") {
        goTo(Screen::Usb);
        ask(QStringLiteral("usb.list"));
        m_Message = QStringLiteral("Looking...");
        return;
    }

    if (choice == "Share it anyway") {
        goTo(Screen::ConfirmShare);  // selection starts on "No"
        m_Message = QStringLiteral("This is the one input you have here.");
        return;
    }

    if (m_Screen == Screen::ConfirmShare) {
        if (choice.startsWith(QLatin1String("Yes, "))) {
            QJsonObject args;
            args["busid"] = m_PendingBusid;
            ask(QStringLiteral("usb.share"), args);
            goTo(Screen::Usb);
            m_Message = QStringLiteral("Sharing %1...").arg(m_PendingName);
        }
        else {
            goTo(Screen::UsbDevice);
        }
        return;
    }

    if (choice == "Share with the host PC" || choice == "Stop sharing") {
        QJsonObject args;
        args["busid"] = m_PendingBusid;
        ask(choice == "Stop sharing" ? QStringLiteral("usb.unshare")
                                     : QStringLiteral("usb.share"), args);
        goTo(Screen::Usb);
        m_Message = QStringLiteral("%1 %2...").arg(
            choice == "Stop sharing" ? QStringLiteral("Stopping") : QStringLiteral("Sharing"),
            m_PendingName);
        return;
    }

    if (choice == "Hand everything back") {
        ask(QStringLiteral("usb.handback"));
        m_Message = QStringLiteral("Asking the host PC to let go...");
        return;
    }

    if (choice == "Power") {
        goTo(Screen::Power);
        return;
    }

    if (choice == "Reboot" || choice == "Shut down") {
        m_PendingPower = choice == "Reboot" ? QStringLiteral("reboot") : QStringLiteral("poweroff");
        goTo(Screen::ConfirmPower);  // selection starts on "No"
        m_Message = QStringLiteral("The stream will end.");
        return;
    }

    if (choice == "No, go back") {
        goTo(Screen::Power);
        return;
    }

    if (choice.startsWith(QLatin1String("Yes, "))) {
        QJsonObject args;
        args["action"] = m_PendingPower;
        ask(QStringLiteral("system.power"), args);
        m_Message = QStringLiteral("Asking the system...");
        return;
    }

    if (choice == "Join") {
        QJsonObject args;
        args["ssid"] = m_PendingSsid;
        if (!m_Password.isEmpty()) {
            args["psk"] = m_Password;
        }

        ask(QStringLiteral("wifi.connect"), args);
        m_Password.clear();
        goTo(Screen::Status);
        m_Message = QStringLiteral("Joining %1...").arg(m_PendingSsid);
        return;
    }

    // A network was chosen. The label carries the signal and lock state after
    // a tab, so the SSID is the part before it.
    if (m_Screen == Screen::Networks) {
        m_PendingSsid = choice.split(QChar('\t')).value(0);
        m_Password.clear();
        goTo(Screen::Password);
    }
}

void PanelModel::applyDevices(const QJsonObject& result)
{
    if (result.contains("present")) {
        m_BtPresent = result.value("present").toBool();
        m_BtPowered = result.value("powered").toBool();
    }
    else {
        // Every other bluetooth op only answers when the radio was on, so a
        // device list is itself proof of an adapter that is powered.
        m_BtPresent = true;
        m_BtPowered = true;
    }

    m_Devices.clear();
    for (auto value : result.value("devices").toArray()) {
        auto object = value.toObject();
        Device device;
        device.mac = object.value("mac").toString();
        device.name = object.value("name").toString();
        device.paired = object.value("paired").toBool();
        device.connected = object.value("connected").toBool();
        m_Devices.append(device);
    }

    // The selection was an index into the old list, and the new one can be a
    // different length -- after forgetting a device, shorter.
    m_Selected = qBound(0, m_Selected, qMax(0, currentItems().size() - 1));
    ensureVisible();
}

void PanelModel::applyUsb(const QJsonObject& result)
{
    m_UsbPaired = result.value("paired").toBool();

    m_UsbDevices.clear();
    for (auto value : result.value("devices").toArray()) {
        auto object = value.toObject();
        UsbDevice device;
        device.busid = object.value("busid").toString();
        device.label = object.value("label").toString();
        device.reason = object.value("reason").toString();
        device.shared = object.value("shared").toBool();
        device.blocked = object.value("protected").toBool();
        m_UsbDevices.append(device);
    }

    // Plugging something in or taking it out changes the list under whatever
    // was selected, and "Hand everything back" comes and goes with it.
    m_Selected = qBound(0, m_Selected, qMax(0, currentItems().size() - 1));
    ensureVisible();
}

void PanelModel::applyReply(const QString& op, const QJsonObject& reply)
{
    if (!reply.value("ok").toBool()) {
        auto error = reply.value("error").toObject();
        auto message = error.value("message").toString();
        m_Message = message.isEmpty() ? QStringLiteral("That did not work") : message;
        return;
    }

    m_Message.clear();
    auto result = reply.value("result").toObject();

    if (op == QLatin1String("wifi.list")) {
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

    if (op.startsWith(QLatin1String("usb."))) {
        applyUsb(result);
        if (result.contains("message")) {
            m_Message = result.value("message").toString();
        }
        return;
    }

    if (op.startsWith(QLatin1String("bluetooth."))) {
        applyDevices(result);
        // An op can succeed and still have something to say -- paired but not
        // yet connected is a real state, and silence would read as failure.
        if (result.contains("message")) {
            m_Message = result.value("message").toString();
        }
        return;
    }

    // status, and wifi.connect which answers with the status it produced.
    // Named rather than left as the fallback: an untracked id would otherwise
    // blank the status lines with the fields it does not have.
    if (op != QLatin1String("status") && op != QLatin1String("wifi.connect")) {
        return;
    }

    m_StatusLines = {
        QStringLiteral("Name     %1").arg(result.value("hostname").toString()),
        QStringLiteral("Address  %1").arg(result.value("address").toString(QStringLiteral("none"))),
        QStringLiteral("Wi-Fi    %1").arg(result.value("wifi").toString(QStringLiteral("not connected"))),
    };
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
            // take(), not value(): the reply is terminal, so the request it
            // answers is done and leaving the id behind would grow the map
            // for as long as the panel is open.
            applyReply(m_Pending.take(reply.value("id").toInt()), reply);
        }
        changed = true;
    }

    return changed;
}
