"""Which reference frame each 512-sample frame of the engine's output is.

    python xma_frame_order.py <engine.pcm> <reference.pcm> <channels>

<engine.pcm> is an XMA stream dump's .pcm (XENON_XMA_DUMP); <reference.pcm>
is xenon_xma_frames_decode's output for the same stream: every frame decoded
once, in stream order. Both are 16-bit big-endian interleaved.

The engine's output may be shifted against the reference by a constant
delay, so the shift is found first (the one that best matches the start).
Then each engine frame is matched to the reference frame it equals, and the
sequence is printed compressed into runs: a correct engine is one run, 0..N.
A run that jumps ahead skipped frames; one that goes back repeated them.
"""

import struct
import sys

FRAME = 512


def load(path, channels):
    data = open(path, "rb").read()
    n = len(data) // 2
    return struct.unpack(">%dh" % n, data[: n * 2])[::channels]


def distance(a, b):
    n = min(len(a), len(b))
    if n == 0:
        return float("inf")
    return sum(abs(x - y) for x, y in zip(a[:n], b[:n])) / (sum(abs(y) for y in b[:n]) + n)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    channels = int(sys.argv[3])
    engine = load(sys.argv[1], channels)
    reference = load(sys.argv[2], channels)
    # The shift: engine sample i = reference sample i + shift.
    probe_start = FRAME * 4
    probe = engine[probe_start : probe_start + FRAME * 4]
    best = min(range(-FRAME * 2, FRAME * 2 + 1, 1),
               key=lambda shift: distance(probe, reference[probe_start + shift : probe_start + shift + len(probe)])
               if probe_start + shift >= 0 else float("inf"))
    print("engine %d frames, reference %d frames; engine = reference shifted by %+d samples"
          % (len(engine) // FRAME, len(reference) // FRAME, best))
    sequence = []
    unmatched = 0
    for e in range(len(engine) // FRAME):
        chunk = engine[e * FRAME : (e + 1) * FRAME]
        if sum(abs(v) for v in chunk) < FRAME * 4:
            sequence.append(None)  # silence matches anything
            continue
        guess = e + best / FRAME
        candidates = range(max(0, int(guess) - 64), min(len(reference) // FRAME, int(guess) + 64))
        scored = [(distance(chunk, reference[r * FRAME + best : r * FRAME + best + FRAME]), r)
                  for r in candidates if r * FRAME + best >= 0]
        score, r = min(scored) if scored else (float("inf"), -1)
        if score > 0.05:
            unmatched += 1
            sequence.append(-1)
        else:
            sequence.append(r)
    # Compress into runs.
    runs = []
    for e, r in enumerate(sequence):
        if r is None:
            continue
        if runs and r != -1 and runs[-1][2] != -1 and r == runs[-1][2] + (e - runs[-1][3]):
            runs[-1][2] = r
            runs[-1][3] = e
        else:
            runs.append([e, r, r, e])
    print("%d run(s), %d unmatched frame(s)" % (len(runs), unmatched))
    for first_e, first_r, last_r, last_e in runs[:60]:
        label = "unmatched" if first_r == -1 else "reference %d..%d" % (first_r, last_r)
        print("  engine %4d..%-4d = %s" % (first_e, last_e, label))
    return 0


if __name__ == "__main__":
    sys.exit(main())
