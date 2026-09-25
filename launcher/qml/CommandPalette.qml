import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    property string query: ""
    property var results: []
    property int selectedIndex: results.length > 0 ? 0 : -1
    property int resultLimit: 24

    signal commandExecuted()

    width: 680
    height: Math.min(540, Math.max(180, contentColumn.implicitHeight + padding * 2))
    padding: Theme.spaceSm
    focus: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

    function refresh() {
        var previousId = root.selectedIndex >= 0 && root.selectedIndex < root.results.length
            ? String(root.results[root.selectedIndex].id || "") : ""
        root.results = launcherBridge.commandPaletteResults(root.query, root.resultLimit)
        var next = root.results.length > 0 ? 0 : -1
        if (previousId.length > 0) {
            for (var i = 0; i < root.results.length; ++i) {
                if (String(root.results[i].id || "") === previousId) {
                    next = i
                    break
                }
            }
        }
        root.selectedIndex = next
        resultList.currentIndex = next
        if (next >= 0) resultList.positionViewAtIndex(next, ListView.Contain)
    }

    function openPalette() {
        root.refresh()
        if (!root.visible) root.open()
    }

    function moveSelection(delta) {
        if (root.results.length === 0) return
        var next = root.selectedIndex
        if (next < 0) next = 0
        else next = (next + delta + root.results.length) % root.results.length
        root.selectedIndex = next
        resultList.currentIndex = next
        resultList.positionViewAtIndex(next, ListView.Contain)
    }

    function activateSelected() {
        if (root.selectedIndex < 0 || root.selectedIndex >= root.results.length) return false
        var entry = root.results[root.selectedIndex]
        launcherBridge.executeCommandPaletteAction(
            String(entry.commandId || ""),
            String(entry.targetId || ""),
            String(entry.sectionId || ""))
        root.close()
        root.commandExecuted()
        return true
    }

    onQueryChanged: {
        if (root.visible) root.refresh()
    }
    onOpened: root.refresh()

    Connections {
        target: launcherBridge
        function onCommandPaletteChanged() {
            if (root.visible) root.refresh()
        }
    }

    background: Rectangle {
        color: Theme.surfaceRaised
        radius: Theme.dialogRadius
        border.width: Theme.focusWidth
        border.color: Theme.accent
    }

    contentItem: ColumnLayout {
        id: contentColumn
        spacing: Theme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXs
            Layout.rightMargin: Theme.spaceXs
            spacing: Theme.spaceSm

            Text {
                Layout.fillWidth: true
                text: root.query.trim().length > 0 ? "Search Xenon" : "Quick actions"
                color: Theme.text
                font.pixelSize: Theme.typeBodyLarge
                font.weight: Font.DemiBold
            }
            Text {
                text: root.results.length + (root.results.length === 1 ? " result" : " results")
                color: Theme.textMuted
                font.pixelSize: Theme.typeCaption
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(392, Math.max(86, root.results.length * 64))

            ListView {
                id: resultList
                anchors.fill: parent
                clip: true
                spacing: 2
                model: root.results
                currentIndex: root.selectedIndex
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }

                delegate: Rectangle {
                    id: resultDelegate
                    required property int index
                    required property var modelData

                    width: resultList.width
                    height: 62
                    radius: Theme.controlRadius
                    color: root.selectedIndex === index ? Theme.accentSoft
                         : hoverArea.containsMouse ? Theme.surfaceHover
                         : "transparent"
                    border.width: root.selectedIndex === index ? Theme.borderWidth : 0
                    border.color: root.selectedIndex === index ? Theme.accentStrong : "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceMd
                        anchors.rightMargin: Theme.spaceMd
                        spacing: Theme.spaceMd

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Text {
                                Layout.fillWidth: true
                                text: String(resultDelegate.modelData.title || "")
                                color: Theme.text
                                font.pixelSize: Theme.typeBody
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: String(resultDelegate.modelData.subtitle || "")
                                color: Theme.textMuted
                                font.pixelSize: Theme.typeCaption
                                elide: Text.ElideRight
                            }
                        }

                        StatusPill {
                            visible: String(resultDelegate.modelData.group || "").length > 0
                            label: String(resultDelegate.modelData.group || "").toUpperCase()
                            tone: Theme.textMuted
                        }

                        Text {
                            visible: String(resultDelegate.modelData.badge || "").length > 0
                            text: String(resultDelegate.modelData.badge || "")
                            color: Theme.textMuted
                            font.pixelSize: Theme.typeCaption
                            font.weight: Font.Medium
                        }
                    }

                    MouseArea {
                        id: hoverArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onEntered: {
                            root.selectedIndex = resultDelegate.index
                            resultList.currentIndex = resultDelegate.index
                        }
                        onClicked: {
                            root.selectedIndex = resultDelegate.index
                            root.activateSelected()
                        }
                    }
                }
            }

            ColumnLayout {
                anchors.centerIn: parent
                visible: root.results.length === 0
                spacing: Theme.spaceXs

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "No matching commands"
                    color: Theme.text
                    font.pixelSize: Theme.typeBody
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Try a game, module, profile, setting or launcher action."
                    color: Theme.textMuted
                    font.pixelSize: Theme.typeCaption
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXs
            Layout.rightMargin: Theme.spaceXs
            spacing: Theme.spaceMd

            Text { text: "↑↓ Navigate"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
            Text { text: "Enter Open"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
            Text { text: "Esc Close"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
            Item { Layout.fillWidth: true }
            Text { text: "Ctrl+K"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption; font.weight: Font.DemiBold }
        }
    }
}
