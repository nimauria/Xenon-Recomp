import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: control

    property string automationId: ""
    objectName: automationId

    implicitHeight: Theme.controlHeight
    leftPadding: 12
    rightPadding: 12
    color: Theme.text
    placeholderTextColor: Theme.textMuted
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentText
    font.pixelSize: Theme.typeBody
    focusPolicy: Qt.StrongFocus

    Accessible.name: accessibleName.length > 0 ? accessibleName : placeholderText
    Accessible.description: accessibleDescription

    property string accessibleName: ""
    property string accessibleDescription: ""

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.input
        border.width: control.activeFocus ? Theme.focusWidth : Theme.borderWidth
        border.color: control.activeFocus ? Theme.focusRing
                     : control.hovered ? Theme.accentStrong
                     : Theme.border
    }
}
