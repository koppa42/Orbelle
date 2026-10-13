"""Edit current project containers for integrity tests, repairing both CRC layers."""
import struct
import zlib


def read_sections(raw):
    sections, offset = [], 24
    while offset < len(raw):
        tag, version, size, _ = struct.unpack_from("<4sIQI", raw, offset)
        body = raw[offset + 20:offset + 20 + size]
        sections.append((tag, version, body))
        offset += 20 + size
    assert offset == len(raw)
    return sections


def with_sections(raw, sections):
    payload = b"".join(
        struct.pack("<4sIQI", tag, version, len(body), zlib.crc32(body)) + body
        for tag, version, body in sections
    )
    return raw[:12] + struct.pack("<QI", len(payload), zlib.crc32(payload)) + payload


def rewrite_section(raw, tag_to_change, mutate):
    sections = []
    for tag, version, body in read_sections(raw):
        if tag == tag_to_change:
            body = mutate(body)
        if body is not None:
            sections.append((tag, version, body))
    return with_sections(raw, sections)
