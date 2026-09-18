import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control

    property string automationId: ""
    objectName: automationId

    property string glyph: "⋯"
    property string iconName: ""
    property string tooltip: ""
    property string variant: "ghost"

    implicitWidth: Theme.controlHeight
    implicitHeight: Theme.controlHeight
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.name: tooltip.length > 0 ? tooltip : (iconName.length > 0 ? iconName : glyph)
    Accessible.role: Accessible.Button

    contentItem: Item {
        XIcon {
            anchors.centerIn: parent
            visible: control.iconName.length > 0
            name: control.iconName
            color: control.enabled ? Theme.text : Theme.textMuted
        }
        Text {
            anchors.fill: parent
            visible: control.iconName.length === 0
            text: control.glyph
            color: control.enabled ? Theme.text : Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            font.pixelSize: Theme.typeBodyLarge
            font.weight: Font.Medium
        }
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: control.hovered ? Theme.surfaceHover
             : control.variant === "filled" ? Theme.surfaceAlt
             : "transparent"
        border.width: control.activeFocus ? Theme.focusWidth
                    : control.variant === "filled" ? Theme.borderWidth : 0
        border.color: control.activeFocus ? Theme.focusRing : Theme.border
    }

    ToolTip.visible: tooltip.length > 0 && hovered
    ToolTip.text: tooltip
    ToolTip.delay: 500
}
