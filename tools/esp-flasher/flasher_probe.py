"""Catching the board, reading back what is on it, and comparing it with a build.

Two jobs that belong together because the second is only meaningful with the
first: `connect_board` and `probe_board` get a description of what is actually
in flash, and `assess` puts that beside a `Firmware` and decides what needs
writing.

The connect loop looks up the port afresh on every attempt. A single esptool
connect holds one device handle and loses the board when it vanishes underneath
it, which is exactly what a boot-looping board does (`LL-06`).
"""

import gc
import re
import time
from datetime import datetime

from flasher_config import (
    CONNECT_TIMEOUT,
    Cancelled,
    DEFAULT_PARTITION_TABLE_OFFSET,
    PARTITION_TABLE_SIZE,
    detect_chip,
)
from flasher_bus import find_port
from flasher_esptool import esptool_word, first_line
from flasher_images import (
    describe_app,
    first_app_offset,
    human_size,
    is_blank,
    parse_app_desc,
    parse_image_header,
    parse_partition_table,
)


# =========================

def connect_board(key, log, timeout=CONNECT_TIMEOUT, cancel=None):
    """Keep trying to put the board in download mode until it answers.

    A single esptool connect with its own retries fails on a boot-looping board,
    because the port vanishes under it mid-retry. Here every attempt starts from
    a fresh port lookup, so an attempt that lands in the ~2 s the board is on
    the bus wins, and once the ROM is in download mode the resets stop.
    """
    deadline = time.monotonic() + timeout
    attempt = 0
    waiting_logged = False
    while time.monotonic() < deadline:
        if cancel is not None and cancel.is_set():
            raise Cancelled()
        port = find_port(key)
        if port is None:
            if not waiting_logged:
                log("Waiting for the board to appear on the bus...\n")
                waiting_logged = True
            time.sleep(0.05)
            continue
        attempt += 1
        error = None
        try:
            return detect_chip(port["device"], 115200, esptool_word("default-reset"), False, 1)
        except Exception as exc:
            error = first_line(exc)
        # A failed connect can leave its serial handle to the garbage collector;
        # collect now so the next attempt can open the port.
        gc.collect()
        log("  attempt {} on {}: {}\n".format(attempt, port["device"], error))
        time.sleep(0.15)
    raise TimeoutError(
        "Could not catch the board in download mode within {:.0f}s. Put it there by hand: hold BOOT, "
        "press and release RESET, release BOOT, then try again.".format(timeout))


def probe_board(key, log, partition_table_offset=DEFAULT_PARTITION_TABLE_OFFSET,
                reset_after=None, cancel=None):
    """Connect, identify the chip, and read back the bootloader, partition table and app header.

    Returns the raw readings; `assess()` compares them with a firmware. With
    `reset_after=None` the board is reset back into its app only if it has one,
    and a blank board is left in download mode, where it stays put on the bus.
    """
    esp = connect_board(key, log, cancel=cancel)
    port = find_port(key)
    result = {"key": key, "device": port["device"] if port else key}
    try:
        result["chip"] = esp.get_chip_description()
        try:
            result["features"] = list(esp.get_chip_features())
        except Exception:
            result["features"] = []
        try:
            mac = esp.read_mac("BASE_MAC")
        except TypeError:
            mac = esp.read_mac()
        result["mac"] = ":".join("{:02x}".format(b) for b in mac)

        esp = esp.run_stub()
        size_code = (esp.flash_id() >> 16) & 0xFF
        result["flash_bytes"] = (1 << size_code) if 0x10 <= size_code <= 0x1F else None

        span = partition_table_offset + PARTITION_TABLE_SIZE
        log("Reading 0x0-0x{:x} (bootloader and partition table)...\n".format(span))
        head = esp.read_flash(0, span)
        result["head"] = head
        result["partition_table_offset"] = partition_table_offset

        entries = parse_partition_table(head[partition_table_offset:])
        app_offset = first_app_offset(entries)
        result["app_offset"] = app_offset
        result["app_head"] = esp.read_flash(app_offset, 0x200) if app_offset is not None else b""

        has_app = parse_app_desc(result["app_head"]) is not None
        do_reset = has_app if reset_after is None else reset_after
        if do_reset:
            log("Resetting the board back into its app.\n")
            esp.hard_reset()
        else:
            log("Leaving the board in download mode.\n")
        result["left_in_download"] = not do_reset
    finally:
        try:
            esp._port.close()
        except Exception:
            pass
    result["time"] = datetime.now().strftime("%H:%M:%S")
    return result


