"""Construct legacy OBJS v2 test payloads from the current v3 writer."""
import struct


def legacy_stations(body):
    """Downgrade GSTN v2 to v1, discarding the added minimum-elevation field."""
    count, = struct.unpack_from('<I', body)
    output = bytearray(body[:4])
    offset = 4
    for _ in range(count):
        begin = offset
        for _ in range(2):
            length, = struct.unpack_from('<I', body, offset)
            offset += 4 + length
        offset += 24
        output.extend(body[begin:offset])
        offset += 8
    assert offset == len(body)
    return bytes(output)


def legacy_objects(body):
    count, = struct.unpack_from('<I', body)
    output = bytearray(body[:4])
    offset = 4

    def skip_text():
        nonlocal offset
        size, = struct.unpack_from('<I', body, offset)
        offset += 4 + size
        return size

    for _ in range(count):
        begin = offset
        skip_text()  # Object ID.
        skip_text()  # Display name.
        offset += 103  # Fixed orbit, display, and model fields.
        skip_text()  # Model path.
        output.extend(body[begin:offset])
        assert skip_text() == 0 and skip_text() == 0, 'Cannot downgrade TLE inputs'
    assert offset == len(body)
    return bytes(output)
