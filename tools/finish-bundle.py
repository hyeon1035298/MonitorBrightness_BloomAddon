# SPDX-License-Identifier: GPL-3.0-only
"""Adds the add-on's own settings schema to the bundle PillTaskbar's tools wrote.

    python finish-bundle.py <bundle folder> <settings.schema.json>

Copies the schema into the bundle, points addon.json's "settings" at it and lists its
SHA-256 in "files", so Bloom copies and checks it like the rest.
"""
import hashlib
import json
import os
import shutil
import sys


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    folder, schema = sys.argv[1], sys.argv[2]
    with open(schema, encoding="utf-8") as f:
        groups = json.load(f)["groups"]
    for group in groups:
        for item in group["items"]:
            for text in [item["name"]] + ([item["about"]] if "about" in item else []):
                if not (text.get("ko") and text.get("en")):
                    raise SystemExit(f"{schema}: {item['key']} needs ko and en text")
    name = "settings.schema.json"
    shutil.copyfile(schema, os.path.join(folder, name))
    path = os.path.join(folder, "addon.json")
    with open(path, encoding="utf-8") as f:
        manifest = json.load(f)
    files = manifest.pop("files")
    manifest["settings"] = name
    files[name] = sha256(os.path.join(folder, name))
    manifest["files"] = files
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"addon.json: settings, {len(files)} files")


if __name__ == "__main__":
    main()
