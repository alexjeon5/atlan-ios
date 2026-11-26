#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Adds a home-screen icon to an unsigned IPA, before it is signed and installed.

The public build ships no icon; the icon stays outside this repository and is added
to the CI-built IPA locally. The icon goes in as loose PNG files listed under
CFBundleIcons / CFBundleIcons~ipad in the app's Info.plist (no asset catalog needed).

usage: inject_app_icon.py IN.ipa ICON [-o OUT.ipa]

ICON is either a folder holding the PNGs named in ICON_FILES, or one square PNG
(at least 180x180, 1024x1024 is best) that is scaled to every size; scaling needs
Pillow. Without -o the result is written next to IN.ipa as <name>-icon.ipa.
"""
import argparse
import io
import os
import plistlib
import sys
import zipfile

# file name -> pixel size. Base names follow Xcode's loose-icon naming.
ICON_FILES = {
    "AppIcon60x60@2x.png": 120,
    "AppIcon60x60@3x.png": 180,
    "AppIcon76x76~ipad.png": 76,
    "AppIcon76x76@2x~ipad.png": 152,
    "AppIcon83.5x83.5@2x~ipad.png": 167,
}
PHONE_BASES = ["AppIcon60x60"]
PAD_BASES = ["AppIcon60x60", "AppIcon76x76", "AppIcon83.5x83.5"]


def load_icons(src):
    if os.path.isdir(src):
        icons = {}
        for name in ICON_FILES:
            path = os.path.join(src, name)
            if not os.path.isfile(path):
                sys.exit(f"missing {path}")
            with open(path, "rb") as f:
                icons[name] = f.read()
        return icons
    try:
        from PIL import Image
    except ImportError:
        sys.exit("scaling one PNG needs Pillow; pass a folder of sized PNGs instead")
    image = Image.open(src).convert("RGB")  # the home screen has no transparency
    if image.width != image.height or image.width < 180:
        sys.exit(f"{src}: need a square PNG of at least 180x180, got {image.width}x{image.height}")
    icons = {}
    for name, size in ICON_FILES.items():
        out = io.BytesIO()
        image.resize((size, size), Image.LANCZOS).save(out, "PNG")
        icons[name] = out.getvalue()
    return icons


def primary_icon(bases):
    return {"CFBundlePrimaryIcon": {"CFBundleIconFiles": bases, "UIPrerenderedIcon": False}}


def inject(ipa_in, ipa_out, icons):
    with zipfile.ZipFile(ipa_in) as zin:
        names = zin.namelist()
        plists = [n for n in names if n.startswith("Payload/") and n.count("/") == 2 and n.endswith(".app/Info.plist")]
        if len(plists) != 1:
            sys.exit(f"{ipa_in}: expected one Payload/*.app/Info.plist, found {len(plists)}")
        app_dir = plists[0][: -len("Info.plist")]
        raw = zin.read(plists[0])
        fmt = plistlib.FMT_BINARY if raw.startswith(b"bplist") else plistlib.FMT_XML
        info = plistlib.loads(raw)
        info["CFBundleIcons"] = primary_icon(PHONE_BASES)
        info["CFBundleIcons~ipad"] = primary_icon(PAD_BASES)
        info.pop("CFBundleIconFiles", None)
        info.pop("CFBundleIconFile", None)
        replaced = {app_dir + n for n in icons} | {plists[0]}
        with zipfile.ZipFile(ipa_out, "w", zipfile.ZIP_DEFLATED) as zout:
            for item in zin.infolist():
                if item.filename in replaced:
                    continue
                zout.writestr(item, zin.read(item.filename))
            zout.writestr(plists[0], plistlib.dumps(info, fmt=fmt))
            for name, data in icons.items():
                zout.writestr(app_dir + name, data)
    return app_dir


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("ipa")
    parser.add_argument("icon")
    parser.add_argument("-o", "--output")
    args = parser.parse_args()
    out = args.output or os.path.splitext(args.ipa)[0] + "-icon.ipa"
    if os.path.abspath(out) == os.path.abspath(args.ipa):
        sys.exit("output must differ from the input IPA")
    app_dir = inject(args.ipa, out, load_icons(args.icon))
    print(f"{out}: icon added to {app_dir}")


if __name__ == "__main__":
    main()
