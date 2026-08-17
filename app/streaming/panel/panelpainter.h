#pragma once

#include <QColor>
#include <QFont>
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
    struct Row {
        QString text;
        QString detail;   // right-aligned, e.g. a signal strength
        bool selectable = true;
    };

    // What to draw. Kept as plain data so the menu logic never touches
    // painting and the painting never has to ask the menu anything.
    struct Model {
        QString title;
        QStringList lines;       // free text above the rows, e.g. status
        QVector<Row> rows;
        int selected = -1;
        int hovered = -1;
        QString message;         // progress or error, below the rows
        QString hint;
    };

    PanelPainter();

    // Draws the model and returns a new surface, or nullptr on failure. The
    // caller owns it.
    SDL_Surface* paint(const Model& model);

    // Which row contains a point, in panel-local coordinates, or -1. Valid
    // after the paint that produced the layout.
    int rowAt(const QPoint& point) const;

    QSize size() const { return m_Size; }

private:
    int measureWidth(const Model& model) const;

    QFont m_TitleFont;
    QFont m_RowFont;
    QFont m_SmallFont;

    // Where each row landed, so a click can be turned back into a row.
    QVector<QRect> m_RowRects;
    QSize m_Size;
};
