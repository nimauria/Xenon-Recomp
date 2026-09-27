import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

XSettingsPage {
    id: root

    title: "Filesystem & Mounts"
    description: "VFS configuration and mounted devices"

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spaceMd

        // Status Section
        XSectionHeader {
            title: "Filesystem Status"
            Layout.fillWidth: true
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 148
            color: Theme.surfaceRaised
            radius: Theme.panelRadius
            border.width: 1
            border.color: Theme.border

            GridLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                columns: 2
                columnSpacing: Theme.spaceMd
                rowSpacing: Theme.spaceSm

                Text {
                    text: "VFS Initialized:"
                    color: Theme.text
                    font.pixelSize: Theme.typeBody
                }
                Text {
                    text: root.filesystemStatus().initialized ? "Yes" : "No"
                    color: root.filesystemStatus().initialized ? Theme.success : Theme.danger
                    font.pixelSize: Theme.typeBody
                    font.weight: Font.DemiBold
                }

                Text {
                    text: "Mounted Devices:"
                    color: Theme.text
                    font.pixelSize: Theme.typeBody
                }
                Text {
                    text: String(root.filesystemStatus().mountCount || 0)
                    color: Theme.text
                    font.pixelSize: Theme.typeBody
                    font.weight: Font.DemiBold
                }

                Text {
                    text: "Symbolic Links:"
                    color: Theme.text
                    font.pixelSize: Theme.typeBody
                }
                Text {
                    text: String(root.filesystemStatus().symbolicLinkCount || 0)
                    color: Theme.text
                    font.pixelSize: Theme.typeBody
                    font.weight: Font.DemiBold
                }

                Text {
                    text: "Working Directory:"
                    color: Theme.text
                    font.pixelSize: Theme.typeBody
                }
                Text {
                    text: root.filesystemStatus().workingDirectory || "(not set)"
                    color: Theme.textMuted
                    font.pixelSize: Theme.typeCaption
                    font.family: "Consolas"
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }
            }
        }

        // Mounts Section
