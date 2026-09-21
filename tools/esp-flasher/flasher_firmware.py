"""A build on disk: where its three images are and what they say they are.

`Firmware` is the answer to "what would I be writing?", read from a build
directory's flasher_args.json and the images beside it. It is separate from the
probe so that the comparison in `flasher_probe.assess` has two independently
built descriptions to put side by side, one from the disk and one from the chip.
"""

import json
import os
from datetime import datetime
from pathlib import Path

from flasher_config import (
    DEFAULT_APP_OFFSET,
    DEFAULT_PARTITION_TABLE_OFFSET,
    PARTITION_ENTRY_MAGIC,
    PARTITION_TABLE_SIZE,
    REPO_ROOT,
)
from flasher_images import (
    describe_app,
    first_app_offset,
    human_size,
    parse_app_desc,
    parse_image_header,
    parse_partition_table,
)


# =========================

class Firmware:
    """The images to write and the flash settings they were built for.

    Loaded either from an ESP-IDF build directory (`flasher_args.json`, which
    names every image and its offset) or from a single .bin, which is either a
    merged image written at 0x0 or an app written at an offset you give.
    """

    def __init__(self, name, source, images, flash_settings=None, chip=None):
        self.name = name
        self.source = source
        self.images = images              # [{offset, path, role, data}], sorted by offset
        self.flash_settings = flash_settings
        self.chip = chip                  # esptool chip name, e.g. "esp32s3"

    # ---- loading ----

    @classmethod
    def from_build(cls, path):
        path = Path(path)
        args_file = path if path.is_file() else path / "flasher_args.json"
        if not args_file.is_file():
            raise ValueError("{} has no flasher_args.json; is it an ESP-IDF build directory? "
                             "Run idf.py build first.".format(path))
        build_dir = args_file.parent
        spec = json.loads(args_file.read_text(encoding="utf-8"))

        roles = {}
        for role in ("bootloader", "partition-table", "app"):
            entry = spec.get(role)
            if isinstance(entry, dict) and "offset" in entry:
                roles[int(entry["offset"], 0)] = role

        images = []
        for offset_text, rel in spec.get("flash_files", {}).items():
            offset = int(offset_text, 0)
            file_path = build_dir / rel
            if not file_path.is_file():
                raise ValueError("{} is listed in {} but missing; rebuild.".format(file_path, args_file))
            images.append({"offset": offset, "path": str(file_path),
                           "role": roles.get(offset, "other"), "data": file_path.read_bytes()})
        if not images:
            raise ValueError("{} lists no images to flash.".format(args_file))
        images.sort(key=lambda i: i["offset"])

        chip = spec.get("extra_esptool_args", {}).get("chip")
        return cls(build_dir.name, str(build_dir), images, spec.get("flash_settings"), chip)

    @classmethod
    def from_bin(cls, path, offset=None):
        path = Path(path)
        data = path.read_bytes()
        pt = data[DEFAULT_PARTITION_TABLE_OFFSET:DEFAULT_PARTITION_TABLE_OFFSET + 2]
        if parse_image_header(data) and pt == PARTITION_ENTRY_MAGIC:
            role, offset = "merged", 0
        else:
            role = "app"
            offset = DEFAULT_APP_OFFSET if offset is None else offset
        image = {"offset": offset, "path": str(path), "role": role, "data": data}
        return cls(path.name, str(path), [image])

    # ---- views of the images ----

    def image(self, role):
        for img in self.images:
            if img["role"] == role:
                return img
        return None

    def region(self, role):
        """(offset, bytes) of the bootloader, partition table or app, from any layout."""
        img = self.image(role)
        if img is not None:
            return img["offset"], img["data"]
        merged = self.image("merged")
        if merged is None:
            return None, None
        data = merged["data"]
        if role == "bootloader":
            return 0, data[:DEFAULT_PARTITION_TABLE_OFFSET]
        pt = data[DEFAULT_PARTITION_TABLE_OFFSET:DEFAULT_PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE]
        if role == "partition-table":
            return DEFAULT_PARTITION_TABLE_OFFSET, pt
        app_at = first_app_offset(parse_partition_table(pt))
        if role == "app" and app_at is not None and app_at < len(data):
            return app_at, data[app_at:]
        return None, None

    @property
    def partition_table_offset(self):
        offset, _ = self.region("partition-table")
        return DEFAULT_PARTITION_TABLE_OFFSET if offset is None else offset

    @property
    def app_desc(self):
        _, data = self.region("app")
        return parse_app_desc(data) if data else None

    @property
    def image_chip(self):
        for role in ("bootloader", "app"):
            _, data = self.region(role)
            header = parse_image_header(data or b"")
            if header:
                return header["chip"]
        return None

    @property
    def required_bytes(self):
        return max(img["offset"] + len(img["data"]) for img in self.images)

    @property
    def has_bootloader(self):
        return self.region("bootloader")[1] is not None

    def summary_lines(self):
        lines = []
        desc = self.app_desc
        lines.append("App: {}".format(describe_app(desc) if desc else "no app description found"))
        chip = self.image_chip or self.chip or "unknown chip"
        settings = self.flash_settings
        if settings:
            lines.append("Chip: {}   flash {} {} {}".format(
                chip, settings.get("flash_mode"), settings.get("flash_freq"), settings.get("flash_size")))
        else:
            lines.append("Chip: {}   flash settings: kept as the board has them".format(chip))
        lines.append("Images: " + ", ".join(
            "{} @ 0x{:x} ({})".format(img["role"], img["offset"], human_size(len(img["data"])))
            for img in self.images))
        return lines

    def label(self):
        desc = self.app_desc
        stamp = datetime.fromtimestamp(os.path.getmtime(self.images[-1]["path"])).strftime("%Y-%m-%d %H:%M")
        what = "{} {}".format(desc["project"], desc["version"]) if desc else self.images[-1]["role"]
        try:
            where = str(Path(self.source).relative_to(REPO_ROOT))
        except ValueError:
            where = self.source
        return "{}   [{}, file date {}]".format(where, what, stamp)


def discover_builds(roots=None):
    """ESP-IDF build directories under the repo root and the working directory, newest first."""
    found = {}
    for root in roots or [REPO_ROOT, Path.cwd()]:
        for args_file in Path(root).glob("build*/flasher_args.json"):
            found[str(args_file.parent.resolve())] = args_file.stat().st_mtime
    return [path for path, _ in sorted(found.items(), key=lambda kv: -kv[1])]
