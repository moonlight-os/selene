import QtQuick 2.0
import QtQuick.Controls 2.2
import SeleneTheme 1.0

ItemDelegate {
    property GridView grid

    highlighted: grid.activeFocus && grid.currentItem === this
    hoverEnabled: true
    scale: highlighted ? 1.025 : (hovered ? 1.012 : 1.0)

    background: Rectangle {
        anchors.fill: parent
        anchors.margins: 4
        radius: 18
        color: parent.highlighted ? SeleneTheme.selection :
               (parent.hovered ? SeleneTheme.hover : SeleneTheme.raised)
        border.width: parent.highlighted ? 3 : 1
        border.color: parent.highlighted ? SeleneTheme.accent : SeleneTheme.border

        Rectangle {
            visible: parent.parent.highlighted
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            width: 4
            height: Math.max(24, parent.height - 48)
            radius: 2
            color: SeleneTheme.accent
        }
    }

    Behavior on scale {
        NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
    }

    Keys.onLeftPressed: {
        grid.moveCurrentIndexLeft()
    }
    Keys.onRightPressed: {
        grid.moveCurrentIndexRight()
    }
    Keys.onDownPressed: {
        grid.moveCurrentIndexDown()
    }
    Keys.onUpPressed: {
        grid.moveCurrentIndexUp()

        // If we've reached the top of the grid, move focus to the toolbar
        if (grid.currentItem === this) {
            nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
        }
    }
    Keys.onReturnPressed: {
        clicked()
    }
    Keys.onEnterPressed: {
        clicked()
    }
}
