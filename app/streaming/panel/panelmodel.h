#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVector>

#include "panelpainter.h"

class HelperClient;

// The settings panel's behaviour, with nothing in it that knows how it is
// drawn or where its input comes from.
//
// There are two hosts. While a stream is running the panel is an overlay
// composited into the client's own surface, driven by SDL events, because
// Qt's event loop is suspended for the whole stream and a second window
// cannot reliably be drawn over a fullscreen client. Outside a stream it is
// an ordinary window driven by Qt events.
//
// Both draw the same PanelPainter::Model with the same painter, so there is
// one appliance interface rather than two that drift. Anything toolkit
// specific belongs in a host, not here.
class PanelModel
{
public:
    enum class Mode {
        ControlCentre,
        FirstRun,
    };

    enum class Key {
        Up,
        Down,
        PageUp,
        PageDown,
        Home,
        End,
        Activate,
        Back,       // Escape: leaves the screen, or closes from the top
        Backspace,
    };

    explicit PanelModel(Mode mode = Mode::ControlCentre);
    ~PanelModel();

    // False when there is no helper to talk to, which is every machine that
    // is not a Moonlight OS appliance. Hosts should not offer a panel then.
    bool isAvailable() const;

    // Back to the top, and ask for the status that the first screen shows.
    void reset();

    PanelPainter::Model model() const;

    // Returns true when the key was consumed. Back from the top screen sets
    // closeRequested().
    bool handleKey(Key key);

    void textEntered(const QString& text);

    // Row indices in and out of this class are indices into what is *drawn*,
    // which is a window onto the item list once a list is longer than the
    // panel can show. The hosts hit-test the surface they were given and know
    // nothing about the scroll position; translating is this class's job.
    //
    // True when the hovered row actually changed. Motion events arrive
    // hundreds of times a second and a repaint is a full QPainter render, a
    // new surface and a texture upload -- redrawing per event rather than per
    // change is the difference between instant and unusable on an Atom.
    bool setHovered(int row);
    void activateRow(int row);

    // Drains helper replies. True when something changed and the host should
    // redraw -- so an idle panel costs a poll and nothing else.
    bool poll();

    // The host owns the platform's text input: SDL and Qt start and stop it
    // differently, and only one of them is right in a given session.
    bool wantsTextInput() const;

    bool closeRequested() const { return m_CloseRequested; }
    void clearCloseRequest() { m_CloseRequested = false; }

private:
    enum class Screen {
        SetupLoading,
        Welcome,
        SetupIntro,
        SetupNetwork,
        SetupComplete,
        Main,
        Streaming,
        Resolution,
        FrameRate,
        Bitrate,
        WindowMode,
        TextEntry,
        Displays,
        DisplayOutput,
        DisplayLayout,
        DisplayResolution,
        DisplayRotation,
        DisplayPosition,
        Sound,
        SoundOutput,
        SoundChannels,
        Devices,
        Trackpad,
        Battery,
        Region,
        KeyboardLayout,
        KeyboardVariant,
        TimeZoneRegion,
        TimeZoneCity,
        Network,
        Tailscale,
        ConfirmTailscaleLogout,
        Status,
        Networks,
        HiddenNetwork,
        Password,
        SavedNetworks,
        ConfirmForgetNetwork,
        RemoteAccess,
        Maintenance,
        Installer,
        ConfirmInstall,
        Diagnostics,
        VideoRecovery,
        Backup,
        ConfirmRestore,
        Power,
        ConfirmPower,
        Bluetooth,
        BluetoothDevice,
        ConfirmForget,
        Usb,
        UsbAutomatic,
        ConfirmUsbAll,
        UsbDevice,
        ConfirmShare,
    };

    struct Settings {
        QString autoconnectHost;
        QString autoconnectApp = QStringLiteral("Desktop");
        QString windowMode = QStringLiteral("borderless");
        int streamWidth = 0;
        int streamHeight = 0;
        int fps = 60;
        int bitrate = 0;
        QString audioChannels = QStringLiteral("stereo");
        bool hostAudio = false;
        bool captureSystemKeys = true;
        bool naturalScroll = false;
        bool forceSoftware = false;
    };

    struct AudioSink {
        int id = -1;
        QString name;
        bool current = false;
    };

    struct DisplayMode {
        int width = 0;
        int height = 0;
        double refresh = 0;
    };

    struct Display {
        QString name;
        QString label;
        bool active = false;
        int width = 0;
        int height = 0;
        double refresh = 0;
        QString transform = QStringLiteral("normal");
        QString side = QStringLiteral("right");
        QVector<DisplayMode> modes;
    };

    struct Device {
        QString mac;
        QString name;
        bool paired = false;
        bool connected = false;
    };

    struct UsbDevice {
        QString busid;
        QString label;
        QString reason;   // why it is not offered; empty when it is
        bool shared = false;
        bool blocked = false;  // `protected` on the wire, and a C++ keyword here

        bool operator==(const UsbDevice& other) const
        {
            return busid == other.busid && label == other.label
                   && reason == other.reason && shared == other.shared
                   && blocked == other.blocked;
        }
    };

    struct RegionChoice {
        QString code;
        QString name;
    };

    struct SavedNetwork {
        QString uuid;
        QString name;
    };

    struct BackupDestination {
        QString path;
        QString label;
        double freeBytes = 0;
    };

    struct BackupArchive {
        QString path;
        QString name;
        QStringList summary;
        bool valid = false;
    };

    struct InstallTarget {
        QString device;
        QString model;
        double sizeBytes = 0;
        QStringList contents;
    };

