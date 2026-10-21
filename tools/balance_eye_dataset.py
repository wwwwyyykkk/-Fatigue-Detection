#!/usr/bin/env python3
"""Create a balanced eye_open / eye_closed dataset by downsampling classes."""

from __future__ import annotations

import argparse
import random
import shutil
from pathlib import Path


IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp"}


def list_images(class_dir: Path) -> list[Path]:
    return sorted(path for path in class_dir.iterdir() if path.suffix.lower() in IMAGE_EXTENSIONS)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--max-per-class", type=int, default=0, help="Optional cap after balancing. 0 keeps all minority samples.")
    args = parser.parse_args()

    random.seed(args.seed)
    classes = ["eye_open", "eye_closed"]
    images_by_class = {label: list_images(args.input_dir / label) for label in classes}
    target_count = min(len(images) for images in images_by_class.values())
    if args.max_per_class > 0:
        target_count = min(target_count, args.max_per_class)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    for label in classes:
      class_out = args.output_dir / label
      class_out.mkdir(parents=True, exist_ok=True)
      chosen = random.sample(images_by_class[label], target_count)
      for index, source in enumerate(chosen):
          shutil.copy2(source, class_out / f"{label}_{index:04d}{source.suffix.lower()}")
      print(f"{label}: {len(chosen)}")


if __name__ == "__main__":
    main()
