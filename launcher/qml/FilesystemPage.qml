import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

XPanel {
    id: root

    title: "Filesystem & Mounts"
    subtitle: "VFS configuration and mounted devices"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing300
        spacing: Theme.spacing300

        // Status Section
        XSectionHeader {
            text: "Filesystem Status"
            Layout.fillWidth: true
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: childrenRect.height + Theme.spacing200 * 2
            color: Theme.surfaceElevated
            radius: Theme.radius100
            border.width: 1
            border.color: Theme.border

            GridLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacing200
                columns: 2
                columnSpacing: Theme.spacing200
                rowSpacing: Theme.spacing100

                Text {
                    text: "VFS Initialized:"
                    color: Theme.textNormal
                    font.pixelSize: Theme.typeBody
                }
                Text {
                    text: root.filesystemStatus().initialized ? "Yes" : "No"
                    color: root.filesystemStatus().initialized ? Theme.success : Theme.error
                    font.pixelSize: Theme.typeBody
                    font.weight: Font.DemiBold
                }

                Text {
                    text: "Mounted Devices:"
                    color: Theme.textNormal
                    font.pixelSize: Theme.typeBody
                }
                Text {
                    text: String(root.filesystemStatus().mountCount || 0)
                    color: Theme.textBright
                    font.pixelSize: Theme.typeBody
                    font.weight: Font.DemiBold
                }

                Text {
                    text: "Symbolic Links:"
                    color: Theme.textNormal
                    font.pixelSize: Theme.typeBody
                }
                Text {
                    text: String(root.filesystemStatus().symbolicLinkCount || 0)
                    color: Theme.textBright
                    font.pixelSize: Theme.typeBody
                    font.weight: Font.DemiBold
                }

                Text {
                    text: "Working Directory:"
                    color: Theme.textNormal
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
            text: "Mounted Devices"
            subtitle: "Active VFS mounts (game:, d:, cache:, etc.)"
            Layout.fillWidth: true
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 200
            color: Theme.surface
            radius: Theme.radius100
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

                    delegate: Rectangle {
                        width: mountsList.width
                        height: 40
                        color: index % 2 === 0 ? Theme.surfaceElevated : Theme.surface

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spacing200
                            anchors.rightMargin: Theme.spacing200
                            spacing: Theme.spacing200

                            Text {
                                text: modelData.mountPoint || ""
                                color: Theme.accentPrimary
                                font.pixelSize: Theme.typeBody
                                font.family: "Consolas"
                                font.weight: Font.DemiBold
                                Layout.preferredWidth: 120
                            }

                            StatusPill {
                                text: modelData.readOnly ? "Read-Only" : "Read-Write"
                                color: modelData.readOnly ? Theme.warning : Theme.success
                            }

                            Item { Layout.fillWidth: true }

                            XButton {
                                text: "Unmount"
                                compact: true
                                secondary: true
                                onClicked: {
                                    var result = launcherBridge.filesystemUnmount(modelData.mountPoint)
                                    if (!result.success) {
                                        launcherBridge.showToast(result.title, result.message, "error")
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Symbolic Links Section
        XSectionHeader {
            text: "Symbolic Links"
            subtitle: "Xbox path aliases (game: -> actual mount)"
            Layout.fillWidth: true
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 150
            color: Theme.surface
            radius: Theme.radius100
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

                    delegate: Rectangle {
                        width: linksList.width
                        height: 36
                        color: index % 2 === 0 ? Theme.surfaceElevated : Theme.surface

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spacing200
                            anchors.rightMargin: Theme.spacing200
                            spacing: Theme.spacing150

                            Text {
                                text: modelData.alias || ""
                                color: Theme.accentSecondary
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
                                color: Theme.textNormal
                                font.pixelSize: Theme.typeBody
                                font.family: "Consolas"
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }

                            XButton {
                                text: "Remove"
                                compact: true
                                secondary: true
                                onClicked: {
                                    var result = launcherBridge.filesystemUnregisterSymbolicLink(modelData.alias)
                                    if (!result.success) {
                                        launcherBridge.showToast(result.title, result.message, "error")
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Quick Actions
        XSectionHeader {
            text: "Quick Actions"
            Layout.fillWidth: true
        }

        RowLayout {
            spacing: Theme.spacing200
            Layout.fillWidth: true

            XButton {
                text: "Mount Host Folder"
                icon: "📁"
                onClicked: mountHostDialog.open()
                Layout.fillWidth: true
            }

            XButton {
                text: "Mount GDFX Image"
                icon: "💿"
                onClicked: mountGdfxDialog.open()
                Layout.fillWidth: true
            }

            XButton {
                text: "Test Path"
                icon: "🔍"
                secondary: true
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
            spacing: Theme.spacing200

            XTextField {
                id: hostMountPoint
                label: "Mount Point (e.g., game:, cache:)"
                placeholderText: "game:"
                Layout.fillWidth: true
            }

            XPathField {
                id: hostPath
                label: "Host Folder Path"
                mode: "directory"
                Layout.fillWidth: true
            }

            Row {
                spacing: Theme.spacing150
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
                secondary: true
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
            spacing: Theme.spacing200

            XTextField {
                id: gdfxMountPoint
                label: "Mount Point"
                placeholderText: "game:"
                text: "game:"
                Layout.fillWidth: true
            }

            XPathField {
                id: gdfxPath
                label: "GDFX Image Path (.iso, .xgd)"
                mode: "file"
                filters: ["Disc Images (*.iso *.xgd)", "All Files (*)"]
                Layout.fillWidth: true
            }
        }

        footer: DialogButtonBox {
            XButton {
                text: "Cancel"
                secondary: true
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
            spacing: Theme.spacing200

            XTextField {
                id: testGuestPath
                label: "Guest Path to Test"
                placeholderText: "game:\\default.xex"
                Layout.fillWidth: true
            }

            Text {
                id: testResult
                text: ""
                color: Theme.textNormal
                font.pixelSize: Theme.typeBody
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        footer: DialogButtonBox {
            XButton {
                text: "Close"
                secondary: true
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
                        testResult.color = Theme.error
                    }
                }
            }
        }
    }
}