    struct Request {
        QString op;
        Screen screen = Screen::Main;
        quint64 generation = 0;
        bool background = false;
        QString action;
        qint64 startedMs = 0;
        int timeoutMs = 120000;
    };

    // As many rows as fit comfortably above a stream without the panel
    // becoming the screen. Anything longer scrolls.
    static const int k_MaxVisibleRows = 9;

    QStringList currentItems() const;
    void activateSelection();

    // Replies are matched to the request that caused them rather than
    // recognised by the shape of their result. Two operations answering with
    // the same field name is a question of what they mean, not of who asked --
    // and every screen added makes guessing from shape likelier to be wrong.
    int ask(const QString& op, const QJsonObject& args = {},
            const QString& workingTitle = {}, const QString& workingDetail = {},
            bool background = false);
    void applyReply(const Request& request, const QJsonObject& reply);
    void applySettings(const QJsonObject& result);
    void applyAudio(const QJsonObject& result);
    void applyDisplays(const QJsonObject& result);
    void updateSetting(const QString& key, const QJsonValue& value,
                       const QString& workingTitle);
    void applyDevices(const QJsonObject& result);
    // True when the list actually differs, so a refresh nobody asked for
    // costs a request and not a repaint.
    bool applyUsb(const QJsonObject& result);

    // Leaves a screen, resetting everything that was about the old one.
    void goTo(Screen screen);
    bool isBusy() const;
    void showNotice(PanelPainter::Tone tone, const QString& title,
                    const QString& detail = {});
    QString screenTitle() const;
    QString screenSection() const;
    Screen parentScreen() const;
    void ensureVisible();
    int itemAt(int drawnRow) const;
    const Device* selectedDevice() const;
    const UsbDevice* selectedUsb() const;
    const Display* selectedDisplay() const;

    HelperClient* m_Helper;
    Mode m_Mode;
    Screen m_Screen;
    int m_Selected;
    int m_Hovered;
    int m_Top;          // first item drawn, when the list is scrolled
    bool m_CloseRequested;

    QHash<int, Request> m_Pending;   // request id -> the page that asked
    QElapsedTimer m_RequestClock;
    quint64 m_Generation;
    int m_ActivityFrame;

    QStringList m_StatusLines;
    QStringList m_TailscaleLines;
    QString m_TailscaleState;
    QString m_TailscaleLoginUrl;
    QStringList m_Networks;
    QStringList m_DiagnosticLines;
    QVector<BackupDestination> m_ReportDestinations;
    QStringList m_RemoteLines;
    QString m_BootMode = QStringLiteral("unknown");
    bool m_Persistence = false;
    bool m_InstallAvailable = false;
    bool m_PersistenceAvailable = false;
    bool m_TerminalAvailable = false;
    bool m_SetupLive = false;
    bool m_SetupConfigured = false;
    bool m_WelcomeDone = false;
    QVector<InstallTarget> m_InstallTargets;
    QString m_PendingInstallDevice;
    QString m_PendingInstallModel;
    QStringList m_PendingInstallContents;
    PanelPainter::Notice m_Notice;

    Settings m_Settings;
    bool m_AudioAvailable = false;
    int m_Volume = 0;
    bool m_Muted = false;
    QString m_CurrentSink;
    QVector<AudioSink> m_AudioSinks;

    QVector<Display> m_Displays;
    QString m_DisplayLayout = QStringLiteral("single");
    QString m_PrimaryDisplay;
    QString m_PendingDisplay;

    QString m_PendingSsid;
    QString m_Password;
    bool m_PendingHidden = false;
    QString m_HiddenSsid;
    QVector<SavedNetwork> m_SavedNetworks;
    QString m_PendingNetworkUuid;
    QString m_PendingNetworkName;
    QString m_TextPurpose;
    QString m_TextValue;
    QString m_PendingPower;
    bool m_SshRunning = false;
    bool m_BatteryAvailable = false;
    int m_BatteryPercent = 0;
    QString m_BatteryState;
    bool m_BatteryWarnings = true;
    QVector<int> m_BatteryLevels;
    QString m_KeyboardLayout = QStringLiteral("us");
    QString m_KeyboardVariant;
    QString m_TimeZone = QStringLiteral("UTC");
    QVector<RegionChoice> m_KeyboardLayouts;
    QVector<RegionChoice> m_KeyboardVariants;
    QStringList m_TimeZoneRegions;
    QStringList m_TimeZones;
    QString m_PendingLayout;
    QString m_PendingRegion;
    QVector<BackupDestination> m_BackupDestinations;
    QVector<BackupArchive> m_BackupArchives;
    QStringList m_BackupContents;
    QString m_PendingArchive;
    QString m_PendingArchiveName;
    QStringList m_PendingArchiveSummary;

    QVector<Device> m_Devices;
    bool m_BtPresent;
    bool m_BtPowered;
    QString m_PendingMac;
    QString m_PendingName;

    // Plugging a device in is the one change to this panel that comes from
    // outside it, so the USB screen asks again while it is open rather than
    // showing whatever was true when it was entered. Nothing else needs this:
    // networks and Bluetooth devices appear because you asked them to.
    static const int k_UsbRefreshMs = 3000;

    bool usbBusy() const;

    QVector<UsbDevice> m_UsbDevices;
    bool m_UsbPaired;
    bool m_UsbAuto = false;
    QString m_UsbAutoPolicy = QStringLiteral("safe");
    QString m_PendingBusid;
    QElapsedTimer m_UsbRefresh;
    bool m_UsbChanged;   // set by applyUsb, read by poll

    // The id of a refresh nobody asked for, so its reply can be applied
    // without stepping on a message the user is still reading.
    int m_BackgroundList;
};
