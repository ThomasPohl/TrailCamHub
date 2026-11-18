#!/usr/bin/env python3
"""Makes thumbnail.jpg and photo.jpg, the pictures the camera mock serves.

The pictures are drawn here (a generic landscape with a "mock" label), not
taken from a real camera. Run: python3 make_images.py  (needs Pillow).
Output is deterministic for one Pillow version; the files are committed.
"""
import os
import random

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))


def landscape(width, height, seed):
    rnd = random.Random(seed)
    img = Image.new("RGB", (width, height))
    draw = ImageDraw.Draw(img)
    horizon = int(height * 0.55)
    for y in range(height):
        if y < horizon:
            t = y / horizon
            colour = (int(90 + 80 * t), int(130 + 70 * t), int(190 + 40 * t))
        else:
            t = (y - horizon) / (height - horizon)
            colour = (int(70 - 30 * t), int(110 - 40 * t), int(50 - 20 * t))
        draw.line([(0, y), (width, y)], fill=colour)
    for _ in range(14):
        x = rnd.randint(0, width)
        h = rnd.randint(int(height * 0.15), int(height * 0.4))
        w = max(4, h // 3)
        draw.polygon(
            [(x, horizon - h), (x - w, horizon + 4), (x + w, horizon + 4)],
            fill=(20 + rnd.randint(0, 25), 70 + rnd.randint(0, 30), 35),
        )
    draw.text((8, height - 18), "TrailCamHub camera mock", fill=(255, 255, 255))
    return img


def main():
    # Thumbnails are a few KB, photos some tens of KB (as the camera's).
    landscape(320, 180, 1).save(
        os.path.join(HERE, "thumbnail.jpg"), "JPEG", quality=60, optimize=True
    )
    landscape(1280, 720, 2).save(
        os.path.join(HERE, "photo.jpg"), "JPEG", quality=70, optimize=True
    )


if __name__ == "__main__":
    main()
