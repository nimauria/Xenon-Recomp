import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Item {
    id: root

    property string searchText: ""
    property int selectedGameIndex: -1

    signal requestPage(int index)

    readonly property bool hasGames: gamesModel.count > 0
    readonly property bool testMode: launcherBridge.testMode

    ListModel { id: gamesModel }
    ListModel { id: dlcModel }

    function emptyGame() {
        return {
            title: "",
            moduleName: "",
            status: "",
            ready: false,
            installed: false,
            tileArt: "",
            heroArt: "",
            description: "",
            gameId: "",
            renderer: "",
            mode: "",
            regions: "",
            content: ""
        }
    }

    function selectedGame() {
        if (!hasGames || selectedGameIndex < 0 || selectedGameIndex >= gamesModel.count)
            return emptyGame()
        return gamesModel.get(selectedGameIndex)
    }

    function matchesSearch(title, moduleName) {
        var needle = searchText.trim().toLowerCase()
        if (needle.length === 0)
            return true
        return title.toLowerCase().indexOf(needle) !== -1
            || moduleName.toLowerCase().indexOf(needle) !== -1
    }

    function clearLibrary() {
        gamesModel.clear()
        dlcModel.clear()
        selectedGameIndex = -1
    }

    function populateTestData() {
        clearLibrary()

        gamesModel.append({
            title: "Xenon Test Flight",
            moduleName: "Test Flight Module",
            status: "Ready to Play",
            ready: true,
            installed: true,
            tileArt: "",
            heroArt: "",
            description: "A fictional launcher-only game used to exercise the complete Project Xenon library interface before the module registry and runtime services are connected. No real game content is bundled or implied.",
            gameId: "TEST0001",
            renderer: "Vulkan",
            mode: "Offline",
            regions: "Test Region A / B",
            content: "Test Base / Test Update / Test Add-ons"
        })

        gamesModel.append({
            title: "Xenon Test Arena",
            moduleName: "Test Arena Module",
            status: "Content Missing",
            ready: false,
            installed: true,
            tileArt: "",
            heroArt: "",
            description: "A second fictional entry used to verify disabled launch states, incomplete content indicators, and multi-game library selection.",
            gameId: "TEST0002",
            renderer: "Automatic",
            mode: "Offline",
            regions: "Module Defined",
            content: "Test Base Missing"
        })

        dlcModel.append({ name: "Test Expansion Alpha", installed: true })
        dlcModel.append({ name: "Test Mission Pack", installed: true })
        dlcModel.append({ name: "Test Vehicle Pack", installed: false })
        dlcModel.append({ name: "Test Cosmetic Pack", installed: false })
        dlcModel.append({ name: "Test Challenge Pack", installed: true })

        selectedGameIndex = 0
    }

    function removeSelectedTestEntry() {
        if (!testMode || !hasGames || selectedGameIndex < 0)
            return

        gamesModel.remove(selectedGameIndex)
        if (gamesModel.count === 0) {
            selectedGameIndex = -1
            dlcModel.clear()
        } else {
            selectedGameIndex = Math.min(selectedGameIndex, gamesModel.count - 1)
        }
    }

    Component.onCompleted: {
        // Normal builds deliberately start empty. Real entries will eventually
        // come from the module/content registry. Only the compile-time test
        // mode injects fictional front-end data.
        if (testMode)
            populateTestData()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 14

        // ---------------------------------------------------------------------
        // Library list
        // ---------------------------------------------------------------------
        XPanel {
            Layout.preferredWidth: 338
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
                        text: "Games"
                        color: Theme.text
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }

                    StatusPill {
                        visible: root.testMode
                        label: "TEST MODE"
                        tone: Theme.warning
                    }

                    Text {
                        text: root.hasGames ? gamesModel.count.toString() : "0"
                        color: Theme.textMuted
                        font.pixelSize: 12
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: root.hasGames ? 1 : 0

                    Item {
                        EmptyState {
                            anchors.fill: parent
                            glyph: "▣"
                            title: root.testMode ? "No test games loaded" : "Your library is empty"
                            description: root.testMode
                                ? "Test mode is enabled, but its fixture data has been cleared. Reload it to exercise the full library interface."
                                : "Xenon does not ship with games or copyrighted content. Import a compatible module and select your own legally obtained local game files."
                            primaryText: root.testMode ? "Reload Test Data" : "Import Module"
                            secondaryText: root.testMode ? "Import Module" : "Select Game Content"
                            onPrimaryClicked: {
                                if (root.testMode)
                                    root.populateTestData()
                                else
                                    addModuleDialog.open()
                            }
                            onSecondaryClicked: {
                                if (root.testMode)
                                    addModuleDialog.open()
                                else
                                    gameContentDialog.open()
                            }
                        }
                    }

                    ListView {
                        id: gameList
                        clip: true
                        spacing: 8
                        model: gamesModel

                        delegate: GameTile {
                            required property int index
                            required property string title
                            required property string moduleName
                            required property string status
                            required property bool ready
                            required property bool installed
                            required property string tileArt

                            width: gameList.width
                            height: visible ? 114 : 0
                            visible: root.matchesSearch(title, moduleName)
                            artworkSource: tileArt
                            selected: root.selectedGameIndex === index

                            onActivated: root.selectedGameIndex = index
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    XButton {
                        Layout.fillWidth: true
                        text: "+  Import Module"
                        variant: "ghost"
                        onClicked: addModuleDialog.open()
                    }

                    XButton {
                        visible: root.hasGames
                        text: "+ Content"
                        variant: "ghost"
                        onClicked: gameContentDialog.open()
                    }
                }
            }
        }

        // ---------------------------------------------------------------------
        // Selected game / empty library page
        // ---------------------------------------------------------------------
        XPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "transparent"
            border.width: 0

            StackLayout {
                anchors.fill: parent
                currentIndex: root.hasGames ? 1 : 0

                Item {
                    EmptyState {
                        anchors.fill: parent
                        glyph: "X"
                        title: root.testMode ? "Xenon UI test mode" : "No game selected"
                        description: root.testMode
                            ? "Load the fictional test fixture to preview the game hero, details, content states, menus, DLC panel, and compatibility cards."
                            : "Once a compatible module and local game content are imported, the selected game page will appear here. Nothing is pre-populated in a normal Xenon build."
                        primaryText: root.testMode ? "Load Test Fixture" : "Import Module"
                        secondaryText: root.testMode ? "" : "Select Game Content"
                        onPrimaryClicked: {
                            if (root.testMode)
                                root.populateTestData()
                            else
                                addModuleDialog.open()
                        }
                        onSecondaryClicked: gameContentDialog.open()
                    }
                }

                ScrollView {
                    id: detailScroll
                    clip: true
                    contentWidth: availableWidth

                    ColumnLayout {
                        width: Math.max(880, detailScroll.availableWidth)
                        spacing: 12

                        ArtworkFrame {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 300
                            source: root.selectedGame().heroArt
                            fallbackTitle: (root.selectedGame().moduleName.length > 0
                                ? root.selectedGame().moduleName.toUpperCase()
                                : "MODULE") + "  •  MODULE-PROVIDED HERO ART"
                            hero: true

                            StatusPill {
                                visible: root.testMode
                                anchors.top: parent.top
                                anchors.right: parent.right
                                anchors.margins: 14
                                label: "FICTIONAL TEST DATA"
                                tone: Theme.warning
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignTop
                                spacing: 9

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Text {
                                        Layout.fillWidth: true
                                        text: root.selectedGame().title
                                        color: Theme.text
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: 31
                                        font.weight: Font.DemiBold
                                    }

                                    XButton {
                                        text: "⋯"
                                        implicitWidth: 48
                                        implicitHeight: 36
                                        onClicked: gameActionsMenu.open()

                                        Menu {
                                            id: gameActionsMenu
                                            width: 230

                                            MenuItem {
                                                text: "Game Properties"
                                                onTriggered: launcherBridge.notifyUnavailable(text)
                                            }
                                            MenuItem {
                                                text: "Module Settings"
                                                onTriggered: launcherBridge.notifyUnavailable(text)
                                            }
                                            MenuSeparator { }
                                            MenuItem {
                                                text: root.testMode ? "Remove Test Entry" : "Remove from Library"
                                                onTriggered: {
                                                    if (root.testMode)
                                                        root.removeSelectedTestEntry()
                                                    else
                                                        launcherBridge.notifyUnavailable(text)
                                                }
                                            }
                                        }
                                    }
                                }

                                RowLayout {
                                    spacing: 7

                                    Repeater {
                                        model: ["Module Managed", "Local Content", "Offline Ready", root.selectedGame().moduleName]

                                        delegate: Rectangle {
                                            required property string modelData
                                            implicitWidth: tagText.implicitWidth + 20
                                            implicitHeight: 26
                                            radius: 13
                                            color: Theme.surfaceAlt
                                            border.width: 1
                                            border.color: Theme.border

                                            Text {
                                                id: tagText
                                                anchors.centerIn: parent
                                                text: modelData
                                                color: Theme.textMuted
                                                font.pixelSize: 10
                                            }
                                        }
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: root.selectedGame().description
                                    color: Theme.textMuted
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 13
                                    lineHeight: 1.25
                                }

                                RowLayout {
                                    spacing: 10

                                    XButton {
                                        Layout.preferredWidth: 205
                                        Layout.preferredHeight: 52
                                        text: "▶   Play"
                                        variant: "primary"
                                        enabled: root.selectedGame().ready
                                        onClicked: launcherBridge.notifyUnavailable("Launch " + root.selectedGame().title)
                                    }

                                    XButton {
                                        id: manageFilesButton
                                        Layout.preferredWidth: 205
                                        Layout.preferredHeight: 52
                                        text: "Manage Files  ▾"
                                        onClicked: manageMenu.open()

                                        Menu {
                                            id: manageMenu
                                            width: 250

                                            MenuItem {
                                                text: "Browse Game Files"
                                                onTriggered: launcherBridge.notifyUnavailable(text)
                                            }
                                            MenuItem {
                                                text: "Open Save Data"
                                                onTriggered: launcherBridge.notifyUnavailable(text)
                                            }
                                            MenuItem {
                                                text: "Open Module Folder"
                                                onTriggered: launcherBridge.notifyUnavailable(text)
                                            }
                                            MenuSeparator { }
                                            MenuItem {
                                                text: "Verify Imported Content"
                                                onTriggered: launcherBridge.notifyUnavailable(text)
                                            }
                                            MenuItem {
                                                text: "Import / Replace Game Content"
                                                onTriggered: gameContentDialog.open()
                                            }
                                        }
                                    }

                                    Item { Layout.fillWidth: true }
                                }
                            }

                            XPanel {
                                Layout.preferredWidth: 405
                                Layout.preferredHeight: 270
                                Layout.alignment: Qt.AlignTop

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    spacing: 6

                                    RowLayout {
                                        Layout.fillWidth: true

                                        Text {
                                            Layout.fillWidth: true
                                            text: "DLC & Add-ons"
                                            color: Theme.text
                                            font.pixelSize: 18
                                            font.weight: Font.DemiBold
                                        }

                                        XButton {
                                            text: "+  Import DLC"
                                            implicitHeight: 36
                                            onClicked: importDlcDialog.open()
                                        }
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 1
                                        color: Theme.divider
                                    }

                                    ListView {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        clip: true
                                        model: dlcModel
                                        spacing: 2

                                        delegate: RowLayout {
                                            required property string name
                                            required property bool installed

                                            width: ListView.view.width
                                            height: 30
                                            spacing: 9

                                            Rectangle {
                                                width: 18
                                                height: 18
                                                radius: 9
                                                color: installed ? Theme.success : Theme.danger

                                                Text {
                                                    anchors.centerIn: parent
                                                    text: installed ? "✓" : "×"
                                                    color: Theme.accentText
                                                    font.pixelSize: 11
                                                    font.weight: Font.Bold
                                                }
                                            }

                                            Text {
                                                Layout.fillWidth: true
                                                text: name
                                                color: Theme.text
                                                font.pixelSize: 11
                                                elide: Text.ElideRight
                                            }

                                            Text {
                                                text: installed ? "Installed" : "Not Installed"
                                                color: installed ? Theme.success : Theme.danger
                                                font.pixelSize: 11
                                            }
                                        }
                                    }

                                    Text {
                                        visible: dlcModel.count === 0
                                        Layout.alignment: Qt.AlignHCenter
                                        text: "No add-on catalogue supplied by this module."
                                        color: Theme.textMuted
                                        font.pixelSize: 10
                                    }
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            XPanel {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 244

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    spacing: 5

                                    Text {
                                        text: "Game Information"
                                        color: Theme.text
                                        font.pixelSize: 17
                                        font.weight: Font.DemiBold
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 1
                                        color: Theme.divider
                                    }

                                    Repeater {
                                        model: [
                                            ["Game ID", root.selectedGame().gameId],
                                            ["Module", root.selectedGame().moduleName],
                                            ["Runtime", "Xenon Recomp"],
                                            ["Renderer", root.selectedGame().renderer],
                                            ["Mode", root.selectedGame().mode],
                                            ["Regions", root.selectedGame().regions],
                                            ["Content", root.selectedGame().content]
                                        ]

                                        delegate: RowLayout {
                                            required property var modelData
                                            Layout.fillWidth: true

                                            Text {
                                                Layout.preferredWidth: 112
                                                text: modelData[0]
                                                color: Theme.textMuted
                                                font.pixelSize: 11
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: modelData[1]
                                                color: Theme.text
                                                font.pixelSize: 11
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }

                                    RowLayout {
                                        spacing: 8
                                        Text {
                                            Layout.preferredWidth: 112
                                            text: "Status"
                                            color: Theme.textMuted
                                            font.pixelSize: 11
                                        }
                                        Rectangle {
                                            width: 9
                                            height: 9
                                            radius: 5
                                            color: root.selectedGame().ready ? Theme.warning : Theme.danger
                                        }
                                        Text {
                                            text: root.selectedGame().ready ? "Early Development" : root.selectedGame().status
                                            color: root.selectedGame().ready ? Theme.warning : Theme.danger
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
                                        }
                                    }
                                }
                            }

                            XPanel {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 244

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    spacing: 5

                                    Text {
                                        text: "Compatibility & Module Status"
                                        color: Theme.text
                                        font.pixelSize: 17
                                        font.weight: Font.DemiBold
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 1
                                        color: Theme.divider
                                    }

                                    Repeater {
                                        model: [
                                            ["Module", root.selectedGame().installed ? "Active" : "Not Installed", root.selectedGame().installed ? "good" : "bad"],
                                            ["Runtime", launcherBridge.backendConnected ? "Connected" : "Front-end Ready", launcherBridge.backendConnected ? "good" : "warn"],
                                            ["Renderer", root.selectedGame().renderer.length === 0 ? "Unavailable" : "Configured", root.selectedGame().renderer.length === 0 ? "bad" : "good"],
                                            ["Game Launch", root.selectedGame().ready ? "UI Ready / Backend Pending" : "Unavailable", root.selectedGame().ready ? "warn" : "bad"],
                                            ["Audio", "Backend Pending", "warn"],
                                            ["Online Features", "Not Implemented", "bad"],
                                            ["Achievements", "Not Implemented", "bad"]
                                        ]

                                        delegate: RowLayout {
                                            required property var modelData
                                            Layout.fillWidth: true

                                            Text {
                                                Layout.fillWidth: true
                                                text: modelData[0]
                                                color: Theme.textMuted
                                                font.pixelSize: 11
                                            }
                                            Rectangle {
                                                width: 9
                                                height: 9
                                                radius: 5
                                                color: modelData[2] === "good" ? Theme.success
                                                     : modelData[2] === "warn" ? Theme.warning
                                                     : Theme.danger
                                            }
                                            Text {
                                                Layout.preferredWidth: 175
                                                text: modelData[1]
                                                color: modelData[2] === "good" ? Theme.success
                                                     : modelData[2] === "warn" ? Theme.warning
                                                     : Theme.danger
                                                font.pixelSize: 11
                                            }
                                        }
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 38
                                        radius: 6
                                        color: Theme.accentSoft

                                        Text {
                                            anchors.fill: parent
                                            anchors.margins: 7
                                            text: root.testMode
                                                ? "Fictional test data is active. Buttons and menus exercise the front-end only."
                                                : "Launcher controls are active. Runtime-backed actions connect when Xenon services become available."
                                            color: Theme.textMuted
                                            wrapMode: Text.WordWrap
                                            font.pixelSize: 9
                                        }
                                    }
                                }
                            }
                        }

                        Item { Layout.preferredHeight: 8 }
                    }
                }
            }
        }
    }

    FileDialog {
        id: importDlcDialog
        title: "Import DLC from local files"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["All files (*)"]
        onAccepted: launcherBridge.notify(
            "DLC selected",
            "Selected " + selectedFiles.length + " local item(s). Validation and import will be delegated to the selected game module when its backend is connected.")
    }

    FileDialog {
        id: addModuleDialog
        title: "Import Xenon game module"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Xenon module manifests (*.json *.xenonmodule)", "All files (*)"]
        onAccepted: launcherBridge.notify(
            "Module selected",
            "A local module package was selected. Installation and manifest registration are waiting for the module backend.")
    }

    FileDialog {
        id: gameContentDialog
        title: "Select legally obtained local game content"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["All files (*)"]
        onAccepted: launcherBridge.notify(
            "Game content selected",
            "Selected " + selectedFiles.length + " local item(s). Xenon will ask the compatible game module to identify and validate this content when the content backend is connected.")
    }
}
