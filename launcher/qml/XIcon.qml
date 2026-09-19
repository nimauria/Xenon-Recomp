import QtQuick
import QtQuick.Shapes

// Small Xenon-owned vector icon set for launcher chrome. Keeping these as
// QML paths avoids platform-font/emoji differences and scales cleanly at high DPI.
Item {
    id: root

    property string name: ""
    property color color: Theme.textMuted
    property real strokeWidth: 1.8

    implicitWidth: 22
    implicitHeight: 22

    readonly property string pathData: {
        switch (name) {
        case "home":
            return "M3 11 L12 3 L21 11 M5 10 L5 21 L19 21 L19 10 M9 21 L9 14 L15 14 L15 21"
        case "library":
            return "M3 3 L10 3 L10 10 L3 10 Z M14 3 L21 3 L21 10 L14 10 Z M3 14 L10 14 L10 21 L3 21 Z M14 14 L21 14 L21 21 L14 21 Z"
        case "modules":
            return "M12 2 L21 7 L21 17 L12 22 L3 17 L3 7 Z M3 7 L12 12 L21 7 M12 12 L12 22"
        case "profiles":
            return "M12 3 C9.8 3 8 4.8 8 7 C8 9.2 9.8 11 12 11 C14.2 11 16 9.2 16 7 C16 4.8 14.2 3 12 3 Z M4 21 C4.8 16.5 7.4 14 12 14 C16.6 14 19.2 16.5 20 21"
        case "settings":
            return "M4 6 L20 6 M8 3 L8 9 M4 12 L20 12 M16 9 L16 15 M4 18 L20 18 M11 15 L11 21"
        case "search":
            return "M10.5 4 C6.9 4 4 6.9 4 10.5 C4 14.1 6.9 17 10.5 17 C14.1 17 17 14.1 17 10.5 C17 6.9 14.1 4 10.5 4 Z M15.5 15.5 L21 21"
        case "keyboard":
            return "M3 6 L21 6 L21 18 L3 18 Z M6 9 L7 9 M10 9 L11 9 M14 9 L15 9 M18 9 L19 9 M6 12 L7 12 M10 12 L11 12 M14 12 L15 12 M7 15 L17 15"
        case "chevron-left":
            return "M15 5 L8 12 L15 19"
        case "chevron-right":
            return "M9 5 L16 12 L9 19"
        case "help":
            return "M12 22 C17.5 22 22 17.5 22 12 C22 6.5 17.5 2 12 2 C6.5 2 2 6.5 2 12 C2 17.5 6.5 22 12 22 Z M9.5 9 C9.7 7.4 10.7 6.5 12.2 6.5 C14 6.5 15 7.5 15 9 C15 10.2 14.3 10.9 13.1 11.6 C12.2 12.1 12 12.7 12 13.5 M12 17 L12.01 17"
        case "notification":
            return "M6 17 L18 17 M8 17 L8 10 C8 7.7 9.8 6 12 6 C14.2 6 16 7.7 16 10 L16 17 M10 20 C10.4 21 11 21.5 12 21.5 C13 21.5 13.6 21 14 20 M10 5 C10.2 3.8 10.9 3 12 3 C13.1 3 13.8 3.8 14 5"
        case "more":
            return "M6 12 L6.01 12 M12 12 L12.01 12 M18 12 L18.01 12"
        case "close":
            return "M5 5 L19 19 M19 5 L5 19"
        default:
            return "M5 5 L19 19 M19 5 L5 19"
        }
    }

    Shape {
        anchors.centerIn: parent
        width: 24
        height: 24
        ShapePath {
            strokeColor: root.color
            strokeWidth: root.strokeWidth
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            fillColor: "transparent"
            PathSvg { path: root.pathData }
        }
    }
}
