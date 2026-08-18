#include "panelpainter.h"

#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

namespace
{
// Selene's Material palette, so the panel belongs to the same application as
// the launcher rather than looking like something bolted on. The primary is
// the same indigo main.cpp sets for Quick Controls.
const QColor kCard(0x20, 0x20, 0x24, 0xF2);
const QColor kBorder(0xFF, 0xFF, 0xFF, 0x1F);
const QColor kAccent(0x3F, 0x51, 0xB5);
const QColor kSelected(0x3F, 0x51, 0xB5, 0xCC);
const QColor kHovered(0xFF, 0xFF, 0xFF, 0x18);
const QColor kText(0xFF, 0xFF, 0xFF);
const QColor kMuted(0xFF, 0xFF, 0xFF, 0xB0);

const int kPadding = 28;
const int kRowHeight = 44;
const int kRadius = 14;
const int kMinWidth = 460;
const int kMaxWidth = 820;

// Room around the card for its shadow. This is not blur -- blurring the video
// behind the panel would mean sampling the frame, which happens inside each of
// the eight renderers in its own API. A shadow gives the panel the same
// separation from the picture for none of that, and works everywhere.
const int kShadowMargin = 36;

QString moreLabel(int count, const QString& arrow)
{
    return QStringLiteral("%1  %2 more").arg(arrow).arg(count);
}
}

PanelPainter::PanelPainter()
{
    // No font family is named. Whatever Qt resolves as the UI font is what the
    // launcher already draws with, and hardcoding one would look right on this
    // machine and wrong on the next.
    m_TitleFont.setPointSize(16);
    m_TitleFont.setBold(true);

    m_RowFont.setPointSize(13);

    m_SmallFont.setPointSize(10);
}

int PanelPainter::measureWidth(const Model& model) const
{
    QFontMetrics titleMetrics(m_TitleFont);
    QFontMetrics rowMetrics(m_RowFont);
    QFontMetrics smallMetrics(m_SmallFont);

    int widest = titleMetrics.horizontalAdvance(model.title);

    for (const auto& line : model.lines) {
        widest = qMax(widest, rowMetrics.horizontalAdvance(line));
    }

    for (const auto& row : model.rows) {
        // The detail sits at the far right, so the row has to fit both plus a
        // gap wide enough that they never read as one string.
        int width = rowMetrics.horizontalAdvance(row.text);
        if (!row.detail.isEmpty()) {
            width += 48 + rowMetrics.horizontalAdvance(row.detail);
        }
        widest = qMax(widest, width);
    }

    if (model.scrollAbove > 0) {
        widest = qMax(widest, smallMetrics.horizontalAdvance(moreLabel(model.scrollAbove, QStringLiteral("\u25B2"))));
    }
    if (model.scrollBelow > 0) {
        widest = qMax(widest, smallMetrics.horizontalAdvance(moreLabel(model.scrollBelow, QStringLiteral("\u25BC"))));
    }

    widest = qMax(widest, smallMetrics.horizontalAdvance(model.message));
    widest = qMax(widest, smallMetrics.horizontalAdvance(model.hint));

    return qBound(kMinWidth, widest + kPadding * 2 + 24, kMaxWidth);
}

