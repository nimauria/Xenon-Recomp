import QtQuick

// Shared aspect-preserving image fitter for real content artwork (cover art,
// backgrounds, avatars). The decoded image size is deliberately decoupled
// from every live geometry tick: window resizing should scale an already
// decoded texture on the GPU and only request a better-sized decode after the
// geometry has settled. This avoids repeated image re-decodes while dragging
// a window, scrolling a grid, or animating a card.
Item {
    id: root

    property url source: ""
    property string fitMode: "cover"
    property real focalX: 0.5
    property real focalY: 0.5
    property real zoom: 1.0
    property bool asynchronous: true
    property bool smooth: true

    // Normal artwork needs only modest decode headroom. Interactive crop
    // editors opt into a larger value so zooming remains sharp without
    // changing sourceSize on every zoom tick.
    property real decodeHeadroom: 2.25
    property int decodeQuantum: 128
    property int decodeResizeDelay: 120
    // Hard cap prevents a maximized/4K launcher window from requesting huge
    // multi-thousand-pixel textures for every artwork surface. Crop editors
    // can raise this locally when they genuinely need more detail.
    property int maximumDecodeDimension: 3072
    property int decodeWidth: 64
    property int decodeHeight: 64
    property url lastReadySource: ""

    readonly property bool imageReady: image.status === Image.Ready
    readonly property size sourceSize: image.sourceSize
    readonly property real renderedWidth: image.width
    readonly property real renderedHeight: image.height

    function quantizedDecodeDimension(value) {
        var quantum = Math.max(32, root.decodeQuantum)
        var quantized = Math.max(64, Math.ceil(Math.max(1, value) / quantum) * quantum)
        var cap = Math.max(64, root.maximumDecodeDimension)
        return Math.min(cap, quantized)
    }

    function updateDecodeSize() {
        var nextWidth = root.quantizedDecodeDimension(root.width * Math.max(1.0, root.decodeHeadroom))
        var nextHeight = root.quantizedDecodeDimension(root.height * Math.max(1.0, root.decodeHeadroom))
        // Avoid pointless provider/cache churn for tiny geometry changes.
        if (Math.abs(nextWidth - root.decodeWidth) >= Math.max(32, root.decodeQuantum / 2))
            root.decodeWidth = nextWidth
        if (Math.abs(nextHeight - root.decodeHeight) >= Math.max(32, root.decodeQuantum / 2))
            root.decodeHeight = nextHeight
    }

    function scheduleDecodeSizeUpdate() {
        if (!decodeResizeTimer.running)
            decodeResizeTimer.start()
        else
            decodeResizeTimer.restart()
    }

    Timer {
        id: decodeResizeTimer
        interval: Math.max(0, root.decodeResizeDelay)
        repeat: false
        onTriggered: root.updateDecodeSize()
    }

    // Keep the last successfully decoded frame underneath the next request.
    // This avoids the white/black flash that otherwise occurs when hero art
    // changes or an image provider briefly returns Loading.
    Image {
        id: retainedImage
        z: 0
        asynchronous: true
        smooth: root.smooth
        cache: true
        fillMode: Image.Pad
        sourceSize.width: root.decodeWidth
        sourceSize.height: root.decodeHeight
        visible: source.toString().length > 0 && opacity > 0
        opacity: image.status === Image.Ready ? 1.0 - image.opacity : 1.0
    }

    Image {
        id: image
        z: 1
        source: root.source
        asynchronous: root.asynchronous
        smooth: root.smooth
        cache: true
        fillMode: Image.Pad
        sourceSize.width: root.decodeWidth
        sourceSize.height: root.decodeHeight
        opacity: status === Image.Ready ? 1.0 : 0.0

        Behavior on opacity {
            NumberAnimation { duration: Theme.motionNormal; easing.type: Easing.OutCubic }
        }
    }

    function relayoutImage(target) {
        if (!target.sourceSize.width || !target.sourceSize.height || root.width <= 0 || root.height <= 0) {
            target.width = root.width
            target.height = root.height
            target.x = 0
            target.y = 0
            return
        }
        const naturalWidth = target.sourceSize.width
        const naturalHeight = target.sourceSize.height
        const coverScale = Math.max(root.width / naturalWidth, root.height / naturalHeight)
        const containScale = Math.min(root.width / naturalWidth, root.height / naturalHeight)
        const baseScale = root.fitMode === "contain" ? containScale : coverScale
        const scale = baseScale * Math.max(0.1, root.zoom)
        const w = naturalWidth * scale
        const h = naturalHeight * scale
        target.width = w
        target.height = h
        if (root.fitMode === "contain") {
            target.x = (root.width - w) / 2
            target.y = (root.height - h) / 2
        } else {
            const fx = Math.max(0, Math.min(1, root.focalX))
            const fy = Math.max(0, Math.min(1, root.focalY))
            // Below 1.0 zoom the source can become smaller than the crop
            // viewport. Centre that axis rather than pinning it to an edge;
            // this makes "zoom out to include the whole logo" predictable.
            target.x = w <= root.width
                ? (root.width - w) / 2
                : Math.min(0, Math.max(root.width - w, root.width / 2 - w * fx))
            target.y = h <= root.height
                ? (root.height - h) / 2
                : Math.min(0, Math.max(root.height - h, root.height / 2 - h * fy))
        }
    }

    function relayout() {
        root.relayoutImage(image)
        root.relayoutImage(retainedImage)
    }

    onWidthChanged: {
        relayout()
        scheduleDecodeSizeUpdate()
    }
    onHeightChanged: {
        relayout()
        scheduleDecodeSizeUpdate()
    }
    onSourceChanged: {
        if (root.lastReadySource.toString().length > 0
                && root.lastReadySource.toString() !== root.source.toString())
            retainedImage.source = root.lastReadySource
        else if (root.source.toString().length === 0)
            retainedImage.source = ""
        updateDecodeSize()
        relayout()
    }
    onDecodeHeadroomChanged: scheduleDecodeSizeUpdate()
    onMaximumDecodeDimensionChanged: scheduleDecodeSizeUpdate()
    onFitModeChanged: relayout()
    onFocalXChanged: relayout()
    onFocalYChanged: relayout()
    onZoomChanged: relayout()

    Connections {
        target: image
        function onStatusChanged() {
            if (image.status === Image.Ready) {
                root.lastReadySource = root.source
                root.relayout()
            }
        }
    }

    Connections {
        target: retainedImage
        function onStatusChanged() {
            if (retainedImage.status === Image.Ready) root.relayout()
        }
    }

    Component.onCompleted: {
        updateDecodeSize()
        relayout()
    }
}
