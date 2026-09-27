# Xenon Filesystem Launcher Integration

This document describes the complete integration of the Xenon VFS (Virtual File System) with the launcher and frontend, following the patterns established by Xenia and other Xbox 360 recompilation projects.

## Architecture Overview

The filesystem integration consists of several layers:

```
┌─────────────────────────────────────────────────────────────┐
│                     QML Frontend (FilesystemPage.qml)         │
│                  - Mount management UI                         │
│                  - VFS status display                          │
│                  - Path testing tools                          │
└────────────────────────────┬─────────────────────────────────┘
                             │
┌────────────────────────────▼─────────────────────────────────┐
│              FrontendBackend::FilesystemFeature               │
│               - Qt/QML bridge layer                           │
│               - ServiceResult wrapping                        │
└────────────────────────────┬─────────────────────────────────┘
                             │
┌────────────────────────────▼─────────────────────────────────┐
│                    FilesystemService                          │
│              - Launcher-side VFS management                   │
│              - Mount/unmount operations                       │
│              - Symbolic link registration                     │
└────────────────────────────┬─────────────────────────────────┘
                             │
┌────────────────────────────▼─────────────────────────────────┐
│                 Xenon::Filesystem::VirtualFileSystem          │
│              - Host-independent VFS core                      │
│              - Device registration                            │
│              - Path resolution                                │
└────────────────────────────┬─────────────────────────────────┘
                             │
         ┌───────────────────┼───────────────────┐
         │                   │                   │
┌────────▼─────────┐  ┌──────▼──────────┐  ┌───▼─────────────┐
│ HostPathDevice   │  │ReadOnlyContent  │  │  NullDevice     │
│  (Folders)       │  │    Device       │  │  (Empty)        │
│                  │  │ (GDFX/STFS)     │  │                 │
└──────────────────┘  └──────┬──────────┘  └─────────────────┘
                             │
                   ┌─────────┴──────────┐
                   │                    │
          ┌────────▼─────────┐  ┌──────▼─────────┐
          │ GdfxImageSource  │  │StfsPackage     │
          │   (.iso/.xgd)    │  │   Source       │
          └──────────────────┘  └────────────────┘
```

## Components

### 1. FilesystemService (`launcher/src/services/filesystem_service.hpp`)

**Purpose**: Provides launcher-level VFS management and bridges the Qt world with Xenon's C++ VFS.

**Key Features**:
- VFS instance ownership and lifecycle
- Mount/unmount operations for host paths, GDFX images, and STFS packages
- Symbolic link management (e.g., `game:` → `\\Device\\Cdrom0`)
- Working directory configuration
- Path testing and diagnostics
- Qt signals for UI reactivity

**API**:
```cpp
// VFS management
std::shared_ptr<xenon::filesystem::VirtualFileSystem> vfs();
void resetVfs();

// Querying
QVariantList getMounts() const;
QVariantList getSymbolicLinks() const;
QString getWorkingDirectory() const;
QVariantMap getFilesystemStatus() const;

// Operations
ServiceResult mountHostPath(mount_point, host_path, read_only);
ServiceResult mountGdfxImage(mount_point, image_path);
ServiceResult mountStfsPackage(mount_point, package_path);
ServiceResult unmount(mount_point);
ServiceResult registerSymbolicLink(alias, target);
ServiceResult setWorkingDirectory(guest_path);
ServiceResult testPath(guest_path);
```

### 2. FilesystemFeature (`launcher/src/frontend_backend/filesystem/filesystem_feature.hpp`)

**Purpose**: Qt/QML-facing facade that exposes filesystem operations to the frontend.

**Integration**: Instantiated in `FrontendBackend` with references to `PathService` and `FilesystemService`.

**Exposed to QML**:
- All mount/unmount operations
- Symbolic link management
- VFS status queries
- Path testing

### 3. FilesystemPage.qml (`launcher/qml/FilesystemPage.qml`)

**Purpose**: User interface for VFS configuration and diagnostics.

**Features**:
- **Status Display**: Shows VFS initialization, mount count, symbolic links, working directory
- **Mounted Devices List**: Interactive list with unmount buttons
- **Symbolic Links List**: Shows aliases with removal options
- **Quick Actions**: 
  - Mount Host Folder dialog
  - Mount GDFX Image dialog
  - Test VFS Path tool
- **Real-time Updates**: Reactive to VFS changes

### 4. LauncherCore Integration

The filesystem service is initialized in `LauncherCore`:

```cpp
// In launcher_core.hpp
FilesystemService filesystem_;

// In launcher_core.cpp constructor
filesystem_(paths_, nullptr)
```

This ensures the VFS is available throughout the launcher's lifecycle.

## Usage Patterns

### Automatic Mount at Game Launch

When launching a game, the launch service should:

1. Reset the VFS for a clean session
2. Mount the game content directory/image
3. Register standard symbolic links (game:, d:, etc.)
4. Set the working directory to `game:`
5. Mount any DLC folders