SDL_Surface* PanelPainter::paint(const Model& model)
{
    m_RowRects.clear();

    QFontMetrics titleMetrics(m_TitleFont);
    QFontMetrics rowMetrics(m_RowFont);
    QFontMetrics smallMetrics(m_SmallFont);

    int width = measureWidth(model);

    int height = kPadding;
    height += titleMetrics.height() + 18;
    if (!model.lines.isEmpty()) {
        height += model.lines.size() * (rowMetrics.height() + 6) + 14;
    }
    if (model.scrollAbove > 0) {
        height += smallMetrics.height() + 6;
    }
    height += model.rows.size() * kRowHeight;
    if (model.scrollBelow > 0) {
        height += 6 + smallMetrics.height();
    }
    if (model.inputActive) {
        height += 12 + rowMetrics.height() + 12 + kRowHeight;
    }
    if (!model.message.isEmpty()) {
        height += 14 + smallMetrics.height();
    }
    if (!model.hint.isEmpty()) {
        height += 18 + smallMetrics.height();
    }
    height += kPadding;

    QImage image(width + kShadowMargin * 2, height + kShadowMargin * 2, QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    // The shadow, as concentric rounded rects fading outwards. Cheap, and at
    // this size indistinguishable from a real blurred one.
    for (int i = kShadowMargin; i > 0; i -= 3) {
        QPainterPath halo;
        halo.addRoundedRect(QRectF(kShadowMargin - i, kShadowMargin - i + 6,
                                   width + i * 2, height + i * 2),
                            kRadius + i, kRadius + i);
        painter.fillPath(halo, QColor(0, 0, 0, 6));
    }

    painter.translate(kShadowMargin, kShadowMargin);

    QRectF card(0.5, 0.5, width - 1.0, height - 1.0);
    QPainterPath cardPath;
    cardPath.addRoundedRect(card, kRadius, kRadius);
    painter.fillPath(cardPath, kCard);
    painter.setPen(QPen(kBorder, 1));
    painter.drawPath(cardPath);

    int y = kPadding;

    painter.setFont(m_TitleFont);
    painter.setPen(kText);
    painter.drawText(QRect(kPadding, y, width - kPadding * 2, titleMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, model.title);

    // A short accent rule under the title, which is what stops the panel
    // reading as an undifferentiated block of text.
    int titleBottom = y + titleMetrics.height() + 8;
    painter.fillRect(QRect(kPadding, titleBottom, 44, 3), kAccent);
    y = titleBottom + 18;

    if (!model.lines.isEmpty()) {
        painter.setFont(m_RowFont);
        painter.setPen(kMuted);
        for (const auto& line : model.lines) {
            painter.drawText(QRect(kPadding, y, width - kPadding * 2, rowMetrics.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, line);
            y += rowMetrics.height() + 6;
        }
        y += 14;
    }

    if (model.scrollAbove > 0) {
        painter.setFont(m_SmallFont);
        painter.setPen(kMuted);
        painter.drawText(QRect(kPadding, y, width - kPadding * 2, smallMetrics.height()),
                         Qt::AlignHCenter | Qt::AlignVCenter,
                         moreLabel(model.scrollAbove, QStringLiteral("\u25B2")));
        y += smallMetrics.height() + 6;
    }

    painter.setFont(m_RowFont);
    for (int i = 0; i < model.rows.size(); i++) {
        const auto& row = model.rows.at(i);
        QRect rect(kPadding / 2, y, width - kPadding, kRowHeight);

        // Stored with the shadow margin added, because drawing happens in
        // card coordinates (the painter is translated) while hit-testing
        // works in image coordinates. Without this the hover is off by
        // exactly the margin, which reads as "the mouse is misaligned".
        m_RowRects.append(rect.translated(kShadowMargin, kShadowMargin));

        if (i == model.selected) {
            QPainterPath path;
            path.addRoundedRect(rect, 8, 8);
            painter.fillPath(path, kSelected);
        }
        else if (i == model.hovered) {
            QPainterPath path;
            path.addRoundedRect(rect, 8, 8);
            painter.fillPath(path, kHovered);
        }

        painter.setPen(kText);
        painter.drawText(rect.adjusted(kPadding / 2 + 8, 0, -(kPadding / 2 + 8), 0),
                         Qt::AlignLeft | Qt::AlignVCenter, row.text);

        if (!row.detail.isEmpty()) {
            painter.setPen(kMuted);
            painter.drawText(rect.adjusted(kPadding / 2 + 8, 0, -(kPadding / 2 + 8), 0),
                             Qt::AlignRight | Qt::AlignVCenter, row.detail);
        }

        y += kRowHeight;
    }

    if (model.scrollBelow > 0) {
        y += 6;
        painter.setFont(m_SmallFont);
        painter.setPen(kMuted);
        painter.drawText(QRect(kPadding, y, width - kPadding * 2, smallMetrics.height()),
                         Qt::AlignHCenter | Qt::AlignVCenter,
                         moreLabel(model.scrollBelow, QStringLiteral("\u25BC")));
        y += smallMetrics.height();
    }

    if (model.inputActive) {
        y += 12;
        painter.setFont(m_RowFont);
        painter.setPen(kMuted);
        painter.drawText(QRect(kPadding, y, width - kPadding * 2, rowMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, model.inputLabel);
        y += rowMetrics.height() + 12;

        QRect field(kPadding, y, width - kPadding * 2, kRowHeight - 8);
        QPainterPath fieldPath;
        fieldPath.addRoundedRect(field, 8, 8);
        painter.fillPath(fieldPath, QColor(0, 0, 0, 0x66));
        painter.setPen(QPen(kAccent, 2));
        painter.drawPath(fieldPath);

        // Dots, not characters. Someone reading over a shoulder is the
        // ordinary case for a device used on a sofa.
        QString shown = model.inputMasked
            ? QString(model.inputValue.length(), QChar(0x2022))
            : model.inputValue;

        painter.setPen(kText);
        painter.drawText(field.adjusted(12, 0, -12, 0),
                         Qt::AlignLeft | Qt::AlignVCenter, shown + QStringLiteral("_"));
        y += kRowHeight - 8;
    }

    if (!model.message.isEmpty()) {
        y += 14;
        painter.setFont(m_SmallFont);
        painter.setPen(kAccent.lighter(140));
        painter.drawText(QRect(kPadding, y, width - kPadding * 2, smallMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, model.message);
        y += smallMetrics.height();
    }

    if (!model.hint.isEmpty()) {
        y += 18;
        painter.setFont(m_SmallFont);
        painter.setPen(kMuted);
        painter.drawText(QRect(kPadding, y, width - kPadding * 2, smallMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, model.hint);
    }

    painter.end();

    m_Size = image.size();

    // SDL and QImage agree on ARGB32 byte order here, but the surface must own
    // its pixels: the QImage dies with this function and the surface outlives
    // the next several frames.
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, image.width(), image.height(),
                                                          32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        return nullptr;
    }

    SDL_LockSurface(surface);
    for (int row = 0; row < image.height(); row++) {
        memcpy((uint8_t*)surface->pixels + row * surface->pitch,
               image.constScanLine(row),
               qMin<int>(surface->pitch, image.bytesPerLine()));
    }
    SDL_UnlockSurface(surface);

    return surface;
}

int PanelPainter::rowAt(const QPoint& point) const
{
    for (int i = 0; i < m_RowRects.size(); i++) {
        if (m_RowRects.at(i).contains(point)) {
            return i;
        }
    }
    return -1;
}
