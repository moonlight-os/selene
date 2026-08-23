#pragma once

#include <QImage>
#include <QRasterWindow>
#include <QTimer>
#include <QWheelEvent>

#include "panelmodel.h"
#include "panelpainter.h"

// The settings panel as an ordinary window, for when there is no stream.
//
// Same model and same painter as the overlay, so the appliance has one
// interface rather than two that drift apart. The only difference is where
// the image ends up: an SDL surface there, a paintEvent here.
//
// This is what replaces whiptail on a TTY for anything reachable from a
// running session. The text menus stay for the case this cannot cover -- a
// compositor that will not start is exactly when a graphical menu is no use.
//
// QRasterWindow rather than QWidget: it lives in QtGui and gives the same
// paintEvent and QPainter, where QWidget would pull QtWidgets into an
// application that is otherwise entirely QML -- a whole module shipped to
// every platform for one window.
class PanelWindow : public QRasterWindow
{
    Q_OBJECT

public:
    explicit PanelWindow(PanelModel::Mode mode = PanelModel::Mode::ControlCentre);

    // False when there is no helper, i.e. this is not an appliance. The
    // caller should not show a window that can do nothing.
    bool isAvailable() const { return m_Model.isAvailable(); }

protected:
    void exposeEvent(QExposeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    QPoint panelOrigin() const;
    void refresh();

    PanelModel m_Model;
    PanelPainter m_Painter;
    QImage m_Image;

    // The helper answers on its own thread and Qt has no signal to wait for,
    // so the window looks for replies on a timer. Quarter of a second is well
    // under the point where a scan feels stuck and far above the cost of
    // checking an empty queue.
    QTimer m_PollTimer;
};
