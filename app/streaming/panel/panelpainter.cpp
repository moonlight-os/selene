#include "panelpainter.h"
#include "gui/selenetheme.h"

#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

namespace
{
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

QColor toneColour(PanelPainter::Tone tone)
{
    const auto& theme = SeleneTheme::palette();
    switch (tone) {
    case PanelPainter::Tone::Success: return theme.success;
    case PanelPainter::Tone::Warning: return theme.warning;
    case PanelPainter::Tone::Error: return theme.danger;
    case PanelPainter::Tone::Working: return theme.accent;
    case PanelPainter::Tone::None: return theme.muted;
    }
    return theme.muted;
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
    widest = qMax(widest, smallMetrics.horizontalAdvance(model.section.toUpper()));

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

    widest = qMax(widest, rowMetrics.horizontalAdvance(model.notice.title) + 54);
    widest = qMax(widest, smallMetrics.horizontalAdvance(model.notice.detail) + 54);
    widest = qMax(widest, smallMetrics.horizontalAdvance(model.hint));

    return qBound(kMinWidth, widest + kPadding * 2 + 24, kMaxWidth);
}

QImage PanelPainter::render(const Model& model, const QSize& maximumSize)
{
    const auto& theme = SeleneTheme::palette();
    m_RowRects.clear();

    QFontMetrics titleMetrics(m_TitleFont);
    QFontMetrics rowMetrics(m_RowFont);
    QFontMetrics smallMetrics(m_SmallFont);

    int width = measureWidth(model);

    int height = kPadding;
    if (!model.section.isEmpty()) {
        height += smallMetrics.height() + 5;
    }
    height += titleMetrics.height() + 18;
    if (!model.lines.isEmpty()) {
        height += model.lines.size() * (rowMetrics.height() + 6) + 14;
    }
    if (model.scrollAbove > 0) {
        height += smallMetrics.height() + 6;
    }
    height += model.rows.size() * kRowHeight;
    height += model.loadingRows * kRowHeight;
    if (model.scrollBelow > 0) {
        height += 6 + smallMetrics.height();
    }
    if (model.inputActive) {
        height += 12 + rowMetrics.height() + 12 + kRowHeight;
    }
    if (model.notice.isVisible()) {
        height += 14 + 18 + rowMetrics.height();
        if (!model.notice.detail.isEmpty()) {
            height += smallMetrics.height() + 5;
        }
    }
    if (!model.hint.isEmpty()) {
        height += 18 + smallMetrics.height();
    }
    height += kPadding;

    // The shadow, as concentric rounded rects fading outwards. Cheap once, and
    // at this size indistinguishable from a real blurred one -- but it is not
    // cheap forty times a second, so it is drawn when the card changes size
    // and copied every other time.
    if (m_ShadowFor != QSize(width, height)) {
        m_Shadow = QImage(width + kShadowMargin * 2, height + kShadowMargin * 2,
                          QImage::Format_ARGB32);
        m_Shadow.fill(Qt::transparent);

        QPainter shadowPainter(&m_Shadow);
        shadowPainter.setRenderHint(QPainter::Antialiasing);
        for (int i = kShadowMargin; i > 0; i -= 3) {
            QPainterPath halo;
            halo.addRoundedRect(QRectF(kShadowMargin - i, kShadowMargin - i + 6,
                                       width + i * 2, height + i * 2),
                                kRadius + i, kRadius + i);
            shadowPainter.fillPath(halo, QColor(0, 0, 0, 6));
        }
        shadowPainter.end();

        m_ShadowFor = QSize(width, height);
    }

    // Shares the cached pixels until the first stroke below detaches it, which
    // is one memcpy against a dozen antialiased rounded rects.
    QImage image = m_Shadow;

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    painter.translate(kShadowMargin, kShadowMargin);

    QRectF card(0.5, 0.5, width - 1.0, height - 1.0);
    QPainterPath cardPath;
    cardPath.addRoundedRect(card, kRadius, kRadius);
    painter.fillPath(cardPath, theme.surface);
    painter.setPen(QPen(theme.border, 1));
    painter.drawPath(cardPath);

    int y = kPadding;

    if (!model.section.isEmpty()) {
        painter.setFont(m_SmallFont);
        painter.setPen(theme.accent);
        painter.drawText(QRect(kPadding, y, width - kPadding * 2, smallMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, model.section.toUpper());
        y += smallMetrics.height() + 5;
    }

    painter.setFont(m_TitleFont);
    painter.setPen(theme.text);
    const int textWidth = width - kPadding * 2;
    painter.drawText(QRect(kPadding, y, textWidth, titleMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     titleMetrics.elidedText(model.title, Qt::ElideRight, textWidth));

    // A short accent rule under the title, which is what stops the panel
    // reading as an undifferentiated block of text.
    int titleBottom = y + titleMetrics.height() + 8;
    painter.fillRect(QRect(kPadding, titleBottom, 44, 3), theme.accent);
    y = titleBottom + 18;

    if (!model.lines.isEmpty()) {
        painter.setFont(m_RowFont);
        painter.setPen(theme.muted);
        for (const auto& line : model.lines) {
            painter.drawText(QRect(kPadding, y, textWidth, rowMetrics.height()),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             rowMetrics.elidedText(line, Qt::ElideRight, textWidth));
            y += rowMetrics.height() + 6;
        }
        y += 14;
    }

    if (model.scrollAbove > 0) {
        painter.setFont(m_SmallFont);
        painter.setPen(theme.muted);
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
        m_RowRects.append(row.selectable
                              ? rect.translated(kShadowMargin, kShadowMargin)
                              : QRect());

        if (row.selectable && i == model.selected) {
            QPainterPath path;
            path.addRoundedRect(rect, 8, 8);
            painter.fillPath(path, theme.selection);
        }
        else if (row.selectable && i == model.hovered) {
            QPainterPath path;
            path.addRoundedRect(rect, 8, 8);
            painter.fillPath(path, theme.hover);
        }

        painter.setPen(!row.selectable ? theme.disabled
                                       : row.destructive ? theme.danger : theme.text);
        const QRect rowTextRect = rect.adjusted(kPadding / 2 + 8, 0, -(kPadding / 2 + 8), 0);
        int rowTextWidth = rowTextRect.width();
        if (!row.detail.isEmpty()) {
            rowTextWidth = qMax(80, rowTextWidth - rowMetrics.horizontalAdvance(row.detail) - 32);
        }
        painter.drawText(QRect(rowTextRect.x(), rowTextRect.y(), rowTextWidth, rowTextRect.height()),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         rowMetrics.elidedText(row.text, Qt::ElideRight, rowTextWidth));

        if (!row.detail.isEmpty()) {
            painter.setPen(theme.muted);
            painter.drawText(rect.adjusted(kPadding / 2 + 8, 0, -(kPadding / 2 + 8), 0),
                             Qt::AlignRight | Qt::AlignVCenter, row.detail);
        }

        y += kRowHeight;
    }

    // A loading screen should still have shape. These quiet rails reserve the
    // space the result will use without pretending stale actions are live.
    for (int i = 0; i < model.loadingRows; i++) {
        QRect rail(kPadding + 12, y + 15, width - kPadding * 2 - 24, 12);
        int shorten = (i % 3) * 58;
        rail.setWidth(qMax(120, rail.width() - shorten));
        QPainterPath railPath;
        railPath.addRoundedRect(rail, 6, 6);
        QColor railColour = theme.muted;
        railColour.setAlpha(0x20);
        painter.fillPath(railPath, railColour);
        y += kRowHeight;
    }

    if (model.scrollBelow > 0) {
        y += 6;
        painter.setFont(m_SmallFont);
        painter.setPen(theme.muted);
        painter.drawText(QRect(kPadding, y, width - kPadding * 2, smallMetrics.height()),
                         Qt::AlignHCenter | Qt::AlignVCenter,
                         moreLabel(model.scrollBelow, QStringLiteral("\u25BC")));
        y += smallMetrics.height();
    }

    if (model.inputActive) {
        y += 12;
        painter.setFont(m_RowFont);
        painter.setPen(theme.muted);
        painter.drawText(QRect(kPadding, y, width - kPadding * 2, rowMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, model.inputLabel);
        y += rowMetrics.height() + 12;

        QRect field(kPadding, y, width - kPadding * 2, kRowHeight - 8);
        QPainterPath fieldPath;
        fieldPath.addRoundedRect(field, 8, 8);
        painter.fillPath(fieldPath, QColor(0, 0, 0, 0x66));
        painter.setPen(QPen(theme.accent, 2));
        painter.drawPath(fieldPath);

        // Dots, not characters. Someone reading over a shoulder is the
        // ordinary case for a device used on a sofa.
        QString shown = model.inputMasked
            ? QString(model.inputValue.length(), QChar(0x2022))
            : model.inputValue;

        painter.setPen(theme.text);
        painter.drawText(field.adjusted(12, 0, -12, 0),
                         Qt::AlignLeft | Qt::AlignVCenter, shown + QStringLiteral("_"));
        y += kRowHeight - 8;
    }

    if (model.notice.isVisible()) {
        y += 14;
        QColor colour = toneColour(model.notice.tone);
        QRect noticeRect(kPadding, y, width - kPadding * 2,
                         18 + rowMetrics.height()
                             + (model.notice.detail.isEmpty() ? 0 : smallMetrics.height() + 5));
        QPainterPath noticePath;
        noticePath.addRoundedRect(noticeRect, 9, 9);
        painter.fillPath(noticePath, QColor(colour.red(), colour.green(), colour.blue(), 0x18));
        painter.fillRect(QRect(noticeRect.x(), noticeRect.y() + 6, 3,
                               noticeRect.height() - 12), colour);

        int textX = noticeRect.x() + 18;
        if (model.notice.isWorking()) {
            // Eight restrained points orbit only while work is actually in
            // flight. At four frames per second it reads as alive without
            // making the Atom redraw a decorative animation continuously.
            QPointF centre(noticeRect.x() + 25, noticeRect.y() + 19);
            for (int dot = 0; dot < 8; dot++) {
                constexpr double pi = 3.14159265358979323846;
                double angle = (dot * 2.0 * pi / 8.0) - pi / 2.0;
                QPointF at = centre + QPointF(qCos(angle) * 8.0, qSin(angle) * 8.0);
                int distance = (dot - model.activityFrame) & 7;
                int alpha = qMax(45, 255 - distance * 28);
                painter.setBrush(QColor(colour.red(), colour.green(), colour.blue(), alpha));
                painter.setPen(Qt::NoPen);
                painter.drawEllipse(at, distance == 0 ? 2.7 : 2.0,
                                     distance == 0 ? 2.7 : 2.0);
            }
            textX = noticeRect.x() + 48;
        }

        painter.setFont(m_RowFont);
        painter.setPen(colour);
        painter.drawText(QRect(textX, noticeRect.y() + 8,
                               noticeRect.right() - textX - 10, rowMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, model.notice.title);
        if (!model.notice.detail.isEmpty()) {
            painter.setFont(m_SmallFont);
            painter.setPen(theme.muted);
            painter.drawText(QRect(textX, noticeRect.y() + 10 + rowMetrics.height(),
                                   noticeRect.right() - textX - 10, smallMetrics.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, model.notice.detail);
        }
        y += noticeRect.height();
    }

    if (!model.hint.isEmpty()) {
        y += 18;
        painter.setFont(m_SmallFont);
        painter.setPen(theme.muted);
        painter.drawText(QRect(kPadding, y, width - kPadding * 2, smallMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, model.hint);
    }

    painter.end();

    if (maximumSize.isValid() && !maximumSize.isEmpty()
            && (image.width() > maximumSize.width() || image.height() > maximumSize.height())) {
        const qreal scale = qMin(qreal(maximumSize.width()) / image.width(),
                                 qreal(maximumSize.height()) / image.height());
        image = image.scaled(QSize(qMax(1, qRound(image.width() * scale)),
                                   qMax(1, qRound(image.height() * scale))),
                             Qt::KeepAspectRatio, Qt::SmoothTransformation);
        for (QRect& rect : m_RowRects) {
            rect = QRect(qRound(rect.x() * scale), qRound(rect.y() * scale),
                         qRound(rect.width() * scale), qRound(rect.height() * scale));
        }
    }

    m_Size = image.size();
    return image;
}

SDL_Surface* PanelPainter::paint(const Model& model, const QSize& maximumSize)
{
    QImage image = render(model, maximumSize);
    if (image.isNull()) {
        return nullptr;
    }

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
