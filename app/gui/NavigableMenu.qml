import QtQuick 2.0
import QtQuick.Controls 2.2
import SeleneTheme 1.0

Menu {
    property var initiator
    padding: 8

    background: Rectangle {
        radius: 16
        color: SeleneTheme.surface
        border.width: 1
        border.color: SeleneTheme.border
    }

    onOpened: {
        // If the initiating object currently has keyboard focus,
        // give focus to the first visible and enabled menu item
        if (initiator.focus) {
            for (var i = 0; i < count; i++) {
                var item = itemAt(i)
                if (item.visible && item.enabled) {
                    item.forceActiveFocus(Qt.TabFocusReason)
                    break
                }
            }
        }
    }
}
