import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    property string gameId: ""
    property var details: ({})

    function openFor(id) {
        gameId = id || ""
        details = launcherBridge.libraryGameProperties(gameId)
        open()
    }

    function formatDuration(milliseconds) {
        var seconds = Math.max(0, Math.floor(Number(milliseconds || 0) / 1000))
        var hours = Math.floor(seconds / 3600)
        var minutes = Math.floor((seconds % 3600) / 60)
        if (hours > 0) return hours + "h " + minutes + "m"
        if (minutes > 0) return minutes + "m"
        return seconds + "s"
    }

    parent: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(820, parent ? parent.width - Theme.space2Xl * 2 : 820)
    height: Math.min(760, parent ? parent.height - Theme.space2Xl * 2 : 760)
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
            Layout.preferredHeight: Math.max(84, propertiesHeading.implicitHeight + Theme.spaceLg * 2)

            ColumnLayout {
                id: propertiesHeading
                anchors.left: parent.left
                anchors.right: closeButton.left
                anchors.leftMargin: Theme.spaceXl
                anchors.rightMargin: Theme.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Text {
                    Layout.fillWidth: true
                    text: String(root.details.title || "Game properties")
                    color: Theme.text
                    font.pixelSize: Theme.typeSubtitle
                    font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    text: "Launcher-managed metadata, paths and module state"
                    color: Theme.textMuted
                    font.pixelSize: Theme.typeCaption
                }
            }

            XIconButton {
                id: closeButton
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
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff

            Item {
                width: parent.width
                implicitHeight: propertiesColumn.implicitHeight + Theme.spaceLg * 2

                ColumnLayout {
                    id: propertiesColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.spaceLg
                    spacing: Theme.spaceMd

                    XPanel {
                        Layout.fillWidth: true
                        implicitHeight: identityColumn.implicitHeight + Theme.spaceLg * 2
                        ColumnLayout {
                            id: identityColumn
                            anchors.fill: parent
                            anchors.margins: Theme.spaceLg
                            spacing: Theme.spaceSm
                            Text { text: "Identity"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                            XInfoRow { label: "Game ID"; value: String(root.details.gameId || "Unknown") }
                            XInfoRow { label: "Module"; value: String(root.details.moduleName || "Unassigned") }
                            XInfoRow { label: "Module ID"; value: String(root.details.moduleId || "Unassigned") }
                            XInfoRow { label: "Module version"; value: String(root.details.moduleVersion || "Unknown") }
                            XInfoRow { label: "Module state"; value: Boolean(root.details.moduleActive) ? "Active" : "Unavailable / disabled" }
                            XInfoRow { label: "Active profile"; value: String(root.details.activeProfile || "Default") }
                        }
                    }

                    XPanel {
                        Layout.fillWidth: true
                        implicitHeight: contentColumn.implicitHeight + Theme.spaceLg * 2
                        ColumnLayout {
                            id: contentColumn
                            anchors.fill: parent
                            anchors.margins: Theme.spaceLg
                            spacing: Theme.spaceSm
                            Text { text: "Content & storage"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                            XInfoRow { label: "Registered content"; value: String(root.details.contentPath || "Not registered") }
                            XInfoRow { label: "Content state"; value: Boolean(root.details.contentExists) ? "Present" : "Missing" }
                            XInfoRow { label: "Managed game root"; value: String(root.details.managedPath || "Not available") }
                            XInfoRow { label: "DLC root"; value: String(root.details.dlcRoot || "Not available") }
                            XInfoRow { label: "Save data"; value: String(root.details.savePath || "Not available") }
                            XInfoRow { label: "Screenshots"; value: String(root.details.screenshotsPath || "Not available") }
                            XInfoRow { label: "DLC"; value: String(root.details.installedDlcCount || 0) + " / " + String(root.details.dlcCount || 0) + " installed" }

                            Flow {
                                Layout.fillWidth: true
                                spacing: Theme.spaceSm
                                XButton {
                                    text: "Browse game files"
                                    enabled: String(root.details.contentFolder || "").length > 0
                                    onClicked: launcherBridge.openFolder(String(root.details.contentFolder || ""))
                                }
                                XButton {
                                    text: "Open managed root"
                                    enabled: String(root.details.managedPath || "").length > 0
                                    onClicked: launcherBridge.openFolder(String(root.details.managedPath || ""))
                                }
                                XButton {
                                    text: "Open DLC folder"
                                    enabled: String(root.details.dlcRoot || "").length > 0
                                    onClicked: launcherBridge.openFolder(String(root.details.dlcRoot || ""))
                                }
                                XButton {
                                    text: "Open save data"
                                    enabled: String(root.details.savePath || "").length > 0
                                    onClicked: launcherBridge.openFolder(String(root.details.savePath || ""))
                                }
                            }
                        }
                    }

                    XPanel {
                        Layout.fillWidth: true
                        implicitHeight: historyColumn.implicitHeight + Theme.spaceLg * 2
                        ColumnLayout {
                            id: historyColumn
                            anchors.fill: parent
                            anchors.margins: Theme.spaceLg
                            spacing: Theme.spaceSm
                            Text { text: "Library state"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                            XInfoRow { label: "Status"; value: String(root.details.status || "Unknown") }
                            XInfoRow { label: "Added"; value: String(root.details.addedAt || "Not recorded") }
                            XInfoRow { label: "Identified"; value: String(root.details.identifiedAt || "Not recorded") }
                            XInfoRow { label: "Last played"; value: String(root.details.lastPlayedAt || root.details.lastPlayed || "Not recorded") }
                            XInfoRow { label: "Play count"; value: String(root.details.playCount || 0) }
                            XInfoRow { label: "Total play time"; value: root.formatDuration(root.details.totalPlayTimeMs || 0) }
                            XInfoRow { label: "Last session result"; value: String(root.details.lastSessionOutcome || "Not recorded") }
                            XInfoRow { label: "Last session duration"; value: root.formatDuration(root.details.lastSessionDurationMs || 0) }
                            Flow {
                                Layout.fillWidth: true
                                spacing: Theme.spaceSm
                                XButton {
                                    text: "Verify library entry"
                                    variant: "primary"
                                    onClicked: launcherBridge.verifyLibraryEntry(root.gameId)
                                }
                                XButton {
                                    text: "Copy game ID"
                                    onClicked: launcherBridge.copyText(root.gameId)
                                }
                            }
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
}
