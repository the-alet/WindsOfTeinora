"""Look at frames by numbers instead of by eye.

  frame.py stats IMG [x y w h]        mean / min / max RGB of a region (pixels of the full image)
  frame.py crop  IMG x y w h OUT.jpg  crop (and shrink to <=1280) for a close look
  frame.py diff  A B                  mean and max absolute difference, per channel
  frame.py row   IMG y [step]         RGB along a row, every `step` pixels (gradients, banding, blur falloff)
  frame.py sharp IMG x y w h          local contrast (mean |gradient|): high = sharp, low = blurred
"""
import sys
import numpy as np
from PIL import Image


def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(np.float64)


def region(a, args):
    if len(args) >= 4:
        x, y, w, h = map(int, args[:4])
        return a[y:y + h, x:x + w]
    return a


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    cmd, img = sys.argv[1], sys.argv[2]
    rest = sys.argv[3:]
    if cmd == "stats":
        r = region(load(img), rest)
        print("mean", np.round(r.mean(axis=(0, 1)), 1), "min", r.min(axis=(0, 1)), "max", r.max(axis=(0, 1)), "size", r.shape[1], "x", r.shape[0])
    elif cmd == "crop":
        x, y, w, h = map(int, rest[:4])
        im = Image.open(img).convert("RGB").crop((x, y, x + w, y + h))
        s = 1280 / max(im.size)
        if s < 1:
            im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
        im.save(rest[4], quality=90)
        print(rest[4], im.size)
    elif cmd == "diff":
        a, b = load(img), load(rest[0])
        d = np.abs(a - b)
        print("mean", np.round(d.mean(axis=(0, 1)), 2), "max", d.max(axis=(0, 1)), "pixels >8:", int((d.max(axis=2) > 8).sum()))
    elif cmd == "row":
        a = load(img)
        y = int(rest[0])
        step = int(rest[1]) if len(rest) > 1 else max(1, a.shape[1] // 16)
        for x in range(0, a.shape[1], step):
            print(x, a[y, x].astype(int).tolist())
    elif cmd == "sharp":
        r = region(load(img), rest).mean(axis=2)
        gx = np.abs(np.diff(r, axis=1)).mean()
        gy = np.abs(np.diff(r, axis=0)).mean()
        print("mean |grad|", round((gx + gy) / 2, 3))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
