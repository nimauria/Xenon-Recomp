import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    property string moduleName: "Module"
    property var settingsSchema: []
    signal settingEdited(string settingId, var value)

    function openFor(moduleTitle, schema) {
        moduleName = moduleTitle
        settingsSchema = schema || []
        open()
    }

    parent: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(760, parent ? parent.width - Theme.space2Xl * 2 : 760)
    height: Math.min(720, parent ? parent.height - Theme.space2Xl * 2 : 720)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Overlay.modal: Rectangle { color: Theme.overlay }

    background: Rectangle {
        color: Theme.surfaceRaised
        radius: Theme.panelRadius
        border.width: Theme.borderWidth
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(82, Theme.typeSubtitle + Theme.typeCaption * 1.5 + Theme.spaceLg * 2)

            ColumnLayout {
                anchors.left: parent.left
                anchors.right: dialogCloseButton.left
                anchors.leftMargin: Theme.spaceXl
                anchors.rightMargin: Theme.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Text {
                    Layout.fillWidth: true
                    text: root.moduleName + " settings"
                    color: Theme.text
                    font.pixelSize: Theme.typeSubtitle
                    font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    text: "Controls are generated from the module manifest and resize with its declared settings."
                    color: Theme.textMuted
                    font.pixelSize: Theme.typeCaption
                    wrapMode: Text.WordWrap
                }
            }

            XIconButton {
                id: dialogCloseButton
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                iconName: "close"
                tooltip: "Close"
                variant: "ghost"
                onClicked: root.close()
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        ScrollView {
            id: settingsScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            Item {
                width: settingsScroll.availableWidth
                implicitHeight: settingsColumn.implicitHeight + Theme.spaceLg * 2

                ColumnLayout {
                    id: settingsColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.spaceLg
                    spacing: Theme.spaceSm

                Repeater {
                    model: root.settingsSchema
                    delegate: XSettingsCard {
                        required property var modelData
                        title: String(modelData.label || modelData.id || "Setting")
                        description: String(modelData.description || "")
                        actionWidth: 300

                        Loader {
                            Layout.fillWidth: true
                            sourceComponent: modelData.type === "bool" ? boolEditor
                                           : modelData.type === "choice" ? choiceEditor
                                           : stringEditor
                            property var settingDefinition: modelData
                        }
                    }
                }

                XPanel {
                    visible: root.settingsSchema.length === 0
                    Layout.fillWidth: true
                    implicitHeight: Math.max(150, emptySettingsText.implicitHeight + Theme.space2Xl * 2)
                    color: Theme.surfaceAlt
                    Text {
                        id: emptySettingsText
                        anchors.centerIn: parent
                        width: parent.width - Theme.spaceXl * 2
                        text: "This module does not currently declare any configurable settings."
                        color: Theme.textMuted
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: Theme.typeBody
                    }
                }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
        RowLayout {
            Layout.fillWidth: true
            Layout.minimumHeight: Math.max(64, Theme.controlHeight + Theme.spaceLg)
            Layout.leftMargin: Theme.spaceXl
            Layout.rightMargin: Theme.spaceXl
            Item { Layout.fillWidth: true }
            XButton { text: "Close"; variant: "primary"; onClicked: root.close() }
        }
    }

    Component {
        id: boolEditor
        XSwitch {
            checked: Boolean(settingDefinition.defaultValue)
            onUserToggled: function(value) { root.settingEdited(String(settingDefinition.id), value) }
        }
    }

    Component {
        id: choiceEditor
        XComboBox {
            Layout.fillWidth: true
            model: settingDefinition.options || []
            currentIndex: Math.max(0, model.indexOf(settingDefinition.defaultValue))
            onActivated: function(index) { root.settingEdited(String(settingDefinition.id), model[index]) }
        }
    }

    Component {
        id: stringEditor
        XTextField {
            Layout.fillWidth: true
            text: String(settingDefinition.defaultValue || "")
            onEditingFinished: root.settingEdited(String(settingDefinition.id), text)
        }
    }
}