XSectionHeader {
    title: "Mounted Devices"
    description: "Active VFS mounts (game:, d:, cache:, etc.)"
    Layout.fillWidth: true
    visible: root.getMounts().length > 0
}

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 200
            visible: root.getMounts().length > 0
            color: Theme.surface
            radius: Theme.panelRadius
            border.width: 1
            border.color: Theme.border

            ScrollView {
                anchors.fill: parent
                anchors.margins: 1
                clip: true

                ListView {
                    id: mountsList
                    model: root.getMounts()
                    spacing: 1
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        width: mountsList.width
                        height: 40
                        color: index % 2 === 0 ? Theme.surfaceRaised : Theme.surface

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spaceMd
                            anchors.rightMargin: Theme.spaceMd
                            spacing: Theme.spaceMd

                            Text {
                                text: modelData.mountPoint || ""
                                color: Theme.accent
                                font.pixelSize: Theme.typeBody
                                font.family: "Consolas"
                                font.weight: Font.DemiBold
                                Layout.preferredWidth: 120
                            }

                            StatusPill {
                                label: modelData.readOnly ? "Read-Only" : "Read-Write"
                                tone: modelData.readOnly ? Theme.warning : Theme.success
                            }

                            Item { Layout.fillWidth: true }

                            XButton {
                                text: "Unmount"
                                variant: "ghost"
                                onClicked: {
                                    var result = launcherBridge.filesystemUnmount(modelData.mountPoint)
                                    if (!result.success) {
                                        launcherBridge.showToast(result.title, result.message, "error")
                                    }
                                }

                                Text {
                                    anchors.centerIn: parent
                                    visible: mountsList.count === 0
                                    text: "No VFS mounts are active."
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.typeBody
                                }
                            }
                        }
                    }
                }
            }
        }

        // Symbolic Links Section
        XSectionHeader {
            title: "Symbolic Links"
            description: "Xbox path aliases (game: -> actual mount)"
            Layout.fillWidth: true
            visible: root.getSymbolicLinks().length > 0
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 150
            visible: root.getSymbolicLinks().length > 0
            color: Theme.surface
            radius: Theme.panelRadius
            border.width: 1
            border.color: Theme.border

            ScrollView {
                anchors.fill: parent
                anchors.margins: 1
                clip: true

                ListView {
                    id: linksList
                    model: root.getSymbolicLinks()
                    spacing: 1
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        width: linksList.width
                        height: 36
                        color: index % 2 === 0 ? Theme.surfaceRaised : Theme.surface

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spaceMd
                            anchors.rightMargin: Theme.spaceMd
                            spacing: Theme.spaceSm

                            Text {
                                text: modelData.alias || ""
                                color: Theme.accent
                                font.pixelSize: Theme.typeBody
                                font.family: "Consolas"
                                font.weight: Font.DemiBold
                                Layout.preferredWidth: 80
                            }

                            Text {
                                text: "→"
                                color: Theme.textMuted
                                font.pixelSize: Theme.typeBody
                            }

                            Text {
                                text: modelData.target || ""
                                color: Theme.text
                                font.pixelSize: Theme.typeBody
                                font.family: "Consolas"
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }

                            XButton {
                                text: "Remove"
                                variant: "ghost"
                                onClicked: {
                                    var result = launcherBridge.filesystemUnregisterSymbolicLink(modelData.alias)
                                    if (!result.success) {
                                        launcherBridge.showToast(result.title, result.message, "error")
                                    }
                                }

                                Text {
                                    anchors.centerIn: parent
                                    visible: linksList.count === 0
                                    text: "No symbolic links are registered."
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.typeBody
                                }
                            }
                        }
                    }
                }
            }
        }

        // Quick Actions
        XSectionHeader {
            title: "Quick Actions"
            Layout.fillWidth: true
        }

        RowLayout {
            spacing: Theme.spaceMd
            Layout.fillWidth: true

            XButton {
                text: "Mount Host Folder"
                onClicked: mountHostDialog.open()
                Layout.fillWidth: true
            }

            XButton {
                text: "Mount GDFX Image"
                onClicked: mountGdfxDialog.open()
                Layout.fillWidth: true
            }

            XButton {
                text: "Test Path"
                variant: "ghost"
                onClicked: testPathDialog.open()
                Layout.fillWidth: true
            }
        }

        Item { Layout.fillHeight: true }

        // Help Text
        Text {
            text: "The VFS is configured automatically at game launch. Manual mounts are for testing and development."
            color: Theme.textMuted
            font.pixelSize: Theme.typeCaption
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }

    // Helper functions
    function filesystemStatus() {
        if (typeof launcherBridge.getFilesystemStatus === "function") {
            return launcherBridge.getFilesystemStatus()
        }
        return { initialized: false, mountCount: 0, symbolicLinkCount: 0, workingDirectory: "" }
    }

    function getMounts() {
        if (typeof launcherBridge.getFilesystemMounts === "function") {
            return launcherBridge.getFilesystemMounts()
        }
        return []
    }

    function getSymbolicLinks() {
        if (typeof launcherBridge.getFilesystemSymbolicLinks === "function") {
            return launcherBridge.getFilesystemSymbolicLinks()
        }
        return []
    }

    // Dialogs
    Dialog {
        id: mountHostDialog
        title: "Mount Host Folder"
        modal: true
        anchors.centerIn: parent
        width: 500

        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.spaceMd

            XTextField {
                id: hostMountPoint
                placeholderText: "Mount Point (e.g., game:, cache:)"
                Layout.fillWidth: true
            }

            XPathField {
                id: hostPath
                placeholderText: "Host Folder Path"
                mode: "directory"
                Layout.fillWidth: true
            }

            Row {
                spacing: Theme.spaceSm
                XSwitch {
                    id: hostReadOnly
                    text: "Read-Only"
                    checked: false
                }
            }
        }

        footer: DialogButtonBox {
            XButton {
                text: "Cancel"
                variant: "ghost"
                onClicked: mountHostDialog.close()
            }
            XButton {
                text: "Mount"
                onClicked: {
                    var result = launcherBridge.filesystemMountHostPath(
                        hostMountPoint.text,
                        hostPath.text,
                        hostReadOnly.checked
                    )
                    if (result.success) {
                        launcherBridge.showToast("Mounted", result.message, "success")
                        mountHostDialog.close()
                    } else {
                        launcherBridge.showToast(result.title, result.message, "error")
                    }
                }
            }
        }
    }

    Dialog {
        id: mountGdfxDialog
        title: "Mount GDFX Disc Image"
        modal: true
        anchors.centerIn: parent
        width: 500

        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.spaceMd

            XTextField {
                id: gdfxMountPoint
                placeholderText: "Mount Point (game:)"
                text: "game:"
                Layout.fillWidth: true
            }

            XPathField {
                id: gdfxPath
                placeholderText: "GDFX Image Path (.iso, .xgd)"
                mode: "file"
                filters: ["Disc Images (*.iso *.xgd)", "All Files (*)"]
                Layout.fillWidth: true
            }
        }

        footer: DialogButtonBox {
            XButton {
                text: "Cancel"
                variant: "ghost"
                onClicked: mountGdfxDialog.close()
            }
            XButton {
                text: "Mount"
                onClicked: {
                    var result = launcherBridge.filesystemMountGdfxImage(
                        gdfxMountPoint.text,
                        gdfxPath.text
                    )
                    if (result.success) {
                        launcherBridge.showToast("Mounted", result.message, "success")
                        mountGdfxDialog.close()
                    } else {
                        launcherBridge.showToast(result.title, result.message, "error")
                    }
                }
            }
        }
    }

    Dialog {
        id: testPathDialog
        title: "Test VFS Path"
        modal: true
        anchors.centerIn: parent
        width: 500

        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.spaceMd

            XTextField {
                id: testGuestPath
                placeholderText: "Guest Path to Test (e.g., game:\\default.xex)"
                Layout.fillWidth: true
            }

            Text {
                id: testResult
                text: ""
                color: Theme.text
                font.pixelSize: Theme.typeBody
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        footer: DialogButtonBox {
            XButton {
                text: "Close"
                variant: "ghost"
                onClicked: testPathDialog.close()
            }
            XButton {
                text: "Test"
                onClicked: {
                    var result = launcherBridge.filesystemTestPath(testGuestPath.text)
                    if (result.success) {
                        var data = result.data || {}
                        testResult.text = "✓ Path exists\nType: " + (data.isDirectory ? "Directory" : "File") + 
                                         "\nSize: " + data.size + " bytes"
                        testResult.color = Theme.success
                    } else {
                        testResult.text = "✗ " + result.message
                        testResult.color = Theme.danger
                    }
                }
            }
        }
    }
}
