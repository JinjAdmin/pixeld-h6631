"""Build the pixeld icon set (.pxi files) from Govee's stock show_mode assets.

  python tools/icon_pack.py <show_mode_dir> <out_dir>

.pxi format (little-endian): b"PXI1", u8 w, u8 h, u16 frames, then per frame: u16 delay_ms + w*h*4 RGBA.
Output names (all lower-case): wx_<cond> (15x18), wxs_<cond> (16x12), env_temp, env_hum,
fin_rise/fall/flat, nfl_<team> (15x5), nba_<team> (20x9), hourglass (14x24), stopwatch (50x30).
Govee's artwork: personal use on this device only.
"""
import glob
import os
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from anim_convert import decode_gif  # noqa: E402
from minipng import read_png  # noqa: E402


def load(path):
    d = open(path, "rb").read()
    if d[:3] == b"GIF":
        frames = []
        for delay, canvas in decode_gif(d):
            # GIF frames come back as RGB on black; treat pure black as transparent
            frames.append((delay, [[(r, g, b, 0 if (r, g, b) == (0, 0, 0) else 255) for r, g, b in row] for row in canvas]))
        return frames
    w, h, rows = read_png(d)
    return [(1000, rows)]


def write_pxi(path, frames):
    h = len(frames[0][1]); w = len(frames[0][1][0])
    with open(path, "wb") as f:
        f.write(b"PXI1" + struct.pack("<BBH", w, h, len(frames)))
        for delay, rows in frames:
            f.write(struct.pack("<H", max(20, min(60000, delay))))
            f.write(bytes(c for row in rows for px in row for c in px))


def main():
    src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    os.makedirs(out, exist_ok=True)
    jobs = {}
    wx = {"sunny": "sunny", "cloudy": "cloudy", "fog": "fog", "hail": "hail", "rain": "rain", "snow": "snow", "thunder": "thurder_rain"}
    for k, v in wx.items():
        jobs[f"wx_{k}"] = src / "weather" / f"15x18_{v}.gif"
    wxs = {"sunny": "sunny", "cloudy": "cloudy", "fog": "flog", "hail": "hail", "rain": "rain", "snow": "snow", "thunder": "thunderstorm"}
    for k, v in wxs.items():
        jobs[f"wxs_{k}"] = src / "weather" / f"{v}_16x12.gif"
    jobs["env_temp"] = src / "envir" / "temperature.png"
    jobs["env_hum"] = src / "envir" / "humidity.png"
    for k in ("rise", "fall", "flat"):
        jobs[f"fin_{k}"] = src / "finance" / f"{k}_9x9.png"
    jobs["hourglass"] = src / "countdown" / "countdown_hourglass_14x24.gif"
    jobs["stopwatch"] = src / "stopwatch" / "stopwatch_round_50x30.gif"
    jobs["clk_face"] = src / "time" / "clock" / "clock_face_30x30.png"
    jobs["clk_left"] = src / "time" / "clock" / "clock_left_5x25.png"
    jobs["clk_right"] = src / "time" / "clock" / "clock_right_9x25.png"
    for p in glob.glob(str(src / "NFL" / "team_15x5_*.png")):
        jobs["nfl_" + p.rsplit("_", 1)[1][:-4].lower()] = pathlib.Path(p)
    for p in glob.glob(str(src / "NBA" / "team_20x9_*.png")):
        jobs["nba_" + p.rsplit("_", 1)[1][:-4].lower()] = pathlib.Path(p)
    for name, path in sorted(jobs.items()):
        try:
            frames = load(path)
            write_pxi(out / f"{name}.pxi", frames)
            print(f"ok  {name}: {len(frames[0][1][0])}x{len(frames[0][1])} x{len(frames)}")
        except Exception as e:
            print(f"SKIP {name} ({path}): {e}")


if __name__ == "__main__":
    main()
