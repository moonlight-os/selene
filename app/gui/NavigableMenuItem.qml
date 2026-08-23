import QtQuick 2.0
import QtQuick.Controls 2.2
import SeleneTheme 1.0

MenuItem {
    // Ensure focus can't be given to an invisible item
    enabled: visible
    height: visible ? implicitHeight : 0
    focusPolicy: visible ? Qt.TabFocus : Qt.NoFocus
    hoverEnabled: true

    contentItem: Label {
        text: parent.text
        color: parent.enabled ? SeleneTheme.text : SeleneTheme.disabled
        font.pointSize: 11
        verticalAlignment: Text.AlignVCenter
        leftPadding: 10
    }

    background: Rectangle {
        radius: 10
        color: parent.activeFocus ? SeleneTheme.selection :
               (parent.hovered ? SeleneTheme.hover : "transparent")
        border.width: parent.activeFocus ? 1 : 0
        border.color: SeleneTheme.accent
    }

    onTriggered: {
        // We must close the context menu first or
        // it can steal focus from any dialogs that
        // onTriggered may spawn.
        menu.close()
    }

    Keys.onReturnPressed: {
        triggered()
    }

    Keys.onEnterPressed: {
        triggered()
    }

    Keys.onEscapePressed: {
        menu.close()
    }
}
