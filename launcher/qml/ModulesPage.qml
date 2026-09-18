import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Item {
    id: root

    property string searchText: ""
    property int selectedModuleIndex: -1

    readonly property bool hasModules: modulesModel.count > 0
    readonly property bool testMode: launcherBridge.testMode

    ListModel { id: modulesModel }

    function emptyModule() {
        return {
            moduleName: "",
            description: "",
            typeName: "",
            version: "",
            active: false,
            status: "",
            regions: "",
            updated: "",
            moduleId: "",
            gameIds: "",
            dependency: "",
            renderer: ""
        }
    }

    function selectedModule() {
        if (!hasModules || selectedModuleIndex < 0 || selectedModuleIndex >= modulesModel.count)
            return emptyModule()
        return modulesModel.get(selectedModuleIndex)
    }

    function matchesSearch(name, typeName) {
        var needle = searchText.trim().toLowerCase()
        if (needle.length === 0)
            return true
        return name.toLowerCase().indexOf(needle) !== -1
            || typeName.toLowerCase().indexOf(needle) !== -1
    }

    function populateTestData() {
        modulesModel.clear()
        modulesModel.append({
            moduleName: "Test Flight Module",
            description: "Fictional game module used exclusively to exercise the Xenon module manager UI.",
            typeName: "Game Module",
            version: "0.1-test",
            active: true,
            status: "Active",
            regions: "Test Region A / B",
            updated: "Test fixture",
            moduleId: "xenon.test.flight",
            gameIds: "TEST0001",
            dependency: "Xenon Recomp",
            renderer: "Vulkan"
        })
        modulesModel.append({
            moduleName: "Test Runtime Component",
            description: "Fictional shared component used to preview runtime-component rows and actions.",
            typeName: "Runtime Component",
            version: "0.4-test",
            active: true,
            status: "Installed",
            regions: "Global",
            updated: "Test fixture",
            moduleId: "xenon.test.runtime",
            gameIds: "Shared",
            dependency: "Core",
            renderer: "Backend Defined"
        })
        modulesModel.append({
            moduleName: "Disabled Test Module",
            description: "Fictional disabled entry used to test module state switching.",
            typeName: "Game Module",
            version: "0.0-test",
            active: false,
            status: "Disabled",
            regions: "Module Defined",
            updated: "Test fixture",
            moduleId: "xenon.test.disabled",
            gameIds: "TEST0002",
            dependency: "Xenon Recomp",
            renderer: "Automatic"
        })
        selectedModuleIndex = 0
    }

    Component.onCompleted: {
        if (testMode)
            populateTestData()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                RowLayout {
                    spacing: 8
                    Text {
                        text: "Modules"
                        color: Theme.text
                        font.pixelSize: 30
                        font.weight: Font.DemiBold
                    }
                    StatusPill {
                        visible: root.testMode
                        label: "TEST MODE"
                        tone: Theme.warning
                    }
                }

                Text {
                    text: "Manage game modules, runtime components, and support packages discovered by Xenon."
                    color: Theme.textMuted
                    font.pixelSize: 13
                }
            }

            XButton {
                text: "+  Import Module"
                onClicked: importModuleDialog.open()
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.hasModules ? 1 : 0

            XPanel {
                EmptyState {
                    anchors.fill: parent
                    glyph: "◇"
                    title: root.testMode ? "No test modules loaded" : "No modules installed"
                    description: root.testMode
                        ? "Reload the fictional module fixtures to exercise the module manager."
                        : "Xenon only shows modules that have actually been imported or discovered. Import a compatible local module package to begin."
                    primaryText: root.testMode ? "Reload Test Modules" : "Import Module"
                    secondaryText: root.testMode ? "Import Module" : ""
                    onPrimaryClicked: {
                        if (root.testMode)
                            root.populateTestData()
                        else
                            importModuleDialog.open()
                    }
                    onSecondaryClicked: importModuleDialog.open()
                }
            }

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    XPanel {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                Text { Layout.preferredWidth: 300; text: "Module"; color: Theme.textMuted; font.pixelSize: 10; font.weight: Font.DemiBold }
                                Text { Layout.preferredWidth: 150; text: "Type"; color: Theme.textMuted; font.pixelSize: 10; font.weight: Font.DemiBold }
                                Text { Layout.preferredWidth: 110; text: "Version"; color: Theme.textMuted; font.pixelSize: 10; font.weight: Font.DemiBold }
                                Text { Layout.preferredWidth: 130; text: "Status"; color: Theme.textMuted; font.pixelSize: 10; font.weight: Font.DemiBold }
                                Text { Layout.fillWidth: true; text: "Supported Regions"; color: Theme.textMuted; font.pixelSize: 10; font.weight: Font.DemiBold }
                            }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                            ListView {
                                id: modulesList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 4
                                model: modulesModel

                                delegate: Rectangle {
                                    required property int index
                                    required property string moduleName
                                    required property string description
                                    required property string typeName
                                    required property string version
                                    required property bool active
                                    required property string status
                                    required property string regions

                                    width: modulesList.width
                                    height: visible ? 60 : 0
                                    visible: root.matchesSearch(moduleName, typeName)
                                    radius: 8
                                    color: root.selectedModuleIndex === index ? Theme.accentSoft
                                         : rowMouse.containsMouse ? Theme.surfaceHover
                                         : "transparent"
                                    border.width: root.selectedModuleIndex === index ? 1 : 0
                                    border.color: Theme.accent

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        spacing: 12

                                        ColumnLayout {
                                            Layout.preferredWidth: 290
                                            spacing: 1
                                            Text { Layout.fillWidth: true; text: moduleName; color: Theme.text; font.pixelSize: 12; font.weight: Font.DemiBold; elide: Text.ElideRight }
                                            Text { Layout.fillWidth: true; text: description; color: Theme.textMuted; font.pixelSize: 9; elide: Text.ElideRight }
                                        }

                                        Text { Layout.preferredWidth: 150; text: typeName; color: Theme.textMuted; font.pixelSize: 11 }
                                        Text { Layout.preferredWidth: 110; text: version; color: Theme.text; font.pixelSize: 11 }

                                        RowLayout {
                                            Layout.preferredWidth: 130
                                            spacing: 6
                                            Rectangle { width: 9; height: 9; radius: 5; color: active ? Theme.success : Theme.textMuted }
                                            Text { text: status; color: active ? Theme.success : Theme.textMuted; font.pixelSize: 11 }
                                        }

                                        Text { Layout.fillWidth: true; text: regions; color: Theme.textMuted; font.pixelSize: 11 }
                                    }

                                    MouseArea {
                                        id: rowMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.selectedModuleIndex = index
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 250
                        spacing: 12

                        XPanel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 16

                                ArtworkFrame {
                                    Layout.preferredWidth: 300
                                    Layout.fillHeight: true
                                    fallbackTitle: root.selectedModule().moduleName.toUpperCase()
                                    hero: true
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 5

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            Layout.fillWidth: true
                                            text: root.selectedModule().moduleName
                                            color: Theme.text
                                            font.pixelSize: 23
                                            font.weight: Font.DemiBold
                                        }
                                        StatusPill {
                                            visible: root.testMode
                                            label: "TEST DATA"
                                            tone: Theme.warning
                                        }
                                    }

                                    StatusPill {
                                        label: root.selectedModule().active ? "Active" : "Disabled"
                                        tone: root.selectedModule().active ? Theme.success : Theme.textMuted
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: root.selectedModule().description
                                        color: Theme.textMuted
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: 11
                                    }

                                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                                    Repeater {
                                        model: [
                                            ["Module ID", root.selectedModule().moduleId],
                                            ["Version", root.selectedModule().version],
                                            ["Type", root.selectedModule().typeName],
                                            ["Supported Regions", root.selectedModule().regions],
                                            ["Supported Game IDs", root.selectedModule().gameIds],
                                            ["Runtime Dependency", root.selectedModule().dependency],
                                            ["Renderer", root.selectedModule().renderer]
                                        ]

                                        delegate: RowLayout {
                                            required property var modelData
                                            Layout.fillWidth: true
                                            Text { Layout.preferredWidth: 140; text: modelData[0]; color: Theme.textMuted; font.pixelSize: 10 }
                                            Text { Layout.fillWidth: true; text: modelData[1]; color: Theme.text; font.pixelSize: 10; elide: Text.ElideRight }
                                        }
                                    }

                                    Item { Layout.fillHeight: true }
                                }
                            }
                        }

                        XPanel {
                            Layout.preferredWidth: 250
                            Layout.fillHeight: true

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 8

                                Text { text: "Module Actions"; color: Theme.text; font.pixelSize: 16; font.weight: Font.DemiBold }

                                XButton {
                                    Layout.fillWidth: true
                                    text: root.selectedModule().active ? "Disable Module" : "Enable Module"
                                    variant: root.selectedModule().active ? "default" : "primary"
                                    onClicked: {
                                        var newState = !root.selectedModule().active
                                        modulesModel.setProperty(root.selectedModuleIndex, "active", newState)
                                        modulesModel.setProperty(root.selectedModuleIndex, "status", newState ? "Active" : "Disabled")
                                        launcherBridge.notify("Module state changed", root.selectedModule().moduleName + (newState ? " enabled for this UI session." : " disabled for this UI session."))
                                    }
                                }

                                XButton { Layout.fillWidth: true; text: "Check for Updates"; onClicked: launcherBridge.notifyUnavailable(text) }
                                XButton { Layout.fillWidth: true; text: "Verify Module"; onClicked: launcherBridge.notifyUnavailable(text) }
                                XButton { Layout.fillWidth: true; text: "Open Module Folder"; onClicked: launcherBridge.notifyUnavailable(text) }

                                XButton {
                                    visible: root.testMode
                                    Layout.fillWidth: true
                                    text: "Remove Test Module"
                                    variant: "danger"
                                    onClicked: {
                                        modulesModel.remove(root.selectedModuleIndex)
                                        if (modulesModel.count === 0)
                                            root.selectedModuleIndex = -1
                                        else
                                            root.selectedModuleIndex = Math.min(root.selectedModuleIndex, modulesModel.count - 1)
                                    }
                                }

                                Item { Layout.fillHeight: true }
                            }
                        }
                    }
                }
            }
        }
    }

    FileDialog {
        id: importModuleDialog
        title: "Import Xenon module"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Xenon module packages (*.xenonmodule *.json)", "All files (*)"]
        onAccepted: launcherBridge.notify(
            "Module selected",
            "A local module package was selected. Installation, validation, and registry insertion will be delegated to the module service once connected.")
    }
}
