#!/usr/bin/env python3
"""Build an eye_open / eye_closed dataset from Edge Impulse bounding boxes.

The public dataset in this workspace labels eyes with Indonesian class names:
  - mata_terbuka  -> eye_open
  - mata_terpejam -> eye_closed

The script crops each annotated eye box, optionally adds padding, resizes it,
and writes a simple folder dataset that can be uploaded to Edge Impulse as a
two-class image classification project.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageOps


LABEL_MAP = {
    "mata_terbuka": "eye_open",
    "mata_terpejam": "eye_closed",
}


def iter_images(source_dir: Path) -> Iterable[Path]:
    for pattern in ("*.jpg", "*.jpeg", "*.png"):
        yield from source_dir.glob(pattern)


def clamp(value: int, low: int, high: int) -> int:
    return max(low, min(value, high))


def crop_with_padding(image: Image.Image, box: dict, padding: float) -> Image.Image:
    width, height = image.size
    x = int(box["x"])
    y = int(box["y"])
    w = int(box["width"])
    h = int(box["height"])
    pad_x = int(w * padding)
    pad_y = int(h * padding)

    left = clamp(x - pad_x, 0, width)
    top = clamp(y - pad_y, 0, height)
    right = clamp(x + w + pad_x, 0, width)
    bottom = clamp(y + h + pad_y, 0, height)
    return image.crop((left, top, right, bottom))


def load_boxes(label_file: Path) -> dict[str, list[dict]]:
    data = json.loads(label_file.read_text(encoding="utf-8"))
    if "boundingBoxes" in data and isinstance(data["boundingBoxes"], dict):
        return data["boundingBoxes"]

    boxes: dict[str, list[dict]] = {}
    for item in data.get("files", []):
        path = Path(item["path"]).name
        boxes[path] = item.get("boundingBoxes", [])
    return boxes


def prepare_dataset(
    source_dir: Path,
    label_file: Path,
    output_dir: Path,
    size: int,
    padding: float,
    grayscale: bool,
) -> dict[str, int]:
    boxes_by_name = load_boxes(label_file)
    output_dir.mkdir(parents=True, exist_ok=True)
    counts = {label: 0 for label in LABEL_MAP.values()}
    skipped = 0

    for image_path in iter_images(source_dir):
        boxes = boxes_by_name.get(image_path.name, [])
        if not boxes:
            skipped += 1
            continue

        with Image.open(image_path) as image:
            for index, box in enumerate(boxes):
                target_label = LABEL_MAP.get(box.get("label"))
                if target_label is None:
                    continue

                cropped = crop_with_padding(image, box, padding)
                if grayscale:
                    cropped = ImageOps.grayscale(cropped)
                cropped = ImageOps.fit(cropped, (size, size), method=Image.Resampling.BILINEAR)

                class_dir = output_dir / target_label
                class_dir.mkdir(parents=True, exist_ok=True)
                output_name = f"{image_path.stem}_eye{index:02d}.jpg"
                cropped.save(class_dir / output_name, quality=92)
                counts[target_label] += 1

    counts["skipped_images_without_boxes"] = skipped
    return counts


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=Path, required=True, help="Directory containing source images.")
    parser.add_argument("--labels", type=Path, required=True, help="Edge Impulse bounding_boxes.labels or info.labels.")
    parser.add_argument("--output-dir", type=Path, required=True, help="Output directory for class folders.")
    parser.add_argument("--size", type=int, default=96, help="Square output size, matching the Edge Impulse input.")
    parser.add_argument("--padding", type=float, default=0.18, help="Relative padding around each eye box.")
    parser.add_argument("--rgb", action="store_true", help="Keep RGB images. Default is grayscale.")
    args = parser.parse_args()

    counts = prepare_dataset(
        source_dir=args.source_dir,
        label_file=args.labels,
        output_dir=args.output_dir,
        size=args.size,
        padding=args.padding,
        grayscale=not args.rgb,
    )

    print("Eye dataset prepared:")
    for label, count in counts.items():
        print(f"  {label}: {count}")


if __name__ == "__main__":
    main()
