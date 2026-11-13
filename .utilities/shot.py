"""Run the game headless and keep its frame: full PNG plus a <=1280 px JPEG to look at.

  .local/venv/Scripts/python.exe .utilities/shot.py NAME [--api d3d12|vulkan|both] [--preset release] -- <game flags>
  e.g. .utilities/shot.py noon --api both -- --hour=14 --frames=30 --camera=0,2,0,30,-5

Frames land in shots/NAME_<api>.png and .jpg. The game never shows a window or takes focus with --frames.
"""
import argparse, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def shrink(png: Path, jpg: Path, max_side=1280):
    from PIL import Image
    im = Image.open(png).convert("RGB")
    s = max_side / max(im.size)
    if s < 1:
        im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
    im.save(jpg, quality=88)


def main():
    argv = sys.argv[1:]
    game_flags = []
    if "--" in argv:
        i = argv.index("--")
        argv, game_flags = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--api", default="both", choices=["d3d12", "vulkan", "both"])
    ap.add_argument("--preset", default="release")
    ap.add_argument("--timeout", type=float, default=120)
    a = ap.parse_args(argv)
    exe = ROOT / "build" / a.preset / "bin" / "WondersOfTaelia.exe"
    if not any(f.startswith("--frames") for f in game_flags):
        game_flags.append("--frames=20")
    out = ROOT / "shots"
    out.mkdir(exist_ok=True)
    failed = False
    for api in (["d3d12", "vulkan"] if a.api == "both" else [a.api]):
        png = out / f"{a.name}_{api}.png"
        cmd = [str(exe), f"--api={api}", f"--shot={png}"] + game_flags
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=a.timeout)
        log = (r.stdout + r.stderr).strip().splitlines()
        if r.returncode != 0 or not png.exists():
            failed = True
            print(f"{api}: FAILED (exit {r.returncode})")
            for line in log[-15:]:
                print("   ", line)
            continue
        jpg = png.with_suffix(".jpg")
        shrink(png, jpg)
        print(f"{api}: {jpg.relative_to(ROOT)}  ({log[-1] if log else ''})")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
