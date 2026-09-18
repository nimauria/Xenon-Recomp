import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string searchText: ""
    property int categoryIndex: 0
    property int settingsRevision: 0

    readonly property var categories: [
        { name: "General",       key: "settings.general",       page: 0,  keywords: "startup language sidebar navigation animations" },
        { name: "Appearance",    key: "settings.appearance",    page: 1,  keywords: "theme accent background density corners colour color" },
        { name: "Library",       key: "settings.library",       page: 2,  keywords: "games dlc compatibility missing content" },
        { name: "Paths",         key: "settings.paths",         page: 3,  keywords: "folders directories games saves profiles modules screenshots cache" },
        { name: "Runtime",       key: "settings.runtime",       page: 4,  keywords: "cpu renderer offline runtime" },
        { name: "Graphics",      key: "settings.graphics",      page: 5,  keywords: "vulkan d3d12 renderer graphics shader" },
        { name: "Input",         key: "settings.input",         page: 6,  keywords: "controller keyboard gamepad input" },
        { name: "Audio",         key: "settings.audio",         page: 7,  keywords: "sound audio volume device" },
        { name: "Network",       key: "settings.network",       page: 8,  keywords: "network online multiplayer" },
        { name: "Updates",       key: "settings.updates",       page: 9,  keywords: "updates modules catalog github releases" },
        { name: "Accessibility", key: "settings.accessibility", page: 10, keywords: "text size scale contrast motion keyboard accessibility" },
        { name: "Developer",     key: "settings.developer",     page: 11, keywords: "test fixture diagnostics gracemeria development" },
        { name: "About",         key: "settings.about",         page: 12, keywords: "version system qt github licence diagnostics" }
    ]

    readonly property var visibleCategories: categories.filter(function(category) {
        if (!launcherBridge.featureEnabled(category.key))
            return false
        var needle = root.searchText.trim().toLowerCase()
        return needle.length === 0
            || category.name.toLowerCase().indexOf(needle) !== -1
            || category.keywords.indexOf(needle) !== -1
    })

    readonly property var themeNames: ["System", "Xenon Dark", "Carbon", "Industrial", "Light"]
    readonly property var themeIds: ["system", "xenon-dark", "carbon", "industrial", "light"]
    readonly property var accentNames: ["Theme Default", "Cyan", "Emerald", "Amber", "Violet", "Rose"]
    readonly property var accentIds: ["default", "cyan", "emerald", "amber", "violet", "rose"]
    readonly property var cornerNames: ["Rounded", "Soft", "Square"]
    readonly property var cornerIds: ["rounded", "soft", "square"]
    readonly property var textSizeNames: ["100%", "110%", "125%", "150%", "175%", "200%"]
    readonly property var textSizeValues: [1.0, 1.1, 1.25, 1.5, 1.75, 2.0]
    readonly property var fixtureNames: ["None", "Generic Xenon UI", "Project Gracemeria UI Preview"]
    readonly property var fixtureIds: ["none", "generic", "gracemeria"]

    readonly property var backgroundVariantNames: {
        if (Theme.effectiveThemeId === "xenon-dark") return ["Orbit", "Tech", "HUD"]
        if (Theme.effectiveThemeId === "carbon") return ["Nebula", "Tech"]
        if (Theme.effectiveThemeId === "industrial") return ["Orbit", "Tech"]
        return ["Minimal"]
    }
    readonly property var backgroundVariantIds: {
        if (Theme.effectiveThemeId === "xenon-dark") return ["orbit", "tech", "hud"]
        if (Theme.effectiveThemeId === "carbon") return ["nebula", "tech"]
        if (Theme.effectiveThemeId === "industrial") return ["orbit", "tech"]
        return ["minimal"]
    }

    function categoryVisibleIndex(page) {
        for (var i = 0; i < visibleCategories.length; ++i)
            if (visibleCategories[i].page === page) return i
        return 0
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

    function indexFor(list, value) {
        var index = list.indexOf(value)
        return index < 0 ? 0 : index
    }
    function getBool(key, fallback) { var r = settingsRevision; return launcherBridge.boolSetting(key, fallback) }
    function getString(key, fallback) { var r = settingsRevision; return launcherBridge.stringSetting(key, fallback) }
    function getNumber(key, fallback) { var r = settingsRevision; return launcherBridge.numberSetting(key, fallback) }
    function save(key, value) { launcherBridge.setSettingValue(key, value) }

    Connections {
        target: launcherBridge
        function onSettingChanged(key, value) { root.settingsRevision += 1 }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spaceMd

        XSectionHeader {
            title: "Settings"
            description: root.searchText.trim().length > 0
                ? "Showing settings categories related to “" + root.searchText.trim() + "”."
                : "Launcher preferences apply immediately. Backend-specific options appear only when their Xenon service is available."
        }

        XPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.textScale >= 1.5
                ? Math.max(64, Theme.controlHeight + Theme.spaceLg * 2)
                : Math.max(56, Theme.controlHeight + Theme.spaceLg)
            color: Theme.surface

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
            color: Theme.surface

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
                        description: "Language used throughout the Xenon interface."
                        XComboBox { Layout.fillWidth: true; model: ["English (UK)"]; enabled: false }
                    }
                    XSettingsCard {
                        title: "Startup page"
                        description: "Page shown when Xenon opens unless Remember last page is enabled."
                        XComboBox {
                            Layout.fillWidth: true
                            model: ["Library", "Modules", "Profiles", "Settings"]
                            currentIndex: Math.max(0, model.indexOf(root.getString("general/startupPage", "Library")))
                            onActivated: function(index) { root.save("general/startupPage", model[index]) }
                        }
                    }
                    XSettingsCard {
                        title: "Sidebar layout"
                        description: "Auto adapts to the window. Compact keeps icon-only primary navigation."
                        XComboBox {
                            Layout.fillWidth: true
                            model: ["Auto", "Expanded", "Compact"]
                            currentIndex: Math.max(0, model.indexOf(root.getString("general/sidebarMode", "Auto")))
                            onActivated: function(index) { root.save("general/sidebarMode", model[index]) }
                        }
                    }
                    XSettingsCard {
                        title: "Remember last page"
                        description: "Resume where you left off on the next launch."
                        XSwitch { checked: root.getBool("general/rememberPage", true); onUserToggled: function(value) { root.save("general/rememberPage", value) } }
                    }
                    XSettingsCard {
                        title: "Interface animations"
                        description: "Use subtle transitions and hover motion. Reduce motion overrides this."
                        XSwitch { checked: root.getBool("general/animations", true); onUserToggled: function(value) { root.save("general/animations", value) } }
                    }
                }

                XSettingsPage {
                    title: "Appearance"
                    description: "Choose a base theme and let Xenon branding follow the active accent automatically."
                    XSettingsCard {
                        title: "Theme"
                        description: "System follows the operating system light/dark preference."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.themeNames
                            currentIndex: root.indexFor(root.themeIds, launcherBridge.themeId)
                            onActivated: function(index) { launcherBridge.themeId = root.themeIds[index]; Theme.setTheme(root.themeIds[index]) }
                        }
                    }
                    XSettingsCard {
                        title: "Accent colour"
                        description: "Brand marks, focus states and primary actions recolour with the active accent."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.accentNames
                            currentIndex: root.indexFor(root.accentIds, launcherBridge.accentId)
                            onActivated: function(index) { launcherBridge.accentId = root.accentIds[index]; Theme.setAccent(root.accentIds[index]) }
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
                            model: root.cornerNames
                            currentIndex: root.indexFor(root.cornerIds, launcherBridge.cornerStyle)
                            onActivated: function(index) { launcherBridge.cornerStyle = root.cornerIds[index]; Theme.setCornerStyle(root.cornerIds[index]) }
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
                            : "Choose artwork designed specifically for the active base theme. Game pages still use module-provided artwork separately."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.backgroundVariantNames
                            enabled: root.backgroundVariantIds.length > 1
                            currentIndex: root.indexFor(
                                root.backgroundVariantIds,
                                root.getString("appearance/backdropVariant/" + Theme.effectiveThemeId, root.backgroundVariantIds[0]))
                            onActivated: function(index) {
                                root.save("appearance/backdropVariant/" + Theme.effectiveThemeId, root.backgroundVariantIds[index])
                            }
                        }
                    }
                    XSettingsCard {
                        title: "Background strength"
                        description: "Adjust the visibility of Xenon-owned theme graphics without affecting module artwork."
                        XComboBox {
                            Layout.fillWidth: true
                            model: ["Subtle", "Standard", "Strong"]
                            currentIndex: {
                                var value = launcherBridge.numberSetting("appearance/backdropIntensity", 0.72)
                                return value < 0.55 ? 0 : value > 0.85 ? 2 : 1
                            }
                            onActivated: function(index) { root.save("appearance/backdropIntensity", [0.42, 0.72, 1.0][index]) }
                        }
                    }
                    XSettingsCard {
                        title: "Game artwork backgrounds"
                        description: "Allow installed game modules to provide artwork for their own pages."
                        XSwitch { checked: root.getBool("appearance/artworkBackgrounds", true); onUserToggled: function(value) { root.save("appearance/artworkBackgrounds", value) } }
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
                            model: ["Show in catalogue", "Hide missing content"]
                            currentIndex: root.getString("library/missingContent", "Show in catalogue") === "Hide missing content" ? 1 : 0
                            onActivated: function(index) { root.save("library/missingContent", model[index]) }
                        }
                    }
                }

                XSettingsPage {
                    title: "Paths"
                    description: "Launcher-wide defaults. Individual profiles may optionally override Games, Saves and Screenshots."
                    XPathField { label: "Game library"; helperText: "Default location for user-provided game content."; pathValue: root.getString("paths/games", launcherBridge.defaultGameLibraryPath); onPathEdited: function(path) { root.save("paths/games", path) } }
                    XPathField { label: "Save data"; helperText: "Default location for profile save data."; pathValue: root.getString("paths/saves", launcherBridge.defaultSaveDataPath); onPathEdited: function(path) { root.save("paths/saves", path) } }
                    XPathField { label: "Profiles"; helperText: "Profiles, local profile images and profiles.json are stored here."; pathValue: root.getString("paths/profiles", launcherBridge.defaultProfilesPath); onPathEdited: function(path) { root.save("paths/profiles", path); ProfileStore.persist() } }
                    XPathField { label: "Modules"; helperText: "Installed Xenon modules and support packages."; pathValue: root.getString("paths/modules", launcherBridge.defaultModulesPath); onPathEdited: function(path) { root.save("paths/modules", path) } }
                    XPathField { label: "Screenshots"; helperText: "Default screenshot location."; pathValue: root.getString("paths/screenshots", launcherBridge.defaultScreenshotsPath); onPathEdited: function(path) { root.save("paths/screenshots", path) } }
                    XPathField { label: "Cache"; helperText: "Launcher/runtime cache files."; pathValue: root.getString("paths/cache", launcherBridge.cachePath); onPathEdited: function(path) { root.save("paths/cache", path) } }
                }

                XSettingsPage {
                    title: "Runtime"
                    description: "Runtime preferences become authoritative when the Xenon backend is connected."
                    XSettingsCard {
                        title: "Default graphics backend"
                        description: "Modules may impose additional compatibility requirements."
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
                }

                XSettingsPage {
                    title: "Graphics"
                    description: "Host graphics preferences. Device-specific controls appear when the renderer backend is connected."
                    XSettingsCard { title: "Renderer capability detection"; description: "Current front-end build exposes host-aware backend choices; adapter probing is backend-owned."; StatusPill { label: launcherBridge.backendConnected ? "Connected" : "Backend pending"; tone: launcherBridge.backendConnected ? Theme.success : Theme.warning } }
                    XSettingsCard { title: "Shader cache"; description: "Prepare the UI contract for renderer-managed shader caches."; XSwitch { checked: root.getBool("graphics/shaderCache", true); onUserToggled: function(value) { root.save("graphics/shaderCache", value) } } }
                }

                XSettingsPage {
                    title: "Input"
                    description: "Input mappings and controller discovery will populate when Xenon Input is connected."
                    XSettingsCard { title: "Input service"; description: "Keyboard/controller configuration is front-end ready but runtime-backed enumeration is pending."; StatusPill { label: "Backend pending"; tone: Theme.warning } }
                }

                XSettingsPage {
                    title: "Audio"
                    description: "Audio device and latency controls will populate when Xenon Audio is connected."
                    XSettingsCard { title: "Audio service"; description: "Device enumeration and latency tuning are runtime-backed."; StatusPill { label: "Backend pending"; tone: Theme.warning } }
                }

                XSettingsPage {
                    title: "Network"
                    description: "Hidden until the Xenon networking service advertises this capability."
                }

                XSettingsPage {
                    title: "Updates"
                    description: "Keep Xenon and open-source modules current. Update sources are managed by Xenon rather than exposed as editable paths."

                    XSettingsCard {
                        title: "Launcher updates"
                        description: "Check the official Project Xenon release source. The frontend core will download and verify updates when the update service is connected."
                        actionWidth: 250
                        XButton {
                            Layout.fillWidth: true
                            text: "Check for updates"
                            onClicked: launcherBridge.requestLauncherUpdateCheck()
                        }
                    }

                    XSettingsCard {
                        title: "Pre-release builds"
                        description: "Include development builds when checking for launcher updates."
                        XSwitch { checked: root.getBool("updates/prerelease", false); onUserToggled: function(value) { root.save("updates/prerelease", value) } }
                    }

                    XSettingsCard {
                        title: "Module updates"
                        description: "Automatically check installed open-source modules for newer compatible releases."
                        XSwitch { checked: root.getBool("updates/modules", true); onUserToggled: function(value) { root.save("updates/modules", value) } }
                    }

                    XSettingsCard {
                        title: "Official module catalog"
                        description: "Xenon discovers public module repositories and releases through the built-in catalog service. Commercial game files, updates and DLC are never distributed by the catalog."
                        actionWidth: 250
                        XButton {
                            Layout.fillWidth: true
                            text: "Refresh catalog"
                            onClicked: launcherBridge.requestModuleCatalogRefresh()
                        }
                    }
                }

                XSettingsPage {
                    title: "Accessibility"
                    description: "Improve readability, focus visibility and motion comfort. Xenon also follows supported OS accessibility hints."
                    XSettingsCard {
                        title: "Text size"
                        description: "Text scales independently of game rendering. Smaller UI text grows more strongly than large headings, while layouts reflow at higher scales."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.textSizeNames
                            currentIndex: root.indexFor(root.textSizeValues, root.getNumber("accessibility/textScale", 1.0))
                            onActivated: function(index) { var value = root.textSizeValues[index]; root.save("accessibility/textScale", value); Theme.setAccessibility(value, root.getBool("accessibility/highContrast", false)) }
                        }
                    }
                    XSettingsCard { title: "High contrast"; description: "Strengthen borders and foreground contrast in addition to OS high-contrast hints."; XSwitch { checked: root.getBool("accessibility/highContrast", false); onUserToggled: function(value) { root.save("accessibility/highContrast", value); Theme.setAccessibility(root.getNumber("accessibility/textScale", 1.0), value) } } }
                    XSettingsCard { title: "Reduce motion"; description: "Disable non-essential interface animation and shorten state transitions."; XSwitch { checked: root.getBool("accessibility/reduceMotion", false); onUserToggled: function(value) { root.save("accessibility/reduceMotion", value) } } }
                    XSettingsCard { title: "Keyboard navigation"; description: "Primary destinations and controls expose visible keyboard focus."; StatusPill { label: "Enabled"; tone: Theme.success } }
                }

                XSettingsPage {
                    title: "Developer"
                    description: "Development-only fixture selection and diagnostics. Hidden from production builds."
                    XSettingsCard {
                        title: "Launcher fixture data"
                        description: "Switch between generic fictional data, a Project Gracemeria UI preview, or an empty launcher without recompiling."
                        XComboBox {
                            Layout.fillWidth: true
                            model: root.fixtureNames
                            currentIndex: root.indexFor(root.fixtureIds, root.getString("developer/fixtureMode", launcherBridge.testMode ? "generic" : "none"))
                            onActivated: function(index) { root.save("developer/fixtureMode", root.fixtureIds[index]) }
                        }
                    }
                    XSettingsCard { title: "Runtime service"; description: "Reports whether the launcher is connected to the Xenon backend service registry."; StatusPill { label: launcherBridge.backendConnected ? "Connected" : "Not connected"; tone: launcherBridge.backendConnected ? Theme.success : Theme.warning } }
                    XPanel {
                        Layout.fillWidth: true
                        implicitHeight: developerColumn.implicitHeight + Theme.spaceLg * 2
                        color: Theme.surfaceAlt
                        ColumnLayout {
                            id: developerColumn
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            anchors.margins: Theme.spaceLg
                            spacing: Theme.spaceSm
                            Text { text: "Developer diagnostics"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                            Text { Layout.fillWidth: true; text: launcherBridge.developerDiagnostics(); color: Theme.textMuted; font.family: "monospace"; font.pixelSize: Theme.typeCaption; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText }
                            RowLayout {
                                Layout.fillWidth: true

                                Item {
                                    Layout.fillWidth: true
                                }

                                XButton {
                                    text: "Copy diagnostics"
                                    onClicked: {
                                        launcherBridge.copyText(launcherBridge.developerDiagnostics())
                                        launcherBridge.notify(
                                            "Diagnostics copied",
                                            "Developer diagnostics were copied to the clipboard."
                                        )
                                    }
                                }
                            }
                        }
                    }
                }

                XSettingsPage {
                    title: "About Xenon"
                    description: "User-focused launcher information and project links."
                    XPanel {
                        Layout.fillWidth: true
                        implicitHeight: aboutColumn.implicitHeight + Theme.spaceXl * 2
                        color: Theme.surfaceAlt
                        ColumnLayout {
                            id: aboutColumn
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            anchors.margins: Theme.spaceXl
                            spacing: Theme.spaceSm
                            XenonBrand { Layout.preferredWidth: 300; Layout.preferredHeight: 68; asset: "lockup"; brandColor: Theme.accent }
                            Text {
                                text: "PLAY  •  PRESERVE  •  REIMAGINE"
                                color: Theme.textMuted
                                font.pixelSize: Theme.typeCaption
                                font.letterSpacing: 2
                                wrapMode: Text.WordWrap
                            }
                            XInfoRow { label: "Launcher version"; value: launcherBridge.version }
                            XInfoRow { label: "System"; value: launcherBridge.platformName + " • " + launcherBridge.hostArchitecture }
                            XInfoRow { label: "Qt"; value: launcherBridge.qtVersion }
                            XInfoRow { label: "Runtime"; value: launcherBridge.backendConnected ? "Connected" : "Front-end only" }
                            XInfoRow { label: "Active profile"; value: launcherBridge.profileName }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spaceSm
                                XButton { text: "Copy system summary"; onClicked: { launcherBridge.copyText(launcherBridge.userDiagnostics()); launcherBridge.notify("Summary copied", "A user-focused system summary was copied to the clipboard.") } }
                                XButton { text: "Project GitHub"; onClicked: launcherBridge.openExternalUrl("https://github.com/nimauria/Xenon-Recomp") }
                                Item { Layout.fillWidth: true }
                            }
                        }
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
}
