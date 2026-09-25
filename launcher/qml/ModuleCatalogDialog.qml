import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    property var catalogEntries: []
    property var catalogState: ({})
    property string searchText: ""

    function reloadCatalog() {
        catalogEntries = launcherBridge.moduleCatalogEntries()
        catalogState = launcherBridge.moduleCatalogState()
    }

    function matches(entry) {
        var needle = searchText.trim().toLowerCase()
        if (needle.length === 0)
            return true
        return String(entry.moduleName || "").toLowerCase().indexOf(needle) >= 0
            || String(entry.moduleId || "").toLowerCase().indexOf(needle) >= 0
            || String(entry.publisher || "").toLowerCase().indexOf(needle) >= 0
            || String(entry.repository || "").toLowerCase().indexOf(needle) >= 0
    }

    function formatBytes(value) {
        var bytes = Number(value || 0)
        if (bytes <= 0) return "0 B"
        if (bytes < 1024) return Math.round(bytes) + " B"
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KiB"
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MiB"
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GiB"
    }

    function primaryLabel(entry) {
        var status = String(entry.updateStatus || "")
        if (!Boolean(entry.installed)) {
            if (status === "checking")
                return "Checking Release…"
            if (status === "downloading")
                return "Downloading Module…"
            if (status === "installing")
                return "Installing Module…"
            return "Install Module"
        }
        if (entry.canInstall)
            return "Install Update"
        if (entry.canDownload)
            return "Download Update"
        if (status === "checking")
            return "Checking…"
        if (status === "downloading")
            return "Downloading…"
        if (status === "installing")
            return "Installing…"
        if (status === "rolling-back")
            return "Rolling Back…"
        if (status === "up-to-date")
            return "Check Again"
        return "Check for Updates"
    }

    function runPrimary(entry) {
        var id = String(entry.moduleId || "")
        if (!Boolean(entry.installed)) {
            launcherBridge.requestModuleInstall(id)
            return
        }
        if (entry.canInstall)
            launcherBridge.requestModuleUpdateInstall(id)
        else if (entry.canDownload)
            launcherBridge.requestModuleUpdateDownload(id)
        else
            launcherBridge.requestModuleUpdateCheck(id)
    }

    parent: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(980, parent ? parent.width - Theme.space2Xl * 2 : 980)
    height: Math.min(760, parent ? parent.height - Theme.space2Xl * 2 : 760)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Overlay.modal: Rectangle { color: Theme.overlay }

    onOpened: reloadCatalog()
    Component.onCompleted: reloadCatalog()

    Connections {
        target: launcherBridge
        function onModuleCatalogChanged() { root.reloadCatalog() }
        function onModuleUpdateStateChanged(moduleId) { root.reloadCatalog() }
        function onModulesChanged() { root.reloadCatalog() }
    }

    background: Rectangle {
        color: Theme.surfaceRaised
        radius: Theme.dialogRadius
        border.width: Theme.borderWidth
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(92, Theme.typeSubtitle + Theme.typeCaption * 2 + Theme.spaceLg * 2)

            ColumnLayout {
                anchors.left: parent.left
                anchors.right: dialogCloseButton.left
                anchors.leftMargin: Theme.spaceXl
                anchors.rightMargin: Theme.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Text {
                    Layout.fillWidth: true
                    text: "Official Xenon Modules registry"
                    color: Theme.text
                    font.pixelSize: Theme.typeSubtitle
                    font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    text: "Discovery metadata comes from Xenon-Modules on GitHub. Packages remain in each module’s own GitHub Releases."
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

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.topMargin: Theme.spaceMd
            Layout.bottomMargin: Theme.spaceMd
            spacing: Theme.spaceSm

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                XTextField {
                    Layout.fillWidth: true
                    placeholderText: "Search modules, publisher or repository…"
                    text: root.searchText
                    onTextChanged: root.searchText = text
                }
                XButton {
                    text: String(root.catalogState.status || "").indexOf("loading") === 0 ? "Refreshing…" : "Refresh Registry"
                    enabled: String(root.catalogState.status || "").indexOf("loading") !== 0
                    onClicked: launcherBridge.requestModuleCatalogRefresh()
                }
                XButton {
                    text: "Open Registry"
                    enabled: String(root.catalogState.registryUrl || "").length > 0
                    onClicked: launcherBridge.openExternalUrl(String(root.catalogState.registryUrl || ""))
                }
                XButton {
                    text: "Check Installed Updates"
                    onClicked: launcherBridge.requestAllModuleUpdateChecks()
                }
            }

            XPanel {
                Layout.fillWidth: true
                implicitHeight: catalogStatusRow.implicitHeight + Theme.spaceMd * 2
                color: root.catalogState.status === "fallback" || root.catalogState.status === "partial"
                       ? Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.08)
                       : Theme.surface
                RowLayout {
                    id: catalogStatusRow
                    anchors.fill: parent
                    anchors.margins: Theme.spaceMd
                    spacing: Theme.spaceSm
                    StatusPill {
                        label: String(root.catalogState.status || "unknown").toUpperCase()
                        tone: root.catalogState.status === "ready" ? Theme.success
                            : String(root.catalogState.status || "").indexOf("loading") === 0 ? Theme.accent : Theme.warning
                    }
                    Text {
                        Layout.fillWidth: true
                        text: String(root.catalogState.message || "Catalog state unavailable.")
                        color: Theme.textMuted
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.typeCaption
                    }
                    Text {
                        text: String(root.catalogState.hostKey || "")
                        color: Theme.textMuted
                        font.pixelSize: Theme.typeCaption
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        ScrollView {
            id: catalogScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff

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

                    EmptyState {
                        visible: root.catalogEntries.length === 0
                        Layout.fillWidth: true
                        title: "No catalog entries"
                        description: "The Xenon Modules registry did not return any module metadata for this refresh."
                        primaryText: "Refresh Registry"
                        onPrimaryClicked: launcherBridge.requestModuleCatalogRefresh()
                    }

                    Repeater {
                        model: root.catalogEntries
                        delegate: XPanel {
                            required property var modelData
                            visible: root.matches(modelData)
                            Layout.fillWidth: true
                            Layout.preferredHeight: visible ? catalogEntryContent.implicitHeight + Theme.spaceLg * 2 : 0
                            implicitHeight: Layout.preferredHeight
                            color: Theme.surface

                            ColumnLayout {
                                id: catalogEntryContent
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: Theme.spaceLg
                                spacing: Theme.spaceMd

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.spaceSm
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Text {
                                            text: String(modelData.moduleName || modelData.moduleId || "Module")
                                            color: Theme.text
                                            font.pixelSize: Theme.typeBodyLarge
                                            font.weight: Font.DemiBold
                                            wrapMode: Text.WordWrap
                                        }
                                        Text {
                                            text: String(modelData.moduleType || "Module")
                                                + " • " + String(modelData.publisher || "Unknown publisher")
                                                + (Boolean(modelData.verified) ? " • Verified publisher" : "")
                                            color: Theme.textMuted
                                            font.pixelSize: Theme.typeCaption
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                    StatusPill {
                                        visible: Boolean(modelData.verified)
                                        label: "VERIFIED"
                                        tone: Theme.accent
                                    }
                                    StatusPill {
                                        visible: Boolean(modelData.installed)
                                        label: Boolean(modelData.active) ? "INSTALLED" : "DISABLED"
                                        tone: Boolean(modelData.active) ? Theme.success : Theme.warning
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: String(modelData.description || "No description supplied by the catalog.")
                                    color: Theme.textMuted
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: Theme.typeBody
                                }

                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: width > 650 ? 2 : 1
                                    columnSpacing: Theme.spaceLg
                                    rowSpacing: Theme.spaceXs
                                    XInfoRow { label: "Module ID"; value: String(modelData.moduleId || "") }
                                    XInfoRow { label: "Repository"; value: String(modelData.repository || "") }
                                    XInfoRow { label: "Registry entry"; value: String(modelData.entryPath || "") }
                                    XInfoRow { label: "Installed"; value: Boolean(modelData.installed) ? String(modelData.installedVersion || "Unknown") : "Not installed" }
                                    XInfoRow { label: "Latest release"; value: String(modelData.availableVersion || "Not checked") }
                                    XInfoRow { label: "Host package"; value: String(modelData.assetName || "Not defined") }
                                    XInfoRow { label: "Update state"; value: String(modelData.updateStatus || "idle") }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: String(modelData.updateMessage || "Check the module's GitHub Releases to discover the latest compatible package.")
                                    color: Boolean(modelData.updateAvailable) ? Theme.warning : Theme.textMuted
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: Theme.typeCaption
                                }

                                ProgressBar {
                                    visible: String(modelData.updateStatus || "") === "downloading"
                                    Layout.fillWidth: true
                                    from: 0; to: 1
                                    value: Number(modelData.downloadProgress || 0)
                                }
                                Text {
                                    visible: String(modelData.updateStatus || "") === "downloading"
                                    Layout.fillWidth: true
                                    text: root.formatBytes(modelData.downloadedBytes)
                                        + (Number(modelData.downloadTotalBytes || 0) > 0
                                           ? " / " + root.formatBytes(modelData.downloadTotalBytes)
                                             + " • " + Math.round(Number(modelData.downloadProgress || 0) * 100) + "%"
                                           : "")
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.typeCaption
                                }

                                Flow {
                                    Layout.fillWidth: true
                                    spacing: Theme.spaceSm
                                    XButton {
                                        visible: String(modelData.repositoryUrl || "").length > 0
                                        text: "View Repository"
                                        onClicked: launcherBridge.openExternalUrl(String(modelData.repositoryUrl || ""))
                                    }
                                    XButton {
                                        visible: String(modelData.registryEntryUrl || "").length > 0
                                        text: "Registry Entry"
                                        onClicked: launcherBridge.openExternalUrl(String(modelData.registryEntryUrl || ""))
                                    }
                                    XButton {
                                        text: root.primaryLabel(modelData)
                                        variant: !Boolean(modelData.installed)
                                            || Boolean(modelData.canInstall)
                                            || Boolean(modelData.canDownload) ? "primary" : "default"
                                        accessibleDescription: !Boolean(modelData.installed)
                                            ? "Checks GitHub Releases, downloads and verifies the module package, then installs it."
                                            : "Checks for or installs a module update."
                                        enabled: Boolean(modelData.packageSupported)
                                            && String(modelData.updateStatus || "") !== "checking"
                                            && String(modelData.updateStatus || "") !== "downloading"
                                            && String(modelData.updateStatus || "") !== "installing"
                                            && String(modelData.updateStatus || "") !== "rolling-back"
                                        onClicked: root.runPrimary(modelData)
                                    }
                                    XButton {
                                        visible: String(modelData.updateStatus || "") === "downloading"
                                        text: "Cancel Download"
                                        variant: "danger"
                                        onClicked: launcherBridge.cancelModuleUpdateDownload(String(modelData.moduleId || ""))
                                    }
                                    XButton {
                                        visible: String(modelData.releaseUrl || "").length > 0
                                        text: "Release Notes"
                                        onClicked: launcherBridge.openExternalUrl(String(modelData.releaseUrl || ""))
                                    }
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
                            text: "The Xenon Modules registry contains discovery metadata only. Module packages are downloaded from each module’s own GitHub Releases, SHA-256 verified, checked for the expected module ID and release version, staged, and installed with retained rollback metadata. Commercial game executables, title updates and DLC are never distributed by the registry."
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
