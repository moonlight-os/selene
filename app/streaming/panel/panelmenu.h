#pragma once

#include <QJsonObject>
#include <QString>
#include <QPoint>
#include <QStringList>

#include "SDL_compat.h"
#include "panelmodel.h"
#include "panelpainter.h"


// The Moonlight OS settings panel, drawn over the stream by Selene itself.
//
// It replaces an external terminal running whiptail, and the reason that had
// to go is not tidiness: a foreign window cannot reliably be composited over a
// fullscreen client, so the old panel needed a workspace of its own, a lock
// file, stale-lock recovery, and a focus and cursor hand-off on the way back.
// A panel drawn into the client's own surface needs none of those, because
// there is no second window to place, raise or focus.
//
// It is text, rendered through the same overlay path as the stats overlay --
// which already wraps multi-line UTF-8 -- so no new rendering machinery
// exists. That also means it works under every video renderer rather than
// only the ones we remembered to teach.
class PanelMenu
{
public:
    PanelMenu();
    ~PanelMenu();

    bool isOpen() const { return m_Open; }

    // Ask the appliance to hand Ctrl+Alt+M over for the duration of a stream,
    // and to take it back afterwards. No-ops anywhere there is no appliance.
    void claimHotkey();
    void releaseHotkey();

    // False when there is nothing to open -- no helper, so no appliance to
    // configure. The caller should then leave the key alone rather than
    // showing an empty panel.
    bool open();
    void close();

    // Returns true when the key was the panel's. While open it takes every
    // key, so a menu keystroke cannot also reach the host -- arrowing through
    // Wi-Fi networks must not walk a game's inventory at the same time.
    bool handleKey(const SDL_KeyboardEvent* event);

    // Mouse and touch. Returns true when the event was the panel's, so it
    // never also reaches the host -- clicking a menu row must not shoot.
    bool handleMouseMotion(int x, int y);
    bool handleMouseButton(const SDL_MouseButtonEvent* event);
    bool handleMouseWheel(const SDL_MouseWheelEvent* event);

    // Typed characters, which only matter while the panel is asking for
    // something. Selene otherwise has no use for SDL text input at all: it
    // sends scancodes to the host and lets the host's layout decide.
    bool handleTextInput(const char* text);

    // The window, so the panel can work out where it is drawn. It is centred
    // by the renderers, and a click has to be tested against the same centre
    // or the mouse lands somewhere the panel is not.
    void setWindow(SDL_Window* window) { m_Window = window; }

    // The stream's resolution. The panel is drawn into the video frame and
    // scaled with it, so turning a pointer position into a row means undoing
    // that scaling -- which needs the frame size, not just the window's.
    void setStreamSize(int width, int height) { m_StreamWidth = width; m_StreamHeight = height; }

    // Drains helper replies and refreshes the drawn text. Called from the SDL
    // loop, and cheap when nothing has changed.
    void poll();

private:
    void redraw();

    // The behaviour lives in the model, shared with the standalone window.
    // This class is only the SDL half: overlay surface out, SDL events in.
    PanelModel m_Model;
    PanelPainter m_Painter;
    bool m_Open;
    bool m_HotkeyClaimed;
    SDL_Window* m_Window;
    int m_StreamWidth;
    int m_StreamHeight;

    QPoint mapToPanel(int x, int y) const;
};
