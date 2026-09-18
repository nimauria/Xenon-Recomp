import QtQuick
import QtQuick.Controls

Button {
    id: control

    property string variant: "default" // default, primary, danger, ghost

    implicitHeight: 42
    implicitWidth: Math.max(110, contentItem.implicitWidth + 30)

    contentItem: Text {
        text: control.text
        color: control.variant === "primary" ? Theme.accentText
             : control.variant === "danger" ? Theme.danger
             : Theme.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        font.pixelSize: 13
        font.weight: control.variant === "primary" ? Font.DemiBold : Font.Medium
        opacity: control.enabled ? 1.0 : 0.45
    }

    background: Rectangle {
        radius: 8
        color: control.variant === "primary" ? Theme.accent
             : control.variant === "danger" ? Theme.surfaceAlt
             : control.variant === "ghost" ? "transparent"
             : (control.hovered ? Theme.surfaceHover : Theme.surfaceAlt)
        border.width: control.variant === "primary" ? 0 : 1
        border.color: control.variant === "danger" ? Theme.danger
                    : control.hovered ? Theme.accent
                    : Theme.border
        opacity: control.enabled ? 1.0 : 0.55
    }
}
