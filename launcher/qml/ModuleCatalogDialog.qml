import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    parent: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(900, parent ? parent.width - Theme.space2Xl * 2 : 900)
    height: Math.min(700, parent ? parent.height - Theme.space2Xl * 2 : 700)
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
                    text: "Browse modules"
                    color: Theme.text
                    font.pixelSize: Theme.typeSubtitle
                    font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    text: "Open-source Xenon modules can be discovered from a public catalog. Game content is never distributed here."
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
            id: catalogScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            Item {
                width: catalogScroll.availableWidth
                implicitHeight: catalogColumn.implicitHeight + Theme.spaceLg * 2

                ColumnLayout {
                    id: catalogColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.spaceLg
                    spacing: Theme.spaceMd

                    XSettingsCard {
                        title: "Official Xenon module catalog"
                        description: "Xenon uses its built-in catalog service to discover compatible public modules. The service will fetch and verify catalog metadata from the built-in Project Xenon catalog source."
                        actionWidth: 220
                        XButton {
                            Layout.fillWidth: true
                            text: "Refresh catalog"
                            onClicked: launcherBridge.requestModuleCatalogRefresh()
                        }
                    }

                    XPanel {
                        Layout.fillWidth: true
                        implicitHeight: catalogPreviewContent.implicitHeight + Theme.spaceLg * 2
                        color: Theme.surface

                        ColumnLayout {
                            id: catalogPreviewContent
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: Theme.spaceLg
                            spacing: Theme.spaceMd
                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text { text: "Project Gracemeria"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold; wrapMode: Text.WordWrap }
                                    Text { text: "Game module • Public repository • Catalog preview"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption; wrapMode: Text.WordWrap }
                                }
                                StatusPill { label: launcherBridge.testMode ? "PREVIEW" : "CATALOG"; tone: Theme.warning }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: "The catalog will point Xenon at signed/versioned module releases and compatibility metadata. Users still provide their own legally obtained game files, title updates and DLC locally."
                                color: Theme.textMuted
                                wrapMode: Text.WordWrap
                                font.pixelSize: Theme.typeBody
                            }
                            Text {
                                Layout.fillWidth: true
                                text: "The current screen is a front-end shell. Catalog discovery, release verification and installation will be connected to a generic module service later."
                                color: Theme.textMuted
                                wrapMode: Text.WordWrap
                                font.pixelSize: Theme.typeCaption
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: Theme.spaceSm
                                XButton {
                                    text: "View repository"
                                    onClicked: launcherBridge.openExternalUrl("https://github.com/nimauria/Project-Gracemeria")
                                }
                                XButton {
                                    text: "Install module"
                                    variant: "primary"
                                    onClicked: launcherBridge.requestModuleInstall("project-gracemeria")
                                }
                            }
                        }
                    }

                    XPanel {
                        Layout.fillWidth: true
                        implicitHeight: safetyText.implicitHeight + Theme.spaceLg * 2
                        color: Theme.accentSoft
                        Text {
                            id: safetyText
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: Theme.spaceLg
                            text: "Catalog packages contain Xenon-compatible open-source module code and metadata only. Commercial game executables, assets, updates and DLC are never distributed by Xenon."
                            color: Theme.textMuted
                            wrapMode: Text.WordWrap
                            font.pixelSize: Theme.typeCaption
                        }
                    }
                }
            }
        }
    }
}