def assess(probe, firmware):
    """Compare a probe with a firmware: what is on the board, and what to write.

    Returns {"lines": [...], "mode": "full"|"app", "reasons": [...], "blockers": [...]}.
    """
    lines, reasons, blockers = [], [], []
    head = probe["head"]
    pt_offset = probe["partition_table_offset"]

    lines.append("{}  MAC {}  flash {}".format(
        probe["chip"], probe["mac"],
        human_size(probe["flash_bytes"]) if probe["flash_bytes"] else "size unknown"))
    if probe["features"]:
        lines.append("Features: " + ", ".join(probe["features"]))

    boot = head[:pt_offset]
    boot_header = parse_image_header(boot)
    if is_blank(boot[:64]):
        boot_state = "blank"
    elif boot_header:
        boot_state = "present ({} image)".format(boot_header["chip"])
    else:
        boot_state = "not a valid image"

    pt = head[pt_offset:pt_offset + PARTITION_TABLE_SIZE]
    entries = parse_partition_table(pt)
    if is_blank(pt[:64]):
        pt_state = "blank"
    elif entries:
        pt_state = "present: " + ", ".join("{}@0x{:x}".format(e["label"], e["offset"]) for e in entries)
    else:
        pt_state = "not a valid table"

    app_desc = parse_app_desc(probe["app_head"])
    if app_desc:
        app_state = describe_app(app_desc)
    elif probe["app_offset"] is None:
        app_state = "no app partition to look in"
    elif is_blank(probe["app_head"][:64]):
        app_state = "blank"
    else:
        app_state = "not a valid app image"

    fw_boot = firmware.region("bootloader")[1] if firmware else None
    fw_pt = firmware.region("partition-table")[1] if firmware else None
    fw_desc = firmware.app_desc if firmware else None

    boot_matches = bool(fw_boot) and boot[:len(fw_boot)] == fw_boot
    pt_matches = bool(fw_pt) and pt[:len(fw_pt)] == fw_pt
    app_matches = bool(fw_desc and app_desc) and fw_desc["elf_sha256"] == app_desc["elf_sha256"]

    def tag(matches):
        return "  = this build" if firmware and matches else ("  != this build" if firmware else "")

    lines.append("Bootloader: {}{}".format(boot_state, tag(boot_matches) if boot_header else ""))
    lines.append("Partition table: {}{}".format(pt_state, tag(pt_matches) if entries else ""))
    lines.append("App: {}{}".format(app_state, tag(app_matches) if app_desc else ""))
    if probe.get("left_in_download"):
        lines.append("Board left in download mode (it will not run until flashed or reset).")

    if firmware is None:
        return {"lines": lines, "mode": "full", "reasons": ["no firmware selected"], "blockers": ["no firmware selected"]}

    image_chip = firmware.image_chip
    board_chip = probe["chip"].split(" ")[0]
    if image_chip and image_chip.lower() != board_chip.lower():
        blockers.append("the build is for {}, the board is {}".format(image_chip, board_chip))
    if probe["flash_bytes"] and firmware.required_bytes > probe["flash_bytes"]:
        blockers.append("the images need {} of flash, the board has {}".format(
            human_size(firmware.required_bytes), human_size(probe["flash_bytes"])))
    settings = firmware.flash_settings or {}
    size_text = settings.get("flash_size", "")
    if probe["flash_bytes"] and re.fullmatch(r"\d+MB", size_text) and \
            int(size_text[:-2]) << 20 > probe["flash_bytes"]:
        blockers.append("the build is configured for {} flash, the board has {}".format(
            size_text, human_size(probe["flash_bytes"])))

    if firmware.image("merged"):
        mode = "full"
        reasons.append("a merged image is always written whole from 0x0")
    elif not firmware.has_bootloader:
        mode = "app"
        if not boot_header or not entries:
            blockers.append("the board has no bootloader or partition table and this image carries "
                            "neither; pick a build directory instead")
        else:
            reasons.append("the image is an app on its own")
    elif not boot_header:
        mode = "full"
        reasons.append("the board has no bootloader")
    elif not pt_matches:
        mode = "full"
        reasons.append("the partition table on the board differs from the build's")
        if entries:
            reasons.append("consider erasing: data partitions such as NVS may not line up with the new table")
    elif not boot_matches:
        mode = "full"
        reasons.append("the bootloader on the board differs from the build's")
    else:
        mode = "app"
        reasons.append("bootloader and partition table already match the build; only the app needs writing")
    if app_matches:
        reasons.append("the board already runs this exact app build")
    return {"lines": lines, "mode": mode, "reasons": reasons, "blockers": blockers}
