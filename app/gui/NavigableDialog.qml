import QtQuick 2.0
import QtQuick.Controls 2.5
import SeleneTheme 1.0

Dialog {
    modal: true
    anchors.centerIn: Overlay.overlay
    padding: 22

    background: Rectangle {
        radius: 20
        color: SeleneTheme.surface
        border.width: 1
        border.color: SeleneTheme.border
    }

    Overlay.modal: Rectangle {
        color: Qt.rgba(SeleneTheme.backdrop.r, SeleneTheme.backdrop.g,
                       SeleneTheme.backdrop.b, 0.74)
    }

    onClosed: {
        // We must force focus back to the last item. If we don't,
        // gamepad and keyboard navigation will break after a
        // dialog appears.
        stackView.forceActiveFocus()
    }
}
