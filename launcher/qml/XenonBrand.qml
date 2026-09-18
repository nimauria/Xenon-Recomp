import QtQuick

Image {
    id: root

    property string asset: "mark" // mark, icon, wordmark, lockup, lockup-full, profile-mark
    property color brandColor: Theme.accent

    source: launcherBridge.themedBrandingDataUrl(asset, String(brandColor))
    fillMode: Image.PreserveAspectFit
    smooth: true
    asynchronous: false
    cache: true
    mipmap: true
}
