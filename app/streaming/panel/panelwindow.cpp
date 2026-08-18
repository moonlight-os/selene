#include "panelwindow.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

PanelWindow::PanelWindow()
{
    setTitle(QStringLiteral("Moonlight OS"));

    m_Model.reset();
    refresh();

    connect(&m_PollTimer, &QTimer::timeout, this, [this]() {
        if (m_Model.poll()) {
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
    m_Image = m_Painter.render(m_Model.model());

    update();
}

QPoint PanelWindow::panelOrigin() const
{
    return QPoint((width() - m_Image.width()) / 2, (height() - m_Image.height()) / 2);
}

void PanelWindow::paintEvent(QPaintEvent*)
{
    if (m_Image.isNull()) {
        return;
    }

    QPainter painter(this);

    // Filled here rather than by a palette: a QWindow has no background of
    // its own, and the panel is a card that needs something behind it.
    painter.fillRect(QRect(0, 0, width(), height()), QColor(0x0A, 0x0A, 0x0C));
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
        m_Model.handleKey(PanelModel::Key::Down);
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
    if (m_Model.setHovered(m_Painter.rowAt(event->position().toPoint() - panelOrigin()))) {
        refresh();
    }
}

void PanelWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }

    m_Model.activateRow(m_Painter.rowAt(event->position().toPoint() - panelOrigin()));

    if (m_Model.closeRequested()) {
        QGuiApplication::quit();
        return;
    }

    refresh();
}
