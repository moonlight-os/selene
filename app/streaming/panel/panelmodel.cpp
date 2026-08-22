#include "panelmodel.h"
#include "helperclient.h"

#include <QCoreApplication>
#include <QJsonArray>

namespace {
QString panelText(const QString& source)
{
    if (source.isEmpty()) {
        return source;
    }
    return QCoreApplication::translate("MoonlightOSPanel", source.toUtf8().constData());
}

void localizeModel(PanelPainter::Model& model)
{
    model.title = panelText(model.title);
    model.section = panelText(model.section);
    model.hint = panelText(model.hint);
    model.inputLabel = panelText(model.inputLabel);
    model.notice.title = panelText(model.notice.title);
    model.notice.detail = panelText(model.notice.detail);
    for (QString& line : model.lines) {
        line = panelText(line);
    }
    for (PanelPainter::Row& row : model.rows) {
        row.text = panelText(row.text);
        row.detail = panelText(row.detail);
    }
}

QString freeSpaceLabel(double bytes)
{
    constexpr double mib = 1024.0 * 1024.0;
    constexpr double gib = 1024.0 * mib;
    return bytes >= gib
        ? QStringLiteral("%1 GiB free").arg(bytes / gib, 0, 'f', 1)
        : QStringLiteral("%1 MiB free").arg(bytes / mib, 0, 'f', 0);
}

QString diskSizeLabel(double bytes)
{
    constexpr double gb = 1000.0 * 1000.0 * 1000.0;
    return QStringLiteral("%1 GB").arg(bytes / gb, 0, 'f', 1);
}

int requestTimeoutMs(const QString& op)
{
    if (op == QLatin1String("backup.save") || op == QLatin1String("backup.restore")) {
        return 10 * 60 * 1000;
    }
    if (op == QLatin1String("bluetooth.scan") || op == QLatin1String("wifi.list")
            || op == QLatin1String("wifi.connect")) {
        return 90 * 1000;
    }
    return 2 * 60 * 1000;
}
}

PanelModel::PanelModel(Mode mode)
    : m_Helper(new HelperClient()),
      m_Mode(mode),
      m_Screen(mode == Mode::FirstRun ? Screen::SetupLoading : Screen::Main),
      m_Selected(0),
      m_Hovered(-1),
      m_Top(0),
      m_CloseRequested(false),
      m_Generation(0),
      m_ActivityFrame(0),
      m_BtPresent(false),
      m_BtPowered(false),
      m_UsbPaired(false),
      m_UsbChanged(false),
      m_BackgroundList(0)
{
    m_RequestClock.start();
}

bool PanelModel::usbBusy() const
{
    // A share or unshare in flight owns the screen until it answers: asking
    // for the list underneath it would race its result and overwrite the
    // progress the user is watching.
    for (const auto& request : m_Pending) {
        if (request.op.startsWith(QLatin1String("usb."))) {
            return true;
        }
    }
    return false;
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
    goTo(m_Mode == Mode::FirstRun ? Screen::SetupLoading : Screen::Main);
    m_Hovered = -1;
    m_CloseRequested = false;
    m_Password.clear();
    m_PendingHidden = false;
    m_HiddenSsid.clear();
    m_TextPurpose.clear();
    m_TextValue.clear();

    if (m_Mode == Mode::FirstRun) {
        ask(QStringLiteral("setup.status"), {}, QStringLiteral("Starting Moonlight OS"),
            QStringLiteral("Checking whether this box still needs setup."));
        ask(QStringLiteral("status"), {}, {}, {}, true);
        ask(QStringLiteral("region.status"), {}, {}, {}, true);
        ask(QStringLiteral("system.context"), {}, {}, {}, true);
        return;
    }

    // Asked on open rather than on entering the status screen: it is the
    // first thing anyone opening this wants to know, and it costs one line.
    ask(QStringLiteral("status"), {}, {}, {}, true);
    ask(QStringLiteral("settings.status"), {}, {}, {}, true);
    ask(QStringLiteral("audio.status"), {}, {}, {}, true);
    ask(QStringLiteral("battery.status"), {}, {}, {}, true);
    ask(QStringLiteral("region.status"), {}, {}, {}, true);
    ask(QStringLiteral("system.context"), {}, {}, {}, true);
}

int PanelModel::ask(const QString& op, const QJsonObject& args,
                    const QString& workingTitle, const QString& workingDetail,
                    bool background)
{
    int id = m_Helper->request(op, args);
    if (id != 0) {
        m_Pending.insert(id, { op, m_Screen, m_Generation, background, workingTitle,
                               m_RequestClock.elapsed(), requestTimeoutMs(op) });
        if (!background) {
            showNotice(PanelPainter::Tone::Working,
                       workingTitle.isEmpty() ? QStringLiteral("Working...") : workingTitle,
                       workingDetail);
            m_ActivityFrame = 0;
        }
    }
    else if (!background) {
        showNotice(PanelPainter::Tone::Error, QStringLiteral("Settings service unavailable"),
                   QStringLiteral("Restart the appliance helper, then try again."));
    }
    return id;
}

void PanelModel::goTo(Screen screen)
{
    m_Screen = screen;
    m_Generation++;
    m_Selected = 0;
    m_Top = 0;
    m_Notice = {};
}

bool PanelModel::isBusy() const
{
    for (const auto& request : m_Pending) {
        if (!request.background && request.screen == m_Screen
                && request.generation == m_Generation) {
            return true;
        }
    }
    return false;
}

void PanelModel::showNotice(PanelPainter::Tone tone, const QString& title,
                            const QString& detail)
{
    m_Notice.tone = tone;
    m_Notice.title = title;
    m_Notice.detail = detail;
}

QString PanelModel::screenTitle() const
{
    switch (m_Screen) {
    case Screen::SetupLoading: return QStringLiteral("Starting up");
    case Screen::Welcome: return QStringLiteral("Welcome to Moonlight OS");
    case Screen::SetupIntro: return QStringLiteral("Set up this Moonlight box");
    case Screen::SetupNetwork: return QStringLiteral("Connect to a network");
    case Screen::SetupComplete: return QStringLiteral("Ready to stream");
    case Screen::Main: return QStringLiteral("Control centre");
    case Screen::Streaming: return QStringLiteral("Streaming");
    case Screen::Resolution: return QStringLiteral("Resolution");
    case Screen::FrameRate: return QStringLiteral("Frame rate");
    case Screen::Bitrate: return QStringLiteral("Bitrate");
    case Screen::WindowMode: return QStringLiteral("Window mode");
    case Screen::TextEntry: return m_TextPurpose == QLatin1String("host")
        ? QStringLiteral("Automatic host") : QStringLiteral("Automatic app");
    case Screen::Displays: return QStringLiteral("Displays");
    case Screen::DisplayOutput: return m_PendingDisplay;
    case Screen::DisplayLayout: return QStringLiteral("Display layout");
    case Screen::DisplayResolution: return QStringLiteral("Display resolution");
    case Screen::DisplayRotation: return QStringLiteral("Display rotation");
    case Screen::DisplayPosition: return QStringLiteral("Display position");
    case Screen::Sound: return QStringLiteral("Sound");
    case Screen::SoundOutput: return QStringLiteral("Sound output");
    case Screen::SoundChannels: return QStringLiteral("Audio channels");
    case Screen::Devices: return QStringLiteral("Devices & input");
    case Screen::Trackpad: return QStringLiteral("Trackpad");
    case Screen::Battery: return QStringLiteral("Battery warnings");
    case Screen::Region: return QStringLiteral("Keyboard & time zone");
    case Screen::KeyboardLayout: return QStringLiteral("Keyboard layout");
    case Screen::KeyboardVariant: return QStringLiteral("Keyboard variant");
    case Screen::TimeZoneRegion: return QStringLiteral("Time-zone region");
    case Screen::TimeZoneCity: return m_PendingRegion;
    case Screen::Network: return QStringLiteral("Network & internet");
    case Screen::Tailscale: return QStringLiteral("Tailscale");
    case Screen::ConfirmTailscaleLogout: return QStringLiteral("Log out of Tailscale?");
    case Screen::Status: return QStringLiteral("This Moonlight box");
    case Screen::Networks: return QStringLiteral("Wi-Fi networks");
    case Screen::HiddenNetwork: return QStringLiteral("Hidden Wi-Fi network");
    case Screen::Password: return QStringLiteral("Join a network");
    case Screen::SavedNetworks: return QStringLiteral("Saved Wi-Fi networks");
    case Screen::ConfirmForgetNetwork: return QStringLiteral("Forget this network?");
    case Screen::RemoteAccess: return QStringLiteral("Remote access");
    case Screen::Maintenance: return QStringLiteral("System & maintenance");
    case Screen::Installer: return QStringLiteral("Install Moonlight OS");
    case Screen::ConfirmInstall: return QStringLiteral("Install Moonlight OS?");
    case Screen::Diagnostics: return QStringLiteral("Health & diagnostics");
    case Screen::VideoRecovery: return QStringLiteral("Video recovery");
    case Screen::Backup: return QStringLiteral("Settings backup");
    case Screen::ConfirmRestore: return QStringLiteral("Restore this backup?");
    case Screen::Power: return QStringLiteral("Power");
    case Screen::ConfirmPower: return QStringLiteral("Confirm power action");
    case Screen::Bluetooth: return QStringLiteral("Bluetooth");
    case Screen::BluetoothDevice: return QStringLiteral("Bluetooth device");
    case Screen::ConfirmForget: return QStringLiteral("Forget device?");
    case Screen::Usb: return QStringLiteral("USB sharing");
    case Screen::UsbAutomatic: return QStringLiteral("Automatic USB sharing");
    case Screen::ConfirmUsbAll: return QStringLiteral("Share every USB device?");
    case Screen::UsbDevice: return QStringLiteral("USB device");
    case Screen::ConfirmShare: return QStringLiteral("Share anyway?");
    }
    return QStringLiteral("Moonlight OS");
}

QString PanelModel::screenSection() const
{
    switch (m_Screen) {
    case Screen::SetupLoading:
    case Screen::Welcome:
    case Screen::SetupIntro:
    case Screen::SetupNetwork:
    case Screen::SetupComplete: return QStringLiteral("Moonlight OS setup");
    case Screen::Main: return QStringLiteral("Moonlight OS");
    case Screen::Streaming:
    case Screen::Resolution:
    case Screen::FrameRate:
    case Screen::Bitrate:
    case Screen::WindowMode: return QStringLiteral("Streaming");
    case Screen::TextEntry: return QStringLiteral("Streaming");
    case Screen::Displays:
    case Screen::DisplayOutput:
    case Screen::DisplayLayout:
    case Screen::DisplayResolution:
    case Screen::DisplayRotation: return QStringLiteral("Picture & displays");
    case Screen::DisplayPosition: return QStringLiteral("Picture & displays");
    case Screen::Sound:
    case Screen::SoundOutput:
    case Screen::SoundChannels: return QStringLiteral("Sound");
    case Screen::Network:
    case Screen::Tailscale:
    case Screen::ConfirmTailscaleLogout:
    case Screen::Networks:
    case Screen::HiddenNetwork:
    case Screen::Password:
    case Screen::SavedNetworks:
    case Screen::ConfirmForgetNetwork:
    case Screen::RemoteAccess:
    case Screen::Status: return QStringLiteral("Network & status");
    case Screen::Devices:
    case Screen::Trackpad:
    case Screen::Battery:
    case Screen::Bluetooth:
    case Screen::BluetoothDevice:
    case Screen::ConfirmForget:
    case Screen::Usb:
    case Screen::UsbAutomatic:
    case Screen::ConfirmUsbAll:
    case Screen::UsbDevice:
    case Screen::ConfirmShare: return QStringLiteral("Devices & input");
    case Screen::Region:
    case Screen::KeyboardLayout:
    case Screen::KeyboardVariant:
    case Screen::TimeZoneRegion:
    case Screen::TimeZoneCity: return m_Mode == Mode::FirstRun
        ? QStringLiteral("Moonlight OS setup")
        : QStringLiteral("Devices & input");
    case Screen::Maintenance:
    case Screen::Installer:
    case Screen::ConfirmInstall:
    case Screen::Diagnostics:
    case Screen::VideoRecovery:
    case Screen::Backup:
    case Screen::ConfirmRestore:
    case Screen::Power:
    case Screen::ConfirmPower: return QStringLiteral("System");
    }
    return {};
}

