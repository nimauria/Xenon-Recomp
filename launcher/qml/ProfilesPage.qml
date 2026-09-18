import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property int selectedProfileIndex: 0
    property int nextProfileNumber: 2

    readonly property bool hasProfiles: profilesModel.count > 0
    readonly property bool testMode: launcherBridge.testMode

    ListModel { id: profilesModel }

    function profileObject(name, description, active) {
        return {
            profileName: name,
            description: description,
            active: active,
            games: 0,
            modules: 0,
            saveSets: 0,
            gamePath: "",
            savePath: "",
            modulePath: "",
            region: "Auto (Global)",
            startupPage: "Library",
            offline: true
        }
    }

    function selectedProfile() {
        if (!hasProfiles)
            return profileObject("", "", false)
        return profilesModel.get(Math.max(0, Math.min(selectedProfileIndex, profilesModel.count - 1)))
    }

    function createProfile(name) {
        profilesModel.append(profileObject(name, "New Xenon launcher profile.", false))
        selectedProfileIndex = profilesModel.count - 1
    }

    function activateSelected() {
        if (!hasProfiles)
            return
        for (var i = 0; i < profilesModel.count; ++i)
            profilesModel.setProperty(i, "active", i === selectedProfileIndex)
        launcherBridge.profileName = selectedProfile().profileName
        launcherBridge.notify("Profile activated", selectedProfile().profileName + " is now the active launcher profile.")
    }

    Component.onCompleted: {
        profilesModel.append(profileObject(launcherBridge.profileName, "Primary Xenon launcher profile.", true))

        if (testMode) {
            profilesModel.append({
                profileName: "Test Profile",
                description: "Fictional profile used to exercise profile switching and editing.",
                active: false,
                games: 2,
                modules: 3,
                saveSets: 1,
                gamePath: "TEST://Games",
                savePath: "TEST://Saves",
                modulePath: "TEST://Modules",
                region: "Auto (Global)",
                startupPage: "Library",
                offline: true
            })
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 12

        XPanel {
            Layout.preferredWidth: 330
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        text: "Profiles"
                        color: Theme.text
                        font.pixelSize: 24
                        font.weight: Font.DemiBold
                    }

                    StatusPill {
                        visible: root.testMode
                        label: "TEST MODE"
                        tone: Theme.warning
                    }

                    XButton {
                        text: "+ New"
                        implicitWidth: 86
                        implicitHeight: 36
                        onClicked: {
                            root.createProfile("Profile " + root.nextProfileNumber)
                            root.nextProfileNumber += 1
                        }
                    }
                }

                ListView {
                    id: profileList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 8
                    model: profilesModel

                    delegate: Rectangle {
                        required property int index
                        required property string profileName
                        required property string description
                        required property bool active
                        required property int games
                        required property int modules

                        width: profileList.width
                        height: 98
                        radius: 9
                        color: root.selectedProfileIndex === index ? Theme.accentSoft
                             : rowMouse.containsMouse ? Theme.surfaceHover
                             : Theme.surface
                        border.width: root.selectedProfileIndex === index ? 1 : 0
                        border.color: Theme.accent

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 10

                            Rectangle {
                                Layout.preferredWidth: 58
                                Layout.preferredHeight: 58
                                radius: 29
                                color: Theme.surfaceAlt
                                border.width: 1
                                border.color: active ? Theme.success : Theme.border

                                Text {
                                    anchors.centerIn: parent
                                    text: profileName.length > 0 ? profileName.charAt(0).toUpperCase() : "?"
                                    color: active ? Theme.success : Theme.textMuted
                                    font.pixelSize: 22
                                    font.weight: Font.DemiBold
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3
                                Text { Layout.fillWidth: true; text: profileName; color: Theme.text; font.pixelSize: 14; font.weight: Font.DemiBold; elide: Text.ElideRight }
                                Text { text: active ? "Active Profile" : "Inactive"; color: active ? Theme.success : Theme.textMuted; font.pixelSize: 10 }
                                Text { text: games + " game(s)  |  " + modules + " module(s)"; color: Theme.textMuted; font.pixelSize: 9 }
                            }
                        }

                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectedProfileIndex = index
                        }
                    }
                }
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: Math.max(760, parent.width)
                spacing: 12

                XPanel {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 170

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 16

                        Rectangle {
                            Layout.preferredWidth: 112
                            Layout.preferredHeight: 112
                            radius: 14
                            color: Theme.accentSoft
                            border.width: 1
                            border.color: root.selectedProfile().active ? Theme.success : Theme.border

                            Text {
                                anchors.centerIn: parent
                                text: root.selectedProfile().profileName.length > 0 ? root.selectedProfile().profileName.charAt(0).toUpperCase() : "?"
                                color: Theme.accent
                                font.pixelSize: 42
                                font.weight: Font.Light
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                Text { Layout.fillWidth: true; text: root.selectedProfile().profileName; color: Theme.text; font.pixelSize: 30; font.weight: Font.DemiBold }
                                StatusPill {
                                    label: root.selectedProfile().active ? "Active Profile" : "Inactive"
                                    tone: root.selectedProfile().active ? Theme.success : Theme.textMuted
                                }
                            }

                            Text { text: root.selectedProfile().description; color: Theme.textMuted; font.pixelSize: 12 }

                            RowLayout {
                                spacing: 16
                                Text { text: root.selectedProfile().games + " games"; color: Theme.text; font.pixelSize: 11 }
                                Text { text: root.selectedProfile().modules + " modules"; color: Theme.text; font.pixelSize: 11 }
                                Text { text: root.selectedProfile().saveSets + " save set(s)"; color: Theme.text; font.pixelSize: 11 }
                            }

                            Item { Layout.fillHeight: true }
                        }

                        ColumnLayout {
                            spacing: 8
                            XButton {
                                text: root.selectedProfile().active ? "Active" : "Set Active"
                                variant: root.selectedProfile().active ? "default" : "primary"
                                enabled: !root.selectedProfile().active
                                onClicked: root.activateSelected()
                            }
                            XButton {
                                text: "Duplicate"
                                onClicked: {
                                    var p = root.selectedProfile()
                                    profilesModel.append({
                                        profileName: p.profileName + " Copy",
                                        description: p.description,
                                        active: false,
                                        games: p.games,
                                        modules: p.modules,
                                        saveSets: p.saveSets,
                                        gamePath: p.gamePath,
                                        savePath: p.savePath,
                                        modulePath: p.modulePath,
                                        region: p.region,
                                        startupPage: p.startupPage,
                                        offline: p.offline
                                    })
                                    root.selectedProfileIndex = profilesModel.count - 1
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    XPanel {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 440

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 10

                            Text { text: "Profile Configuration"; color: Theme.text; font.pixelSize: 18; font.weight: Font.DemiBold }

                            Text { text: "Profile Name"; color: Theme.textMuted; font.pixelSize: 10 }
                            TextField {
                                Layout.fillWidth: true
                                text: root.selectedProfile().profileName
                                color: Theme.text
                                selectionColor: Theme.accent
                                selectedTextColor: Theme.accentText
                                background: Rectangle { radius: 7; color: Theme.input; border.width: 1; border.color: Theme.border }
                                onEditingFinished: {
                                    var value = text.trim()
                                    if (value.length > 0) {
                                        profilesModel.setProperty(root.selectedProfileIndex, "profileName", value)
                                        if (root.selectedProfile().active)
                                            launcherBridge.profileName = value
                                    }
                                }
                            }

                            Text { text: "Description"; color: Theme.textMuted; font.pixelSize: 10 }
                            TextField {
                                Layout.fillWidth: true
                                text: root.selectedProfile().description
                                color: Theme.text
                                background: Rectangle { radius: 7; color: Theme.input; border.width: 1; border.color: Theme.border }
                                onEditingFinished: profilesModel.setProperty(root.selectedProfileIndex, "description", text)
                            }

                            Text { text: "Default Game Directory"; color: Theme.textMuted; font.pixelSize: 10 }
                            TextField {
                                Layout.fillWidth: true
                                text: root.selectedProfile().gamePath
                                placeholderText: "Not configured"
                                color: Theme.text
                                placeholderTextColor: Theme.textMuted
                                background: Rectangle { radius: 7; color: Theme.input; border.width: 1; border.color: Theme.border }
                                onEditingFinished: profilesModel.setProperty(root.selectedProfileIndex, "gamePath", text)
                            }

                            Text { text: "Save Data Location"; color: Theme.textMuted; font.pixelSize: 10 }
                            TextField {
                                Layout.fillWidth: true
                                text: root.selectedProfile().savePath
                                placeholderText: "Not configured"
                                color: Theme.text
                                placeholderTextColor: Theme.textMuted
                                background: Rectangle { radius: 7; color: Theme.input; border.width: 1; border.color: Theme.border }
                                onEditingFinished: profilesModel.setProperty(root.selectedProfileIndex, "savePath", text)
                            }

                            Text { text: "Preferred Regions"; color: Theme.textMuted; font.pixelSize: 10 }
                            ComboBox {
                                Layout.fillWidth: true
                                model: ["Auto (Global)", "NTSC-U", "PAL", "NTSC-J"]
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Text { Layout.fillWidth: true; text: "Start Page"; color: Theme.textMuted; font.pixelSize: 10 }
                                ComboBox { Layout.preferredWidth: 220; model: ["Library", "Modules", "Profiles"] }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Offline Mode"; color: Theme.text; font.pixelSize: 11 }
                                    Text { text: "Start this profile without online services."; color: Theme.textMuted; font.pixelSize: 9 }
                                }
                                Switch { checked: root.selectedProfile().offline }
                            }

                            Item { Layout.fillHeight: true }
                        }
                    }

                    XPanel {
                        Layout.preferredWidth: 360
                        Layout.preferredHeight: 440

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 10

                            Text { text: "Profile Summary"; color: Theme.text; font.pixelSize: 18; font.weight: Font.DemiBold }

                            Repeater {
                                model: [
                                    ["Games", root.selectedProfile().games],
                                    ["Modules", root.selectedProfile().modules],
                                    ["Save Sets", root.selectedProfile().saveSets],
                                    ["Region", root.selectedProfile().region]
                                ]

                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Text { Layout.fillWidth: true; text: modelData[0]; color: Theme.textMuted; font.pixelSize: 11 }
                                    Text { text: modelData[1]; color: Theme.text; font.pixelSize: 11; font.weight: Font.Medium }
                                }
                            }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                            Text { text: "Default Content Directories"; color: Theme.text; font.pixelSize: 14; font.weight: Font.DemiBold }
                            Text { Layout.fillWidth: true; text: "Games  " + (root.selectedProfile().gamePath.length > 0 ? root.selectedProfile().gamePath : "Not configured"); color: Theme.textMuted; font.pixelSize: 10; elide: Text.ElideMiddle }
                            Text { Layout.fillWidth: true; text: "Saves  " + (root.selectedProfile().savePath.length > 0 ? root.selectedProfile().savePath : "Not configured"); color: Theme.textMuted; font.pixelSize: 10; elide: Text.ElideMiddle }
                            Text { Layout.fillWidth: true; text: "Modules  " + (root.selectedProfile().modulePath.length > 0 ? root.selectedProfile().modulePath : "Not configured"); color: Theme.textMuted; font.pixelSize: 10; elide: Text.ElideMiddle }

                            Item { Layout.fillHeight: true }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                            Text { text: "Profile Actions"; color: Theme.text; font.pixelSize: 14; font.weight: Font.DemiBold }

                            RowLayout {
                                Layout.fillWidth: true
                                XButton { Layout.fillWidth: true; text: "Export"; onClicked: launcherBridge.notifyUnavailable("Export Profile") }
                                XButton {
                                    Layout.fillWidth: true
                                    text: "Delete"
                                    variant: "danger"
                                    enabled: profilesModel.count > 1 && !root.selectedProfile().active
                                    onClicked: {
                                        profilesModel.remove(root.selectedProfileIndex)
                                        root.selectedProfileIndex = Math.max(0, Math.min(root.selectedProfileIndex, profilesModel.count - 1))
                                    }
                                }
                            }

                            XButton { Layout.fillWidth: true; text: "Import Profile"; onClicked: launcherBridge.notifyUnavailable("Import Profile") }
                        }
                    }
                }
            }
        }
    }
}
