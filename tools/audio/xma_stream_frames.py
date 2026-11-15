"""Count the frames in a raw XMA2 packet stream, parsed straight through.

    python xma_stream_frames.py <stream.xma> [<engine.pcm> <channels>] [--frames <out>]

<stream.xma> is a ctx<N>_s<serial>.xma from Xenon's XMA stream dump
(XENON_XMA_DUMP): 2048-byte packets in the order the title gave them. Each
packet starts with a 32-bit header - frame count (6 bits), the bit offset of
the first frame that starts in it (15), metadata (3), packets to skip after
it (8) - and the frames run as one bit stream across packets: a frame is a
15-bit length (in bits, length included) followed by its data, and its last
bit says whether another frame follows in the same packet. Lengths of 0 or
0x7FFF are padding to the end of the packet.

This walks the stream from its first packet to its last in one pass, with
none of the engine's buffer swapping or resuming, and prints the frame count
and the duration it holds (512 samples per frame). Given the engine's .pcm
for the same stream, it also prints how many samples the engine produced:
fewer means frames were dropped, more that some were decoded twice.

--start-packet <n> starts the walk at packet n instead of 0: the first
packet of the second (third...) stream of a multi-stream file, whose packets
are interleaved with the others' and reached through each header's skip.

--frames <out> writes every frame, in stream order, for
xenon_xma_frames_decode: per frame a little-endian u32 length in bits, then
the frame's bits packed from bit 0 of its first byte.
"""

import sys

PACKET = 2048
PACKET_BITS = PACKET * 8


def bits(data, start, count):
    value = 0
    for i in range(count):
        byte = data[(start + i) >> 3]
        value = (value << 1) | ((byte >> (7 - ((start + i) & 7))) & 1)
    return value


def header(data, packet):
    word = int.from_bytes(data[packet * PACKET : packet * PACKET + 4], "big")
    return {
        "frames": word >> 26,
        "first_frame": (word >> 11) & 0x7FFF,
        "metadata": (word >> 8) & 7,
        "skip": word & 0xFF,
    }


def stream_bits(data, packet, bit, count):
    """Reads `count` stream bits from (packet, bit within packet), crossing
    packet headers and skips; returns (value, packet, bit) after them."""
    value = 0
    packets = len(data) // PACKET
    for _ in range(count):
        if bit >= PACKET_BITS:
            packet += header(data, packet)["skip"] + 1
            bit = 32
        if packet >= packets:
            return None, packet, bit
        value = (value << 1) | bits(data, packet * PACKET_BITS + bit, 1)
        bit += 1
    return value, packet, bit


def walk(data, start_packet=0):
    packets = len(data) // PACKET
    frames = []
    packet = start_packet
    while packet < packets:
        h = header(data, packet)
        if h["first_frame"] < PACKET_BITS - 32:
            break
        packet += h["skip"] + 1
    else:
        return frames
    bit = 32 + header(data, packet)["first_frame"]
    while packet < packets:
        start = (packet, bit)
        length, p, b = stream_bits(data, packet, bit, 15)
        if length is None:
            break
        if length == 0 or length == 0x7FFF:
            # Padding: the stream resumes at the next packet's first frame.
            packet += header(data, packet)["skip"] + 1
            while packet < packets and header(data, packet)["first_frame"] >= PACKET_BITS - 32:
                packet += header(data, packet)["skip"] + 1
            if packet >= packets:
                break
            bit = 32 + header(data, packet)["first_frame"]
            continue
        rest, p, b = stream_bits(data, p, b, length - 15)
        if rest is None:
            break
        frames.append((start, length))
        packet, bit = p, b
        follows = rest & 1
        if not follows:
            # The last frame that starts in its packet. If it ran on into
            # the stream's next packet, the next frame starts in that packet,
            # after the run-on (its header's first-frame offset); otherwise
            # in the packet after the start packet and its skip.
            if packet == start[0] and bit < PACKET_BITS:
                packet = start[0] + header(data, start[0])["skip"] + 1
            elif bit >= PACKET_BITS:
                packet += header(data, packet)["skip"] + 1
            while packet < packets and header(data, packet)["first_frame"] >= PACKET_BITS - 32:
                packet += header(data, packet)["skip"] + 1
            if packet >= packets:
                break
            bit = 32 + header(data, packet)["first_frame"]
    return frames


def frame_bytes(data, start, length):
    packet, bit = start
    out = bytearray((length + 7) // 8)
    for i in range(length):
        value, packet, bit = stream_bits(data, packet, bit, 1)
        if value:
            out[i >> 3] |= 0x80 >> (i & 7)
    return bytes(out)


def main():
    args = sys.argv[1:]
    frames_out = None
    start_packet = 0
    if "--start-packet" in args:
        i = args.index("--start-packet")
        start_packet = int(args[i + 1])
        del args[i : i + 2]
    if "--frames" in args:
        i = args.index("--frames")
        frames_out = args[i + 1]
        del args[i : i + 2]
    if len(args) not in (1, 3):
        print(__doc__)
        return 2
    sys.argv = [sys.argv[0]] + args
    data = open(sys.argv[1], "rb").read()
    frames = walk(data, start_packet)
    if frames_out:
        with open(frames_out, "wb") as out:
            for start, length in frames:
                out.write(length.to_bytes(4, "little"))
                out.write(frame_bytes(data, start, length))
    print("%d packets, %d frames = %d samples per channel"
          % (len(data) // PACKET, len(frames), len(frames) * 512))
    if len(sys.argv) == 4:
        channels = int(sys.argv[3])
        pcm = open(sys.argv[2], "rb").read()
        samples = len(pcm) // (2 * channels)
        print("engine output %d samples per channel = %.2f frames (%+.1f%% of the stream)"
              % (samples, samples / 512, 100.0 * (samples - len(frames) * 512) / max(1, len(frames) * 512)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
