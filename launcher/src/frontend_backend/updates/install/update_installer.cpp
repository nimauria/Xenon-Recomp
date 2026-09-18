#include "update_installer.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QUuid>

namespace xenon::launcher::frontend_backend {
namespace {

QString updateTempRoot() {
  auto root = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  if (root.trimmed().isEmpty()) root = QDir::tempPath();
  root = QDir{root}.filePath(QStringLiteral("Project-Xenon-Updater"));
  QDir{}.mkpath(root);
  return root;
}

QString installLogPath() {
  auto root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (root.trimmed().isEmpty()) root = QDir::tempPath();
  QDir{}.mkpath(root);
  return QDir{root}.filePath(QStringLiteral("update-install.log"));
}

QString updaterScript() {
  return QString::fromUtf8(R"PS1(param(
    [Parameter(Mandatory=$true)][string]$PackagePath,
    [Parameter(Mandatory=$true)][string]$InstallDir,
    [Parameter(Mandatory=$true)][string]$LauncherName,
    [Parameter(Mandatory=$true)][int]$ParentPid,
    [Parameter(Mandatory=$true)][string]$LogPath,
    [Parameter(Mandatory=$true)][string]$ExpectedVersion
)
$ErrorActionPreference = 'Stop'

function Write-UpdateLog([string]$Message) {
    $stamp = (Get-Date).ToString('o')
    Add-Content -LiteralPath $LogPath -Value "$stamp  $Message"
}

$extractRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("XenonUpdate-" + [guid]::NewGuid().ToString('N'))
$backupDir = $InstallDir + '.xenon-backup'
$newInstallDir = $InstallDir + '.xenon-new'
$oldInstallMoved = $false

try {
    Write-UpdateLog "Preparing Xenon Launcher update $ExpectedVersion"
    try { Wait-Process -Id $ParentPid -ErrorAction SilentlyContinue } catch {}

    if (-not (Test-Path -LiteralPath $PackagePath -PathType Leaf)) {
        throw "Update package was not found: $PackagePath"
    }

    New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null
    Expand-Archive -LiteralPath $PackagePath -DestinationPath $extractRoot -Force

    $payloadRoot = $extractRoot
    if (-not (Test-Path -LiteralPath (Join-Path $payloadRoot $LauncherName) -PathType Leaf)) {
        $directories = @(Get-ChildItem -LiteralPath $extractRoot -Directory)
        if ($directories.Count -eq 1 -and
            (Test-Path -LiteralPath (Join-Path $directories[0].FullName $LauncherName) -PathType Leaf)) {
            $payloadRoot = $directories[0].FullName
        }
    }

    $payloadExe = Join-Path $payloadRoot $LauncherName
    if (-not (Test-Path -LiteralPath $payloadExe -PathType Leaf)) {
        throw "Update payload does not contain $LauncherName"
    }

    if (Test-Path -LiteralPath $backupDir) {
        Remove-Item -LiteralPath $backupDir -Recurse -Force
    }
    if (Test-Path -LiteralPath $newInstallDir) {
        Remove-Item -LiteralPath $newInstallDir -Recurse -Force
    }

    Move-Item -LiteralPath $payloadRoot -Destination $newInstallDir
    Move-Item -LiteralPath $InstallDir -Destination $backupDir
    $oldInstallMoved = $true
    Move-Item -LiteralPath $newInstallDir -Destination $InstallDir

    $newExe = Join-Path $InstallDir $LauncherName
    Write-UpdateLog "Installed update payload; restarting launcher"
    $newProcess = Start-Process -FilePath $newExe -WorkingDirectory $InstallDir -PassThru
    Start-Sleep -Seconds 3

    if ($newProcess.HasExited) {
        throw "Updated launcher exited during its startup verification window with code $($newProcess.ExitCode)"
    }

    try {
        if (Test-Path -LiteralPath $backupDir) {
            Remove-Item -LiteralPath $backupDir -Recurse -Force
        }
    } catch {
        Write-UpdateLog ("Backup cleanup deferred: " + $_.Exception.Message)
    }
    try {
        if (Test-Path -LiteralPath $PackagePath) {
            Remove-Item -LiteralPath $PackagePath -Force
        }
        if (Test-Path -LiteralPath $extractRoot) {
            Remove-Item -LiteralPath $extractRoot -Recurse -Force
        }
    } catch {
        Write-UpdateLog ("Temporary-file cleanup deferred: " + $_.Exception.Message)
    }
    Write-UpdateLog "Xenon Launcher update completed successfully"
    exit 0
}
catch {
    Write-UpdateLog ("Update failed: " + $_.Exception.Message)
    try {
        if ($oldInstallMoved) {
            if (Test-Path -LiteralPath $InstallDir) {
                Remove-Item -LiteralPath $InstallDir -Recurse -Force
            }
            if (Test-Path -LiteralPath $backupDir) {
                Move-Item -LiteralPath $backupDir -Destination $InstallDir
            }
        }
    } catch {
        Write-UpdateLog ("Rollback failed: " + $_.Exception.Message)
    }
    try {
        if (Test-Path -LiteralPath $newInstallDir) {
            Remove-Item -LiteralPath $newInstallDir -Recurse -Force
        }
        if (Test-Path -LiteralPath $extractRoot) {
            Remove-Item -LiteralPath $extractRoot -Recurse -Force
        }
    } catch {}

    # If rollback restored the previous deployment, reopen it so a failed update
    # does not leave the user with a closed launcher and no visible recovery.
    try {
        $restoredExe = Join-Path $InstallDir $LauncherName
        if (Test-Path -LiteralPath $restoredExe -PathType Leaf) {
            Write-UpdateLog "Restarting the restored Xenon Launcher after rollback"
            Start-Process -FilePath $restoredExe -WorkingDirectory $InstallDir | Out-Null
        }
    } catch {
        Write-UpdateLog ("Restored launcher could not be restarted automatically: " + $_.Exception.Message)
    }
    exit 1
}
)PS1");
}

}  // namespace

