"""Stream frames to the Govee Pixel over UDP (port 7777, raw 52x32 RGB = 4992 bytes per packet).

  python tools/pixel_udp.py [seconds] [host]      # rainbow demo animation

Use frame()/send() as a starting point for your own displays: build a 52x32 grid of
(r, g, b) tuples, row by row from the top-left, and send it. The panel goes back to its
normal mode 3 s after the last frame.
"""
import colorsys
import math
import socket
import sys
import time

W, H = 52, 32
HOST = "govee-pixel"  # the display's IP or hostname; also the 2nd command-line argument
PORT = 7777


def send(sock: socket.socket, pixels, host: str = HOST) -> None:
    buf = bytearray()
    for r, g, b in pixels:
        buf += bytes((r, g, b))
    assert len(buf) == W * H * 3
    sock.sendto(buf, (host, PORT))


def rainbow(t: float):
    for y in range(H):
        for x in range(W):
            h = (x / W + y / H * 0.5 + t * 0.2) % 1.0
            v = 0.6 + 0.4 * math.sin(t * 3 + x * 0.3)
            r, g, b = colorsys.hsv_to_rgb(h, 1, v)
            yield int(r * 255), int(g * 255), int(b * 255)


def main() -> None:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10
    host = sys.argv[2] if len(sys.argv) > 2 else HOST
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    t0 = time.time()
    n = 0
    while time.time() - t0 < secs:
        send(s, rainbow(time.time() - t0), host)
        n += 1
        time.sleep(1 / 20)
    print(f"sent {n} frames")


if __name__ == "__main__":
    main()
