#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include "SDL_compat.h"

// Draws the settings panel with QPainter, so it looks like the rest of Selene
// rather than like a terminal.
//
// Not QML, and that is a deliberate trade rather than laziness: Qt's graphics
// API is chosen once for the whole process before the first QQuickWindow
// exists, so rendering QML offscreen would force the launcher into software
// rendering too -- on the appliance that means the box art view software-drawn
// at 2560x1440, on the weakest GPU we target. QPainter gets the antialiased
// fonts, the theme and the layout with none of that.
//
// The output is a QImage converted to an SDL_Surface, which joins the same
// overlay path the stats overlay uses -- so it composites under every video
// renderer without any of them knowing what a panel is.
class PanelPainter
{
public:
    enum class Tone {
        None,
        Working,
        Success,
        Warning,
        Error,
    };

    struct Row {
        QString text;
        QString detail;   // right-aligned, e.g. a signal strength
        bool selectable = true;
        bool destructive = false;
    };

    struct Notice {
        Tone tone = Tone::None;
        QString title;
        QString detail;

        bool isVisible() const { return tone != Tone::None || !title.isEmpty() || !detail.isEmpty(); }
        bool isWorking() const { return tone == Tone::Working; }
    };

    // What to draw. Kept as plain data so the menu logic never touches
    // painting and the painting never has to ask the menu anything.
    struct Model {
        QString title;
        QString section;         // quiet breadcrumb above the title
        QStringList lines;       // free text above the rows, e.g. status
        QVector<Row> rows;
        int selected = -1;
        int hovered = -1;

        // How many rows are scrolled out of sight either way. A list long
        // enough to need this -- Wi-Fi in a block of flats, Bluetooth in a
        // room with two dozen radios in it -- would otherwise grow a panel
        // taller than the screen it is centred on.
        int scrollAbove = 0;
        int scrollBelow = 0;
        Notice notice;           // progress, success, warning, or error
        int activityFrame = 0;   // the working orbit's animation phase
        int loadingRows = 0;     // placeholders while the first result loads
        QString hint;

        // An input field, shown when the panel is asking for something. The
        // value is drawn as dots when masked, because a Wi-Fi password typed
        // on a screen behind someone's shoulder is still a password.
        QString inputLabel;
        QString inputValue;
        bool inputMasked = false;
        bool inputActive = false;
    };

    PanelPainter();

    // Draws the model. The window host wants the image; the in-stream overlay
    // wants an SDL surface, and pays for the conversion rather than making
    // the host pay for one it would only undo.
    QImage render(const Model& model, const QSize& maximumSize = {});

    // Draws the model and returns a new surface, or nullptr on failure. The
    // caller owns it.
    SDL_Surface* paint(const Model& model, const QSize& maximumSize = {});

    // Which row contains a point, in panel-local coordinates, or -1. Valid
    // after the paint that produced the layout.
    int rowAt(const QPoint& point) const;

    QSize size() const { return m_Size; }

private:
    int measureWidth(const Model& model) const;

    QFont m_TitleFont;
    QFont m_RowFont;
    QFont m_SmallFont;

    // The shadow costs more than half of a repaint -- a dozen antialiased
    // rounded rects over the whole card -- and depends on nothing but the
    // card's size, which does not change when a highlight moves. Hovering a
    // row would otherwise redraw it from scratch, and on the appliance's Atom
    // that is the difference between a menu that tracks the mouse and one
    // that does not.
    QImage m_Shadow;
    QSize m_ShadowFor;

    // Where each row landed, so a click can be turned back into a row.
    QVector<QRect> m_RowRects;
    QSize m_Size;
};