bool UpdateInstaller::platformSupported() noexcept {
#if defined(Q_OS_WIN)
  return true;
#else
  return false;
#endif
}

ServiceResult UpdateInstaller::installAndRestart(const QString& package_path,
                                                 const QString& version) const {
#if !defined(Q_OS_WIN)
  Q_UNUSED(package_path);
  Q_UNUSED(version);
  return ServiceResult::failure(
      QStringLiteral("Automatic installation unavailable"),
      QStringLiteral("Automatic launcher replacement is currently implemented for Windows builds only."));
#else
  const QFileInfo package{package_path};
  if (!package.exists() || !package.isFile()) {
    return ServiceResult::failure(QStringLiteral("Update package missing"),
                                  QStringLiteral("The staged launcher package no longer exists."));
  }

  const auto application_file = QFileInfo{QCoreApplication::applicationFilePath()};
  const auto install_dir = application_file.absolutePath();
  if (install_dir.trimmed().isEmpty() || application_file.fileName().trimmed().isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Update installation unavailable"),
                                  QStringLiteral("Xenon could not determine the launcher installation directory."));
  }

  const auto script_path = QDir{updateTempRoot()}.filePath(
      QStringLiteral("apply-%1.ps1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  QFile script{script_path};
  if (!script.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    return ServiceResult::failure(QStringLiteral("Update installation unavailable"),
                                  QStringLiteral("Xenon could not create its temporary update helper script."));
  }
  QTextStream stream{&script};
  stream << updaterScript();
  stream.flush();
  script.close();

  const QStringList args{
      QStringLiteral("-NoProfile"),
      QStringLiteral("-NonInteractive"),
      QStringLiteral("-ExecutionPolicy"),
      QStringLiteral("Bypass"),
      QStringLiteral("-File"),
      QDir::toNativeSeparators(script_path),
      QStringLiteral("-PackagePath"),
      QDir::toNativeSeparators(package.absoluteFilePath()),
      QStringLiteral("-InstallDir"),
      QDir::toNativeSeparators(install_dir),
      QStringLiteral("-LauncherName"),
      application_file.fileName(),
      QStringLiteral("-ParentPid"),
      QString::number(QCoreApplication::applicationPid()),
      QStringLiteral("-LogPath"),
      QDir::toNativeSeparators(installLogPath()),
      QStringLiteral("-ExpectedVersion"),
      version};

  qint64 process_id = 0;
  if (!QProcess::startDetached(QStringLiteral("powershell.exe"), args, updateTempRoot(), &process_id)) {
    QFile::remove(script_path);
    return ServiceResult::failure(QStringLiteral("Update installation unavailable"),
                                  QStringLiteral("Xenon could not start the Windows update helper."));
  }

  return ServiceResult::success(
      QStringLiteral("Restarting to update"),
      QStringLiteral("The verified launcher package is ready. Xenon will close, replace the launcher files, and restart."),
      QVariantMap{{QStringLiteral("helperPid"), process_id},
                  {QStringLiteral("logPath"), installLogPath()}});
#endif
}

}  // namespace xenon::launcher::frontend_backend
