#include "panelwindow.h"
#include "gui/selenetheme.h"

#include <QExposeEvent>
#include <QAccessible>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

PanelWindow::PanelWindow(PanelModel::Mode mode)
    : m_Model(mode)
{
    setTitle(QStringLiteral("Moonlight OS"));

    m_Model.reset();
    refresh();

    connect(&m_PollTimer, &QTimer::timeout, this, [this]() {
        if (m_Model.poll()) {
            if (m_Model.closeRequested()) {
                QGuiApplication::quit();
                return;
            }
            refresh();
        }
    });
    m_PollTimer.start(250);
}

void PanelWindow::refresh()
{
    // The image directly, not the SDL surface the overlay needs: going through
    // one meant allocating a surface, copying the card into it row by row, and
    // copying it straight back out again -- twice the pixels of the drawing
    // itself, on every keypress and every hover.
    const auto model = m_Model.model();
    const QSize available(qMax(1, width() - 24), qMax(1, height() - 24));
    m_Image = m_Painter.render(model, available);

    QString accessibleTitle = QStringLiteral("Moonlight OS — %1").arg(model.title);
    if (model.selected >= 0 && model.selected < model.rows.size()) {
        accessibleTitle += QStringLiteral(" — %1").arg(model.rows.at(model.selected).text);
    }
    if (title() != accessibleTitle) {
        setTitle(accessibleTitle);
        QAccessibleEvent event(this, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }

    update();
}

QPoint PanelWindow::panelOrigin() const
{
    return QPoint((width() - m_Image.width()) / 2, (height() - m_Image.height()) / 2);
}

void PanelWindow::exposeEvent(QExposeEvent* event)
{
    QRasterWindow::exposeEvent(event);

    // refresh() runs once in the constructor so the first frame is ready,
    // but that update request can be consumed before a Wayland surface is
    // exposed. On Sway, a cold fullscreen launch then maps with no committed
    // buffer and shows the empty workspace until the window is resized. Ask
    // for a fresh frame at the point the compositor can actually receive it.
    if (isExposed()) {
        refresh();
    }
}

void PanelWindow::paintEvent(QPaintEvent*)
{
    if (m_Image.isNull()) {
        return;
    }

    QPainter painter(this);

    // Filled here rather than by a palette: a QWindow has no background of
    // its own, and the panel is a card that needs something behind it.
    painter.fillRect(QRect(0, 0, width(), height()), SeleneTheme::palette().backdrop);
    painter.drawImage(panelOrigin(), m_Image);
}

void PanelWindow::keyPressEvent(QKeyEvent* event)
{
    bool handled = true;

    switch (event->key()) {
    case Qt::Key_Up:
        m_Model.handleKey(PanelModel::Key::Up);
        break;
    case Qt::Key_Down:
    case Qt::Key_Tab:
        m_Model.handleKey(PanelModel::Key::Down);
        break;
    case Qt::Key_Backtab:
        m_Model.handleKey(PanelModel::Key::Up);
        break;
    case Qt::Key_PageUp:
        m_Model.handleKey(PanelModel::Key::PageUp);
        break;
    case Qt::Key_PageDown:
        m_Model.handleKey(PanelModel::Key::PageDown);
        break;
    case Qt::Key_Home:
        m_Model.handleKey(PanelModel::Key::Home);
        break;
    case Qt::Key_End:
        m_Model.handleKey(PanelModel::Key::End);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        m_Model.handleKey(PanelModel::Key::Activate);
        break;
    case Qt::Key_Backspace:
        m_Model.handleKey(PanelModel::Key::Backspace);
        break;
    case Qt::Key_Escape:
        m_Model.handleKey(PanelModel::Key::Back);
        break;
    default:
        // Typed characters, which only mean anything while a field is up.
        // Qt delivers them here rather than through a separate text event,
        // so there is nothing to start or stop -- unlike SDL, where input has
        // to be switched on deliberately.
        if (m_Model.wantsTextInput() && !event->text().isEmpty()) {
            m_Model.textEntered(event->text());
        }
        else {
            handled = false;
        }
        break;
    }

    if (m_Model.closeRequested()) {
        // Closing the top screen leaves the panel, and outside a stream that
        // means leaving the application: this window is the whole interface.
        QGuiApplication::quit();
        return;
    }

    if (handled) {
        refresh();
    }
    else {
        QRasterWindow::keyPressEvent(event);
    }
}

void PanelWindow::mouseMoveEvent(QMouseEvent* event)
{
    // Only on a change. A repaint here is a full render, and motion events
    // arrive far faster than anything needs redrawing.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint position = event->position().toPoint();
#else
    const QPoint position = event->localPos().toPoint();
#endif
    if (m_Model.setHovered(m_Painter.rowAt(position - panelOrigin()))) {
        refresh();
    }
}

void PanelWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint position = event->position().toPoint();
#else
    const QPoint position = event->localPos().toPoint();
#endif
    m_Model.activateRow(m_Painter.rowAt(position - panelOrigin()));

    if (m_Model.closeRequested()) {
        QGuiApplication::quit();
        return;
    }

    refresh();
}

void PanelWindow::wheelEvent(QWheelEvent* event)
{
    if (event->angleDelta().y() == 0) {
        return;
    }
    m_Model.handleKey(event->angleDelta().y() > 0 ? PanelModel::Key::Up
                                                  : PanelModel::Key::Down);
    refresh();
    event->accept();
}
