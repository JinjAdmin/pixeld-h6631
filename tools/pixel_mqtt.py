"""Publish a command to the Govee Pixel over MQTT (no dependencies).

Reads broker/user/password from private/mqtt.conf (never printed).
  python tools/pixel_mqtt.py notify "Front door"
  python tools/pixel_mqtt.py notify '{"message":"Door","duration":10,"color":"#ff0000"}'
  python tools/pixel_mqtt.py text "Hello world"     | mode clock | power OFF | brightness 50
  python tools/pixel_mqtt.py image path/to/frame.rgb   (raw 52x32 RGB, 4992 bytes)
"""
import pathlib
import socket
import struct
import sys

BASE = "govee-pixel"
CONF = pathlib.Path(__file__).resolve().parent.parent / "private" / "mqtt.conf"


def enc_str(s: bytes) -> bytes:
    return struct.pack("!H", len(s)) + s


def enc_len(n: int) -> bytes:
    out = bytearray()
    while True:
        d, n = n % 128, n // 128
        out.append(d | (128 if n else 0))
        if not n:
            return bytes(out)


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    cmd, arg = sys.argv[1], sys.argv[2]
    conf = dict(l.split("=", 1) for l in CONF.read_text().splitlines() if "=" in l)
    payload = pathlib.Path(arg).read_bytes() if cmd == "image" else arg.encode()
    topic = f"{BASE}/{cmd}/set".encode()

    var = enc_str(b"MQTT") + bytes([4, 0x02 | 0x80 | 0x40, 0, 30])
    pl = enc_str(b"pixel-cli") + enc_str(conf["user"].encode()) + enc_str(conf["pass"].encode())
    s = socket.create_connection((conf["host"], int(conf.get("port", 1883))), timeout=5)
    s.sendall(b"\x10" + enc_len(len(var + pl)) + var + pl)
    ack = s.recv(4)
    if len(ack) != 4 or ack[3] != 0:
        sys.exit(f"broker refused connection: {ack!r}")
    body = enc_str(topic) + payload
    s.sendall(b"\x30" + enc_len(len(body)) + body + b"\xe0\x00")
    s.close()
    print(f"sent {cmd} ({len(payload)} bytes)")


if __name__ == "__main__":
    main()