PanelModel::Screen PanelModel::parentScreen() const
{
    switch (m_Screen) {
    case Screen::SetupLoading: return Screen::SetupLoading;
    case Screen::Welcome: return Screen::Welcome;
    case Screen::SetupIntro: return m_SetupLive ? Screen::Welcome : Screen::SetupIntro;
    case Screen::SetupNetwork: return Screen::Region;
    case Screen::SetupComplete: return Screen::SetupNetwork;
    case Screen::Streaming: return Screen::Main;
    case Screen::Resolution:
    case Screen::FrameRate:
    case Screen::Bitrate:
    case Screen::WindowMode: return Screen::Streaming;
    case Screen::TextEntry: return Screen::Streaming;
    case Screen::Displays: return Screen::Main;
    case Screen::DisplayOutput:
    case Screen::DisplayLayout: return Screen::Displays;
    case Screen::DisplayResolution:
    case Screen::DisplayRotation:
    case Screen::DisplayPosition: return Screen::DisplayOutput;
    case Screen::Sound: return Screen::Main;
    case Screen::SoundOutput:
    case Screen::SoundChannels: return Screen::Sound;
    case Screen::Devices: return Screen::Main;
    case Screen::Trackpad:
    case Screen::Bluetooth:
    case Screen::Usb:
    case Screen::Battery: return Screen::Devices;
    case Screen::Region: return m_Mode == Mode::FirstRun ? Screen::SetupIntro
                                                         : Screen::Devices;
    case Screen::KeyboardLayout:
    case Screen::TimeZoneRegion: return Screen::Region;
    case Screen::KeyboardVariant: return Screen::KeyboardLayout;
    case Screen::TimeZoneCity: return Screen::TimeZoneRegion;
    case Screen::BluetoothDevice:
    case Screen::ConfirmForget: return Screen::Bluetooth;
    case Screen::UsbDevice: return Screen::Usb;
    case Screen::UsbAutomatic: return Screen::Usb;
    case Screen::ConfirmUsbAll: return Screen::UsbAutomatic;
    case Screen::ConfirmShare: return Screen::UsbDevice;
    case Screen::Network: return Screen::Main;
    case Screen::Tailscale:
    case Screen::Status:
    case Screen::RemoteAccess: return Screen::Network;
    case Screen::Networks: return m_Mode == Mode::FirstRun ? Screen::SetupNetwork
                                                           : Screen::Network;
    case Screen::HiddenNetwork: return Screen::Networks;
    case Screen::SavedNetworks: return Screen::Network;
    case Screen::ConfirmForgetNetwork: return Screen::SavedNetworks;
    case Screen::ConfirmTailscaleLogout: return Screen::Tailscale;
    case Screen::Password: return Screen::Networks;
    case Screen::Maintenance: return Screen::Main;
    case Screen::Installer: return m_Mode == Mode::FirstRun ? Screen::Welcome
                                                            : Screen::Maintenance;
    case Screen::ConfirmInstall: return Screen::Installer;
    case Screen::Diagnostics:
    case Screen::VideoRecovery:
    case Screen::Backup:
    case Screen::Power: return Screen::Maintenance;
    case Screen::ConfirmRestore: return Screen::Backup;
    case Screen::ConfirmPower: return Screen::Power;
    case Screen::Main: return Screen::Main;
    }
    return Screen::Main;
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

const PanelModel::Display* PanelModel::selectedDisplay() const
{
    for (const auto& display : m_Displays) {
        if (display.name == m_PendingDisplay) {
            return &display;
        }
    }
    return nullptr;
}

QStringList PanelModel::currentItems() const
{
    switch (m_Screen) {
    case Screen::SetupLoading:
        return {};
    case Screen::Welcome:
        return { "Try Moonlight OS from this USB", "Install Moonlight OS",
                 "Set up keyboard, clock, and network" };
    case Screen::SetupIntro:
        return { "Start setup" };
    case Screen::SetupNetwork:
        return { "Connect to Wi-Fi", "Continue without a network", "Back" };
    case Screen::SetupComplete:
        return { "Start Selene" };
    case Screen::Main:
        return { "Streaming", "Displays", "Sound", "Devices & input", "Network & internet",
                 "System & maintenance", "Close" };
    case Screen::Streaming:
    {
        QStringList items {
                 QStringLiteral("Auto-connect host\t%1").arg(
                     m_Settings.autoconnectHost.isEmpty() ? QStringLiteral("off")
                                                          : m_Settings.autoconnectHost),
                 QStringLiteral("Auto-connect app\t%1").arg(m_Settings.autoconnectApp),
        };
        if (!m_Settings.autoconnectHost.isEmpty()) {
            items.append(QStringLiteral("Stop auto-connecting"));
        }
        items.append(QStringList { QStringLiteral("Resolution\t%1").arg(
                     m_Settings.streamWidth > 0 && m_Settings.streamHeight > 0
                         ? QStringLiteral("%1x%2").arg(m_Settings.streamWidth).arg(m_Settings.streamHeight)
                         : QStringLiteral("match screen")),
                 QStringLiteral("Frame rate\t%1 fps").arg(m_Settings.fps),
                 QStringLiteral("Bitrate\t%1").arg(m_Settings.bitrate > 0
                     ? QStringLiteral("%1 Mbps").arg(m_Settings.bitrate / 1000)
                     : QStringLiteral("automatic")),
                 QStringLiteral("Window mode\t%1").arg(m_Settings.windowMode),
                 m_Settings.captureSystemKeys ? QStringLiteral("Send system keys to the Moonlight box")
                                              : QStringLiteral("Send system keys to the host PC"),
                 "Back" });
        return items;
    }
    case Screen::Resolution:
        return { "Match this screen", "1280x720", "1920x1080", "2560x1440", "3840x2160", "Back" };
    case Screen::FrameRate:
        return { "30 fps", "60 fps", "90 fps", "120 fps", "Back" };
    case Screen::Bitrate:
        return { "Automatic", "10 Mbps", "20 Mbps", "40 Mbps", "80 Mbps", "Back" };
    case Screen::WindowMode:
        return { "Borderless (recommended)", "Exclusive fullscreen", "Back" };
    case Screen::TextEntry:
        return { "Save", "Cancel" };
    case Screen::Displays: {
        QStringList items;
        if (m_Displays.size() > 1) {
            QString layout = m_DisplayLayout == QLatin1String("mirror") ? QStringLiteral("mirrored")
                             : m_DisplayLayout == QLatin1String("extend") ? QStringLiteral("extended")
                                                                          : QStringLiteral("one screen");
            items.append(QStringLiteral("Layout\t%1").arg(layout));
        }
        for (const auto& display : m_Displays) {
            QString detail = display.active && display.width > 0
                ? QStringLiteral("%1x%2").arg(display.width).arg(display.height)
                : QStringLiteral("off");
            if (display.name == m_PrimaryDisplay) detail.append(QStringLiteral(" · main"));
            items.append(QStringLiteral("%1\t%2").arg(display.name, detail));
        }
        items.append(QStringLiteral("Check again"));
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::DisplayOutput: {
        const Display* display = selectedDisplay();
        if (display == nullptr) return { "Back" };
        QStringList items {
            QStringLiteral("Resolution\t%1x%2").arg(display->width).arg(display->height),
            QStringLiteral("Rotation\t%1").arg(display->transform),
        };
        if (m_DisplayLayout == QLatin1String("extend") && display->name != m_PrimaryDisplay) {
            items.append(QStringLiteral("Position\t%1 of %2").arg(display->side, m_PrimaryDisplay));
        }
        if (display->name != m_PrimaryDisplay) items.append(QStringLiteral("Open Moonlight here"));
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::DisplayLayout: {
        QStringList items;
        for (const auto& display : m_Displays) {
            items.append(QStringLiteral("Use %1 only").arg(display.name));
        }
        items.append(QStringLiteral("Mirror every display"));
        items.append(QStringLiteral("Extend across displays"));
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::DisplayResolution: {
        QStringList items { QStringLiteral("Automatic") };
        const Display* display = selectedDisplay();
        if (display != nullptr) {
            for (const auto& mode : display->modes) {
                items.append(QStringLiteral("%1x%2\t%3 Hz")
                                 .arg(mode.width).arg(mode.height).arg(mode.refresh, 0, 'f', 2));
            }
        }
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::DisplayRotation:
        return { "Normal", "Top to the left", "Top to the right", "Upside down", "Back" };
    case Screen::DisplayPosition:
        return { "To the left", "To the right", "Above", "Below", "Back" };
    case Screen::Sound:
        if (!m_AudioAvailable) {
            return { "Back" };
        }
        return { "Louder", "Quieter", m_Muted ? QStringLiteral("Unmute") : QStringLiteral("Mute"),
                 QStringLiteral("Sound output\t%1").arg(m_CurrentSink),
                 QStringLiteral("Audio channels\t%1").arg(m_Settings.audioChannels),
                 m_Settings.hostAudio ? QStringLiteral("Play audio on this Moonlight box")
                                      : QStringLiteral("Play audio on the host PC"),
                 "Test the speakers", "Back" };
    case Screen::SoundOutput: {
        QStringList items;
        for (const auto& sink : m_AudioSinks) {
            items.append(QStringLiteral("%1\t%2").arg(sink.name,
                sink.current ? QStringLiteral("current") : QString()));
        }
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::SoundChannels:
        return { "Stereo", "5.1 surround", "7.1 surround", "Back" };
    case Screen::Devices:
    {
        QStringList items { "Bluetooth", "USB devices", "Trackpad", "Keyboard & time zone" };
        if (m_BatteryAvailable) items.append(QStringLiteral("Battery warnings"));
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::Trackpad:
        return { "Standard scrolling", "Natural scrolling", "Back" };
    case Screen::Battery:
        return { m_BatteryWarnings ? QStringLiteral("Turn warnings off")
                                   : QStringLiteral("Turn warnings on"),
                 "Warn at 20, 10, and 5%", "Warn at 15 and 5%",
                 "Show a test warning", "Back" };
    case Screen::Region: {
        QStringList items { QStringLiteral("Keyboard\t%1%2").arg(m_KeyboardLayout,
                    m_KeyboardVariant.isEmpty() ? QString() : QStringLiteral(" · %1").arg(m_KeyboardVariant)),
                 QStringLiteral("Time zone\t%1").arg(m_TimeZone), "Check again" };
        if (m_Mode == Mode::FirstRun) items.append(QStringLiteral("Continue"));
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::KeyboardLayout: {
        QStringList items;
        for (const auto& layout : m_KeyboardLayouts) {
            items.append(QStringLiteral("%1\t%2%3").arg(layout.name, layout.code,
                layout.code == m_KeyboardLayout ? QStringLiteral(" · current") : QString()));
        }
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::KeyboardVariant: {
        QStringList items { QStringLiteral("Default") };
        for (const auto& variant : m_KeyboardVariants) {
            items.append(QStringLiteral("%1\t%2").arg(variant.name, variant.code));
        }
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::TimeZoneRegion:
        return m_TimeZoneRegions + QStringList { QStringLiteral("Back") };
    case Screen::TimeZoneCity: {
        QStringList items;
        for (const auto& zone : m_TimeZones) {
            QString label = zone == m_PendingRegion ? zone : zone.mid(m_PendingRegion.size() + 1);
            items.append(QStringLiteral("%1\t%2").arg(label, zone == m_TimeZone
                ? QStringLiteral("current") : QString()));
        }
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::Network:
        return { "Status", "Wi-Fi networks", "Saved Wi-Fi networks", "Tailscale", "Remote access", "Back" };
    case Screen::Tailscale:
        if (m_TailscaleState == QLatin1String("Running")) {
            return { "Disconnect", "Log out", "Check again", "Back" };
        }
        if (m_TailscaleState == QLatin1String("Stopped")) {
            return { "Reconnect", "Log out", "Check again", "Back" };
        }
        return { "Connect / log in", "Check again", "Back" };
    case Screen::ConfirmTailscaleLogout:
        return { "No, stay logged in", "Yes, log out" };
    case Screen::Status:
        return { "Back" };
    case Screen::Networks:
        return m_Networks + QStringList { "Join a hidden network", "Back" };
    case Screen::HiddenNetwork:
        return { "Next", "Cancel" };
    case Screen::Password:
        return { "Join", "Cancel" };
    case Screen::SavedNetworks: {
        QStringList items;
        for (const auto& network : m_SavedNetworks) items.append(network.name);
        items.append(QStringLiteral("Check again"));
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::ConfirmForgetNetwork:
        return { "No, keep it", "Yes, forget this network" };
    case Screen::RemoteAccess:
        return { m_SshRunning ? QStringLiteral("Stop remote access")
                              : QStringLiteral("Start remote access"),
                 "Password setup (guarded terminal)", "Check again", "Back" };
    case Screen::Maintenance:
    {
        QStringList items { "Health & diagnostics", "Video recovery", "Settings backup" };
        if (m_InstallAvailable) items.append(QStringLiteral("Install to this computer"));
        if (m_PersistenceAvailable && !m_Persistence) {
            items.append(QStringLiteral("Save settings to this USB stick"));
        }
        if (m_TerminalAvailable) items.append(QStringLiteral("Open command line"));
        items.append(QStringLiteral("Power"));
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::Installer: {
        QStringList items;
        for (const auto& target : m_InstallTargets) {
            items.append(QStringLiteral("%1 · %2\t%3")
                .arg(target.model, target.device, diskSizeLabel(target.sizeBytes)));
        }
        items.append(QStringLiteral("Check again"));
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::ConfirmInstall:
        return { "No, keep using the live USB", "Continue to the disk installer" };
    case Screen::Diagnostics:
    {
        QStringList items;
        for (const auto& destination : m_ReportDestinations) {
            items.append(QStringLiteral("Save full report to %1\t%2")
                .arg(destination.label)
                .arg(freeSpaceLabel(destination.freeBytes)));
        }
        items.append(QStringLiteral("Check again"));
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::VideoRecovery:
        return { m_Settings.forceSoftware ? QStringLiteral("Use hardware rendering")
                                          : QStringLiteral("Force software rendering"),
                 "Back" };
    case Screen::Backup: {
        QStringList items;
        for (const auto& destination : m_BackupDestinations) {
            items.append(QStringLiteral("Save to %1\t%2")
                .arg(destination.label).arg(freeSpaceLabel(destination.freeBytes)));
        }
        for (const auto& archive : m_BackupArchives) {
            items.append(QStringLiteral("Restore %1\t%2")
                .arg(archive.name, archive.valid ? QStringLiteral("ready") : QStringLiteral("damaged")));
        }
        items.append(QStringLiteral("Check again"));
        items.append(QStringLiteral("Back"));
        return items;
    }
    case Screen::ConfirmRestore:
        return { "No, keep current settings", "Yes, restore this backup" };
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
        items.append(QStringLiteral("Automatic sharing\t%1").arg(
            !m_UsbAuto ? QStringLiteral("off")
                       : m_UsbAutoPolicy == QLatin1String("all")
                           ? QStringLiteral("everything") : QStringLiteral("safe devices")));
        items.append(QStringLiteral("Back"));
        return items;
    }

    case Screen::UsbAutomatic:
        return { "Off", "Safe devices (recommended)", "Every non-protected device", "Back" };
    case Screen::ConfirmUsbAll:
        return { "No, keep the safe policy", "Yes, share every allowed device" };

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
    return m_Screen == Screen::Password || m_Screen == Screen::TextEntry
           || m_Screen == Screen::HiddenNetwork;
}

PanelPainter::Model PanelModel::model() const
{
    PanelPainter::Model out;
    out.title = screenTitle();
    out.section = screenSection();
    out.notice = m_Notice;
    out.activityFrame = m_ActivityFrame;
    out.hint = isBusy()
        ? QStringLiteral("Esc goes back  ·  Work may finish in the background")
        : QStringLiteral("Arrows or mouse  ·  Enter to choose  ·  Esc to go back");

    if (m_Screen == Screen::SetupLoading) {
        out.lines = QStringList{ QStringLiteral("Preparing the graphical first-run experience.") };
    }

    if (m_Screen == Screen::Welcome) {
        out.lines = QStringList{
            QStringLiteral("This session is running safely from the USB stick."),
            QStringLiteral("Nothing is installed until a disk is explicitly confirmed."),
        };
    }

    if (m_Screen == Screen::SetupIntro) {
        out.lines = QStringList{
            QStringLiteral("Choose your keyboard and time zone, then connect the box."),
            QStringLiteral("Every choice can be changed later in the control centre."),
        };
    }

    if (m_Screen == Screen::SetupNetwork) {
        out.lines = m_StatusLines;
        if (out.lines.isEmpty()) {
            out.lines = QStringList{ QStringLiteral("Ethernet and remembered Wi-Fi connect automatically.") };
        }
    }

    if (m_Screen == Screen::SetupComplete) {
        out.lines = QStringList{
            QStringLiteral("Keyboard, clock, and first-run choices are saved."),
            QStringLiteral("Ctrl+Alt+M opens this control centre at any time."),
        };
    }

    if (m_Screen == Screen::Streaming) {
        out.lines = QStringList{
            m_Settings.autoconnectHost.isEmpty()
                ? QStringLiteral("Starts in the launcher; no automatic host.")
                : QStringLiteral("Starts %1 on %2.").arg(m_Settings.autoconnectApp,
                                                          m_Settings.autoconnectHost),
            QStringLiteral("Changes apply when Selene restarts."),
        };
    }

    if (m_Screen == Screen::Resolution) {
        out.lines = QStringList{ QStringLiteral("Higher resolutions use more bandwidth and decoding power.") };
    }

    if (m_Screen == Screen::Bitrate) {
        out.lines = QStringList{ QStringLiteral("Automatic is best unless a weak connection breaks the picture up.") };
    }

    if (m_Screen == Screen::WindowMode) {
        out.lines = QStringList{ QStringLiteral("Borderless avoids risky monitor mode switches on older Intel hardware.") };
    }

    if (m_Screen == Screen::Displays) {
        if (m_Displays.isEmpty()) {
            out.lines = QStringList{ QStringLiteral("No displays were reported by the graphical session."),
                          QStringLiteral("Check the cable, then look again.") };
        }
        else {
            out.lines = QStringList{ QStringLiteral("Changes apply live and are remembered after reboot.") };
        }
    }

    if (m_Screen == Screen::DisplayOutput) {
        const Display* display = selectedDisplay();
        if (display != nullptr) {
            out.lines = QStringList{ display->label == display->name
                              ? display->name
                              : QStringLiteral("%1  ·  %2").arg(display->name, display->label),
                          display->active
                              ? QStringLiteral("Active at %1 Hz").arg(display->refresh, 0, 'f', 2)
                              : QStringLiteral("Currently switched off") };
        }
    }

    if (m_Screen == Screen::DisplayLayout) {
        out.lines = QStringList{ QStringLiteral("Choose one screen, mirror the picture, or extend the desktop.") };
    }

    if (m_Screen == Screen::DisplayResolution) {
        out.lines = QStringList{ QStringLiteral("Automatic chooses the largest mode this display reports.") };
    }

    if (m_Screen == Screen::DisplayRotation) {
        out.lines = QStringList{ QStringLiteral("Use rotation when a panel is physically mounted on its side.") };
    }

    if (m_Screen == Screen::DisplayPosition) {
        out.lines = QStringList{ QStringLiteral("Place %1 relative to the main display, %2.")
                          .arg(m_PendingDisplay, m_PrimaryDisplay),
                      QStringLiteral("The extended desktop moves immediately.") };
    }

    if (m_Screen == Screen::Sound) {
        if (!m_AudioAvailable) {
            out.lines = QStringList{ QStringLiteral("Local sound controls are unavailable."),
                          QStringLiteral("PipeWire may still be starting.") };
        }
        else {
            out.lines = QStringList{ QStringLiteral("Volume  %1%2").arg(m_Volume)
                              .arg(m_Muted ? QStringLiteral("%  ·  muted") : QStringLiteral("%")) };
        }
    }

    if (m_Screen == Screen::SoundOutput && m_AudioSinks.isEmpty()) {
        out.lines = QStringList{ QStringLiteral("No sound outputs were found.") };
    }

    if (m_Screen == Screen::SoundChannels) {
        out.lines = QStringList{ QStringLiteral("Choose what the host sends; surround needs matching speakers.") };
    }

    if (m_Screen == Screen::Trackpad) {
        out.lines = QStringList{ m_Settings.naturalScroll
                          ? QStringLiteral("Current direction: natural; content follows your fingers.")
                          : QStringLiteral("Current direction: standard."),
                      QStringLiteral("The change applies to the panel and streamed pointer immediately.") };
    }

    if (m_Screen == Screen::Battery) {
        QStringList levels;
        for (int level : m_BatteryLevels) levels.append(QString::number(level));
        out.lines = QStringList{
            QStringLiteral("Battery  %1% · %2").arg(m_BatteryPercent).arg(m_BatteryState),
            m_BatteryWarnings
                ? QStringLiteral("Warnings on at %1%.").arg(levels.join(QStringLiteral("%, ")))
                : QStringLiteral("Low-battery warnings are off."),
            QStringLiteral("The final level repeats until the charger is connected."),
        };
    }

    if (m_Screen == Screen::Region) {
        out.lines = QStringList{
            QStringLiteral("Keyboard changes apply to Sway and the console immediately."),
            QStringLiteral("The clock stays synchronized over the network."),
        };
    }

    if (m_Screen == Screen::KeyboardLayout) {
        out.lines = QStringList{ QStringLiteral("Choose the markings printed on this keyboard.") };
        if (m_KeyboardLayouts.isEmpty()) {
            out.lines.append(QStringLiteral("No keyboard layouts were reported by the OS."));
        }
    }

    if (m_Screen == Screen::KeyboardVariant) {
        out.lines = QStringList{ QStringLiteral("Default is right for most keyboards."),
                      QStringLiteral("The change applies as soon as you choose.") };
    }

    if (m_Screen == Screen::TimeZoneRegion) {
        out.lines = QStringList{ QStringLiteral("First choose the broad region closest to you.") };
    }

    if (m_Screen == Screen::TimeZoneCity) {
        out.lines = QStringList{ QStringLiteral("Choose the closest city with the same local time rules.") };
    }

    if (m_Screen == Screen::Tailscale) {
        out.lines = m_TailscaleLines;
        if (!m_TailscaleLoginUrl.isEmpty()) {
            out.lines.prepend(m_TailscaleLoginUrl);
            out.lines.prepend(QStringLiteral("Approve this box on another device:"));
        }
    }

    if (m_Screen == Screen::ConfirmTailscaleLogout) {
        out.lines = QStringList{
            QStringLiteral("This removes this box from the current Tailscale account."),
            QStringLiteral("Reconnecting later requires browser approval again."),
        };
    }

    if (m_Screen == Screen::Status) {
        out.lines = m_StatusLines;
    }

    if (m_Screen == Screen::RemoteAccess) {
        out.lines = m_RemoteLines;
        if (m_RemoteLines.isEmpty()) {
            out.lines = QStringList{ QStringLiteral("Reading SSH server state...") };
        }
    }

    if (m_Screen == Screen::Maintenance) {
        out.lines = QStringList{
            m_BootMode == QLatin1String("live")
                ? m_Persistence
                    ? QStringLiteral("Live USB · settings persistence is active.")
                    : QStringLiteral("Live USB · settings are temporary until persistence is enabled.")
                : QStringLiteral("Running from the installed Moonlight OS disk."),
            QStringLiteral("Guided disk workflows open in the existing guarded terminal."),
        };
    }

    if (m_Screen == Screen::Installer) {
        if (m_InstallTargets.isEmpty()) {
        out.lines = QStringList{
                QStringLiteral("No eligible internal disk was found."),
                QStringLiteral("The live USB and disks smaller than 6 GB are never offered."),
            };
        }
        else {
        out.lines = QStringList{
                QStringLiteral("Choose the whole disk that Moonlight OS may erase."),
                QStringLiteral("The USB stick this session booted from is excluded."),
            };
        }
    }

    if (m_Screen == Screen::ConfirmInstall) {
        out.lines = QStringList{
            QStringLiteral("%1 · %2").arg(m_PendingInstallModel, m_PendingInstallDevice),
        };
        if (m_PendingInstallContents.isEmpty()) {
            out.lines.append(QStringLiteral("No existing partitions were reported."));
        }
        else {
            const int shown = qMin(3, m_PendingInstallContents.size());
            for (int i = 0; i < shown; i++) {
                out.lines.append(QStringLiteral("Existing: %1").arg(m_PendingInstallContents.at(i)));
            }
            if (m_PendingInstallContents.size() > shown) {
                out.lines.append(QStringLiteral("%1 more partitions are present.")
                                     .arg(m_PendingInstallContents.size() - shown));
            }
        }
        out.lines.append(QStringLiteral("Installation can erase this entire disk."));
        out.lines.append(QStringLiteral("The guarded installer verifies the target again."));
        out.lines.append(QStringLiteral("Nothing is written until ERASE is entered there."));
    }

    if (m_Screen == Screen::Diagnostics) {
        out.lines = m_DiagnosticLines;
        if (m_DiagnosticLines.isEmpty()) {
            out.lines = QStringList{ QStringLiteral("No health report has been read yet.") };
        }
        out.lines.append(m_ReportDestinations.isEmpty()
            ? QStringLiteral("Plug in a writable USB drive to export the full report.")
            : QStringLiteral("A full report can be saved without leaving the stream."));
    }

    if (m_Screen == Screen::VideoRecovery) {
        out.lines = QStringList{
            m_Settings.forceSoftware
                ? QStringLiteral("Software rendering is forced. It is slower, but bypasses the GPU path.")
                : QStringLiteral("Hardware rendering is enabled."),
            QStringLiteral("The change takes effect when Selene restarts."),
        };
    }

    if (m_Screen == Screen::Backup) {
        if (m_BackupDestinations.isEmpty()) {
        out.lines = QStringList{
                QStringLiteral("No writable removable drive is mounted."),
                QStringLiteral("Plug in a USB drive, then choose Check again."),
            };
        }
        else {
        out.lines = QStringList{
                QStringLiteral("Backups preserve Selene pairing and appliance settings."),
                QStringLiteral("Keep a copy somewhere other than this Moonlight box."),
            };
        }
        if (!m_BackupContents.isEmpty()) {
            out.lines.append(QStringLiteral("This box currently has %1 settings paths to save.")
                                 .arg(m_BackupContents.size()));
        }
    }

    if (m_Screen == Screen::ConfirmRestore) {
        out.lines = QStringList{ m_PendingArchiveName };
        out.lines.append(m_PendingArchiveSummary);
        out.lines.append(QStringLiteral("Matching settings on this box will be replaced."));
        out.lines.append(QStringLiteral("A reboot is required afterwards."));
    }

    if (m_Screen == Screen::Bluetooth) {
        if (!m_BtPresent) {
            out.lines = QStringList{ QStringLiteral("This machine has no Bluetooth adapter.") };
        }
        else if (!m_BtPowered) {
            out.lines = QStringList{ QStringLiteral("Bluetooth is off.") };
        }
        else if (m_Devices.isEmpty()) {
            out.lines = QStringList{ QStringLiteral("Nothing paired yet.") };
        }
    }

    if (m_Screen == Screen::BluetoothDevice) {
        out.lines = QStringList{ m_PendingName };
    }

    if (m_Screen == Screen::Usb) {
        bool shareable = false;
        for (const auto& device : m_UsbDevices) {
            shareable = shareable || device.reason.isEmpty();
        }
        if (m_UsbDevices.isEmpty()) {
            out.lines = QStringList{ QStringLiteral("Nothing is plugged in.") };
        }
        else if (!shareable) {
            // Otherwise this screen is a list of things saying "not offered"
            // and no indication of what would be.
            out.lines = QStringList{ QStringLiteral("Nothing plugged in needs sharing."),
                          QStringLiteral("Wheels, pedals and dongles do; controllers already work.") };
        }
    }

    if (m_Screen == Screen::UsbAutomatic) {
        out.lines = QStringList{
            QStringLiteral("Safe shares wheels, pedals, HOTAS hardware, and dongles."),
            QStringLiteral("Keyboards, mice, audio, webcams, storage, hubs, and the boot disk stay here."),
        };
    }

    if (m_Screen == Screen::ConfirmUsbAll) {
        out.lines = QStringList{
            QStringLiteral("This can take keyboards, mice, storage, webcams, and audio away from this box."),
            QStringLiteral("The boot disk and active network adapter remain protected."),
        };
    }

    if (m_Screen == Screen::ConfirmShare) {
        out.lines = QStringList{ m_PendingName };
        const UsbDevice* device = selectedUsb();
        if (device != nullptr) {
            out.lines.append(device->reason);
        }
    }

    if (m_Screen == Screen::UsbDevice) {
        const UsbDevice* device = selectedUsb();
        out.lines = QStringList{ m_PendingName };
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
        out.lines = QStringList{ QStringLiteral("Leave this empty only when the network has no password.") };
        out.inputActive = true;
        out.inputMasked = true;
        out.inputLabel = QStringLiteral("Password for %1").arg(m_PendingSsid);
        out.inputValue = m_Password;
    }

    if (m_Screen == Screen::HiddenNetwork) {
        out.lines = QStringList{ QStringLiteral("Enter the network name exactly; hidden networks are case-sensitive.") };
        out.inputActive = true;
        out.inputMasked = false;
        out.inputLabel = QStringLiteral("Network name (SSID)");
        out.inputValue = m_HiddenSsid;
    }

    if (m_Screen == Screen::SavedNetworks && m_SavedNetworks.isEmpty()) {
        out.lines = QStringList{ QStringLiteral("No saved Wi-Fi networks were found.") };
    }

    if (m_Screen == Screen::ConfirmForgetNetwork) {
        out.lines = QStringList{
            m_PendingNetworkName,
            QStringLiteral("This box will stop reconnecting to it automatically."),
            QStringLiteral("The current connection may remain active until it disconnects."),
        };
    }

    if (m_Screen == Screen::TextEntry) {
        out.lines = QStringList{ m_TextPurpose == QLatin1String("host")
                          ? QStringLiteral("Name or IP address of the PC to stream from.")
                          : QStringLiteral("The Sunshine app name; usually Desktop.") };
        out.inputActive = true;
        out.inputMasked = false;
        out.inputLabel = m_TextPurpose == QLatin1String("host")
            ? QStringLiteral("Host name or address") : QStringLiteral("App to launch");
        out.inputValue = m_TextValue;
    }

    // Only the visible window is handed to the painter, which sizes the card
    // to what it is given -- so a scan that turns up two dozen radios makes a
    // panel that scrolls rather than one taller than the display.
    auto items = currentItems();
    if (isBusy()) {
        // The old page is not a loading screen. Leaving its actions visible
        // makes them look usable and permits a second operation to race the
        // first, so work in flight gets an honest placeholder composition.
        out.lines.clear();
        out.loadingRows = qBound(3, items.size(), 5);
        localizeModel(out);
        return out;
    }
    int first = qBound(0, m_Top, qMax(0, items.size() - 1));
    int last = qMin(items.size(), first + k_MaxVisibleRows);

    for (int i = first; i < last; i++) {
        PanelPainter::Row row;
        // Rows arrive tab-separated so a detail -- a signal strength, whether
        // a device is connected -- can sit at the right edge instead of being
        // run into the name.
        auto parts = items.at(i).split(QChar('\t'));
        row.text = parts.value(0);
        row.destructive = row.text == QLatin1String("Shut down")
                          || row.text.startsWith(QLatin1String("Yes, shut down"))
                          || row.text == QLatin1String("Continue to the disk installer");
        if (parts.size() > 1) {
            row.detail = parts.mid(1).join(QChar(' ')).trimmed();
        }
        out.rows.append(row);
    }

    out.selected = m_Selected - first;
    out.hovered = m_Hovered < 0 ? -1 : m_Hovered - first;
    out.scrollAbove = first;
    out.scrollBelow = items.size() - last;

    localizeModel(out);
    return out;
}

bool PanelModel::handleKey(Key key)
{
    if (isBusy() && key != Key::Back) {
        return true;
    }

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
    case Key::PageUp:
        if (!items.isEmpty()) {
            m_Selected = qMax(0, m_Selected - k_MaxVisibleRows);
            ensureVisible();
        }
        break;
    case Key::PageDown:
        if (!items.isEmpty()) {
            m_Selected = qMin(items.size() - 1, m_Selected + k_MaxVisibleRows);
            ensureVisible();
        }
        break;
    case Key::Home:
        if (!items.isEmpty()) {
            m_Selected = 0;
            ensureVisible();
        }
        break;
    case Key::End:
        if (!items.isEmpty()) {
            m_Selected = items.size() - 1;
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
        else if (m_Screen == Screen::TextEntry) {
            m_TextValue.chop(1);
        }
        else if (m_Screen == Screen::HiddenNetwork) {
            m_HiddenSsid.chop(1);
        }
        break;
    case Key::Back:
        if (m_Screen == Screen::Password) {
            m_Password.clear();
            m_PendingHidden = false;
        }
        if (m_Screen == Screen::TextEntry) {
            m_TextPurpose.clear();
            m_TextValue.clear();
        }
        if (m_Screen == Screen::HiddenNetwork) {
            m_HiddenSsid.clear();
        }
        if (m_Screen == Screen::Main) {
            m_CloseRequested = true;
            break;
        }
        goTo(parentScreen());
        break;
    }

    return true;
}

void PanelModel::textEntered(const QString& text)
{
    if (m_Screen == Screen::Password && m_Password.size() < 63) {
        m_Password.append(text.left(63 - m_Password.size()));
    }
    else if (m_Screen == Screen::TextEntry && m_TextValue.size() < 255) {
        m_TextValue.append(text.left(255 - m_TextValue.size()));
    }
    else if (m_Screen == Screen::HiddenNetwork && m_HiddenSsid.size() < 32) {
        m_HiddenSsid.append(text.left(32 - m_HiddenSsid.size()));
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
    QString action = choice.section(QChar('\t'), 0, 0);

    if (m_Screen == Screen::Welcome) {
        if (action == "Install Moonlight OS") {
            goTo(Screen::Installer);
            m_InstallTargets.clear();
            ask(QStringLiteral("install.status"), {}, QStringLiteral("Looking for installable disks"),
                QStringLiteral("The live USB is excluded before anything is shown."));
        }
        else if (action == "Try Moonlight OS from this USB") {
            QJsonObject args;
            args["completed"] = true;
            ask(QStringLiteral("welcome.complete"), args,
                QStringLiteral("Preparing the live session"));
        }
        else {
            goTo(Screen::SetupIntro);
        }
        return;
    }

    if (m_Screen == Screen::SetupIntro && action == "Start setup") {
        goTo(Screen::Region);
        ask(QStringLiteral("region.status"), {}, QStringLiteral("Reading keyboard and clock"));
        return;
    }

    if (m_Screen == Screen::Region && m_Mode == Mode::FirstRun && action == "Continue") {
        goTo(Screen::SetupNetwork);
        ask(QStringLiteral("status"), {}, QStringLiteral("Checking the network"));
        return;
    }

    if (m_Screen == Screen::SetupNetwork) {
        if (action == "Connect to Wi-Fi") {
            goTo(Screen::Networks);
            ask(QStringLiteral("wifi.list"), {}, QStringLiteral("Scanning for Wi-Fi"),
                QStringLiteral("Nearby networks will appear when the scan completes."));
        }
        else if (action == "Continue without a network") {
            goTo(Screen::SetupComplete);
        }
        else {
            goTo(Screen::Region);
        }
        return;
    }

    if (m_Screen == Screen::SetupComplete && action == "Start Selene") {
        QJsonObject args;
        args["completed"] = true;
        ask(QStringLiteral("setup.complete"), args,
            QStringLiteral("Finishing setup"),
            QStringLiteral("The control centre remains available with Ctrl+Alt+M."));
        return;
    }

    if (m_Screen == Screen::Diagnostics && m_Selected < m_ReportDestinations.size()) {
        const auto& destination = m_ReportDestinations.at(m_Selected);
        QJsonObject args;
        args["destination"] = destination.path;
        ask(QStringLiteral("diagnostics.export"), args,
            QStringLiteral("Saving diagnostics to %1").arg(destination.label),
            QStringLiteral("System, graphics, display, network, and recent stream logs are included."));
        return;
    }

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

    if (m_Screen == Screen::SavedNetworks && m_Selected < m_SavedNetworks.size()) {
        const auto& network = m_SavedNetworks.at(m_Selected);
        m_PendingNetworkUuid = network.uuid;
        m_PendingNetworkName = network.name;
        goTo(Screen::ConfirmForgetNetwork);
        return;
    }

    if (m_Screen == Screen::KeyboardLayout && m_Selected < m_KeyboardLayouts.size()) {
        m_PendingLayout = m_KeyboardLayouts.at(m_Selected).code;
        QJsonObject args;
        args["layout"] = m_PendingLayout;
        ask(QStringLiteral("region.variants"), args,
            QStringLiteral("Reading keyboard variants"));
        return;
    }

    if (m_Screen == Screen::KeyboardVariant
            && m_Selected <= m_KeyboardVariants.size()) {
        QJsonObject args;
        args["layout"] = m_PendingLayout;
        args["variant"] = m_Selected == 0 ? QString()
            : m_KeyboardVariants.at(m_Selected - 1).code;
        goTo(Screen::Region);
        ask(QStringLiteral("region.keyboard"), args,
            QStringLiteral("Applying keyboard layout"),
            QStringLiteral("The keyboard changes immediately."));
        return;
    }

    if (m_Screen == Screen::TimeZoneRegion && m_Selected < m_TimeZoneRegions.size()) {
        m_PendingRegion = m_TimeZoneRegions.at(m_Selected);
        QJsonObject args;
        args["region"] = m_PendingRegion;
        ask(QStringLiteral("region.zones"), args, QStringLiteral("Reading time zones"));
        return;
    }

    if (m_Screen == Screen::TimeZoneCity && m_Selected < m_TimeZones.size()) {
        QJsonObject args;
        args["timezone"] = m_TimeZones.at(m_Selected);
        goTo(Screen::Region);
        ask(QStringLiteral("region.timezone"), args,
            QStringLiteral("Setting time zone"),
            QStringLiteral("Network time synchronization stays enabled."));
        return;
    }

    if (m_Screen == Screen::Backup && m_Selected < m_BackupDestinations.size()) {
        const auto& destination = m_BackupDestinations.at(m_Selected);
        QJsonObject args;
        args["destination"] = destination.path;
        ask(QStringLiteral("backup.save"), args,
            QStringLiteral("Saving settings to %1").arg(destination.label),
            QStringLiteral("Pairing, network, input, display, sound, and remote-access settings are included."));
        return;
    }

    if (m_Screen == Screen::Backup) {
        int archiveIndex = m_Selected - m_BackupDestinations.size();
        if (archiveIndex >= 0 && archiveIndex < m_BackupArchives.size()) {
            const auto& archive = m_BackupArchives.at(archiveIndex);
            if (!archive.valid) {
                showNotice(PanelPainter::Tone::Error, QStringLiteral("Backup cannot be restored"),
                           QStringLiteral("The archive is damaged or uses an unsupported format."));
                return;
            }
            m_PendingArchive = archive.path;
            m_PendingArchiveName = archive.name;
            m_PendingArchiveSummary = archive.summary;
            goTo(Screen::ConfirmRestore);
            return;
        }
    }

    if (m_Screen == Screen::SoundOutput && m_Selected < m_AudioSinks.size()) {
        QJsonObject args;
        args["id"] = m_AudioSinks.at(m_Selected).id;
        goTo(Screen::Sound);
        ask(QStringLiteral("audio.output"), args, QStringLiteral("Changing sound output"));
        return;
    }

    if (m_Screen == Screen::ConfirmForget) {
        if (action.startsWith(QLatin1String("Yes, "))) {
            QJsonObject args;
            args["mac"] = m_PendingMac;
            goTo(Screen::Bluetooth);
            ask(QStringLiteral("bluetooth.forget"), args,
                QStringLiteral("Forgetting %1").arg(m_PendingName),
                QStringLiteral("The pairing will be removed from this box."));
        }
        else {
            goTo(Screen::Bluetooth);
        }
        return;
    }

    if (m_Screen == Screen::ConfirmForgetNetwork) {
        if (action == "Yes, forget this network") {
            QJsonObject args;
            args["uuid"] = m_PendingNetworkUuid;
            goTo(Screen::SavedNetworks);
            ask(QStringLiteral("wifi.forget"), args,
                QStringLiteral("Forgetting %1").arg(m_PendingNetworkName));
        }
        else {
            goTo(Screen::SavedNetworks);
        }
        return;
    }

    if (action == "Close") {
        m_CloseRequested = true;
        return;
    }

    if (action == "Back" || action == "Cancel") {
        if (m_Screen == Screen::Password) {
            m_Password.clear();
            m_PendingHidden = false;
        }
        if (m_Screen == Screen::TextEntry) {
            m_TextPurpose.clear();
            m_TextValue.clear();
        }
        if (m_Screen == Screen::HiddenNetwork) {
            m_HiddenSsid.clear();
        }
        goTo(parentScreen());
        return;
    }

    if (action == "Streaming") {
        goTo(Screen::Streaming);
        ask(QStringLiteral("settings.status"), {}, QStringLiteral("Reading stream settings"));
        return;
    }

    if (m_Screen == Screen::Streaming
            && (action == "Auto-connect host" || action == "Auto-connect app")) {
        m_TextPurpose = action == "Auto-connect host" ? QStringLiteral("host")
                                                       : QStringLiteral("app");
        m_TextValue = m_TextPurpose == QLatin1String("host")
            ? m_Settings.autoconnectHost : m_Settings.autoconnectApp;
        goTo(Screen::TextEntry);
        return;
    }

    if (m_Screen == Screen::Streaming && action == "Stop auto-connecting") {
        updateSetting(QStringLiteral("autoconnect_host"), QString(),
                      QStringLiteral("Disabling automatic connection"));
        return;
    }

    if (m_Screen == Screen::TextEntry && action == "Save") {
        const QString purpose = m_TextPurpose;
        const QString value = m_TextValue.trimmed();
        m_TextPurpose.clear();
        m_TextValue.clear();
        goTo(Screen::Streaming);
        updateSetting(purpose == QLatin1String("host") ? QStringLiteral("autoconnect_host")
                                                        : QStringLiteral("autoconnect_app"),
                      value,
                      purpose == QLatin1String("host")
                          ? QStringLiteral("Saving automatic host")
                          : QStringLiteral("Saving automatic app"));
        return;
    }

    if (action == "Sound") {
        goTo(Screen::Sound);
        ask(QStringLiteral("audio.status"), {}, QStringLiteral("Reading sound controls"));
        return;
    }

    if (action == "Displays") {
        goTo(Screen::Displays);
        m_Displays.clear();
        ask(QStringLiteral("display.status"), {}, QStringLiteral("Looking for displays"));
        return;
    }

    if (m_Screen == Screen::Displays) {
        if (action == "Layout") {
            goTo(Screen::DisplayLayout);
            return;
        }
        if (action == "Check again") {
            ask(QStringLiteral("display.status"), {}, QStringLiteral("Looking again for displays"));
            return;
        }
        for (const auto& display : m_Displays) {
            if (action == display.name) {
                m_PendingDisplay = display.name;
                goTo(Screen::DisplayOutput);
                return;
            }
        }
    }

    if (m_Screen == Screen::DisplayOutput) {
        if (action == "Resolution") { goTo(Screen::DisplayResolution); return; }
        if (action == "Rotation") { goTo(Screen::DisplayRotation); return; }
        if (action == "Position") { goTo(Screen::DisplayPosition); return; }
        if (action == "Open Moonlight here") {
            QJsonObject args;
            args["layout"] = m_DisplayLayout;
            args["output"] = m_PendingDisplay;
            goTo(Screen::Displays);
            ask(QStringLiteral("display.layout"), args, QStringLiteral("Moving Moonlight"),
                QStringLiteral("The current stream keeps running until Selene restarts."));
            return;
        }
    }

    if (m_Screen == Screen::DisplayLayout) {
        QJsonObject args;
        if (action.startsWith(QLatin1String("Use ")) && action.endsWith(QLatin1String(" only"))) {
            args["layout"] = QStringLiteral("single");
            args["output"] = action.mid(4, action.size() - 9);
        }
        else if (action == "Mirror every display") {
            args["layout"] = QStringLiteral("mirror");
            args["output"] = m_PrimaryDisplay;
        }
        else if (action == "Extend across displays") {
            args["layout"] = QStringLiteral("extend");
            args["output"] = m_PrimaryDisplay;
        }
        else {
            return;
        }
        goTo(Screen::Displays);
        ask(QStringLiteral("display.layout"), args, QStringLiteral("Applying display layout"));
        return;
    }

    if (m_Screen == Screen::DisplayResolution) {
        QJsonObject args;
        args["output"] = m_PendingDisplay;
        if (action == "Automatic") {
            args["mode"] = QStringLiteral("automatic");
        }
        else {
            auto dimensions = action.split(QChar('x'));
            if (dimensions.size() != 2) return;
            QJsonObject mode;
            mode["width"] = dimensions.at(0).toInt();
            mode["height"] = dimensions.at(1).toInt();
            args["mode"] = mode;
        }
        goTo(Screen::DisplayOutput);
        ask(QStringLiteral("display.mode"), args, QStringLiteral("Applying display resolution"));
        return;
    }

    if (m_Screen == Screen::DisplayRotation) {
        QString rotation = action == "Normal" ? QStringLiteral("normal")
                           : action == "Top to the left" ? QStringLiteral("left")
                           : action == "Top to the right" ? QStringLiteral("right")
                                                            : QStringLiteral("inverted");
        QJsonObject args;
        args["output"] = m_PendingDisplay;
        args["rotation"] = rotation;
        goTo(Screen::DisplayOutput);
        ask(QStringLiteral("display.rotation"), args, QStringLiteral("Rotating display"));
        return;
    }


    if (m_Screen == Screen::DisplayPosition) {
        QString side = action == "To the left" ? QStringLiteral("left")
                       : action == "To the right" ? QStringLiteral("right")
                       : action == "Above" ? QStringLiteral("above")
                                              : QStringLiteral("below");
        QJsonObject args;
        args["output"] = m_PendingDisplay;
        args["side"] = side;
        goTo(Screen::DisplayOutput);
        ask(QStringLiteral("display.position"), args, QStringLiteral("Moving display"));
        return;
    }

    if (action == "Devices & input") {
        goTo(Screen::Devices);
        return;
    }

    if (action == "Keyboard & time zone") {
        goTo(Screen::Region);
        ask(QStringLiteral("region.status"), {}, QStringLiteral("Reading regional settings"));
        return;
    }

    if (m_Screen == Screen::Tailscale && action == "Disconnect") {
        ask(QStringLiteral("tailscale.disconnect"), {},
            QStringLiteral("Disconnecting Tailscale"),
            QStringLiteral("Local-network streaming keeps working."));
        return;
    }

    if (m_Screen == Screen::Tailscale
            && (action == "Reconnect" || action == "Connect / log in")) {
        ask(QStringLiteral("tailscale.connect"), {},
            action == "Reconnect" ? QStringLiteral("Reconnecting Tailscale")
                                    : QStringLiteral("Starting Tailscale login"),
            QStringLiteral("A login link will appear if approval is needed."));
        return;
    }

    if (m_Screen == Screen::Tailscale && action == "Log out") {
        goTo(Screen::ConfirmTailscaleLogout);
        return;
    }

    if (m_Screen == Screen::ConfirmTailscaleLogout) {
        if (action == "Yes, log out") {
            goTo(Screen::Tailscale);
            ask(QStringLiteral("tailscale.logout"), {}, QStringLiteral("Logging out of Tailscale"),
                QStringLiteral("This box will need browser approval before reconnecting."));
        }
        else {
            goTo(Screen::Tailscale);
        }
        return;
    }

    if (m_Screen == Screen::Region && action == "Keyboard") {
        goTo(Screen::KeyboardLayout);
        return;
    }

    if (m_Screen == Screen::Region && action == "Time zone") {
        goTo(Screen::TimeZoneRegion);
        return;
    }

    if (m_Screen == Screen::Region && action == "Check again") {
        ask(QStringLiteral("region.status"), {}, QStringLiteral("Reading regional settings again"));
        return;
    }

    if (action == "Network & internet") {
        goTo(Screen::Network);
        return;
    }

    if (action == "System & maintenance") {
        goTo(Screen::Maintenance);
        ask(QStringLiteral("system.context"), {}, QStringLiteral("Checking system options"));
        return;
    }

    if (m_Screen == Screen::Maintenance
            && action == "Install to this computer") {
        goTo(Screen::Installer);
        m_InstallTargets.clear();
        ask(QStringLiteral("install.status"), {}, QStringLiteral("Looking for installable disks"),
            QStringLiteral("The live USB is excluded before anything is shown."));
        return;
    }

    if (m_Screen == Screen::Installer && m_Selected < m_InstallTargets.size()) {
        const auto& target = m_InstallTargets.at(m_Selected);
        m_PendingInstallDevice = target.device;
        m_PendingInstallModel = target.model;
        m_PendingInstallContents = target.contents;
        goTo(Screen::ConfirmInstall);
        return;
    }

    if (m_Screen == Screen::Installer && action == "Check again") {
        m_InstallTargets.clear();
        ask(QStringLiteral("install.status"), {}, QStringLiteral("Looking again for installable disks"));
        return;
    }

    if (m_Screen == Screen::ConfirmInstall) {
        if (action == "Continue to the disk installer") {
            QJsonObject args;
            args["workflow"] = QStringLiteral("install");
            args["device"] = m_PendingInstallDevice;
            ask(QStringLiteral("system.launch"), args,
                QStringLiteral("Opening guarded disk installer"),
                QStringLiteral("The panel closes, then the installer opens on its own workspace."));
        }
        else {
            goTo(Screen::Maintenance);
        }
        return;
    }

    if (m_Screen == Screen::Maintenance
            && (action == "Save settings to this USB stick"
                || action == "Open command line")) {
        QJsonObject args;
        args["workflow"] = action == "Save settings to this USB stick"
            ? QStringLiteral("persist") : QStringLiteral("shell");
        ask(QStringLiteral("system.launch"), args,
            action == "Open command line" ? QStringLiteral("Opening command line")
                                              : QStringLiteral("Opening persistence setup"),
            QStringLiteral("The panel closes before the guided terminal opens."));
        return;
    }

    if (action == "Health & diagnostics") {
        goTo(Screen::Diagnostics);
        m_DiagnosticLines.clear();
        ask(QStringLiteral("system.health"), {}, QStringLiteral("Checking system health"));
        return;
    }

    if (m_Screen == Screen::Diagnostics && action == "Check again") {
        ask(QStringLiteral("system.health"), {}, QStringLiteral("Checking system health again"));
        return;
    }

    if (action == "Video recovery") {
        goTo(Screen::VideoRecovery);
        return;
    }

    if (action == "Settings backup") {
        goTo(Screen::Backup);
        ask(QStringLiteral("backup.status"), {}, QStringLiteral("Looking for backup drives"));
        return;
    }

    if (m_Screen == Screen::Backup && action == "Check again") {
        ask(QStringLiteral("backup.status"), {}, QStringLiteral("Looking again for backup drives"));
        return;
    }

    if (m_Screen == Screen::ConfirmRestore) {
        if (action == "Yes, restore this backup") {
            QJsonObject args;
            args["archive"] = m_PendingArchive;
            goTo(Screen::Backup);
            ask(QStringLiteral("backup.restore"), args,
                QStringLiteral("Restoring settings"),
                QStringLiteral("Do not unplug the drive. Reboot after this finishes."));
        }
        else {
            goTo(Screen::Backup);
        }
        return;
    }

    if (m_Screen == Screen::VideoRecovery
            && (action == "Force software rendering" || action == "Use hardware rendering")) {
        updateSetting(QStringLiteral("force_software"), !m_Settings.forceSoftware,
                      action == "Force software rendering"
                          ? QStringLiteral("Enabling software rendering")
                          : QStringLiteral("Restoring hardware rendering"));
        return;
    }

    if (action == "Remote access") {
        goTo(Screen::RemoteAccess);
        m_RemoteLines.clear();
        ask(QStringLiteral("ssh.status"), {}, QStringLiteral("Checking remote access"));
        return;
    }

    if (m_Screen == Screen::RemoteAccess && action == "Check again") {
        ask(QStringLiteral("ssh.status"), {}, QStringLiteral("Checking remote access again"));
        return;
    }

    if (m_Screen == Screen::RemoteAccess && action == "Password setup (guarded terminal)") {
        QJsonObject args;
        args["workflow"] = QStringLiteral("ssh");
        ask(QStringLiteral("system.launch"), args,
            QStringLiteral("Opening secure SSH setup"),
            QStringLiteral("The panel closes before the guided terminal opens."));
        return;
    }

    if (m_Screen == Screen::RemoteAccess
            && (action == "Start remote access" || action == "Stop remote access")) {
        QJsonObject args;
        args["enabled"] = action == "Start remote access";
        ask(QStringLiteral("ssh.enabled"), args,
            action == "Start remote access" ? QStringLiteral("Starting remote access")
                                              : QStringLiteral("Stopping remote access"),
            action == "Stop remote access"
                ? QStringLiteral("Existing SSH sessions may stay open until they disconnect.")
                : QString());
        return;
    }

    if (action == "Resolution") { goTo(Screen::Resolution); return; }
    if (action == "Frame rate") { goTo(Screen::FrameRate); return; }
    if (action == "Bitrate") { goTo(Screen::Bitrate); return; }
    if (action == "Window mode") { goTo(Screen::WindowMode); return; }

    if (m_Screen == Screen::Resolution) {
        QJsonObject resolution;
        if (action == "Match this screen") {
            resolution["automatic"] = true;
        }
        else {
            auto dimensions = action.split(QChar('x'));
            if (dimensions.size() != 2) return;
            resolution["width"] = dimensions.at(0).toInt();
            resolution["height"] = dimensions.at(1).toInt();
        }
        updateSetting(QStringLiteral("resolution"), resolution, QStringLiteral("Saving resolution"));
        return;
    }

    if (m_Screen == Screen::FrameRate && action.endsWith(QLatin1String(" fps"))) {
        updateSetting(QStringLiteral("fps"), action.section(QChar(' '), 0, 0).toInt(),
                      QStringLiteral("Saving frame rate"));
        return;
    }

    if (m_Screen == Screen::Bitrate) {
        int bitrate = action == "Automatic" ? 0 : action.section(QChar(' '), 0, 0).toInt() * 1000;
        updateSetting(QStringLiteral("bitrate"), bitrate, QStringLiteral("Saving bitrate"));
        return;
    }

    if (m_Screen == Screen::WindowMode) {
        updateSetting(QStringLiteral("window_mode"),
                      action.startsWith(QLatin1String("Borderless"))
                          ? QJsonValue(QStringLiteral("borderless"))
                          : QJsonValue(QStringLiteral("fullscreen")),
                      QStringLiteral("Saving window mode"));
        return;
    }

    if (action.startsWith(QLatin1String("Send system keys"))) {
        updateSetting(QStringLiteral("capture_system_keys"), !m_Settings.captureSystemKeys,
                      QStringLiteral("Saving keyboard routing"));
        return;
    }

    if (action == "Louder" || action == "Quieter") {
        QJsonObject args;
        args["volume"] = qBound(0, m_Volume + (action == "Louder" ? 10 : -10), 150);
        ask(QStringLiteral("audio.volume"), args,
            action == "Louder" ? QStringLiteral("Turning the volume up")
                               : QStringLiteral("Turning the volume down"));
        return;
    }

    if (action == "Mute" || action == "Unmute") {
        QJsonObject args;
        args["muted"] = action == "Mute";
        ask(QStringLiteral("audio.mute"), args,
            action == "Mute" ? QStringLiteral("Muting sound") : QStringLiteral("Restoring sound"));
        return;
    }

    if (action == "Sound output") {
        goTo(Screen::SoundOutput);
        return;
    }
    if (action == "Audio channels") { goTo(Screen::SoundChannels); return; }

    if (m_Screen == Screen::SoundChannels) {
        QString channels = action == "Stereo" ? QStringLiteral("stereo")
                           : action.startsWith(QLatin1String("5.1")) ? QStringLiteral("5.1")
                                                                     : QStringLiteral("7.1");
        updateSetting(QStringLiteral("audio_channels"), channels,
                      QStringLiteral("Saving audio channels"));
        return;
    }

    if (action.startsWith(QLatin1String("Play audio on"))) {
        updateSetting(QStringLiteral("host_audio"), !m_Settings.hostAudio,
                      QStringLiteral("Saving audio destination"));
        return;
    }

    if (action == "Test the speakers") {
        ask(QStringLiteral("audio.test"), {}, QStringLiteral("Testing the speakers"),
            QStringLiteral("A two-second tone should play on the current output."));
        return;
    }

    if (action == "Trackpad") { goTo(Screen::Trackpad); return; }
    if (m_Screen == Screen::Trackpad
            && (action == "Standard scrolling" || action == "Natural scrolling")) {
        QJsonObject args;
        args["natural"] = action == "Natural scrolling";
        ask(QStringLiteral("input.scroll"), args, QStringLiteral("Applying trackpad direction"));
        return;
    }

    if (action == "Battery warnings") {
        goTo(Screen::Battery);
        ask(QStringLiteral("battery.status"), {}, QStringLiteral("Reading battery status"));
        return;
    }

    if (m_Screen == Screen::Battery
            && (action == "Turn warnings on" || action == "Turn warnings off")) {
        QJsonObject args;
        args["warnings"] = action == "Turn warnings on";
        ask(QStringLiteral("battery.update"), args, QStringLiteral("Updating battery warnings"));
        return;
    }

    if (m_Screen == Screen::Battery && action.startsWith(QLatin1String("Warn at "))) {
        QJsonArray levels;
        if (action.contains(QLatin1String("20"))) {
            levels = QJsonArray{ 20, 10, 5 };
        }
        else {
            levels = QJsonArray{ 15, 5 };
        }
        QJsonObject args;
        args["levels"] = levels;
        ask(QStringLiteral("battery.update"), args, QStringLiteral("Saving warning levels"));
        return;
    }

    if (m_Screen == Screen::Battery && action == "Show a test warning") {
        ask(QStringLiteral("battery.test"), {}, QStringLiteral("Testing the battery warning"),
            QStringLiteral("The panel will close so the Sway notification is visible."));
        return;
    }

    if (action == "Tailscale" || action == "Check again") {
        if (m_Screen != Screen::Tailscale) goTo(Screen::Tailscale);
        ask(QStringLiteral("tailscale.status"), {}, QStringLiteral("Checking Tailscale"));
        return;
    }

    if (action == "Status") {
        goTo(Screen::Status);
        ask(QStringLiteral("status"), {}, QStringLiteral("Reading system status"));
        return;
    }

    if (action == "Wi-Fi networks") {
        goTo(Screen::Networks);
        m_Networks.clear();
        ask(QStringLiteral("wifi.list"), {}, QStringLiteral("Looking for Wi-Fi"),
            QStringLiteral("Nearby access points will appear here."));
        return;
    }

    if (action == "Saved Wi-Fi networks") {
        goTo(Screen::SavedNetworks);
        m_SavedNetworks.clear();
        ask(QStringLiteral("wifi.saved"), {}, QStringLiteral("Reading saved Wi-Fi networks"));
        return;
    }

    if (m_Screen == Screen::SavedNetworks && action == "Check again") {
        m_SavedNetworks.clear();
        ask(QStringLiteral("wifi.saved"), {}, QStringLiteral("Reading saved Wi-Fi networks again"));
        return;
    }

    if (action == "Bluetooth") {
        goTo(Screen::Bluetooth);
        ask(QStringLiteral("bluetooth.status"), {}, QStringLiteral("Checking Bluetooth"));
        return;
    }

    if (choice == "Look for new devices") {
        ask(QStringLiteral("bluetooth.scan"), {}, QStringLiteral("Looking for devices"),
            QStringLiteral("Put the device in pairing mode. This takes about 12 seconds."));
        return;
    }

    if (choice == "Turn Bluetooth on" || choice == "Turn Bluetooth off") {
        QJsonObject args;
        args["on"] = choice.endsWith(QLatin1String("on"));
        ask(QStringLiteral("bluetooth.power"), args,
            choice.endsWith(QLatin1String("on")) ? QStringLiteral("Turning Bluetooth on")
                                                  : QStringLiteral("Turning Bluetooth off"));
        return;
    }

    if (choice == "Connect" || choice == "Disconnect" || choice == "Pair and connect") {
        QJsonObject args;
        args["mac"] = m_PendingMac;
        // Straight back to the list, where the progress events land and the
        // refreshed state shows up. Waiting on the device screen would mean
        // watching a row whose label is about to stop being true.
        goTo(Screen::Bluetooth);
        ask(choice == "Disconnect" ? QStringLiteral("bluetooth.disconnect")
                                    : choice == "Connect" ? QStringLiteral("bluetooth.connect")
                                                          : QStringLiteral("bluetooth.pair"),
            args,
            QStringLiteral("%1 %2").arg(
                choice == "Disconnect" ? QStringLiteral("Disconnecting") : QStringLiteral("Connecting to"),
                m_PendingName),
            choice == "Pair and connect"
                ? QStringLiteral("Pairing, trusting, then connecting in one step.") : QString());
        return;
    }

    if (choice == "Forget this device") {
        goTo(Screen::ConfirmForget);
        showNotice(PanelPainter::Tone::Warning, QStringLiteral("This removes the pairing"),
                   QStringLiteral("You will have to pair the device again to use it here."));
        return;
    }

    if (choice == "USB devices") {
        goTo(Screen::Usb);
        ask(QStringLiteral("usb.list"), {}, QStringLiteral("Reading plugged-in USB devices"));
        m_UsbRefresh.start();
        return;
    }

    if (m_Screen == Screen::Usb && action == "Automatic sharing") {
        goTo(Screen::UsbAutomatic);
        m_Selected = !m_UsbAuto ? 0 : m_UsbAutoPolicy == QLatin1String("all") ? 2 : 1;
        return;
    }

    if (m_Screen == Screen::UsbAutomatic) {
        if (action == "Every non-protected device") {
            goTo(Screen::ConfirmUsbAll);
            showNotice(PanelPainter::Tone::Warning, QStringLiteral("Broad USB policy"),
                       QStringLiteral("Use this only when the safe policy skips hardware you need."));
            return;
        }
        if (action == "Off" || action == "Safe devices (recommended)") {
            QJsonObject args;
            args["enabled"] = action != "Off";
            args["policy"] = QStringLiteral("safe");
            goTo(Screen::Usb);
            ask(QStringLiteral("usb.autoshare"), args,
                action == "Off" ? QStringLiteral("Turning automatic sharing off")
                                : QStringLiteral("Enabling safe automatic sharing"),
                action == "Off" ? QStringLiteral("Automatically held devices will be returned.")
                                : QStringLiteral("New eligible devices will appear on the host PC."));
            return;
        }
    }

    if (m_Screen == Screen::ConfirmUsbAll) {
        if (action == "Yes, share every allowed device") {
            QJsonObject args;
            args["enabled"] = true;
            args["policy"] = QStringLiteral("all");
            goTo(Screen::Usb);
            ask(QStringLiteral("usb.autoshare"), args,
                QStringLiteral("Enabling broad automatic sharing"),
                QStringLiteral("Protected system devices will still remain on this box."));
        }
        else {
            goTo(Screen::UsbAutomatic);
            m_Selected = !m_UsbAuto ? 0 : m_UsbAutoPolicy == QLatin1String("all") ? 2 : 1;
        }
        return;
    }

    if (choice == "Share it anyway") {
        goTo(Screen::ConfirmShare);  // selection starts on "No"
        const UsbDevice* device = selectedUsb();
        const QString reason = device == nullptr ? QString() : device->reason;
        if (reason.contains(QLatin1String("keyboard or mouse"))) {
            showNotice(PanelPainter::Tone::Warning,
                       QStringLiteral("Moonlight already handles this input"),
                       QStringLiteral("Sharing it may take the only keyboard or mouse away from this box."));
        }
        else if (reason.contains(QLatin1String("storage"))) {
            showNotice(PanelPainter::Tone::Warning,
                       QStringLiteral("This storage will leave the Moonlight box"),
                       QStringLiteral("Make sure nothing here is using the drive before sharing it."));
        }
        else {
            showNotice(PanelPainter::Tone::Warning,
                       QStringLiteral("Safe sharing skips this device"),
                       reason.isEmpty() ? QStringLiteral("Only continue when the host PC needs the raw USB device.")
                                        : reason);
        }
        return;
    }

    if (m_Screen == Screen::ConfirmShare) {
        if (choice.startsWith(QLatin1String("Yes, "))) {
            QJsonObject args;
            args["busid"] = m_PendingBusid;
            goTo(Screen::Usb);
            ask(QStringLiteral("usb.share"), args,
                QStringLiteral("Sharing %1").arg(m_PendingName),
                QStringLiteral("The host PC will attach it when it answers."));
        }
        else {
            goTo(Screen::UsbDevice);
        }
        return;
    }

    if (choice == "Share with the host PC" || choice == "Stop sharing") {
        QJsonObject args;
        args["busid"] = m_PendingBusid;
        goTo(Screen::Usb);
        ask(choice == "Stop sharing" ? QStringLiteral("usb.unshare")
                                      : QStringLiteral("usb.share"), args,
            QStringLiteral("%1 %2").arg(
                choice == "Stop sharing" ? QStringLiteral("Stopping") : QStringLiteral("Sharing"),
                m_PendingName));
        return;
    }

    if (choice == "Hand everything back") {
        ask(QStringLiteral("usb.handback"), {}, QStringLiteral("Handing every USB device back"),
            QStringLiteral("Waiting for the host PC to release its attachments."));
        return;
    }

    if (choice == "Power") {
        goTo(Screen::Power);
        return;
    }

    if (choice == "Reboot" || choice == "Shut down") {
        m_PendingPower = choice == "Reboot" ? QStringLiteral("reboot") : QStringLiteral("poweroff");
        goTo(Screen::ConfirmPower);  // selection starts on "No"
        showNotice(PanelPainter::Tone::Warning, QStringLiteral("The stream will end"),
                   QStringLiteral("Unsaved work on this Moonlight box will be lost."));
        return;
    }

    if (choice == "No, go back") {
        goTo(Screen::Power);
        return;
    }

    if (choice.startsWith(QLatin1String("Yes, "))) {
        QJsonObject args;
        args["action"] = m_PendingPower;
        ask(QStringLiteral("system.power"), args,
            m_PendingPower == QLatin1String("reboot") ? QStringLiteral("Restarting Moonlight OS")
                                                       : QStringLiteral("Shutting Moonlight OS down"));
        return;
    }

    if (choice == "Join") {
        if (!m_Password.isEmpty() && m_Password.size() < 8) {
            showNotice(PanelPainter::Tone::Error, QStringLiteral("Password is too short"),
                       QStringLiteral("Use at least 8 characters, or leave it empty for an open network."));
            return;
        }
        QJsonObject args;
        args["ssid"] = m_PendingSsid;
        if (!m_Password.isEmpty()) {
            args["psk"] = m_Password;
        }
        if (m_PendingHidden) {
            args["hidden"] = true;
        }

        m_Password.clear();
        goTo(m_Mode == Mode::FirstRun ? Screen::SetupNetwork : Screen::Status);
        ask(QStringLiteral("wifi.connect"), args,
            QStringLiteral("Joining %1").arg(m_PendingSsid),
            QStringLiteral("The stream may pause while the network changes."));
        return;
    }

    if (m_Screen == Screen::Networks && action == "Join a hidden network") {
        m_HiddenSsid.clear();
        goTo(Screen::HiddenNetwork);
        return;
    }

    if (m_Screen == Screen::HiddenNetwork && action == "Next") {
        m_HiddenSsid = m_HiddenSsid.trimmed();
        if (m_HiddenSsid.isEmpty()) {
            showNotice(PanelPainter::Tone::Error, QStringLiteral("Network name is required"),
                       QStringLiteral("Enter the hidden Wi-Fi network's exact name."));
            return;
        }
        m_PendingSsid = m_HiddenSsid;
        m_PendingHidden = true;
        m_Password.clear();
        goTo(Screen::Password);
        return;
    }

    // A network was chosen. The label carries the signal and lock state after
    // a tab, so the SSID is the part before it.
    if (m_Screen == Screen::Networks) {
        m_PendingSsid = choice.split(QChar('\t')).value(0);
        m_PendingHidden = false;
        m_Password.clear();
        goTo(Screen::Password);
    }
}

void PanelModel::updateSetting(const QString& key, const QJsonValue& value,
                               const QString& workingTitle)
{
    QJsonObject values;
    values[key] = value;
    QJsonObject args;
    args["values"] = values;
    ask(QStringLiteral("settings.update"), args, workingTitle,
        QStringLiteral("The current stream keeps running while this is saved."));
}

void PanelModel::applySettings(const QJsonObject& result)
{
    m_Settings.autoconnectHost = result.value("autoconnect_host").toString();
    m_Settings.autoconnectApp = result.value("autoconnect_app").toString(QStringLiteral("Desktop"));
    m_Settings.windowMode = result.value("window_mode").toString(QStringLiteral("borderless"));
    auto resolution = result.value("resolution").toObject();
    m_Settings.streamWidth = resolution.value("width").toInt();
    m_Settings.streamHeight = resolution.value("height").toInt();
    m_Settings.fps = result.value("fps").toInt(60);
    m_Settings.bitrate = result.value("bitrate").toInt();
    m_Settings.audioChannels = result.value("audio_channels").toString(QStringLiteral("stereo"));
    m_Settings.hostAudio = result.value("host_audio").toBool();
    m_Settings.captureSystemKeys = result.value("capture_system_keys").toBool(true);
    m_Settings.naturalScroll = result.value("natural_scroll").toBool();
    m_Settings.forceSoftware = result.value("force_software").toBool();
}

void PanelModel::applyAudio(const QJsonObject& result)
{
    m_AudioAvailable = result.value("available").toBool();
    m_Volume = result.value("volume").toInt();
    m_Muted = result.value("muted").toBool();
    m_CurrentSink = result.value("current").toString(QStringLiteral("unknown"));

    m_AudioSinks.clear();
    for (auto value : result.value("sinks").toArray()) {
        auto object = value.toObject();
        AudioSink sink;
        sink.id = object.value("id").toInt(-1);
        sink.name = object.value("name").toString();
        sink.current = object.value("current").toBool();
        if (sink.id >= 0 && !sink.name.isEmpty()) {
            m_AudioSinks.append(sink);
        }
    }

    if (m_Screen == Screen::SoundOutput) {
        m_Selected = qBound(0, m_Selected, qMax(0, currentItems().size() - 1));
        ensureVisible();
    }
}

void PanelModel::applyDisplays(const QJsonObject& result)
{
    m_DisplayLayout = result.value("layout").toString(QStringLiteral("single"));
    m_PrimaryDisplay = result.value("primary").toString();
    m_Displays.clear();

    for (auto value : result.value("outputs").toArray()) {
        auto object = value.toObject();
        Display display;
        display.name = object.value("name").toString();
        display.label = object.value("label").toString(display.name);
        display.active = object.value("active").toBool();
        display.width = object.value("width").toInt();
        display.height = object.value("height").toInt();
        display.refresh = object.value("refresh").toDouble();
        display.side = object.value("side").toString(QStringLiteral("right"));
        QString transform = object.value("transform").toString(QStringLiteral("normal"));
        display.transform = transform == QLatin1String("90") ? QStringLiteral("top right")
                            : transform == QLatin1String("270") ? QStringLiteral("top left")
                            : transform == QLatin1String("180") ? QStringLiteral("upside down")
                                                                  : QStringLiteral("normal");
        for (auto modeValue : object.value("modes").toArray()) {
            auto modeObject = modeValue.toObject();
            DisplayMode mode;
            mode.width = modeObject.value("width").toInt();
            mode.height = modeObject.value("height").toInt();
            mode.refresh = modeObject.value("refresh").toDouble();
            if (mode.width > 0 && mode.height > 0) display.modes.append(mode);
        }
        if (!display.name.isEmpty()) m_Displays.append(display);
    }

    if (!m_PendingDisplay.isEmpty() && selectedDisplay() == nullptr
            && (m_Screen == Screen::DisplayOutput || m_Screen == Screen::DisplayResolution
                || m_Screen == Screen::DisplayRotation || m_Screen == Screen::DisplayPosition)) {
        goTo(Screen::Displays);
        showNotice(PanelPainter::Tone::Warning, QStringLiteral("That display was disconnected"),
                   QStringLiteral("The list has been refreshed."));
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
    if (m_Screen == Screen::Bluetooth || m_Screen == Screen::BluetoothDevice
            || m_Screen == Screen::ConfirmForget) {
        m_Selected = qBound(0, m_Selected, qMax(0, currentItems().size() - 1));
        ensureVisible();
    }
}

bool PanelModel::applyUsb(const QJsonObject& result)
{
    auto previous = m_UsbDevices;
    bool wasPaired = m_UsbPaired;
    bool wasAuto = m_UsbAuto;
    QString previousPolicy = m_UsbAutoPolicy;

    m_UsbPaired = result.value("paired").toBool();
    m_UsbAuto = result.value("autoshare").toBool();
    m_UsbAutoPolicy = result.value("autoshare_policy").toString(QStringLiteral("safe"));

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
    if (m_Screen == Screen::Usb || m_Screen == Screen::UsbDevice
            || m_Screen == Screen::ConfirmShare || m_Screen == Screen::UsbAutomatic
            || m_Screen == Screen::ConfirmUsbAll) {
        m_Selected = qBound(0, m_Selected, qMax(0, currentItems().size() - 1));
        ensureVisible();
    }

    return m_UsbDevices != previous || m_UsbPaired != wasPaired
        || m_UsbAuto != wasAuto || m_UsbAutoPolicy != previousPolicy;
}

void PanelModel::applyReply(const Request& request, const QJsonObject& reply)
{
    const QString& op = request.op;
    bool visible = !request.background && request.screen == m_Screen
                   && request.generation == m_Generation;

    if (!reply.value("ok").toBool()) {
        if (visible) {
            auto error = reply.value("error").toObject();
            auto message = error.value("message").toString();
            showNotice(PanelPainter::Tone::Error,
                       request.action.isEmpty() ? QStringLiteral("That did not work")
                                                : QStringLiteral("Could not finish that action"),
                       message.isEmpty() ? QStringLiteral("Try again. If it keeps failing, open diagnostics.")
                                         : message);
        }
        return;
    }

    if (visible) {
        m_Notice = {};
    }
    auto result = reply.value("result").toObject();

    if (op == QLatin1String("settings.status") || op == QLatin1String("settings.update")) {
        applySettings(result);
        if (visible && op == QLatin1String("settings.update")) {
            showNotice(PanelPainter::Tone::Success, QStringLiteral("Setting saved"),
                       result.value("restart_required").toBool()
                           ? QStringLiteral("Restart Selene when convenient to apply it.")
                           : QStringLiteral("It is active now."));
        }
        return;
    }

    if (op == QLatin1String("input.scroll")) {
        m_Settings.naturalScroll = result.value("natural_scroll").toBool();
        if (visible) {
            showNotice(PanelPainter::Tone::Success, QStringLiteral("Trackpad updated"),
                       result.value("applied_live").toBool()
                           ? QStringLiteral("The new direction is active now.")
                           : QStringLiteral("It will apply when the graphical session starts."));
        }
        return;
    }

    if (op.startsWith(QLatin1String("battery."))) {
        m_BatteryAvailable = result.value("available").toBool();
        m_BatteryPercent = result.value("percent").toInt();
        m_BatteryState = result.value("state").toString();
        m_BatteryWarnings = result.value("warnings").toBool(true);
        m_BatteryLevels.clear();
        for (auto value : result.value("levels").toArray()) {
            m_BatteryLevels.append(value.toInt());
        }
        if (visible && op != QLatin1String("battery.status")) {
            if (op == QLatin1String("battery.test")) {
                // The control panel is itself an overlay-layer surface and
                // therefore covers mako while it is open. Closing reveals the
                // real notification instead of claiming an invisible test
                // succeeded inside the panel.
                m_CloseRequested = true;
            }
            else {
                showNotice(PanelPainter::Tone::Success,
                           QStringLiteral("Battery warnings updated"),
                           QStringLiteral("The watcher reads the new setting immediately."));
            }
        }
        return;
    }

    if (op.startsWith(QLatin1String("audio."))) {
        applyAudio(result);
        if (visible && op != QLatin1String("audio.status")) {
            showNotice(PanelPainter::Tone::Success,
                       op == QLatin1String("audio.test") ? QStringLiteral("Test tone finished")
                                                         : QStringLiteral("Sound updated"));
        }
        return;
    }

    if (op.startsWith(QLatin1String("display."))) {
        applyDisplays(result);
        if (visible && op != QLatin1String("display.status")) {
            showNotice(PanelPainter::Tone::Success, QStringLiteral("Display updated"),
                       op == QLatin1String("display.layout")
                           ? QStringLiteral("The layout is active; restart Selene to move an existing stream.")
                           : QStringLiteral("The change is active and remembered."));
        }
        return;
    }

    if (op == QLatin1String("system.health")) {
        int uptime = result.value("uptime_seconds").toInt();
        double freeGiB = result.value("free_bytes").toDouble() / (1024.0 * 1024.0 * 1024.0);
        auto renderNodes = result.value("render_nodes").toArray();
        int notable = result.value("notable_log_lines").toInt();
        m_DiagnosticLines = QStringList{
            QStringLiteral("Boot     %1 · up %2h %3m")
                .arg(result.value("boot_mode").toString(QStringLiteral("unknown")))
                .arg(uptime / 3600).arg((uptime / 60) % 60),
            QStringLiteral("GPU      %1").arg(result.value("graphics").toString(QStringLiteral("not reported"))),
            QStringLiteral("Render   %1").arg(renderNodes.isEmpty()
                ? QStringLiteral("no render node")
                : QStringLiteral("%1 available").arg(renderNodes.size())),
            QStringLiteral("Storage  %1 GiB free").arg(freeGiB, 0, 'f', 1),
            QStringLiteral("Session  %1").arg(result.value("session_log").toBool()
                ? QStringLiteral("log present · %1 notable lines").arg(notable)
                : QStringLiteral("no log yet")),
            QStringLiteral("Video    %1 rendering").arg(result.value("force_software").toBool()
                ? QStringLiteral("software") : QStringLiteral("hardware")),
        };
        m_ReportDestinations.clear();
        for (auto value : result.value("report_destinations").toArray()) {
            auto item = value.toObject();
            BackupDestination destination {
                item.value("path").toString(), item.value("label").toString(),
                item.value("free_bytes").toDouble(),
            };
            if (!destination.path.isEmpty()) m_ReportDestinations.append(destination);
        }
        if (visible && (renderNodes.isEmpty() || freeGiB < 0.5)) {
            showNotice(PanelPainter::Tone::Warning, QStringLiteral("System needs attention"),
                       renderNodes.isEmpty() ? QStringLiteral("No GPU render node is available.")
                                             : QStringLiteral("Less than 500 MB of storage is free."));
        }
        return;
    }

    if (op == QLatin1String("setup.status") || op == QLatin1String("setup.complete")
            || op == QLatin1String("welcome.complete")) {
        m_SetupConfigured = result.value("configured").toBool();
        m_SetupLive = result.value("live").toBool();
        m_WelcomeDone = result.value("welcome_done").toBool();

        if (visible && op == QLatin1String("setup.complete")) {
            m_CloseRequested = true;
        }
        else if (visible && op == QLatin1String("welcome.complete")) {
            if (m_SetupConfigured) m_CloseRequested = true;
            else goTo(Screen::SetupIntro);
        }
        else if (visible) {
            if (m_SetupLive && !m_WelcomeDone) goTo(Screen::Welcome);
            else if (!m_SetupConfigured) goTo(Screen::SetupIntro);
            else m_CloseRequested = true;
        }
        return;
    }

    if (op == QLatin1String("system.context")) {
        m_BootMode = result.value("boot_mode").toString(QStringLiteral("unknown"));
        m_Persistence = result.value("persistence").toBool();
        m_InstallAvailable = result.value("install_available").toBool();
        m_PersistenceAvailable = result.value("persistence_available").toBool();
        m_TerminalAvailable = result.value("terminal_available").toBool();
        return;
    }

    if (op == QLatin1String("install.status")) {
        m_InstallTargets.clear();
        for (auto value : result.value("targets").toArray()) {
            auto item = value.toObject();
            InstallTarget target;
            target.device = item.value("device").toString();
            target.model = item.value("model").toString(QStringLiteral("Unknown disk"));
            target.sizeBytes = item.value("size_bytes").toDouble();
            for (auto content : item.value("contents").toArray()) {
                target.contents.append(content.toString());
            }
            if (!target.device.isEmpty()) m_InstallTargets.append(target);
        }
        if (visible && m_InstallTargets.isEmpty()) {
            showNotice(PanelPainter::Tone::Warning, QStringLiteral("No installable disk found"),
                       result.value("reason").toString(
                           QStringLiteral("Only the live USB is present, or every other disk is too small.")));
        }
        return;
    }

    if (op == QLatin1String("system.launch")) {
        if (visible) m_CloseRequested = true;
        return;
    }

    if (op == QLatin1String("diagnostics.export")) {
        if (visible) {
            showNotice(PanelPainter::Tone::Success, QStringLiteral("Diagnostic report saved"),
                       result.value("saved_to").toString());
        }
        return;
    }

    if (op == QLatin1String("ssh.status") || op == QLatin1String("ssh.enabled")) {
        m_SshRunning = result.value("running").toBool();
        m_RemoteLines = QStringList{
            QStringLiteral("SSH server  %1").arg(m_SshRunning ? QStringLiteral("running")
                                                              : QStringLiteral("stopped")),
            QStringLiteral("Login       %1").arg(result.value("password_logins").toBool()
                ? QStringLiteral("keys or password") : QStringLiteral("keys only")),
            QStringLiteral("Keys        %1 installed").arg(result.value("authorized_keys").toInt()),
            QStringLiteral("Connect     ssh moonlight@%1.local")
                .arg(result.value("hostname").toString(QStringLiteral("moonlight-os"))),
            QStringLiteral("Password    Setup opens a guarded terminal"),
        };
        auto addresses = result.value("addresses").toArray();
        if (!addresses.isEmpty()) {
            m_RemoteLines.append(QStringLiteral("Address     ssh moonlight@%1")
                                     .arg(addresses.first().toString()));
        }
        auto fingerprints = result.value("host_fingerprints").toArray();
        if (!fingerprints.isEmpty()) {
            const QString fingerprint = fingerprints.first().toString();
            const QStringList fields = fingerprint.split(QChar(' '), Qt::SkipEmptyParts);
            if (fields.size() >= 2 && fields.at(1).startsWith(QLatin1String("SHA256:"))) {
                QString algorithm;
                int algorithmStart = fingerprint.lastIndexOf(QChar('('));
                if (algorithmStart >= 0 && fingerprint.endsWith(QChar(')'))) {
                    algorithm = fingerprint.mid(algorithmStart + 1,
                                                fingerprint.size() - algorithmStart - 2);
                }
                m_RemoteLines.append(QStringLiteral("Host key    %1%2")
                    .arg(algorithm.isEmpty() ? QStringLiteral("SSH") : algorithm,
                         fields.first().isEmpty() ? QString()
                                                  : QStringLiteral(" · %1 bit").arg(fields.first())));
                m_RemoteLines.append(QStringLiteral("            %1").arg(fields.at(1)));
            }
            else {
                m_RemoteLines.append(QStringLiteral("Host key    %1").arg(fingerprint));
            }
            if (fingerprints.size() > 1) {
                m_RemoteLines.append(QStringLiteral("            %1 more host-key fingerprints")
                                         .arg(fingerprints.size() - 1));
            }
        }
        if (visible && op == QLatin1String("ssh.enabled")) {
            showNotice(PanelPainter::Tone::Success,
                       m_SshRunning ? QStringLiteral("Remote access started")
                                    : QStringLiteral("Remote access stopped"),
                       m_SshRunning ? QStringLiteral("Key-based SSH connections are accepted now.")
                                    : QStringLiteral("It will start again on the next boot."));
        }
        return;
    }

    if (op == QLatin1String("region.variants")) {
        m_KeyboardVariants.clear();
        for (auto value : result.value("variants").toArray()) {
            auto item = value.toObject();
            RegionChoice choice { item.value("code").toString(), item.value("name").toString() };
            if (!choice.code.isEmpty()) m_KeyboardVariants.append(choice);
        }
        if (visible) goTo(Screen::KeyboardVariant);
        return;
    }

    if (op == QLatin1String("region.zones")) {
        m_TimeZones.clear();
        for (auto value : result.value("zones").toArray()) {
            const QString zone = value.toString();
            if (!zone.isEmpty()) m_TimeZones.append(zone);
        }
        if (visible) goTo(Screen::TimeZoneCity);
        return;
    }

    if (op == QLatin1String("region.status")
            || op == QLatin1String("region.keyboard")
            || op == QLatin1String("region.timezone")) {
        m_KeyboardLayout = result.value("layout").toString(QStringLiteral("us"));
        m_KeyboardVariant = result.value("variant").toString();
        m_TimeZone = result.value("timezone").toString(QStringLiteral("UTC"));
        m_KeyboardLayouts.clear();
        for (auto value : result.value("layouts").toArray()) {
            auto item = value.toObject();
            RegionChoice choice { item.value("code").toString(), item.value("name").toString() };
            if (!choice.code.isEmpty()) m_KeyboardLayouts.append(choice);
        }
        m_KeyboardVariants.clear();
        for (auto value : result.value("variants").toArray()) {
            auto item = value.toObject();
            RegionChoice choice { item.value("code").toString(), item.value("name").toString() };
            if (!choice.code.isEmpty()) m_KeyboardVariants.append(choice);
        }
        m_TimeZoneRegions.clear();
        for (auto value : result.value("regions").toArray()) {
            const QString region = value.toString();
            if (!region.isEmpty()) m_TimeZoneRegions.append(region);
        }
        if (visible && op != QLatin1String("region.status")) {
            showNotice(PanelPainter::Tone::Success,
                       op == QLatin1String("region.keyboard")
                           ? QStringLiteral("Keyboard updated")
                           : QStringLiteral("Time zone updated"),
                       QStringLiteral("The change is active and remembered."));
        }
        return;
    }

    if (op.startsWith(QLatin1String("backup."))) {
        m_BackupDestinations.clear();
        for (auto value : result.value("destinations").toArray()) {
            auto item = value.toObject();
            BackupDestination destination {
                item.value("path").toString(), item.value("label").toString(),
                item.value("free_bytes").toDouble(),
            };
            if (!destination.path.isEmpty()) m_BackupDestinations.append(destination);
        }
        m_BackupArchives.clear();
        for (auto value : result.value("archives").toArray()) {
            auto item = value.toObject();
            BackupArchive archive;
            archive.path = item.value("path").toString();
            archive.name = item.value("name").toString();
            archive.valid = item.value("valid").toBool();
            for (auto line : item.value("summary").toArray()) {
                if (!line.toString().isEmpty()) archive.summary.append(line.toString());
            }
            if (!archive.path.isEmpty()) m_BackupArchives.append(archive);
        }
        m_BackupContents.clear();
        for (auto value : result.value("saved_paths").toArray()) {
            if (!value.toString().isEmpty()) m_BackupContents.append(value.toString());
        }
        if (visible && op == QLatin1String("backup.save")) {
            showNotice(PanelPainter::Tone::Success, QStringLiteral("Settings backup saved"),
                       result.value("saved_to").toString());
        }
        else if (visible && op == QLatin1String("backup.restore")) {
            showNotice(PanelPainter::Tone::Success, QStringLiteral("Settings restored"),
                       QStringLiteral("Reboot this Moonlight box to apply everything."));
        }
        else if (visible && m_BackupDestinations.isEmpty()) {
            showNotice(PanelPainter::Tone::Warning, QStringLiteral("No backup drive found"),
                       QStringLiteral("Mount a writable USB drive and check again."));
        }
        return;
    }

    if (op.startsWith(QLatin1String("tailscale."))) {
        m_TailscaleLines.clear();
        m_TailscaleState = result.value("state").toString(QStringLiteral("unknown"));
        m_TailscaleLoginUrl = result.value("login_url").toString();
        QString name = result.value("name").toString();
        m_TailscaleLines.append(QStringLiteral("State    %1").arg(m_TailscaleState));
        if (!name.isEmpty()) {
            m_TailscaleLines.append(QStringLiteral("Name     %1").arg(name));
        }
        auto addresses = result.value("addresses").toArray();
        for (auto address : addresses) {
            m_TailscaleLines.append(QStringLiteral("Address  %1").arg(address.toString()));
        }
        auto peers = result.value("peers").toArray();
        int online = 0;
        for (auto value : peers) online += value.toObject().value("online").toBool() ? 1 : 0;
        if (!peers.isEmpty()) {
            m_TailscaleLines.append(QStringLiteral("Peers    %1 online · %2 known")
                                        .arg(online).arg(peers.size()));
            int shown = 0;
            for (auto value : peers) {
                auto peer = value.toObject();
                if (!peer.value("online").toBool()) continue;
                if (shown == 3) break;
                m_TailscaleLines.append(QStringLiteral("  %1  %2")
                    .arg(peer.value("name").toString(), peer.value("address").toString()));
                shown++;
            }
            if (online > shown) {
                m_TailscaleLines.append(QStringLiteral("  %1 more online").arg(online - shown));
            }
        }
        if (visible && op != QLatin1String("tailscale.status")) {
            if (!m_TailscaleLoginUrl.isEmpty()) {
                showNotice(PanelPainter::Tone::Warning, QStringLiteral("Approval required"),
                           QStringLiteral("Open the login link shown above on another device."));
            }
            else {
                showNotice(PanelPainter::Tone::Success, QStringLiteral("Tailscale updated"),
                           m_TailscaleState == QLatin1String("Running")
                               ? QStringLiteral("Remote streaming is connected again.")
                               : QStringLiteral("The local network still works."));
            }
        }
        else if (visible && m_TailscaleState != QLatin1String("Running")) {
            showNotice(PanelPainter::Tone::Warning, QStringLiteral("Tailscale is not connected"),
                       QStringLiteral("Remote streaming will use the local network only."));
        }
        return;
    }

    if (op == QLatin1String("wifi.list")) {
        m_Networks.clear();
        for (auto value : result.value("networks").toArray()) {
            auto network = value.toObject();
            m_Networks.append(QStringLiteral("%1\t%2%\t%3")
                                  .arg(network.value("ssid").toString())
                                  .arg(network.value("signal").toInt())
                                  .arg(network.value("secure").toBool() ? "locked" : ""));
        }
        if (visible && m_Networks.isEmpty()) {
            showNotice(PanelPainter::Tone::Warning, QStringLiteral("No Wi-Fi networks found"),
                       QStringLiteral("Move closer to the access point, then scan again."));
        }
        return;
    }

    if (op == QLatin1String("wifi.saved") || op == QLatin1String("wifi.forget")) {
        m_SavedNetworks.clear();
        for (auto value : result.value("connections").toArray()) {
            auto item = value.toObject();
            SavedNetwork network { item.value("uuid").toString(), item.value("name").toString() };
            if (!network.uuid.isEmpty() && !network.name.isEmpty()) m_SavedNetworks.append(network);
        }
        if (visible && op == QLatin1String("wifi.forget")) {
            showNotice(PanelPainter::Tone::Success, QStringLiteral("Saved network forgotten"),
                       QStringLiteral("This box will no longer reconnect to it automatically."));
        }
        return;
    }

    if (op.startsWith(QLatin1String("usb."))) {
        m_UsbChanged = applyUsb(result);
        if (visible && result.contains("message")) {
            showNotice(PanelPainter::Tone::Warning, QStringLiteral("USB state changed"),
                       result.value("message").toString());
        }
        else if (visible && op != QLatin1String("usb.list")) {
            showNotice(PanelPainter::Tone::Success,
                       op == QLatin1String("usb.share") ? QStringLiteral("Device shared")
                       : op == QLatin1String("usb.unshare") ? QStringLiteral("Device returned")
                       : op == QLatin1String("usb.autoshare") ? QStringLiteral("Automatic sharing updated")
                                                            : QStringLiteral("Everything handed back"));
        }
        return;
    }

    if (op.startsWith(QLatin1String("bluetooth."))) {
        applyDevices(result);
        // An op can succeed and still have something to say -- paired but not
        // yet connected is a real state, and silence would read as failure.
        if (visible && result.contains("message")) {
            showNotice(PanelPainter::Tone::Warning, QStringLiteral("Bluetooth needs attention"),
                       result.value("message").toString());
        }
        else if (visible && op != QLatin1String("bluetooth.status")
                 && op != QLatin1String("bluetooth.scan")) {
            showNotice(PanelPainter::Tone::Success, QStringLiteral("Bluetooth updated"));
        }
        return;
    }

    // status, and wifi.connect which answers with the status it produced.
    // Named rather than left as the fallback: an untracked id would otherwise
    // blank the status lines with the fields it does not have.
    if (op != QLatin1String("status") && op != QLatin1String("wifi.connect")) {
        return;
    }

    m_StatusLines = QStringList{
        QStringLiteral("Name     %1").arg(result.value("hostname").toString()),
        QStringLiteral("Address  %1").arg(result.value("address").toString(QStringLiteral("none"))),
        QStringLiteral("Wi-Fi    %1").arg(result.value("wifi").toString(QStringLiteral("not connected"))),
    };
    if (visible && op == QLatin1String("wifi.connect")) {
        showNotice(PanelPainter::Tone::Success, QStringLiteral("Connected"),
                   QStringLiteral("This box is now using %1.").arg(
                       result.value("wifi").toString(QStringLiteral("the selected network"))));
    }
}

bool PanelModel::poll()
{
    bool changed = false;
    QJsonObject reply;

    while (m_Helper->takeReply(reply)) {
        int id = reply.value("id").toInt();

        // Progress events carry a message and no verdict; showing them is the
        // difference between "scanning" and an apparently frozen panel.
        if (reply.value("event").toString() == QLatin1String("progress")) {
            auto request = m_Pending.value(id);
            if (!request.background && request.screen == m_Screen
                    && request.generation == m_Generation) {
                QString detail = reply.value("message").toString();
                showNotice(PanelPainter::Tone::Working,
                           request.action.isEmpty() ? QStringLiteral("Working...") : request.action,
                           detail);
                changed = true;
            }
            continue;
        }

        // take(), not value(): the reply is terminal, so the request it
        // answers is done and leaving the id behind would grow the map
        // for as long as the panel is open.
        auto pending = m_Pending.find(id);
        if (pending == m_Pending.end()) {
            // A late terminal reply for a request already timed out cannot
            // revive its old screen or replace the timeout explanation.
            continue;
        }
        Request request = m_Pending.take(id);

        bool background = request.background || (id != 0 && id == m_BackgroundList);
        if (background) {
            m_BackgroundList = 0;

            // A refresh nobody asked for says nothing when it fails: the
            // device is simply still listed as it was, which is true, and an
            // error appearing on its own every three seconds would be worse
            // than the stale row it is complaining about.
            if (!reply.value("ok").toBool()) {
                continue;
            }
        }

        PanelPainter::Notice showing = m_Notice;
        m_UsbChanged = true;
        applyReply(request, reply);
        if (background) {
            m_Notice = showing;
            // Nothing was plugged in or taken out, so there is nothing new to
            // draw. Repainting anyway would have the panel redrawing itself
            // every three seconds for as long as the screen is open.
            changed = changed || m_UsbChanged;
            continue;
        }

        changed = true;
    }

    // A helper can remain connected while an underlying tool wedges. Every
    // normal handler has its own subprocess timeout, but this client-side
    // ceiling is the final guarantee that no page can load forever.
    const qint64 now = m_RequestClock.elapsed();
    for (auto it = m_Pending.begin(); it != m_Pending.end(); ) {
        const Request request = it.value();
        if (now - request.startedMs <= request.timeoutMs) {
            ++it;
            continue;
        }

        const int id = it.key();
        it = m_Pending.erase(it);
        if (id == m_BackgroundList) m_BackgroundList = 0;
        if (!request.background && request.screen == m_Screen
                && request.generation == m_Generation) {
            showNotice(PanelPainter::Tone::Error, QStringLiteral("That took too long"),
                       QStringLiteral("The panel stopped waiting. Check the current state before trying again."));
            changed = true;
        }
    }

    // Devices arrive and leave while the screen is open -- that is the whole
    // reason this screen exists -- so it asks again rather than showing what
    // was true when it was entered.
    if (m_Screen == Screen::Usb && m_BackgroundList == 0 && !usbBusy()
            && m_UsbRefresh.isValid() && m_UsbRefresh.elapsed() >= k_UsbRefreshMs) {
        m_BackgroundList = ask(QStringLiteral("usb.list"), {}, {}, {}, true);
        m_UsbRefresh.restart();
    }

    if (isBusy()) {
        m_ActivityFrame = (m_ActivityFrame + 1) & 7;
        changed = true;
    }

    return changed;
}
