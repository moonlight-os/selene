#pragma once

#include <QHash>
#include <QJsonObject>
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
    enum class Key {
        Up,
        Down,
        Activate,
        Back,       // Escape: leaves the screen, or closes from the top
        Backspace,
    };

    PanelModel();
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
        Main,
        Status,
        Networks,
        Password,
        Power,
        ConfirmPower,
        Bluetooth,
        BluetoothDevice,
        ConfirmForget,
    };

    struct Device {
        QString mac;
        QString name;
        bool paired = false;
        bool connected = false;
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
    int ask(const QString& op, const QJsonObject& args = {});
    void applyReply(const QString& op, const QJsonObject& reply);
    void applyDevices(const QJsonObject& result);

    // Leaves a screen, resetting everything that was about the old one.
    void goTo(Screen screen);
    void ensureVisible();
    int itemAt(int drawnRow) const;
    const Device* selectedDevice() const;

    HelperClient* m_Helper;
    Screen m_Screen;
    int m_Selected;
    int m_Hovered;
    int m_Top;          // first item drawn, when the list is scrolled
    bool m_CloseRequested;

    QHash<int, QString> m_Pending;   // request id -> op

    QStringList m_StatusLines;
    QStringList m_Networks;
    QString m_Message;

    QString m_PendingSsid;
    QString m_Password;
    QString m_PendingPower;

    QVector<Device> m_Devices;
    bool m_BtPresent;
    bool m_BtPowered;
    QString m_PendingMac;
    QString m_PendingName;
};
