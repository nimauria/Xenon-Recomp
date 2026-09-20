# Xenon Content Services V1

## Overview

Content Services V1 provides a comprehensive content management system for Xbox 360 titles, building on the existing GDFX/STFS/filesystem infrastructure. It implements an explicit content graph model that represents base games, title updates, DLC, and save data without physically merging original files, creating effective mounted views instead.

## Design Principles

1. **Never Modify Source Images**: All original content packages and images remain immutable
2. **Explicit Content Graph**: Content relationships are represented structurally, not through physical merging
3. **Effective Mounted Views**: Create virtual unified views through VFS mounting
4. **Profile Isolation**: Save data and writable storage are isolated per-profile and per-game
5. **Atomic Operations**: Save writes use atomic semantics with automatic backups
6. **Validation First**: All content is validated before use (title ID, media ID, content ID matching)

## Architecture

### Content Graph (`content_graph.hpp`)

The content graph explicitly represents all installed content for a title:

```
Game (Title ID: 12345678)
 ├─ Base Game
 │   └─ default.xex (media_id, version)
 ├─ Title Update (selected)
 │   └─ TU v1.2.0 (compatible with base)
 ├─ Available Title Updates
 │   ├─ TU v1.2.0 ✓ selected
 │   └─ TU v1.1.0
 ├─ DLC
 │   ├─ DLC Pack 1 (content_id: AABBCC..., validated)
 │   └─ DLC Pack 2 (content_id: DDEEFF..., validated)
 └─ Saves (Profile: 0x0123456789ABCDEF)
     ├─ Save Slot 1 (with 3 backups)
     └─ Save Slot 2
```

#### Content Node Types

- **BaseGame**: Primary game executable and base content
- **TitleUpdate**: Patches that modify the base executable
- **DLC**: Additional content packages (STFS containers)
- **SaveData**: Per-profile, per-game save containers
- **WritableStorage**: Game-specific persistent storage

#### Content Status

- **Unknown**: Not yet identified
- **Available**: Identified and ready to use
- **Mounted**: Currently active in VFS
- **Missing**: Referenced but not found
- **Invalid**: Failed validation
- **Incompatible**: Doesn't match requirements

### Title Update Manager (`title_update_manager.hpp`)

Automatically identifies and validates title updates against the base game.

#### Discovery and Validation

```cpp
auto tu_manager = std::make_unique<TitleUpdateManager>();
tu_manager->initialize();

// Search for title updates
auto updates = tu_manager->discover_title_updates(search_path, title_id);

// Validate against base game
auto validation = tu_manager->validate_title_update(*title_update, base_game);
if (validation.is_valid) {
    // Title update is compatible:
    // - Title ID matches
    // - Media ID compatible
    // - Version requirements met
    // - Base executable supported
}

// Select best compatible update
auto selected = tu_manager->select_best_title_update(available_updates, base_game);
```

#### XEX Loading Integration

```cpp
// Load base XEX
XexImage base_image;
parse_xex_image(base_xex_bytes, base_image);

// Apply title update if selected
if (content_graph.has_title_update()) {
    auto tu_bytes = read_file(content_graph.selected_title_update->source_path);
    XexImage patched_image;
    apply_title_update(base_image, tu_bytes, patched_image);
    // Use patched_image for execution
}
```

### DLC Manager (`dlc_manager.hpp`)

Identifies and validates DLC packages against game-specific manifests.

#### Manifest Registration

```cpp
DLCManager dlc_manager;
dlc_manager.initialize();

// Game modules register valid DLC content IDs
std::vector<DLCManifestEntry> dlc_catalog = {
    {
        .content_id = hex_to_bytes("AABBCC..."),
        .content_id_hex = "AABBCC...",
        .display_name = "Expansion Pack 1",
        .title_id = 0x12345678,
    },
};

dlc_manager.register_dlc_manifest(title_id, dlc_catalog);
```

#### Discovery and Validation

```cpp
// Discover DLC packages
auto dlc_packages = dlc_manager.discover_dlc_packages(dlc_path, title_id);

for (const auto& dlc : dlc_packages) {
    // Validation checks:
    // - Valid STFS structure (20-byte content ID)
    // - Title ID match
    // - Content ID declared in manifest
    // - Media ID match (if specified)
    if (dlc->is_legitimate) {
        content_graph.installed_dlc.push_back(std::move(dlc));
    }
}
```

#### VFS Mounting

