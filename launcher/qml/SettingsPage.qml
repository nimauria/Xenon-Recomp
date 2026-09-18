import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string searchText: ""
    property int categoryIndex: 0
    property int settingsRevision: 0
    property var launcherUpdateState: launcherBridge.launcherUpdateState()

    readonly property var categories: launcherBridge.settingsCategories()
    readonly property var visibleCategories: categories.filter(function(category) {
        if (!launcherBridge.featureEnabled(category.feature))
            return false
        var needle = root.searchText.trim().toLowerCase()
        return needle.length === 0
            || String(category.name).toLowerCase().indexOf(needle) !== -1
            || String(category.keywords).toLowerCase().indexOf(needle) !== -1
    })

    readonly property var themeEntries: launcherBridge.themeCatalog()
    readonly property var accentEntries: launcherBridge.accentCatalog()
    readonly property var cornerEntries: launcherBridge.cornerStyleCatalog()
    readonly property var backgroundEntries: launcherBridge.themeBackgroundVariants(Theme.effectiveThemeId)

    function labels(entries) {
        var result = []
        for (var i = 0; i < entries.length; ++i)
            result.push(String(entries[i].label !== undefined ? entries[i].label : entries[i].name))
        return result
    }

    function ids(entries) {
        var result = []
        for (var i = 0; i < entries.length; ++i)
            result.push(entries[i].id)
        return result
    }

    function optionEntries(key) {
        var r = settingsRevision
        return launcherBridge.settingOptions(key)
    }

    function optionLabels(key) { return root.labels(root.optionEntries(key)) }
    function optionValues(key) {
        var entries = root.optionEntries(key)
        var result = []
        for (var i = 0; i < entries.length; ++i)
            result.push(entries[i].value)
        return result
    }

    function valuesEqual(left, right) {
        if (typeof left === "number" || typeof right === "number")
            return Math.abs(Number(left) - Number(right)) < 0.000001
        return String(left) === String(right)
    }

    function indexFor(list, value) {
        for (var i = 0; i < list.length; ++i)
            if (root.valuesEqual(list[i], value)) return i
        return 0
    }

    function optionIndex(key) {
        return root.indexFor(root.optionValues(key), launcherBridge.settingValue(key, launcherBridge.settingDefaultValue(key)))
    }

    function saveOption(key, index) {
        var values = root.optionValues(key)
        if (index >= 0 && index < values.length)
            root.save(key, values[index])
    }

    function categoryVisibleIndex(page) {
        for (var i = 0; i < visibleCategories.length; ++i)
            if (visibleCategories[i].page === page) return i
        return 0
    }

    function getBool(key, fallback) { var r = settingsRevision; return launcherBridge.boolSetting(key, fallback) }
    function getString(key, fallback) { var r = settingsRevision; return launcherBridge.stringSetting(key, fallback) }
    function getNumber(key, fallback) { var r = settingsRevision; return launcherBridge.numberSetting(key, fallback) }
    function save(key, value) { launcherBridge.setSettingValue(key, value) }


    function updateStatusLabel() {
        var status = String(root.launcherUpdateState.status || "idle")
        if (status === "checking") return "Checking"
        if (status === "update-available") return "Update available"
        if (status === "downloading") return "Downloading"
        if (status === "ready-to-install") return "Ready to install"
        if (status === "installing") return "Installing"
        if (status === "up-to-date") return "Up to date"
        if (status === "asset-unavailable") return "Package unavailable"
        if (status === "no-releases") return "No releases"
        if (status === "error") return "Update error"
        return "Ready"
    }

    function updateStatusTone() {
        var status = String(root.launcherUpdateState.status || "idle")
        if (status === "up-to-date" || status === "ready-to-install") return Theme.success
        if (status === "update-available" || status === "checking" || status === "downloading" || status === "installing") return Theme.warning
        if (status === "error" || status === "asset-unavailable") return Theme.danger
        return Theme.textMuted
    }

    function formatBytes(value) {
        var bytes = Number(value || 0)
        if (bytes < 1024) return Math.round(bytes) + " B"
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KiB"
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MiB"
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GiB"
    }

    function releaseNotesPreview() {
        var notes = String(root.launcherUpdateState.releaseNotes || "").trim()
        if (notes.length === 0) return "No release notes were provided for this build."
        notes = notes.replace(/\r/g, "").replace(/\n+/g, " ")
        return notes.length > 420 ? notes.slice(0, 417) + "…" : notes
    }

    function runtimeServiceLabel(service) {
        if (launcherBridge.runtimeCapability(service)) return "Connected"
        if (launcherBridge.runtimeCapability(service + "Compiled")) return "Compiled • service pending"
        return "Backend pending"
    }

    function runtimeServiceTone(service) {
        return launcherBridge.runtimeCapability(service) ? Theme.success : Theme.warning
    }

    onSearchTextChanged: Qt.callLater(function() {
        if (root.searchText.trim().length === 0 || root.visibleCategories.length === 0)
            return
        var currentVisible = false
        for (var i = 0; i < root.visibleCategories.length; ++i) {
            if (root.visibleCategories[i].page === root.categoryIndex) {
                currentVisible = true
                break
            }
        }
        if (!currentVisible)
            root.categoryIndex = root.visibleCategories[0].page
    })

    Connections {
        target: launcherBridge
        function onSettingChanged(key, value) { root.settingsRevision += 1 }
        function onThemeIdChanged() { root.settingsRevision += 1 }
        function onAccentIdChanged() { root.settingsRevision += 1 }
        function onCustomAccentColorChanged() { root.settingsRevision += 1 }
        function onCornerStyleChanged() { root.settingsRevision += 1 }
        function onUpdateStateChanged() { root.launcherUpdateState = launcherBridge.launcherUpdateState() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spaceMd

        XSectionHeader {
            title: "Settings"
            description: root.searchText.trim().length > 0
                ? "Showing settings categories related to “" + root.searchText.trim() + "”."
                : "Launcher preferences are validated and persisted by Launcher Core. Runtime-owned controls activate as their Xenon services become available."
        }

        XPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.textScale >= 1.5
                ? Math.max(64, Theme.controlHeight + Theme.spaceLg * 2)
                : Math.max(56, Theme.controlHeight + Theme.spaceLg)
            color: Theme.highContrast ? Theme.surface : Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, Theme.panelOpacity)

            StackLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceSm
                currentIndex: Theme.textScale >= 1.5 ? 1 : 0

                Flickable {
                    id: categoryStrip
                    contentWidth: categoryRow.implicitWidth
                    contentHeight: height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    RowLayout {
                        id: categoryRow
                        height: parent.height
                        spacing: Theme.spaceXs

                        Repeater {
                            model: root.visibleCategories
                            delegate: XButton {
                                required property var modelData
                                text: modelData.name
                                variant: root.categoryIndex === modelData.page ? "primary" : "ghost"
                                onClicked: root.categoryIndex = modelData.page
                            }
                        }

                        Text {
                            visible: root.visibleCategories.length === 0
                            text: "No settings categories match this search."
                            color: Theme.textMuted
                            font.pixelSize: Theme.typeCaption
                            Layout.alignment: Qt.AlignVCenter
                        }
                    }
                }

                XComboBox {
                    Layout.fillWidth: true
                    model: root.visibleCategories.map(function(category) { return category.name })
                    currentIndex: root.categoryVisibleIndex(root.categoryIndex)
                    onActivated: function(index) {
                        if (index >= 0 && index < root.visibleCategories.length)
                            root.categoryIndex = root.visibleCategories[index].page
                    }
                }
            }
        }

        XPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.highContrast ? Theme.surface : Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, Theme.panelOpacity)

            StackLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceLg
                visible: root.visibleCategories.length > 0
                currentIndex: root.categoryIndex

                XSettingsPage {
                    title: "General"
                    description: "Core launcher behaviour and navigation preferences."

                    XSettingsCard {
                        title: "Launcher language"
                        description: "Language used throughout the Xenon interface. Additional translations can be added without changing Launcher Core."
                        XComboBox { Layout.fillWidth: true; model: ["English (UK)"]; enabled: false }
                    }

                    XSettingsCard {
                        title: "Startup page"
                        description: "Page shown when Xenon opens unless Open last page on startup is enabled."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("general/startupPage")
                            currentIndex: root.optionIndex("general/startupPage")
                            onActivated: function(index) { root.saveOption("general/startupPage", index) }
                        }
                    }

                    XSettingsCard {
                        title: "Sidebar layout"
                        description: "Auto adapts to the window. Compact keeps icon-only primary navigation."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("general/sidebarMode")
                            currentIndex: root.optionIndex("general/sidebarMode")
                            onActivated: function(index) { root.saveOption("general/sidebarMode", index) }
                        }
                    }

                    XSettingsCard {
                        title: "Open last page on startup"
                        description: "Resume the page that was active when Xenon last closed."
                        XSwitch { checked: root.getBool("general/restoreLastPage", false); onUserToggled: function(value) { root.save("general/restoreLastPage", value) } }
                    }

                    XSettingsCard {
                        title: "Interface animations"
                        description: "Use subtle transitions and hover motion. Reduce motion overrides non-essential animation."
                        XSwitch { checked: root.getBool("general/animations", true); onUserToggled: function(value) { root.save("general/animations", value) } }
                    }

                    XSettingsCard {
                        title: "Reset general settings"
                        description: "Restore registered General preferences without changing profiles, paths or theme choices."
                        XButton { Layout.fillWidth: true; text: "Reset General"; onClicked: launcherBridge.resetSettingsCategory("general") }
                    }
                }

                XSettingsPage {
                    title: "Appearance"
                    description: "Theme definitions, accents and background assets are supplied by Launcher Core rather than hard-coded by this page."

                    XSettingsCard {
                        title: "Theme"
                        description: "System follows the operating-system light/dark preference."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.labels(root.themeEntries)
                            currentIndex: root.indexFor(root.ids(root.themeEntries), launcherBridge.themeId)
                            onActivated: function(index) { launcherBridge.themeId = root.ids(root.themeEntries)[index] }
                        }
                    }

                    XSettingsCard {
                        title: "Accent colour"
                        description: "Brand marks, focus states and primary actions recolour from one semantic accent definition."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.labels(root.accentEntries)
                            currentIndex: root.indexFor(root.ids(root.accentEntries), launcherBridge.accentId)
                            onActivated: function(index) { launcherBridge.accentId = root.ids(root.accentEntries)[index] }
                        }
                    }

                    XSettingsCard {
                        visible: launcherBridge.accentId === "custom"
                        title: "Custom accent"
                        description: "Use a CSS-style colour such as #35D7EA. Launcher Core derives strong, soft and readable text variants automatically."
                        actionWidth: 300
                        Rectangle {
                            Layout.preferredWidth: 34
                            Layout.preferredHeight: 34
                            radius: Theme.controlRadius
                            color: Theme.accent
                            border.width: Theme.borderWidth
                            border.color: Theme.border
                        }
                        XTextField {
                            Layout.fillWidth: true
                            text: launcherBridge.customAccentColor
                            placeholderText: "#35D7EA"
                            onEditingFinished: {
                                launcherBridge.customAccentColor = text
                                text = launcherBridge.customAccentColor
                            }
                        }
                    }

                    XSettingsCard {
                        title: "Interface density"
                        description: "Compact reduces page margins and collapses primary navigation."
                        XComboBox {
                            Layout.fillWidth: true
                            model: ["Comfortable", "Compact"]
                            currentIndex: root.getBool("general/compact", false) ? 1 : 0
                            onActivated: function(index) { root.save("general/compact", index === 1) }
                        }
                    }

                    XSettingsCard {
                        title: "Control corners"
                        description: "Changes panel, field and button corner styling."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.labels(root.cornerEntries)
                            currentIndex: root.indexFor(root.ids(root.cornerEntries), launcherBridge.cornerStyle)
                            onActivated: function(index) { launcherBridge.cornerStyle = root.ids(root.cornerEntries)[index] }
                        }
                    }

                    XSettingsCard {
                        title: "Decorative detail"
                        description: "Controls Xenon HUD rails, corner marks and technical ornaments independently of the background artwork."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("appearance/decorLevel")
                            currentIndex: root.optionIndex("appearance/decorLevel")
                            onActivated: function(index) { root.saveOption("appearance/decorLevel", index) }
                        }
                    }

                    XSettingsCard {
                        title: "Panel opacity"
                        description: "Set launcher panel opacity directly. 0% is fully transparent and 100% is fully opaque. High contrast always forces 100%."
                        actionWidth: 320

                        RowLayout {
                            Layout.fillWidth: true
                            layoutDirection: Qt.LeftToRight
                            spacing: Theme.spaceSm

                            XSlider {
                                id: panelOpacitySlider
                                Layout.fillWidth: true
                                from: 0
                                to: 100
                                stepSize: 1
                                value: Math.round(root.getNumber("appearance/panelOpacity", 0.94) * 100)
                                accessibleName: "Panel opacity"
                                accessibleDescription: "Adjust panel opacity from zero to one hundred percent"
                                onMoved: root.save("appearance/panelOpacity", value / 100.0)
                            }

                            Text {
                                Layout.preferredWidth: 48
                                horizontalAlignment: Text.AlignRight
                                text: Math.round(panelOpacitySlider.value) + "%"
                                color: Theme.text
                                font.pixelSize: Theme.typeBody
                                font.weight: Font.DemiBold
                            }
                        }
                    }

                    XSettingsCard {
                        title: "Themed background graphics"
                        description: "Show Xenon-owned decorative background graphics behind launcher content."
                        XSwitch { checked: root.getBool("appearance/themeBackdrop", true); onUserToggled: function(value) { root.save("appearance/themeBackdrop", value) } }
                    }

                    XSettingsCard {
                        title: "Theme background"
                        description: Theme.effectiveThemeId === "light"
                            ? "The Light theme uses the minimal vector treatment."
                            : "Choose artwork registered for the active base theme. Game pages still use module-provided artwork separately."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.labels(root.backgroundEntries)
                            enabled: root.backgroundEntries.length > 1
                            currentIndex: root.indexFor(root.ids(root.backgroundEntries), launcherBridge.themeBackgroundVariant(Theme.effectiveThemeId))
                            onActivated: function(index) {
                                root.save("appearance/backdropVariant/" + Theme.effectiveThemeId, root.ids(root.backgroundEntries)[index])
                            }
                        }
                    }

                    XSettingsCard {
                        title: "Background strength"
                        description: "Adjust the visibility of Xenon-owned theme graphics without affecting module artwork."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("appearance/backdropIntensity")
                            currentIndex: root.optionIndex("appearance/backdropIntensity")
                            onActivated: function(index) { root.saveOption("appearance/backdropIntensity", index) }
                        }
                    }

                    XSettingsCard {
                        title: "Game artwork backgrounds"
                        description: "Allow installed game modules to provide artwork for their own pages."
                        XSwitch { checked: root.getBool("appearance/artworkBackgrounds", true); onUserToggled: function(value) { root.save("appearance/artworkBackgrounds", value) } }
                    }

                    XSettingsCard {
                        title: "Reset appearance"
                        description: "Restore the base theme, accent, custom colour, corner style and all background/decor settings."
                        XButton { Layout.fillWidth: true; text: "Reset Appearance"; onClicked: launcherBridge.resetSettingsCategory("appearance") }
                    }
                }

                XSettingsPage {
                    title: "Library"
                    description: "Control how installed and missing game content is presented."
                    XSettingsCard {
                        title: "Compatibility panel"
                        description: "Show runtime and module compatibility status on game pages."
                        XSwitch { checked: root.getBool("library/showCompatibility", true); onUserToggled: function(value) { root.save("library/showCompatibility", value) } }
                    }
                    XSettingsCard {
                        title: "Missing add-ons"
                        description: "Choose whether module-catalogued DLC that is not locally installed remains visible."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("library/missingContent")
                            currentIndex: root.optionIndex("library/missingContent")
                            onActivated: function(index) { root.saveOption("library/missingContent", index) }
                        }
                    }
                    XSettingsCard {
                        title: "Reset library presentation"
                        description: "Restore Library display preferences without removing games, modules or DLC."
                        XButton { Layout.fillWidth: true; text: "Reset Library"; onClicked: launcherBridge.resetSettingsCategory("library") }
                    }
                }

                XSettingsPage {
                    title: "Paths"
                    description: "Launcher-wide defaults. Individual profiles may optionally override Games, Saves and Screenshots."
                    XPathField { label: "Game library"; helperText: "Default location for user-provided game content."; pathValue: root.getString("paths/games", launcherBridge.defaultGameLibraryPath); allowClear: true; onPathEdited: function(path) { root.save("paths/games", path) } }
                    XPathField { label: "Save data"; helperText: "Default location for profile save data."; pathValue: root.getString("paths/saves", launcherBridge.defaultSaveDataPath); allowClear: true; onPathEdited: function(path) { root.save("paths/saves", path) } }
                    XPathField { label: "Profiles"; helperText: "Profiles, local profile images and profiles.json are stored here."; pathValue: root.getString("paths/profiles", launcherBridge.defaultProfilesPath); allowClear: true; onPathEdited: function(path) { root.save("paths/profiles", path) } }
                    XPathField { label: "Modules"; helperText: "Installed Xenon modules and support packages."; pathValue: root.getString("paths/modules", launcherBridge.defaultModulesPath); allowClear: true; onPathEdited: function(path) { root.save("paths/modules", path) } }
                    XPathField { label: "Screenshots"; helperText: "Default screenshot location."; pathValue: root.getString("paths/screenshots", launcherBridge.defaultScreenshotsPath); allowClear: true; onPathEdited: function(path) { root.save("paths/screenshots", path) } }
                    XPathField { label: "Cache"; helperText: "Launcher/runtime cache files."; pathValue: root.getString("paths/cache", launcherBridge.cachePath); allowClear: true; onPathEdited: function(path) { root.save("paths/cache", path) } }
                    XSettingsCard {
                        title: "Reset all paths"
                        description: "Return every launcher-wide storage location to its platform default. Profiles and modules are reloaded through Launcher Core."
                        XButton { Layout.fillWidth: true; text: "Reset Paths"; onClicked: launcherBridge.resetSettingsCategory("paths") }
                    }
                }

                XSettingsPage {
                    title: "Runtime"
                    description: "Runtime preferences are validated now and become authoritative session inputs when the corresponding Xenon service is connected."
                    XSettingsCard {
                        title: "Default graphics backend"
                        description: "Only renderers compiled for this host are offered. Modules may impose additional compatibility requirements."
                        XComboBox {
                            Layout.fillWidth: true
                            model: launcherBridge.availableGraphicsBackends
                            currentIndex: Math.max(0, model.indexOf(root.getString("runtime/graphicsBackend", "Automatic")))
                            onActivated: function(index) { root.save("runtime/graphicsBackend", model[index]) }
                        }
                    }
                    XSettingsCard {
                        title: "Default offline mode"
                        description: "Prefer local/offline service fallbacks when a module supports them."
                        XSwitch { checked: root.getBool("runtime/offline", true); onUserToggled: function(value) { root.save("runtime/offline", value) } }
                    }
                    XSettingsCard {
                        title: "Runtime connection"
                        description: "Launcher Core is isolated from runtime implementation details through RuntimeBridge."
                        StatusPill { label: launcherBridge.backendConnected ? "Connected" : "Disconnected"; tone: launcherBridge.backendConnected ? Theme.success : Theme.warning }
                    }
                    XSettingsCard {
                        title: "Reset runtime preferences"
                        description: "Restore automatic renderer selection and the default offline preference."
                        XButton { Layout.fillWidth: true; text: "Reset Runtime"; onClicked: launcherBridge.resetSettingsCategory("runtime") }
                    }
                }

                XSettingsPage {
                    title: "Graphics"
                    description: "Host graphics preferences are persisted now; renderer-specific device controls will appear when the live graphics service exposes them."
                    XSettingsCard {
                        title: "Renderer capability detection"
                        description: "The launcher only presents backends compiled for the current Xenon build and host platform."
                        StatusPill { label: root.runtimeServiceLabel("graphics"); tone: root.runtimeServiceTone("graphics") }
                    }
                    XSettingsCard {
                        title: "Shader cache"
                        description: "Allow renderer-managed shader cache data when the graphics session API is active."
                        XSwitch { checked: root.getBool("graphics/shaderCache", true); onUserToggled: function(value) { root.save("graphics/shaderCache", value) } }
                    }
                    XSettingsCard {
                        visible: root.getBool("graphics/shaderCache", true)
                        title: "Shader cache mode"
                        description: "Persistent keeps reusable host shader data between sessions; Session only discards it when Xenon closes."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("graphics/shaderCacheMode")
                            currentIndex: root.optionIndex("graphics/shaderCacheMode")
                            onActivated: function(index) { root.saveOption("graphics/shaderCacheMode", index) }
                        }
                    }
                    XSettingsCard {
                        title: "Reset graphics preferences"
                        description: "Restore launcher-side graphics defaults."
                        XButton { Layout.fillWidth: true; text: "Reset Graphics"; onClicked: launcherBridge.resetSettingsCategory("graphics") }
                    }
                }

                XSettingsPage {
                    title: "Input"
                    description: "Launcher-side input preferences are ready now; live device enumeration and mappings remain owned by Xenon Input."
                    XSettingsCard {
                        title: "Input service"
                        description: "Shows whether this build contains and exposes the live input subsystem."
                        StatusPill { label: root.runtimeServiceLabel("input"); tone: root.runtimeServiceTone("input") }
                    }
                    XSettingsCard {
                        title: "Preferred device"
                        description: "Default device family requested when a game profile does not override input selection."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("input/preferredDevice")
                            currentIndex: root.optionIndex("input/preferredDevice")
                            onActivated: function(index) { root.saveOption("input/preferredDevice", index) }
                        }
                    }
                    XSettingsCard {
                        title: "Controller deadzone"
                        description: "Default stick deadzone passed to Xenon Input once live device profiles are available."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("input/deadzone")
                            currentIndex: root.optionIndex("input/deadzone")
                            onActivated: function(index) { root.saveOption("input/deadzone", index) }
                        }
                    }
                    XSettingsCard {
                        title: "Controller rumble"
                        description: "Allow vibration when supported by the selected controller and game module."
                        XSwitch { checked: root.getBool("input/rumble", true); onUserToggled: function(value) { root.save("input/rumble", value) } }
                    }
                    XSettingsCard {
                        title: "Reset input preferences"
                        description: "Restore launcher-side input defaults without deleting future per-device mappings."
                        XButton { Layout.fillWidth: true; text: "Reset Input"; onClicked: launcherBridge.resetSettingsCategory("input") }
                    }
                }

                XSettingsPage {
                    title: "Audio"
                    description: "Launcher-side audio policy is ready now; device enumeration remains owned by Xenon Audio."
                    XSettingsCard {
                        title: "Audio service"
                        description: "Shows whether this build contains and exposes the live audio subsystem."
                        StatusPill { label: root.runtimeServiceLabel("audio"); tone: root.runtimeServiceTone("audio") }
                    }
                    XSettingsCard {
                        title: "Master volume"
                        description: "Default runtime output level before game-specific mixing."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("audio/masterVolume")
                            currentIndex: root.optionIndex("audio/masterVolume")
                            onActivated: function(index) { root.saveOption("audio/masterVolume", index) }
                        }
                    }
                    XSettingsCard {
                        title: "Latency profile"
                        description: "Requests a latency/buffering policy from Xenon Audio when the service is available."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("audio/latencyProfile")
                            currentIndex: root.optionIndex("audio/latencyProfile")
                            onActivated: function(index) { root.saveOption("audio/latencyProfile", index) }
                        }
                    }
                    XSettingsCard {
                        title: "Mute when unfocused"
                        description: "Silence game audio while the Xenon launcher/game session is not the active application."
                        XSwitch { checked: root.getBool("audio/muteUnfocused", false); onUserToggled: function(value) { root.save("audio/muteUnfocused", value) } }
                    }
                    XSettingsCard {
                        title: "Reset audio preferences"
                        description: "Restore launcher-side audio defaults."
                        XButton { Layout.fillWidth: true; text: "Reset Audio"; onClicked: launcherBridge.resetSettingsCategory("audio") }
                    }
                }

                XSettingsPage {
                    title: "Network"
                    description: "This category becomes visible when Xenon networking advertises its frontend capability."
                    XSettingsCard {
                        title: "Network service"
                        description: "Online identity, matchmaking and service-replacement controls remain runtime-owned."
                        StatusPill { label: root.runtimeServiceLabel("network"); tone: root.runtimeServiceTone("network") }
                    }
                }

                XSettingsPage {
                    title: "Updates"
                    description: "Launcher Core checks Project Xenon GitHub Releases, verifies downloaded packages with SHA-256, and stages installation outside the QML layer."

                    XSettingsCard {
                        title: "Launcher update status"
                        description: String(root.launcherUpdateState.statusMessage || "Ready to check for updates.")
                        actionWidth: 360
                        StatusPill { label: root.updateStatusLabel(); tone: root.updateStatusTone() }
                        XButton {
                            text: "Check for updates"
                            enabled: !Boolean(root.launcherUpdateState.busy)
                            onClicked: launcherBridge.requestLauncherUpdateCheck()
                        }
                    }
                    XSettingsCard {
                        title: "Installed version"
                        description: "The version embedded into this launcher build. Release builds receive their semantic version from the GitHub release tag."
                        StatusPill { label: String(root.launcherUpdateState.currentVersion || launcherBridge.version); tone: Theme.textMuted }
                    }
                    XSettingsCard {
                        visible: String(root.launcherUpdateState.availableVersion || "").length > 0
                        title: "Latest release • " + String(root.launcherUpdateState.availableVersion || "")
                        description: root.releaseNotesPreview()
                        actionWidth: 310
                        XButton {
                            visible: String(root.launcherUpdateState.releaseUrl || "").length > 0
                            text: "View on GitHub"
                            onClicked: launcherBridge.openExternalUrl(String(root.launcherUpdateState.releaseUrl))
                        }
                    }
                    XSettingsCard {
                        visible: Boolean(root.launcherUpdateState.canDownload) || String(root.launcherUpdateState.status) === "downloading"
                        title: "Download update"
                        description: String(root.launcherUpdateState.assetName || "")
                            + (Number(root.launcherUpdateState.assetSize || 0) > 0 ? " • " + root.formatBytes(root.launcherUpdateState.assetSize) : "")
                            + " • SHA-256 verification required before installation"
                        actionWidth: 330
                        XButton {
                            visible: String(root.launcherUpdateState.status) !== "downloading"
                            text: "Download & verify"
                            variant: "primary"
                            onClicked: launcherBridge.requestLauncherUpdateDownload()
                        }
                        XButton {
                            visible: String(root.launcherUpdateState.status) === "downloading"
                            text: "Cancel download"
                            onClicked: launcherBridge.cancelLauncherUpdateDownload()
                        }
                    }
                    XSettingsCard {
                        visible: String(root.launcherUpdateState.status) === "downloading"
                        title: "Download progress"
                        description: root.formatBytes(root.launcherUpdateState.downloadedBytes)
                            + (Number(root.launcherUpdateState.downloadTotalBytes || 0) > 0
                               ? " of " + root.formatBytes(root.launcherUpdateState.downloadTotalBytes)
                               : " downloaded")
                        actionWidth: 330
                        ProgressBar {
                            Layout.fillWidth: true
                            from: 0
                            to: 1
                            indeterminate: Number(root.launcherUpdateState.downloadTotalBytes || 0) <= 0
                            value: Number(root.launcherUpdateState.downloadProgress || 0)
                        }
                    }
                    XSettingsCard {
                        visible: Boolean(root.launcherUpdateState.canInstall) || String(root.launcherUpdateState.status) === "ready-to-install"
                        title: "Install verified update"
                        description: Boolean(root.launcherUpdateState.installerSupported)
                            ? "The launcher will close, replace its deployed files using the staged verified package, and restart automatically. The previous installation is kept until the updated launcher starts successfully."
                            : "The update is verified and staged, but automatic replacement is not implemented for this platform yet."
                        actionWidth: 280
                        XButton {
                            text: "Install & restart"
                            variant: "primary"
                            enabled: Boolean(root.launcherUpdateState.canInstall)
                            onClicked: launcherBridge.requestLauncherUpdateInstall()
                        }
                    }
                    XSettingsCard {
                        title: "Automatic update checks"
                        description: "Check GitHub in the background when the selected interval is due. Failed automatic checks remain silent and are reflected in this status page."
                        XSwitch { checked: root.getBool("updates/automaticChecks", true); onUserToggled: function(value) { root.save("updates/automaticChecks", value) } }
                    }
                    XSettingsCard {
                        visible: root.getBool("updates/automaticChecks", true)
                        title: "Check interval"
                        description: "At startup checks every launch; Daily and Weekly use the last successful GitHub check time; Manual disables scheduled checks."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("updates/checkInterval")
                            currentIndex: root.optionIndex("updates/checkInterval")
                            onActivated: function(index) { root.saveOption("updates/checkInterval", index) }
                        }
                    }
                    XSettingsCard {
                        title: "Pre-release builds"
                        description: "Include GitHub prereleases when selecting the newest semantic version. Draft releases are always ignored."
                        XSwitch { checked: root.getBool("updates/prerelease", false); onUserToggled: function(value) { root.save("updates/prerelease", value) } }
                    }
                    XSettingsCard {
                        title: "Update source"
                        description: "GitHub repository: " + String(root.launcherUpdateState.repository || "nimauria/Xenon-Recomp")
                            + (String(root.launcherUpdateState.lastCheckedAt || "").length > 0
                               ? " • Last successful check: " + String(root.launcherUpdateState.lastCheckedAt)
                               : " • Not checked successfully yet")
                        actionWidth: 220
                        XButton {
                            text: "Open repository"
                            onClicked: launcherBridge.openExternalUrl("https://github.com/" + String(root.launcherUpdateState.repository || "nimauria/Xenon-Recomp"))
                        }
                    }
                    XSettingsCard {
                        title: "Module updates"
                        description: "Automatically check installed catalog modules against their own GitHub Releases. Module packages and update state are completely separate from Xenon Launcher releases."
                        XSwitch { checked: root.getBool("updates/modules", true); onUserToggled: function(value) { root.save("updates/modules", value) } }
                    }
                    XSettingsCard {
                        title: "Module pre-release builds"
                        description: "Allow the module updater to select prerelease module packages. This does not change the Xenon Launcher release channel."
                        XSwitch { checked: root.getBool("updates/modulePrerelease", false); onUserToggled: function(value) { root.save("updates/modulePrerelease", value) } }
                    }
                    XSettingsCard {
                        title: "Official module catalog"
                        description: "Xenon discovers public module repositories and releases through the built-in catalog service. Commercial game files, updates and DLC are never distributed by the catalog."
                        actionWidth: 250
                        XButton { Layout.fillWidth: true; text: "Refresh catalog"; onClicked: launcherBridge.requestModuleCatalogRefresh() }
                    }
                    XSettingsCard {
                        title: "Reset update preferences"
                        description: "Restore automatic checks, stable-channel preference and module update defaults. Downloaded/staged packages are not installed by a reset."
                        XButton { Layout.fillWidth: true; text: "Reset Updates"; onClicked: launcherBridge.resetSettingsCategory("updates") }
                    }
                }

                XSettingsPage {
                    title: "Community"
                    description: "Support links and optional Discord integration for the Xenon community."

                    XPanel {
                        Layout.fillWidth: true
                        implicitHeight: communityPanelColumn.implicitHeight + Theme.spaceXl * 2
                        color: Theme.highContrast ? Theme.surfaceAlt : Qt.rgba(Theme.surfaceAlt.r, Theme.surfaceAlt.g, Theme.surfaceAlt.b, Theme.panelOpacity)
                        decorated: true
                        ColumnLayout {
                            id: communityPanelColumn
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            anchors.margins: Theme.spaceXl
                            spacing: Theme.spaceMd
                            Text {
                                text: "Xenon Discord"
                                color: Theme.text
                                font.pixelSize: Theme.typeSubtitle
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.maximumWidth: 700
                                text: String(launcherBridge.communityInfo.supportText || "Join the Xenon community for support and development updates.")
                                color: Theme.textMuted
                                font.pixelSize: Theme.typeBody
                                wrapMode: Text.WordWrap
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: Theme.spaceSm
                                XButton { text: "Join Xenon Discord"; variant: "primary"; onClicked: launcherBridge.openDiscordCommunity() }
                                XButton { text: "Project GitHub"; onClicked: launcherBridge.openProjectCommunity() }
                                XButton {
                                    text: "Copy invite"
                                    onClicked: {
                                        launcherBridge.copyText(String(launcherBridge.communityInfo.discordInvite || ""))
                                        launcherBridge.notify("Discord invite copied", "The Xenon Discord invite was copied to the clipboard.")
                                    }
                                }
                            }
                        }
                    }

                    XSettingsCard {
                        title: "Discord Rich Presence"
                        description: Boolean(launcherBridge.discordPresenceState.providerAvailable)
                            ? "Share Xenon activity with the Discord desktop client. This remains optional and disabled by default."
                            : "Planned integration. Rich Presence is intentionally disabled in this build; Discord community and support links still work normally."
                        XSwitch {
                            enabled: Boolean(launcherBridge.discordPresenceState.providerAvailable)
                            checked: Boolean(launcherBridge.discordPresenceState.enabled)
                            onUserToggled: function(value) { root.save("community/discordRichPresence", value) }
                        }
                    }

                    XSettingsCard {
                        visible: Boolean(launcherBridge.discordPresenceState.enabled)
                        title: "Show game title"
                        description: "When a game session is running, include its title in Rich Presence. Turn this off to show only that Xenon is being used."
                        XSwitch {
                            checked: root.getBool("community/discordShowGameTitle", true)
                            onUserToggled: function(value) { root.save("community/discordShowGameTitle", value) }
                        }
                    }

                    XSettingsCard {
                        title: "Discord provider"
                        description: String(launcherBridge.discordPresenceState.providerStatus || "Discord Rich Presence is unavailable in this build.")
                        actionWidth: 300
                        StatusPill {
                            label: Boolean(launcherBridge.discordPresenceState.providerAvailable) ? "Available" : "Planned"
                            tone: Boolean(launcherBridge.discordPresenceState.providerAvailable) ? Theme.success : Theme.textMuted
                        }
                        XButton {
                            visible: Boolean(launcherBridge.discordPresenceState.providerAvailable)
                            text: "Refresh presence"
                            enabled: Boolean(launcherBridge.discordPresenceState.enabled)
                            onClicked: launcherBridge.refreshDiscordPresence()
                        }
                    }

                    XPanel {
                        visible: Boolean(launcherBridge.discordPresenceState.enabled)
                        Layout.fillWidth: true
                        implicitHeight: presencePreviewColumn.implicitHeight + Theme.spaceLg * 2
                        color: Theme.highContrast ? Theme.surfaceAlt : Qt.rgba(Theme.surfaceAlt.r, Theme.surfaceAlt.g, Theme.surfaceAlt.b, Theme.panelOpacity)
                        ColumnLayout {
                            id: presencePreviewColumn
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            anchors.margins: Theme.spaceLg
                            spacing: Theme.spaceSm
                            Text { text: "Rich Presence preview"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                            XInfoRow { label: "Details"; value: String((launcherBridge.discordPresenceState.activity || {}).details || "Browsing the game library") }
                            XInfoRow { label: "State"; value: String((launcherBridge.discordPresenceState.activity || {}).state || "Powered by Xenon") }
                            XInfoRow { label: "Application ID"; value: String(launcherBridge.discordPresenceState.applicationId || "Not configured") }
                            XInfoRow { label: "Game title sharing"; value: Boolean(launcherBridge.discordPresenceState.showGameTitle) ? "Enabled" : "Hidden" }
                            Text {
                                Layout.fillWidth: true
                                text: "Discord displays the application name from the Xenon Discord application itself. The activity then uses the lines above and can expose Join Xenon Discord and Project Xenon buttons."
                                color: Theme.textMuted
                                font.pixelSize: Theme.typeCaption
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    XSettingsCard {
                        title: "Reset community preferences"
                        description: "Turn Rich Presence back off and restore its privacy defaults. Community links are built into Xenon and are not removed."
                        XButton { Layout.fillWidth: true; text: "Reset Community"; onClicked: launcherBridge.resetSettingsCategory("community") }
                    }
                }

                XSettingsPage {
                    title: "Accessibility"
                    description: "Readability, focus and motion settings are applied by the shared Theme layer and follow supported OS accessibility hints."
                    XSettingsCard {
                        title: "Text size"
                        description: "Text scales independently of game rendering. Smaller UI text grows more strongly than large headings while layouts reflow."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("accessibility/textScale")
                            currentIndex: root.optionIndex("accessibility/textScale")
                            onActivated: function(index) { root.saveOption("accessibility/textScale", index) }
                        }
                    }
                    XSettingsCard {
                        title: "High contrast"
                        description: "Strengthen borders and foreground contrast in addition to OS high-contrast hints."
                        XSwitch { checked: root.getBool("accessibility/highContrast", false); onUserToggled: function(value) { root.save("accessibility/highContrast", value) } }
                    }
                    XSettingsCard {
                        title: "Enhanced focus ring"
                        description: "Use a stronger keyboard-focus outline even when high-contrast mode is not enabled."
                        XSwitch { checked: root.getBool("accessibility/enhancedFocus", false); onUserToggled: function(value) { root.save("accessibility/enhancedFocus", value) } }
                    }
                    XSettingsCard {
                        title: "Reduce motion"
                        description: "Disable non-essential interface animation and shorten state transitions."
                        XSwitch { checked: root.getBool("accessibility/reduceMotion", false); onUserToggled: function(value) { root.save("accessibility/reduceMotion", value) } }
                    }
                    XSettingsCard {
                        title: "Keyboard navigation"
                        description: "Primary destinations and controls expose visible keyboard focus and standard tab navigation."
                        StatusPill { label: "Enabled"; tone: Theme.success }
                    }
                    XSettingsCard {
                        title: "Reset accessibility"
                        description: "Restore launcher accessibility overrides while continuing to follow OS-level accessibility hints."
                        XButton { Layout.fillWidth: true; text: "Reset Accessibility"; onClicked: launcherBridge.resetSettingsCategory("accessibility") }
                    }
                }

                XSettingsPage {
                    title: "Developer"
                    description: "Development-only fixture selection and diagnostics. Hidden from production builds."
                    XSettingsCard {
                        title: "Launcher fixture data"
                        description: "Switch between generic fictional data, a Project Gracemeria UI preview, or an empty launcher without recompiling."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.optionLabels("developer/fixtureMode")
                            currentIndex: root.optionIndex("developer/fixtureMode")
                            onActivated: function(index) { root.saveOption("developer/fixtureMode", index) }
                        }
                    }
                    XSettingsCard {
                        title: "Verbose launcher logging"
                        description: "Keep extra frontend-backend diagnostics available for development builds."
                        XSwitch { checked: root.getBool("developer/verboseLogging", false); onUserToggled: function(value) { root.save("developer/verboseLogging", value) } }
                    }
                    XSettingsCard { title: "Runtime service"; description: "Reports whether the launcher is connected to the Xenon backend service registry."; StatusPill { label: launcherBridge.backendConnected ? "Connected" : "Not connected"; tone: launcherBridge.backendConnected ? Theme.success : Theme.warning } }
                    XPanel {
                        Layout.fillWidth: true
                        implicitHeight: developerColumn.implicitHeight + Theme.spaceLg * 2
                        color: Theme.highContrast ? Theme.surfaceAlt : Qt.rgba(Theme.surfaceAlt.r, Theme.surfaceAlt.g, Theme.surfaceAlt.b, Theme.panelOpacity)
                        ColumnLayout {
                            id: developerColumn
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            anchors.margins: Theme.spaceLg
                            spacing: Theme.spaceSm
                            Text { text: "Developer diagnostics"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                            Text { Layout.fillWidth: true; text: launcherBridge.developerDiagnostics(); color: Theme.textMuted; font.family: "monospace"; font.pixelSize: Theme.typeCaption; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText }
                            RowLayout {
                                Layout.fillWidth: true
                                Item { Layout.fillWidth: true }
                                XButton {
                                    text: "Copy diagnostics"
                                    onClicked: {
                                        launcherBridge.copyText(launcherBridge.developerDiagnostics())
                                        launcherBridge.notify("Diagnostics copied", "Developer diagnostics were copied to the clipboard.")
                                    }
                                }
                            }
                        }
                    }
                    XSettingsCard {
                        title: "Reset developer settings"
                        description: "Restore fixture and launcher diagnostic preferences."
                        XButton { Layout.fillWidth: true; text: "Reset Developer"; onClicked: launcherBridge.resetSettingsCategory("developer") }
                    }
                }

                XSettingsPage {
                    title: "About Xenon"
                    description: "User-focused launcher information, diagnostics and project links."
                    XPanel {
                        Layout.fillWidth: true
                        implicitHeight: aboutColumn.implicitHeight + Theme.spaceXl * 2
                        color: Theme.highContrast ? Theme.surfaceAlt : Qt.rgba(Theme.surfaceAlt.r, Theme.surfaceAlt.g, Theme.surfaceAlt.b, Theme.panelOpacity)
                        decorated: true
                        ColumnLayout {
                            id: aboutColumn
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            anchors.margins: Theme.spaceXl
                            spacing: Theme.spaceSm
                            XenonBrand {
                                Layout.preferredWidth: Math.min(300, Math.max(210, aboutColumn.width * 0.42))
                                Layout.preferredHeight: 62
                                asset: "lockup"
                                brandColor: Theme.accent
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.maximumWidth: 460
                                text: "PLAY  •  PRESERVE  •  REIMAGINE"
                                color: Theme.textMuted
                                font.pixelSize: Theme.typeCaption
                                font.letterSpacing: Math.min(2, Theme.typeCaption * 0.10)
                                wrapMode: Text.WordWrap
                            }
                            XInfoRow { label: "Launcher version"; value: launcherBridge.version }
                            XInfoRow { label: "System"; value: launcherBridge.platformName + " • " + launcherBridge.hostArchitecture }
                            XInfoRow { label: "Qt"; value: launcherBridge.qtVersion }
                            XInfoRow { label: "Runtime"; value: launcherBridge.backendConnected ? "Connected" : "Front-end only" }
                            XInfoRow { label: "Theme"; value: Theme.name + " • " + launcherBridge.accentId }
                            XInfoRow { label: "Active profile"; value: launcherBridge.profileName }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spaceSm
                                XButton { text: "Copy system summary"; onClicked: { launcherBridge.copyText(launcherBridge.userDiagnostics()); launcherBridge.notify("Summary copied", "A user-focused system summary was copied to the clipboard.") } }
                                XButton { text: "Join Discord"; variant: "primary"; onClicked: launcherBridge.openDiscordCommunity() }
                                XButton { text: "Project GitHub"; onClicked: launcherBridge.openProjectCommunity() }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }
                    XSettingsCard {
                        title: "Reset all launcher settings"
                        description: "Restore all registered preferences and launcher-wide paths. Game library entries, profiles and installed modules are not deleted."
                        XButton { Layout.fillWidth: true; text: "Reset All Settings"; variant: "danger"; onClicked: resetAllConfirm.open() }
                    }
                }
            }

            EmptyState {
                anchors.fill: parent
                anchors.margins: Theme.spaceLg
                visible: root.visibleCategories.length === 0
                glyph: "?"
                title: "No settings found"
                description: "Try a different search term."
                primaryText: ""
                secondaryText: ""
            }
        }
    }

    XConfirmDialog {
        id: resetAllConfirm
        title: "Reset all launcher settings?"
        message: "This restores launcher preferences and paths to their defaults. Profiles, library entries, modules, saves and game content are not deleted."
        confirmText: "Reset Settings"
        destructive: true
        onConfirmed: launcherBridge.resetAllSettings()
    }
}
