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

    Image {
        id: image
        source: root.source
        asynchronous: root.asynchronous
        smooth: root.smooth
        cache: true
        fillMode: Image.Pad
        sourceSize.width: root.decodeWidth
        sourceSize.height: root.decodeHeight
    }

    function relayout() {
        if (!image.sourceSize.width || !image.sourceSize.height || root.width <= 0 || root.height <= 0) {
            image.width = root.width
            image.height = root.height
            image.x = 0
            image.y = 0
            return
        }
        const naturalWidth = image.sourceSize.width
        const naturalHeight = image.sourceSize.height
        const coverScale = Math.max(root.width / naturalWidth, root.height / naturalHeight)
        const containScale = Math.min(root.width / naturalWidth, root.height / naturalHeight)
        const baseScale = root.fitMode === "contain" ? containScale : coverScale
        const scale = baseScale * Math.max(1.0, root.zoom)
        const w = naturalWidth * scale
        const h = naturalHeight * scale
        image.width = w
        image.height = h
        if (root.fitMode === "contain") {
            image.x = (root.width - w) / 2
            image.y = (root.height - h) / 2
        } else {
            const fx = Math.max(0, Math.min(1, root.focalX))
            const fy = Math.max(0, Math.min(1, root.focalY))
            image.x = Math.min(0, Math.max(root.width - w, root.width / 2 - w * fx))
            image.y = Math.min(0, Math.max(root.height - h, root.height / 2 - h * fy))
        }
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
            if (image.status === Image.Ready) root.relayout()
        }
    }

    Component.onCompleted: {
        updateDecodeSize()
        relayout()
    }
}
