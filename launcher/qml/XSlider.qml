import QtQuick
import QtQuick.Controls.Basic

Slider {
    id: control

    property string automationId: ""
    property string accessibleName: ""
    property string accessibleDescription: ""

    objectName: automationId
    implicitWidth: 220
    implicitHeight: Theme.controlHeight
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.name: accessibleName
    Accessible.description: accessibleDescription

    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        width: control.availableWidth
        height: Math.max(4, Theme.borderWidth * 2)
        radius: height / 2
        color: Theme.border

        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: parent.radius
            color: Theme.accent
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + (control.availableHeight - height) / 2
        width: Math.max(16, Math.round(18 * Theme.bodyScale))
        height: width
        radius: width / 2
        color: control.pressed ? Theme.accentStrong : Theme.accent
        border.width: control.activeFocus ? Theme.focusWidth : Theme.borderWidth
        border.color: control.activeFocus ? Theme.focusRing : Theme.surfaceRaised
    }
}
