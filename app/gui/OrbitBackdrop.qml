import QtQuick 2.9

import SeleneTheme 1.0

Item {
    id: backdrop
    clip: true

    Rectangle {
        anchors.fill: parent
        color: SeleneTheme.backdrop
    }

    // Selene's signature horizon: quiet orbital paths that frame the library
    // without competing with box art or changing the meaning of controls.
    Repeater {
        model: 4
        Rectangle {
            width: Math.max(backdrop.width, backdrop.height) * (0.72 + index * 0.19)
            height: width
            radius: width / 2
            x: backdrop.width - width * 0.61
            y: backdrop.height - height * 0.28
            color: "transparent"
            border.width: index === 0 ? 2 : 1
            border.color: Qt.rgba(SeleneTheme.accent.r,
                                  SeleneTheme.accent.g,
                                  SeleneTheme.accent.b,
                                  0.18 - index * 0.025)
        }
    }

    Rectangle {
        width: 9
        height: 9
        radius: 5
        x: backdrop.width * 0.82
        y: backdrop.height * 0.77
        color: SeleneTheme.accent
        opacity: 0.78
    }
}