```cpp
// Mount DLC as read-only STFS packages
for (size_t i = 0; i < content_graph.installed_dlc.size(); ++i) {
    const auto& dlc = content_graph.installed_dlc[i];
    auto stfs_source = std::make_unique<StfsPackageSource>(dlc->source_path);
    auto device = std::make_unique<ReadOnlyContentDevice>(std::move(stfs_source));
    vfs.register_device("dlc" + std::to_string(i) + ":", std::move(device));
}
```

### Save Manager (`save_manager.hpp`)

Implements per-profile, per-game save storage with atomic writes and automatic backups.

#### Directory Structure

```
<base_directory>/
  <profile_xuid_hex>/              # e.g., 0123456789ABCDEF
    <title_id_hex>/                # e.g., 12345678
      Save_Slot_1/                 # Save container
      Save_Slot_1.backups/         # Automatic backups
        backup_20260920_153045/
        backup_20260920_140022/
        backup_20260920_120010/
      Save_Slot_2/
      Save_Slot_2.backups/
```

#### Atomic Save Writes

```cpp
SaveManager save_manager;
save_manager.initialize(base_save_directory);

// Write save data atomically
auto result = save_manager.write_save_atomic(
    profile_xuid,
    title_id,
    "Save_Slot_1",
    source_data_path  // Temporary staging area
);

// Process:
// 1. Create backup of existing save (if it exists)
// 2. Write new data to .tmp location
// 3. Atomic rename from .tmp to final location
// 4. Clean up old backups (keep 3 most recent)
```

#### Backup and Recovery

```cpp
// Create manual backup
auto backup_result = save_manager.create_backup(profile_xuid, title_id, "Save_Slot_1");

// List available backups (sorted newest first)
auto backups = save_manager.list_backups(profile_xuid, title_id, "Save_Slot_1");

// Restore from backup
auto restore_result = save_manager.restore_from_backup(
    profile_xuid, title_id, "Save_Slot_1", 0  // Index 0 = most recent
);
```

## Complete Workflow

### 1. Initialize Content Services

```cpp
auto vfs = std::make_unique<VirtualFileSystem>();
auto content_manager = std::make_unique<ContentManager>();
content_manager->initialize();
```

### 2. Build Content Graph

```cpp
// Build complete content graph for a title
auto graph = content_manager->build_content_graph(
    title_id, base_game_path, title_update_path, dlc_path, profile_xuid
);
```

### 3. Mount Content

```cpp
// Mount content to VFS
content_manager->mount_content_graph(*vfs, *graph);
// VFS now has: game:, dlc0:, dlc1:, saves:
```

### 4. Load and Execute

```cpp
// Load base XEX with title update if available
auto base_xex_bytes = read_file(graph->base->default_xex_path);
XexImage xex_image;

if (graph->has_title_update()) {
    auto tu_bytes = read_file(graph->selected_title_update->source_path);
    XexImage base_image;
    parse_xex_image(base_xex_bytes, base_image);
    apply_title_update(base_image, tu_bytes, xex_image);
} else {
    parse_xex_image(base_xex_bytes, xex_image);
}

LoadedXex loaded;
load_xex(memory, xex_image.effective_image, loaded);
```

## Security Guarantees

### Source Immutability
- Original game images, STFS packages, and title updates are never modified
- All modifications happen in memory or in isolated save directories

### Validation Layers
1. **STFS Structure**: Package header and structure validation
2. **Content ID Matching**: DLC content IDs must be declared in game module manifest
3. **Title ID Verification**: All content must match the running title's ID
4. **Media ID Compatibility**: Title updates must support the base media ID
5. **Version Requirements**: Title updates validate base version compatibility

### Profile Isolation
- Saves are strictly isolated by profile XUID
- No cross-profile data access
- Profile-specific writable storage areas

### Atomic Save Operations
- No partial writes visible to game
- Automatic backups before overwrites
- Rollback capability through backup system
- Corruption protection through atomic rename

## Summary

Content Services V1 provides:

✅ **Explicit Content Graph** - Structural representation of all game content  
✅ **Title Update Management** - Automatic identification, validation, and application  
✅ **DLC Support** - Manifest-based validation and VFS mounting  
✅ **Save Management** - Atomic writes, automatic backups, profile isolation  
✅ **Source Immutability** - Original content never modified  
✅ **Effective Mounted Views** - Virtual unified content through VFS  
✅ **Xbox API Compatibility** - Proper content enumeration and device management  

## References

- [Filesystem V1](filesystem/FILESYSTEM_V1.md) - VFS foundation
- [XEX Loader](XEX_LOADER.md) - Title update application
- [STFS Package Format](https://free60project.github.io/wiki/STFS.html)
- [XContent Format](https://free60project.github.io/wiki/XContent.html)

