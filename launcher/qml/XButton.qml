import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control

    property string automationId: ""
    objectName: automationId

    property string variant: "default" // default, primary, danger, ghost
    property string accessibleDescription: ""

    implicitHeight: Theme.controlHeight
    implicitWidth: Math.max(104, contentItem.implicitWidth + 30)
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.name: text
    Accessible.description: accessibleDescription
    Accessible.role: Accessible.Button

    contentItem: Text {
        text: control.text
        color: !control.enabled ? Theme.textMuted
             : control.variant === "primary" ? Theme.accentText
             : control.variant === "danger" ? Theme.danger
             : Theme.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        font.pixelSize: Theme.typeBody
        font.weight: control.variant === "primary" ? Font.DemiBold : Font.Medium
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: !control.enabled ? Theme.surfaceAlt
             : control.variant === "primary" ? (control.down ? Theme.accentStrong : Theme.accent)
             : control.variant === "danger" ? (control.hovered ? Theme.surfaceHover : Theme.surfaceAlt)
             : control.variant === "ghost" ? (control.hovered ? Theme.surfaceHover : "transparent")
             : (control.down ? Theme.surfaceHover : control.hovered ? Theme.surfaceHover : Theme.surfaceAlt)
        border.width: control.activeFocus ? Theme.focusWidth : Theme.borderWidth
        border.color: control.activeFocus ? Theme.focusRing
                    : control.variant === "primary" ? Theme.accentStrong
                    : control.variant === "danger" ? Theme.danger
                    : control.hovered ? Theme.accentStrong
                    : Theme.border
        opacity: control.enabled ? 1.0 : 0.58
    }
}
