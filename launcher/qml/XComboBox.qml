import QtQuick
import QtQuick.Controls.Basic

ComboBox {
    id: control

    property string automationId: ""
    objectName: automationId

    property string accessibleName: ""
    property string accessibleDescription: ""

    implicitHeight: Theme.controlHeight
    implicitWidth: 220
    leftPadding: 12
    rightPadding: 34
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.name: accessibleName.length > 0 ? accessibleName : displayText
    Accessible.description: accessibleDescription

    contentItem: Text {
        leftPadding: 2
        rightPadding: 4
        text: control.displayText
        color: control.enabled ? Theme.text : Theme.textMuted
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        font.pixelSize: Theme.typeBody
    }

    indicator: Text {
        x: control.width - width - 12
        anchors.verticalCenter: parent.verticalCenter
        text: "⌄"
        color: control.enabled ? Theme.textMuted : Theme.border
        font.pixelSize: 18
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.input
        border.width: control.activeFocus ? Theme.focusWidth : Theme.borderWidth
        border.color: control.activeFocus ? Theme.focusRing
                    : control.hovered ? Theme.accentStrong
                    : Theme.border
    }

    delegate: ItemDelegate {
        id: delegateItem
        required property int index
        required property var modelData
        width: control.width
        implicitHeight: Theme.controlHeight
        highlighted: control.highlightedIndex === index

        contentItem: Text {
            text: modelData
            color: delegateItem.highlighted ? Theme.text : Theme.textMuted
            verticalAlignment: Text.AlignVCenter
            leftPadding: 8
            font.pixelSize: Theme.typeBody
            elide: Text.ElideRight
        }

        background: Rectangle {
            radius: Math.max(3, Theme.controlRadius - 2)
            color: delegateItem.highlighted ? Theme.accentSoft : Theme.surface
        }
    }

    popup: Popup {
        y: control.height + 4
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + 8, 320)
        padding: 4

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        }

        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.surfaceRaised
            border.width: Theme.borderWidth
            border.color: Theme.border
        }
    }
}
