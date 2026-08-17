#include "overlaymanager.h"
#include "path.h"

using namespace Overlay;

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf"))
{
    memset(m_Overlays, 0, sizeof(m_Overlays));

    m_Overlays[OverlayType::OverlayDebug].color = {0xD0, 0xD0, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;

    // White and larger than the debug overlay: this one is read deliberately
    // rather than glanced at, and it is the whole interface while it is up.
    m_Overlays[OverlayType::OverlayPanel].color = {0xFF, 0xFF, 0xFF, 0xFF};
    m_Overlays[OverlayType::OverlayPanel].fontSize = 28;

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
}

OverlayManager::~OverlayManager()
{
    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        if (m_Overlays[i].surface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].surface);
        }
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
    }

    TTF_Quit();

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    return m_Overlays[type].enabled;
}

char* OverlayManager::getOverlayText(OverlayType type)
{
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text)
{
    SDL_utf8strlcpy(m_Overlays[type].text, text, sizeof(m_Overlays[0].text));
    setOverlayTextUpdated(type);
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    return m_Overlays[type].fontSize;
}

SDL_Surface* OverlayManager::getUpdatedOverlaySurface(OverlayType type)
{
    // If a new surface is available, return it. If not, return nullptr.
    // Caller must free the surface on success.
    return (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
}

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
    // Only update the overlay state if it's enabled. If it's not enabled,
    // the renderer has already been notified by setOverlayState().
    if (m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    bool stateChanged = m_Overlays[type].enabled != enabled;

    m_Overlays[type].enabled = enabled;

    if (stateChanged) {
        if (!enabled) {
            // Set the text to empty string on disable
            m_Overlays[type].text[0] = 0;
        }

        notifyOverlayUpdated(type);
    }
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlaySurface(OverlayType type, SDL_Surface* surface)
{
    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr(
        (void**)&m_Overlays[type].surface, surface);

    if (m_Renderer != nullptr) {
        m_Renderer->notifyOverlayUpdated(type);
    }

    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }
}

void OverlayManager::setPanelSelectedLine(int line)
{
    m_PanelSelectedLine = line;
}

/**
 * Wrap the panel's text in something that looks deliberate.
 *
 * All of this lives in the surface rather than in the renderers, and that is
 * the point: there are eight renderers and each composites overlays in its own
 * API. Anything drawn here works under all of them, including the ones nobody
 * remembered to update.
 */
SDL_Surface* OverlayManager::decorateAsPanel(SDL_Surface* text, int selectedLine, int lineHeight)
{
    if (text == nullptr) {
        return nullptr;
    }

    const int padding = 28;
    const int border = 2;

    SDL_Surface* card = SDL_CreateRGBSurfaceWithFormat(0,
                                                       text->w + padding * 2,
                                                       text->h + padding * 2,
                                                       32,
                                                       SDL_PIXELFORMAT_ARGB8888);
    if (card == nullptr) {
        return text;
    }

    // Dark and mostly opaque: it sits over moving video, and a panel you have
    // to squint past the game to read is not a panel.
    SDL_FillRect(card, nullptr, SDL_MapRGBA(card->format, 0x10, 0x10, 0x14, 0xE8));

    SDL_Rect inner = { border, border, card->w - border * 2, card->h - border * 2 };
    SDL_FillRect(card, &inner, SDL_MapRGBA(card->format, 0x10, 0x10, 0x14, 0xE8));

    SDL_Rect edge = { 0, 0, card->w, border };
    Uint32 edgeColor = SDL_MapRGBA(card->format, 0xFF, 0xFF, 0xFF, 0x33);
    SDL_FillRect(card, &edge, edgeColor);
    edge.y = card->h - border;
    SDL_FillRect(card, &edge, edgeColor);
    edge = { 0, 0, border, card->h };
    SDL_FillRect(card, &edge, edgeColor);
    edge.x = card->w - border;
    SDL_FillRect(card, &edge, edgeColor);

    // A bar behind the selected row. The caller counts lines; this only has
    // to know how tall one is.
    if (selectedLine >= 0 && lineHeight > 0) {
        SDL_Rect highlight = {
            padding / 2,
            padding + selectedLine * lineHeight,
            card->w - padding,
            lineHeight,
        };
        SDL_FillRect(card, &highlight, SDL_MapRGBA(card->format, 0xFF, 0xFF, 0xFF, 0x24));
    }

    SDL_Rect at = { padding, padding, text->w, text->h };
    SDL_SetSurfaceBlendMode(text, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(text, nullptr, card, &at);
    SDL_FreeSurface(text);

    return card;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    m_Renderer = renderer;
}

void OverlayManager::notifyOverlayUpdated(OverlayType type)
{
    if (m_Renderer == nullptr) {
        return;
    }

    // Construct the required font to render the overlay
    if (m_Overlays[type].font == nullptr) {
        if (m_FontData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL overlay font failed to load");
            return;
        }

        // m_FontData must stay around until the font is closed
        m_Overlays[type].font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                               1,
                                               m_Overlays[type].fontSize);
        if (m_Overlays[type].font == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TTF_OpenFont() failed: %s",
                        TTF_GetError());

            // Can't proceed without a font
            return;
        }
    }

    SDL_Surface* newSurface = nullptr;
    if (m_Overlays[type].enabled) {
        // The _Wrapped variant is required for line breaks to work
        newSurface = RenderTextOutlinedWrapped(m_Overlays[type].font,
                                               m_Overlays[type].text,
                                               m_Overlays[type].color,
                                               {0, 0, 0, 255},
                                               4,
                                               1024);

        if (type == OverlayType::OverlayPanel) {
            newSurface = decorateAsPanel(newSurface,
                                         m_PanelSelectedLine,
                                         TTF_FontLineSkip(m_Overlays[type].font));
        }
    }

    // Exchange the old surface with the new one
    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr(
        (void**)&m_Overlays[type].surface, newSurface);

    // Notify the renderer
    m_Renderer->notifyOverlayUpdated(type);

    // Free the old surface
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }
}

SDL_Surface* OverlayManager::RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth) {
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }

    int oldOutline = TTF_GetFontOutline(font);
    TTF_SetFontOutline(font, outlineWidth);

    // Verify that the string won't require wrapping (which could cause the outline and the text
    // to diverge due to different wrapping positions).
    //
    // FIXME: We do this rather than just disabling wrapping entirely (wrapWidth = 0) because we
    // need further testing to ensure that all renderers can handle non-NPOT overlay textures.
    for (const QString& line : QString(text).split('\n')) {
        int extent, count;
        if (TTF_MeasureUTF8(font, line.toUtf8(), wrapWidth, &extent, &count) == 0 && count < line.size()) {
            // If it requires wrapping, render it without the outline
            TTF_SetFontOutline(font, oldOutline);
            return TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
        }
    }

    // Draw text twice, but outline is a bit bigger
    auto outlineSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, outlineColor, wrapWidth);
    TTF_SetFontOutline(font, 0);
    auto textSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
    TTF_SetFontOutline(font, oldOutline);

    if (outlineSurface == nullptr || textSurface == nullptr) {
        SDL_FreeSurface(outlineSurface);
        SDL_FreeSurface(textSurface);
        return nullptr;
    }

    // Merge the texts
    SDL_Rect dst = { outlineWidth, outlineWidth, textSurface->w, textSurface->h };
    SDL_BlitSurface(textSurface, nullptr, outlineSurface, &dst);

    SDL_FreeSurface(textSurface);
    return outlineSurface;
}


