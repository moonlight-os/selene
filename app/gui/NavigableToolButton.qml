import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import SeleneTheme 1.0

ToolButton {
    property string iconSource

    implicitWidth: 48
    implicitHeight: 48

    activeFocusOnTab: true

    icon.source: iconSource
    icon.width: 30
    icon.height: 30
    icon.color: enabled ? SeleneTheme.text : SeleneTheme.disabled
    hoverEnabled: true

    background: Rectangle {
        radius: 15
        color: parent.activeFocus ? SeleneTheme.selection :
               (parent.hovered ? SeleneTheme.hover : "transparent")
        border.width: parent.activeFocus ? 2 : 0
        border.color: SeleneTheme.accent
    }

    // This determines the size of the Material highlight. We increase it
    // from the default because we use larger than normal icons for TV readability.
    Layout.preferredHeight: parent.height

    Keys.onReturnPressed: {
        clicked()
    }

    Keys.onEnterPressed: {
        clicked()
    }

    Keys.onRightPressed: {
        nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
    }

    Keys.onLeftPressed: {
        nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
    }
}
