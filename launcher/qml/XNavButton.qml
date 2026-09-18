import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Button {
    id: control

    property string automationId: ""
    objectName: automationId

    property string glyph: ""
    property string iconName: ""
    property bool active: false
    property bool compact: false

    implicitHeight: compact ? 44 : Math.max(50, Theme.controlHeight + 8)
    implicitWidth: compact ? 44 : 164
    leftPadding: compact ? 0 : Theme.spaceMd
    rightPadding: compact ? 0 : Theme.spaceMd
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: text
    Accessible.role: Accessible.Button

    contentItem: Item {
        XIcon {
            visible: control.compact && control.iconName.length > 0
            anchors.centerIn: parent
            name: control.iconName
            color: control.active ? Theme.accent : Theme.textMuted
        }

        Text {
            visible: control.compact && control.iconName.length === 0
            anchors.centerIn: parent
            text: control.glyph
            color: control.active ? Theme.accent : Theme.textMuted
            font.pixelSize: Theme.typeBodyLarge
        }

        RowLayout {
            visible: !control.compact
            anchors.fill: parent
            spacing: Theme.spaceMd

            Item {
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24

                XIcon {
                    anchors.centerIn: parent
                    visible: control.iconName.length > 0
                    name: control.iconName
                    color: control.active ? Theme.accent : Theme.textMuted
                }

                Text {
                    anchors.centerIn: parent
                    visible: control.iconName.length === 0
                    text: control.glyph
                    color: control.active ? Theme.accent : Theme.textMuted
                    font.pixelSize: Theme.typeBodyLarge
                }
            }

            Text {
                Layout.fillWidth: true
                text: control.text
                color: control.active ? Theme.text : Theme.textMuted
                font.pixelSize: Theme.typeBody
                font.weight: control.active ? Font.DemiBold : Font.Normal
                elide: Text.ElideRight
            }
        }
    }

    background: Rectangle {
        radius: control.compact ? 10 : Theme.controlRadius
        color: control.active ? Theme.accentSoft
             : control.hovered ? Theme.surfaceHover
             : "transparent"
        border.width: control.active || control.activeFocus ? Theme.borderWidth : 0
        border.color: control.activeFocus ? Theme.focusRing : Theme.accent
    }

    ToolTip.visible: control.compact && control.hovered
    ToolTip.text: control.text
    ToolTip.delay: 450
}