Example:
```cpp
// In launch_service.cpp
filesystem_.resetVfs();

// Mount extracted game
filesystem_.mountHostPath("game:", game_content_path, true);

// Or mount GDFX image
filesystem_.mountGdfxImage("game:", image_path);

// Register symbolic links
filesystem_.registerSymbolicLink("d:", "game:");

// Set working directory
filesystem_.setWorkingDirectory("game:");

// Mount DLC
for (const auto& dlc : installed_dlc) {
    filesystem_.mountHostPath("dlc" + dlc.slot + ":", dlc.path, true);
}
```

### Manual Testing (FilesystemPage.qml)

Users can manually configure mounts for testing:

1. Open Settings → Filesystem page
2. Click "Mount Host Folder"
3. Enter mount point (e.g., `game:`) and select folder
4. Use "Test Path" to verify resolution

### Runtime Access (Future)

When the runtime core is enhanced:

```cpp
// In runtime_bridge.cpp
auto vfs = core_.filesystem().vfs();
// Pass VFS to Xenon Runtime for guest file I/O
runtime_->setVirtualFileSystem(vfs);
```

## Device Types

### HostPathDevice
- **Use**: Extracted games, DLC folders, save directories
- **Features**: Read/write support, case-insensitive lookup
- **Security**: Sandboxed (no `..` traversal, symlink escape detection)

### ReadOnlyContentDevice + GdfxImageSource
- **Use**: Xbox 360 disc images (.iso, .xgd)
- **Features**: Direct GDFX parsing, 2 KiB sector addressing
- **Validation**: Signature checks, cycle detection, bounds verification

### ReadOnlyContentDevice + StfsPackageSource
- **Use**: DLC packages (CON, LIVE, PIRS)
- **Features**: STFS volume parsing, 4 KiB blocks, hash table translation
- **Validation**: Package structure validation (no cryptographic verification)

### NullDevice
- **Use**: Intentionally empty optional devices
- **Features**: Always returns NotFound, zero capacity

## Comparison with Xenia

### Similarities
- VFS/device abstraction layer
- GDFX and STFS support
- Symbolic link system for Xbox paths
- Host path sandboxing

### Xenon Enhancements
- **Launcher Integration**: First-class launcher service with Qt binding
- **UI Management**: Full mount management UI in launcher
- **Service Architecture**: Clean separation between VFS, service, and frontend layers
- **Diagnostics**: Built-in path testing and VFS status display
- **Lifecycle**: Explicit VFS reset per session
- **Content Probe**: Integrated XEX/GDFX/STFS identification

## Future Enhancements

### Generation 12+ (Planned)
1. **Runtime VFS Sharing**: Pass VFS to Xenon Runtime for guest NtCreateFile/NtReadFile
2. **Live Mount Updates**: Hot-mount DLC while game is running
3. **Save Game Devices**: Dedicated save container devices with profile isolation
4. **SVOD Support**: Multi-fragment SVOD container provider
5. **Network Devices**: SMB/WebDAV remote content mounting
6. **Compression**: On-the-fly ZLIB/LZX decompression for compressed content

### UI Enhancements
1. Mount presets (common configurations)
2. Mount history
3. Device capacity visualization
4. File browser for mounted devices
5. Mount verification (scan for required files)

## Testing

### Manual Testing Procedure

1. **VFS Initialization**:
   - Launch Xenon Launcher
   - Navigate to Settings → Filesystem
   - Verify "VFS Initialized: Yes"

2. **Host Path Mount**:
   - Click "Mount Host Folder"
   - Mount point: `test:`
   - Select any folder
   - Verify mount appears in list

3. **GDFX Mount**:
   - Click "Mount GDFX Image"
   - Mount point: `game:`
   - Select .iso file
   - Verify mount appears

4. **Path Testing**:
   - Click "Test Path"
   - Enter `test:\` or `game:\default.xex`
   - Verify resolution

5. **Symbolic Links**:
   - Use QML console or future UI
   - Register `d:` → `game:`
   - Verify link appears

6. **Unmount**:
   - Click unmount button
   - Verify mount removed

### Automated Testing

Unit tests exist in `tests/filesystem/` for:
- VFS path resolution
- Device mounting
- Symbolic link expansion
- GDFX parsing
- STFS parsing
- Directory cursors

## Security Considerations

1. **Path Traversal**: HostPathDevice rejects `..` and validates canonical paths
2. **Symlink Escapes**: Detected and blocked on all platforms
3. **Read-Only Enforcement**: Device-level read-only policy (not OS-dependent)
4. **Bounds Checking**: All GDFX/STFS reads validate offsets before accessing
5. **Cycle Detection**: GDFX directory trees checked for cycles
6. **Resource Limits**: Directory query limits prevent DoS

## References

- `docs/filesystem/FILESYSTEM_V1.md` - Core VFS design
- `include/xenon/filesystem/` - VFS API headers
- `src/filesystem/` - VFS implementation
- Xenia VFS: `src/xenia/vfs/` in github.com/xenia-project/xenia
- ReXGlue VFS: Used by AC6_recomp and other static recompilation projects

## Generation History

- **Gen 1-3**: Core VFS, devices, content sources
- **Gen 4**: XEX metadata parsing, content probe
- **Gen 5**: GDFX disc image support
- **Gen 6**: STFS package support, materialization
- **Gen 7-10**: Kernel I/O bridge (separate from launcher)
- **Gen 11**: Import dispatch for guest code
- **Gen 12 (This Integration)**: Full launcher/frontend integration
