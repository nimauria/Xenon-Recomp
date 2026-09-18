import QtQuick
import QtQuick.Controls.Basic

Switch {
    id: control

    property string automationId: ""
    property string accessibleName: ""
    property string accessibleDescription: ""
    objectName: automationId

    signal userToggled(bool value)

    implicitWidth: Math.max(52, Theme.controlHeight + 12)
    implicitHeight: Math.max(30, Theme.controlHeight - 8)
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.role: Accessible.CheckBox
    Accessible.name: accessibleName
    Accessible.description: accessibleDescription
    Accessible.checked: checked

    indicator: Rectangle {
        implicitWidth: Math.max(46, Theme.controlHeight + 6)
        implicitHeight: Math.max(24, Theme.controlHeight - 14)
        x: control.leftPadding
        y: parent.height / 2 - height / 2
        radius: height / 2
        color: control.checked ? Theme.accent : Theme.surfaceAlt
        border.width: control.activeFocus ? Theme.focusWidth : Theme.borderWidth
        border.color: control.activeFocus ? Theme.focusRing
                    : control.checked ? Theme.accentStrong : Theme.border

        Rectangle {
            width: Math.max(18, parent.height - 6)
            height: width
            radius: width / 2
            y: (parent.height - height) / 2
            x: control.checked ? parent.width - width - 3 : 3
            color: control.checked ? Theme.accentText : Theme.textMuted

            Behavior on x {
                NumberAnimation {
                    duration: launcherBridge.boolSetting("accessibility/reduceMotion", false) ? 0 : 100
                    easing.type: Easing.OutCubic
                }
            }
        }
    }

    contentItem: Item { }

    // clicked is only emitted for an actual user interaction. Using it rather
    // than toggled prevents programmatic binding updates from writing unrelated
    // preference values while a settings page is being refreshed.
    onClicked: control.userToggled(control.checked)
}
