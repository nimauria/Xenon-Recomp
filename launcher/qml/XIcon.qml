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
        case "downloads":
            return "M12 3 L12 14 M7 9 L12 14 L17 9 M4 18 L4 20 L20 20 L20 18"
        case "captures":
            return "M4 8 L8 8 L9.5 5.5 L14.5 5.5 L16 8 L20 8 L20 19 L4 19 Z M12 9 C9.8 9 8 10.8 8 13 C8 15.2 9.8 17 12 17 C14.2 17 16 15.2 16 13 C16 10.8 14.2 9 12 9 Z"
        case "network":
            return "M12 3 C17 3 21 7 21 12 C21 17 17 21 12 21 C7 21 3 17 3 12 C3 7 7 3 12 3 Z M3.5 12 L20.5 12 M12 3 C14.5 5.5 14.5 18.5 12 21 M12 3 C9.5 5.5 9.5 18.5 12 21"
        case "support":
            return "M12 3 C17 3 21 7 21 12 C21 17 17 21 12 21 C7 21 3 17 3 12 C3 7 7 3 12 3 Z M12 8 C14.2 8 16 9.8 16 12 C16 14.2 14.2 16 12 16 C9.8 16 8 14.2 8 12 C8 9.8 9.8 8 12 8 Z M5.5 5.5 L9 9 M15 9 L18.5 5.5 M15 15 L18.5 18.5 M9 15 L5.5 18.5"
        case "developer":
            return "M4 5 L20 5 L20 19 L4 19 Z M7 9.5 L10 12 L7 14.5 M12.5 15 L17 15"
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
