"""Reading an ESP-IDF image and a partition table out of raw bytes.

Pure parsing: hand it what was read back off the chip or off a .bin on disk and
it says what the thing claims to be. No serial port, no esptool, no UI, which
is what lets the probe and the build comparison share exactly one reading of
the format.
"""

import struct

from flasher_config import (
    APP_DESC_MAGIC,
    APP_DESC_OFFSET,
    CHIP_IDS,
    IMAGE_MAGIC,
    PARTITION_ENTRY_MAGIC,
    PARTITION_MD5_MAGIC,
)


# =========================

def cstr(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="replace")


def is_blank(data):
    return bool(data) and all(b == 0xFF for b in data)


def parse_image_header(data):
    """The ESP-IDF image header, or None if `data` does not start with one."""
    if len(data) < 24 or data[0] != IMAGE_MAGIC:
        return None
    chip_id = struct.unpack_from("<H", data, 12)[0]
    return {
        "segments": data[1],
        "chip_id": chip_id,
        "chip": CHIP_IDS.get(chip_id, "chip id {}".format(chip_id)),
    }


def parse_app_desc(data):
    """esp_app_desc_t from the start of an app image, or None."""
    base = APP_DESC_OFFSET
    if len(data) < base + 176:
        return None
    if struct.unpack_from("<I", data, base)[0] != APP_DESC_MAGIC:
        return None
    return {
        "version": cstr(data[base + 16:base + 48]),
        "project": cstr(data[base + 48:base + 80]),
        "time": cstr(data[base + 80:base + 96]),
        "date": cstr(data[base + 96:base + 112]),
        "idf": cstr(data[base + 112:base + 144]),
        "elf_sha256": data[base + 144:base + 176].hex(),
    }


def describe_app(desc):
    return "{} {} (built {} {}, IDF {})".format(
        desc["project"], desc["version"], desc["date"], desc["time"], desc["idf"])


def parse_partition_table(data):
    """Entries of a binary partition table, stopping at the first non-entry."""
    entries = []
    for pos in range(0, len(data) - 31, 32):
        entry = data[pos:pos + 32]
        if entry[:2] == PARTITION_MD5_MAGIC:
            continue
        if entry[:2] != PARTITION_ENTRY_MAGIC:
            break
        ptype, subtype, offset, size = struct.unpack_from("<BBII", entry, 2)
        entries.append({"label": cstr(entry[12:28]), "type": ptype, "subtype": subtype,
                        "offset": offset, "size": size})
    return entries


def first_app_offset(entries):
    """Where the bootloader will look for an app: factory first, else the first app slot."""
    apps = [e for e in entries if e["type"] == 0]
    for entry in apps:
        if entry["subtype"] == 0x00:
            return entry["offset"]
    return apps[0]["offset"] if apps else None


def human_size(n):
    if n >= 1 << 20 and n % (1 << 20) == 0:
        return "{}MB".format(n >> 20)
    if n >= 1 << 10:
        return "{:.1f}KB".format(n / 1024)
    return "{}B".format(n)
