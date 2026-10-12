# EXIF Editor

A small Qt 6 / Exiv2 desktop app for viewing and editing EXIF, IPTC and XMP metadata, including AI provenance markers.

## Written with Claude.ai
I did not write this code at all, I did review it, but this was 100% written by
claude ai.  Therefore the code is public domain.

## Features
- Open JPEG, TIFF, PNG, WebP, HEIC/AVIF and most RAW formats (via Exiv2); drag & drop or `exif-editor photo.jpg`
- Table of every EXIF tag: key, name, type, raw value (editable) and interpreted value
- Double-click a value to edit it; invalid input for the tag's type is rejected
- Add any standard tag (searchable list with descriptions), remove selected tags
- EXIF, IPTC and XMP tags in one table, all editable (add any standard tag from a searchable list)
- One-click "Remove GPS Data" (EXIF and XMP GPS) and "Remove All EXIF"
- AI markers highlighted in amber, and a "Remove AI Markers…" dialog with a checklist of what was found:
  - `Xmp.iptcExt.DigitalSourceType` set to an AI value (trainedAlgorithmicMedia, compositeWithTrainedAlgorithmicMedia, …)
  - Tool fields mentioning AI generators (Software, CreatorTool, XMP edit history, credits, descriptions…)
  - Stable Diffusion / ComfyUI prompts in EXIF UserComment or PNG text chunks
  - C2PA Content Credentials manifests (removal supported for JPEG, PNG and WebP; C2PA from cameras
    with no AI terms is listed but unchecked by default)
- Setting `DigitalSourceType` from the Add Tag dialog offers the official IPTC values
- Edited tags are shown in bold; Save writes to the file, Revert reloads it
- Optional: keep the file's modification time when saving (File menu, on by default)
- Preview with correct orientation, or the embedded preview for RAW files
- Invisible pixel watermarks (e.g. SynthID) are part of the image itself and are not affected

Binary/structural tags (MakerNote, IFD pointers, thumbnail offsets) are read-only to avoid corrupting files.

## Build on Fedora

```bash
sudo dnf install gcc-c++ cmake qt6-qtbase-devel exiv2-devel zlib-devel
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/exif-editor
```

## Install (adds it to the Plasma launcher and "Open With")

```bash
sudo cmake --install build          # /usr/local/bin + /usr/local/share/applications
# or per-user:
cmake --install build --prefix ~/.local
```

## Value formats
| Type | Example |
|---|---|
| Ascii | `Jane Doe` |
| Dates | `2024:05:01 10:00:00` |
| Rational | `1/200`, GPS: `37/1 20/1 1234/100` |
| Short/Long | `1` (Orientation), `400` (ISO) |
| Undefined | space-separated bytes, e.g. `48 50 51 48` |

Tip: try it on a copy of a photo first — saving rewrites the file in place.
