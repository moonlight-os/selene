#pragma once

#include <QString>

#include "SDL_compat.h"
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    // The Moonlight OS settings panel, drawn into our own surface rather than
    // a window of its own. Two things force that and both were learned the
    // hard way: Qt's event loop is suspended for the whole stream, so QML
    // cannot draw here at all; and a second toplevel cannot reliably be
    // composited over a fullscreen client, which is what made the external
    // panel need a workspace to itself.
    OverlayPanel,
    OverlayMax
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    // Which line of the panel is selected, so a bar can be drawn behind it.
    // -1 for none. Kept here rather than in the panel because this is where
    // the surface -- and therefore the line height -- is known.
    void setPanelSelectedLine(int line);

    // Hand over an already-drawn surface instead of text to be rendered.
    // The panel paints itself with QPainter -- real fonts, real layout -- and
    // this is where the result joins the path every renderer already
    // composites. Ownership of the surface passes to us.
    void setOverlaySurface(OverlayType type, SDL_Surface* surface);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void notifyOverlayUpdated(OverlayType type);
    SDL_Surface* decorateAsPanel(SDL_Surface* text, int selectedLine, int lineHeight);
    SDL_Surface* RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth);

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[1024];

        TTF_Font* font;
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    int m_PanelSelectedLine = -1;
    QByteArray m_FontData;
};

}
