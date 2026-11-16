#!/usr/bin/env python3
"""pixeld installer for the Govee H6631, run on a PC connected to the display's serial console.

    python pixeld_install.py                 install (the display must be running Govee's stock firmware)
    python pixeld_install.py --rollback      switch back to the stock firmware

What it does, all through the serial console and your Wi-Fi:
  1. checks the device (H6631, running stock firmware) and finds the unused A/B slot;
  2. backs up every flash partition to this PC and verifies each copy;
  3. sets bootdelay=3 so U-Boot can always be reached over serial;
  4. builds the custom firmware image from the backup of YOUR device's stock firmware (nothing of Govee's is
     downloaded or included here);
  5. writes it to the unused slot (the stock slot is never written), installs pixeld into /data/custom;
  6. switches to the new slot, reboots and checks that it came up.

Only the Python standard library is needed (3.8+).
"""
import argparse
import datetime
import functools
import getpass
import hashlib
import http.server
import io
import os
import pathlib
import re
import socket
import subprocess
import sys
import tarfile
import threading
import time
import uuid

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import serialport  # noqa: E402
import squashfs    # noqa: E402

BAUD = 1500000
STAGE = "/mnt/UDISK/pixeld-install"
NOISE = re.compile(r"^\[(debug|info|err|warn|error)\]\[|recvfrom timeout|wait for exit timeout")


def say(msg=""):
    print(msg, flush=True)


def die(msg):
    say("\nERROR: " + msg)
    sys.exit(1)


def ask(prompt, default=""):
    try:
        a = input(prompt).strip()
    except EOFError:
        a = ""
    return a or default


def confirm(prompt, assume_yes):
    if assume_yes:
        return True
    return ask(prompt + " [y/N] ").lower() in ("y", "yes")


# ----------------------------------------------------------------------------------------------- serial console
class Console:
    """Root shell on the H6631 serial console. Commands are wrapped in unique markers so the output can be
    picked out of the stock firmware's debug spam."""

    def __init__(self, port, log_path=None):
        self.sp = serialport.Serial(port, BAUD)
        self.log = open(log_path, "ab") if log_path else None
        self.buf = b""

    def close(self):
        self.sp.close()
        if self.log:
            self.log.close()

    def _pump(self):
        d = self.sp.read()
        if d:
            self.buf += d
            if self.log:
                self.log.write(d)
                self.log.flush()
        return d

    def type(self, data):
        """Send keystrokes slowly: at 1.5 Mbaud the display's UART drops characters if a whole line arrives at once."""
        for i in range(0, len(data), 8):
            self.sp.write(data[i:i + 8])
            time.sleep(0.003)
            self._pump()

    def wake(self, timeout=6):
        end = time.time() + timeout
        while time.time() < end:
            self.buf = b""
            self.sp.write(b"\n")
            t = time.time() + 1.0
            while time.time() < t:
                self._pump()
            if re.search(rb"# ?$", self.buf.rstrip(b"\r\n ") + b" ") or b"# " in self.buf:
                return True
        return False

    def run(self, cmd, timeout=30, check=True):
        if len(cmd) > 300:
            raise ValueError("command too long for the serial console")
        # The stock firmware writes its own logs to this console at any moment, so the command's output goes to a
        # file first and is then printed with every line tagged; only tagged lines are kept.
        tag = uuid.uuid4().hex[:8]
        o = f"/tmp/.o{tag}"
        self.buf = b""
        self.type((f"{{ {cmd} ; }} >{o} 2>&1; r=$?; sed 's/^/@@L{tag} /' {o}; rm -f {o}; "
                       f"printf '\\n@@E%s %d@@\\n' {tag} $r\n").encode())
        end_re = re.compile(rb"@@E" + tag.encode() + rb" (\d+)@@")
        pre = f"@@L{tag} "
        end = time.time() + timeout
        while time.time() < end:
            self._pump()
            m = end_re.search(self.buf)
            if m:
                text = self.buf[:m.start()].decode("latin-1").replace("\r", "")
                lines = [ln.lstrip("\0 ")[len(pre):] for ln in text.split("\n") if ln.lstrip("\0 ").startswith(pre)]
                status = int(m.group(1))
                if check and status != 0:
                    raise RuntimeError(f"device command failed ({status}): {cmd}\n" + "\n".join(lines))
                return status, "\n".join(lines)
        raise TimeoutError(f"no answer within {timeout} s: {cmd}")

    def out(self, cmd, **kw):
        return self.run(cmd, **kw)[1].strip()


# ----------------------------------------------------------------------------------------------- PC-side servers
class Receiver:
    """TCP listener; the display sends files with `nc <pc> <port> < file`."""

    def __init__(self):
        self.s = socket.socket()
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("0.0.0.0", 0))
        self.s.listen(1)
        self.port = self.s.getsockname()[1]

    def receive_bg(self, path, timeout):
        """accept one connection in the background; returns a function that waits for (md5, size)."""
        res = {}

        def work():
            try:
                self.s.settimeout(timeout)
                c, _ = self.s.accept()
                c.settimeout(60)
                h, n = hashlib.md5(), 0
                with open(path, "wb") as f:
                    while True:
                        d = c.recv(1 << 16)
                        if not d:
                            break
                        f.write(d)
                        h.update(d)
                        n += len(d)
                c.close()
                res["r"] = (h.hexdigest(), n)
            except Exception as e:  # noqa: BLE001
                res["e"] = e

        t = threading.Thread(target=work, daemon=True)
        t.start()

        def wait():
            t.join()
            if "e" in res:
                raise res["e"]
            return res["r"]
        return wait


class _QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass


def start_http(directory):
    h = functools.partial(_QuietHandler, directory=str(directory))
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), h)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def pc_ip_towards(ip):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((ip, 80))
        return s.getsockname()[0]
    finally:
        s.close()


# ----------------------------------------------------------------------------------------------- image build
def _text(data):
    return data.replace(b"\r\n", b"\n") if b"\0" not in data else data


def customize(root, overlay_dir, stamp):
    """Apply the custom-firmware changes to the stock root filesystem tree (same as firmware/build_rootfs.sh)."""
    N = squashfs.Node
    now = int(time.time())
    if root.lookup("/etc/custom_release"):
        raise ValueError("the source image is already custom firmware, not Govee's stock firmware")
    for must in ("/etc/init.d/govee_init.sh", "/etc/init.d/rc.final", "/bin/busybox"):
        if root.lookup(must) is None:
            raise ValueError(f"unexpected stock firmware: {must} is missing")
    # 1. overlay files (init scripts)
    for p in sorted(pathlib.Path(overlay_dir).rglob("*")):
        rel = "/" + p.relative_to(overlay_dir).as_posix()
        if p.is_dir():
            if root.lookup(rel) is None:
                root.put(rel, N("dir", 0o755, now))
            continue
        root.put(rel, N("file", 0o755 if rel.endswith(".sh") else 0o644, now, data=_text(p.read_bytes())))
    # 2. no telnetd (password login service)
    rc = root.lookup("/etc/init.d/rc.final")
    rc.data = re.sub(rb"(?m)^telnetd -l /bin/login &", b"# telnetd removed (custom firmware)", rc.data)
    if re.search(rb"(?m)^telnetd", rc.data):
        raise ValueError("couldn't disable telnetd in rc.final")
    # 3. root's ~/.ssh lives in persistent storage
    root.remove("/root/.ssh")
    root.put("/root/.ssh", N("symlink", 0o777, now, target=b"/data/custom/ssh"))
    # 4. marker for which firmware booted
    root.put("/etc/custom_release", N("file", 0o644, now, data=f"custom-{stamp}\n".encode()))
    # 5. the console device node
    if root.lookup("/dev") and root.lookup("/dev/console") is None:
        root.put("/dev/console", N("chr", 0o600, now, rdev=(5, 1)))
    return root


def build_payload(tz, hostname, keys, mqtt=("keep", None)):
    dc = ROOT / "firmware" / "data-custom"
    files = {
        "install.sh": (_text((HERE / "device_install.sh").read_bytes()), 0o755),
        "switch.sh": (_text((HERE / "switch_slot.sh").read_bytes()), 0o755),
        "custom/start.sh": (_text((dc / "start.sh").read_bytes()), 0o755),
        "custom/ble.conf": (_text((dc / "ble.conf").read_bytes()), 0o644),
        "custom/apps/chime.sh": (_text((dc / "apps" / "chime.sh").read_bytes()), 0o755),
    }
    for f in sorted((dc / "apps" / "autostart").iterdir()):
        files[f"custom/apps/autostart/{f.name}"] = (_text(f.read_bytes()), 0o755)
    for b in ("pixeld", "bled"):
        p = find_binary(b)
        files[f"custom/apps/{b}"] = (p.read_bytes(), 0o755)
    if tz:
        files["config/tz"] = (tz.encode() + b"\n", 0o644)
    if hostname:
        files["config/hostname"] = (hostname.encode() + b"\n", 0o644)
    if mqtt[0] == "direct":
        files["config/mqtt.conf"] = (mqtt[1].encode(), 0o600)
    elif mqtt[0] == "pixel2mqtt":
        files["config/mqtt_remove"] = (b"1\n", 0o644)
    if keys:
        files["config/authorized_keys"] = ("\n".join(keys).encode() + b"\n", 0o600)
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for name, (data, mode) in files.items():
            ti = tarfile.TarInfo(name)
            ti.size, ti.mode, ti.mtime = len(data), mode, int(time.time())
            tf.addfile(ti, io.BytesIO(data))
    return bio.getvalue()


def find_binary(name):
    for p in (HERE / "bin" / name, ROOT / "build" / name):
        if p.is_file():
            return p
    die(f"{name} binary not found (expected installer/bin/{name} or build/{name}; run build.sh or use a release zip)")


# ----------------------------------------------------------------------------------------------- settings
WIN_TZ = {
    "Eastern Standard Time": "EST5EDT,M3.2.0,M11.1.0", "Central Standard Time": "CST6CDT,M3.2.0,M11.1.0",
    "Mountain Standard Time": "MST7MDT,M3.2.0,M11.1.0", "US Mountain Standard Time": "MST7",
    "Pacific Standard Time": "PST8PDT,M3.2.0,M11.1.0", "Alaskan Standard Time": "AKST9AKDT,M3.2.0,M11.1.0",
    "Hawaiian Standard Time": "HST10", "Atlantic Standard Time": "AST4ADT,M3.2.0,M11.1.0",
    "GMT Standard Time": "GMT0BST,M3.5.0/1,M10.5.0", "W. Europe Standard Time": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Romance Standard Time": "CET-1CEST,M3.5.0,M10.5.0/3", "Central Europe Standard Time": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Central European Standard Time": "CET-1CEST,M3.5.0,M10.5.0/3", "E. Europe Standard Time": "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "FLE Standard Time": "EET-2EEST,M3.5.0/3,M10.5.0/4", "AUS Eastern Standard Time": "AEST-10AEDT,M10.1.0,M4.1.0/3",
    "New Zealand Standard Time": "NZST-12NZDT,M9.5.0,M4.1.0/3", "UTC": "UTC0",
}


def guess_tz():
    try:
        if sys.platform == "win32":
            name = subprocess.run(["tzutil", "/g"], capture_output=True, text=True, timeout=5).stdout.strip()
            return WIN_TZ.get(name, "")
        tail = pathlib.Path("/etc/localtime").read_bytes().rstrip(b"\n").rsplit(b"\n", 1)[-1]
        return tail.decode() if tail and len(tail) < 60 else ""
    except Exception:  # noqa: BLE001
        return ""


def choose_mqtt(args, has_conf):
    """-> ("keep" | "pixel2mqtt" | "direct", mqtt.conf text or None)"""
    mode = args.mqtt
    if not mode:
        if args.yes:
            mode = "keep" if has_conf else "pixel2mqtt"
        else:
            say("\nHow should Home Assistant talk to this display?\n"
                "  1) through pixel2mqtt (recommended): no MQTT login on the display, one bridge for all your\n"
                "     displays, plus the web editor. Set up pixel2mqtt after this.\n"
                "  2) directly to your MQTT broker: no pixel2mqtt needed")
            if has_conf:
                say("  3) keep the direct MQTT login this display already has")
            dflt = "3" if has_conf else "1"
            a = ask(f"Choice [{dflt}]: ", dflt)
            mode = {"1": "pixel2mqtt", "2": "direct", "3": "keep"}.get(a, "pixel2mqtt")
    if mode == "keep" and not has_conf:
        mode = "pixel2mqtt"
    if mode != "direct":
        return mode, None
    host = args.mqtt_host or ask("MQTT broker address: ")
    try:
        ip = socket.gethostbyname(host)   # the display needs an IP address
    except OSError:
        die(f"can't resolve the MQTT broker address {host!r}")
    port = args.mqtt_port or int(ask("MQTT port [1883]: ", "1883"))
    user = args.mqtt_user if args.mqtt_user is not None else ask("MQTT user (empty for none): ")
    pw = os.environ.get("PIXELD_MQTT_PASSWORD")
    if pw is None and user:
        pw = getpass.getpass("MQTT password (not shown): ")
    conf = f"host={ip}\nport={port}\n" + (f"user={user}\npass={pw or ''}\n" if user else "")
    return "direct", conf


def ssh_keys(arg_key, assume_yes):
    if arg_key:
        return [pathlib.Path(arg_key).expanduser().read_text().strip()]
    d = pathlib.Path.home() / ".ssh"
    for n in ("id_ed25519.pub", "id_ecdsa.pub", "id_rsa.pub"):
        if (d / n).is_file():
            say(f"SSH key: {d / n}")
            return [(d / n).read_text().strip()]
    if confirm("No SSH key found. Create one now with ssh-keygen (needed to log in to the display later)?", assume_yes):
        d.mkdir(exist_ok=True)
        try:
            subprocess.run(["ssh-keygen", "-t", "ed25519", "-f", str(d / "id_ed25519"), "-N", ""], check=True)
            return [(d / "id_ed25519.pub").read_text().strip()]
        except Exception as e:  # noqa: BLE001
            say(f"  couldn't create a key ({e}); continuing without one")
    return []


# ----------------------------------------------------------------------------------------------- main steps
def pick_port(arg):
    if arg:
        return arg
    ports = serialport.list_ports()
    if not ports:
        die("no serial ports found. Plug in the ESP32 / USB-serial adapter (and its driver), or pass --port.")
    if len(ports) == 1:
        return ports[0]
    say("Serial ports: " + ", ".join(ports))
    return ask(f"Which one is the display? [{ports[0]}] ", ports[0])


def connect(port, log_path):
    say(f"Opening {port} at {BAUD} baud...")
    con = Console(port, log_path)
    if not con.wake():
        con.close()
        die("no shell prompt on the serial console. Check the wiring (ESP32: EN to GND, display TX->TX0, "
            "RX->RX0, GND->GND; other adapters: TX->RX, RX->TX), that the display is powered, and the port.")
    return con


def device_info(con):
    target = con.out("grep DISTRIB_TARGET /etc/openwrt_release")
    if "H6631" not in target:
        die(f"this doesn't look like an H6631 ({target or 'no /etc/openwrt_release'}).")
    run = con.out("fw_printenv -n boot_partition").replace("boot", "")
    if run not in ("A", "B"):
        die(f"unexpected boot_partition '{run}'")
    custom = con.out("cat /etc/custom_release 2>/dev/null", check=False)
    vols = {}
    for line in con.out("for v in /sys/class/ubi/ubi0_*; do echo ${v##*/} $(cat $v/name) $(cat $v/data_bytes); done").split("\n"):
        p = line.split()
        if len(p) == 3:
            vols[p[1]] = (p[0], int(p[2]))
    mtd = re.findall(r"^(mtd\d+): \S+ \S+ \"([^\"]+)\"", con.out("cat /proc/mtd"), re.M)
    ip = re.search(r"inet (\d+\.\d+\.\d+\.\d+)", con.out("ip -4 addr show wlan0", check=False))
    return {"running": run, "custom": custom, "vols": vols, "mtd": mtd, "ip": ip.group(1) if ip else None,
            "tz": con.out("cat /data/custom/tz 2>/dev/null", check=False),
            "mqtt": con.out("[ -s /data/custom/mqtt.conf ] && echo yes", check=False) == "yes"}


def backup(con, info, rx, pc, outdir):
    outdir.mkdir(parents=True, exist_ok=True)
    items = [(f"/dev/{dev}", f"{dev}-{name}.bin") for dev, name in info["mtd"] if name != "sys"]  # sys = the UBI container
    items += [(f"/dev/{dev}", f"{dev}-{name}.bin") for name, (dev, _) in sorted(info["vols"].items(), key=lambda kv: int(kv[1][0].split("_")[1]))
              if name != "UDISK"]
    sums = []
    for dev, fname in items:
        say(f"  {fname} ...")
        want = con.out(f"md5sum {dev}", timeout=120).split()[0]
        wait = rx.receive_bg(outdir / fname, timeout=30)
        con.run(f"nc {pc} {rx.port} < {dev}", timeout=600)
        got, n = wait()
        if got != want:
            die(f"backup of {dev} is damaged (md5 {got} != {want}); try again")
        sums.append(f"{got}  {fname}")
    con.run("fw_printenv > /tmp/env.txt")
    wait = rx.receive_bg(outdir / "uboot-env.txt", timeout=30)
    con.run(f"nc {pc} {rx.port} < /tmp/env.txt")
    wait()
    (outdir / "md5sums.txt").write_text("\n".join(sums) + "\n")
    (outdir / "README.txt").write_text(
        "Backup of a Govee H6631 made by pixeld_install.py before installing the custom firmware.\n"
        "It contains your Wi-Fi credentials and device keys: keep it private. md5sums.txt has the checksums.\n")


def install(args):
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")
    outdir = pathlib.Path(args.backup_dir or f"h6631-backup-{stamp}").resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    keys = ssh_keys(args.key, args.yes)
    con = connect(pick_port(args.port), outdir / "serial.log")
    try:
        info = device_info(con)
        run = info["running"]
        tgt = "B" if run == "A" else "A"
        say(f"Display: H6631, running slot {run}" + (f" (custom firmware {info['custom']})" if info["custom"] else " (stock firmware)"))
        if info["custom"] and not args.force:
            die("the running slot already has the custom firmware. Boot the stock slot first, or use --rollback.")
        if not info["ip"]:
            die("the display isn't on Wi-Fi. Set it up in the Govee Home app first, then run this again.")
        for v in (f"rootfs{tgt}", f"boot{tgt}", f"rootfs{run}"):
            if v not in info["vols"]:
                die(f"no {v} volume on this device")
        pc = pc_ip_towards(info["ip"])
        say(f"Display IP {info['ip']}, this PC {pc}")
        tz = args.tz
        if not info["tz"] and not tz:
            g = guess_tz()
            tz = ask(f"Time zone as a POSIX TZ string (e.g. EST5EDT,M3.2.0,M11.1.0) [{g or 'UTC0'}]: ", g or "UTC0")
        mqtt = choose_mqtt(args, info["mqtt"])
        mqtt_line = {"keep": "keeping its existing direct MQTT login",
                     "pixel2mqtt": "Home Assistant through pixel2mqtt, no MQTT login on the display",
                     "direct": "direct MQTT to " + (mqtt[1] or "").split("\n")[0][5:]}[mqtt[0]]
        say(f"\nThis will:\n  - back up every flash partition to {outdir}\n  - set bootdelay=3\n"
            f"  - write the custom firmware to slot {tgt} (slot {run}, the stock firmware, is not touched)\n"
            f"  - install pixeld into /data/custom ({mqtt_line}), switch to slot {tgt} and reboot")
        if not confirm("Continue?", args.yes):
            die("cancelled")

        rx = Receiver()
        say("\n[1/6] Checking the network path to this PC...")
        wait = rx.receive_bg(outdir / ".ping", timeout=15)
        con.run(f"echo ping | nc {pc} {rx.port}", timeout=20, check=False)
        try:
            wait()
        except Exception:  # noqa: BLE001
            die("the display can't reach this PC. Allow Python through your firewall (Windows: allow "
                "python.exe on private networks) and make sure both are on the same network.")
        (outdir / ".ping").unlink(missing_ok=True)

        say("[2/6] Backing up the flash (about 170 MB)...")
        backup(con, info, rx, pc, outdir)
        say(f"  backup verified: {outdir}")

        say("[3/6] Making U-Boot reachable (bootdelay=3)...")
        if con.out("fw_printenv -n bootdelay", check=False) != "3":
            con.run("fw_setenv bootdelay 3")

        say("[4/6] Building the custom firmware from your stock firmware...")
        dev = info["vols"][f"rootfs{run}"][0]
        stock = (outdir / f"{dev}-rootfs{run}.bin").read_bytes()
        root = customize(squashfs.read(stock), ROOT / "firmware" / "overlay", stamp)
        img = squashfs.write(root)
        if len(img) >= info["vols"][f"rootfs{tgt}"][1]:
            die("the image is too big for the slot")
        md5 = hashlib.md5(img).hexdigest()
        stage = outdir / "stage"
        stage.mkdir(exist_ok=True)
        (stage / "rootfs.sqfs").write_bytes(img)
        (stage / "payload.tar").write_bytes(build_payload(tz if not info["tz"] else "", args.hostname, keys, mqtt))
        say(f"  image {len(img) / 1e6:.1f} MB, md5 {md5}")

        say("[5/6] Sending it to the display and writing slot " + tgt + "...")
        http = start_http(stage)
        url = f"http://{pc}:{http.server_address[1]}"
        con.run(f"rm -rf {STAGE} && mkdir -p {STAGE}")
        con.run(f"wget -q -O {STAGE}/rootfs.sqfs {url}/rootfs.sqfs", timeout=600)
        con.run(f"wget -q -O {STAGE}/payload.tar {url}/payload.tar", timeout=120)
        http.shutdown()
        pmd5 = hashlib.md5((stage / "payload.tar").read_bytes()).hexdigest()
        if con.out(f"md5sum {STAGE}/payload.tar").split()[0] != pmd5:
            die("the payload was damaged on the way to the display; run the installer again")
        con.run(f"cd {STAGE} && tar -xf payload.tar")
        env = "FORCE=1 " if args.force else ""
        st, outp = con.run(f"cd {STAGE} && {env}sh install.sh {tgt} {md5} {len(img)} 2>&1", timeout=900, check=False)
        for ln in outp.split("\n"):
            say("  " + ln)
        if "INSTALL-OK" not in outp:
            die("installation failed (see above). Nothing was switched: the display still boots slot " + run + ".")

        say(f"[6/6] Switching to slot {tgt} and rebooting...")
        env_now = con.out(f"sh {STAGE}/switch.sh {tgt}")   # a checksum-verified script, not typed commands
        if f"boot_partition=boot{tgt}" not in env_now or f"root_partition=rootfs{tgt}" not in env_now:
            die("couldn't switch slots:\n" + env_now)
        con.type(b"sync; reboot\n")
        wait_boot(con, tgt)
        for f in ("rootfs.sqfs", "payload.tar"):   # the payload may hold the MQTT password
            (stage / f).unlink(missing_ok=True)
        stage.rmdir()
    finally:
        con.close()


def wait_boot(con, slot, timeout=240):
    say("  waiting for the display to boot...")
    time.sleep(25)
    end = time.time() + timeout
    while time.time() < end:
        if con.wake(timeout=3):
            try:
                rel = con.out("cat /etc/custom_release 2>/dev/null", check=False, timeout=10)
                if rel.startswith("custom-"):
                    ip = None
                    t2 = time.time() + 90
                    while time.time() < t2 and not ip:
                        m = re.search(r"inet (\d+\.\d+\.\d+\.\d+)", con.out("ip -4 addr show wlan0", check=False, timeout=10))
                        ip = m.group(1) if m else None
                        if not ip:
                            time.sleep(5)
                    con.run(f"rm -rf {STAGE}", check=False)
                    say(f"\nDone: slot {slot} runs the custom firmware ({rel}).")
                    if ip:
                        say(f"  SSH:  ssh root@{ip}")
                    else:
                        say("  Wi-Fi isn't connected yet. Reconnect the panel: the display opens a setup hotspot "
                            "and shows its password there.")
                    say("Now unplug it, reconnect the panel cables, and close the case.\n"
                        "Back to stock later: python pixeld_install.py --rollback")
                    return
            except (TimeoutError, RuntimeError):
                pass
        time.sleep(5)
    die("the display didn't come back with the custom firmware in time. Check the serial log; to go back to "
        "stock, run this with --rollback (or use U-Boot, see docs/INSTALL.md).")


def rollback(args):
    con = connect(pick_port(args.port), None)
    try:
        info = device_info(con)
        if not info["custom"]:
            die("this display already runs the stock firmware.")
        stock = con.out("cat /data/custom/stock_slot 2>/dev/null", check=False) or ("B" if info["running"] == "A" else "A")
        if stock == info["running"]:
            die("the running slot is the recorded stock slot; nothing to do.")
        if not confirm(f"Switch to slot {stock} (stock firmware) and reboot? /data/custom stays, so you can switch back.", args.yes):
            die("cancelled")
        con.run(f"fw_setenv boot_partition boot{stock} && fw_setenv root_partition rootfs{stock} && fw_setenv systemAB_next {stock}")
        env_now = con.out("fw_printenv boot_partition root_partition systemAB_next")
        if f"boot_partition=boot{stock}" not in env_now or f"root_partition=rootfs{stock}" not in env_now:
            die("couldn't switch slots:\n" + env_now)
        con.type(b"sync; reboot\n")
        say(f"Rebooting into slot {stock} (stock firmware).")
    finally:
        con.close()


def main():
    ap = argparse.ArgumentParser(description="Install pixeld custom firmware on a Govee H6631 over its serial console.")
    ap.add_argument("--port", help="serial port (e.g. COM6 or /dev/ttyUSB0); asks if there are several")
    ap.add_argument("--tz", help="POSIX time zone, e.g. EST5EDT,M3.2.0,M11.1.0 (asks if not set on the device)")
    ap.add_argument("--hostname", help="network name for the display (default govee-pixel)")
    ap.add_argument("--mqtt", choices=("pixel2mqtt", "direct", "keep"),
                    help="how Home Assistant connects: pixel2mqtt (recommended), direct MQTT, or keep what's there")
    ap.add_argument("--mqtt-host", help="direct MQTT: broker address")
    ap.add_argument("--mqtt-port", type=int, help="direct MQTT: broker port (default 1883)")
    ap.add_argument("--mqtt-user", help="direct MQTT: user (password from PIXELD_MQTT_PASSWORD or a prompt)")
    ap.add_argument("--key", help="SSH public key file to authorise (default: ~/.ssh/id_ed25519.pub etc.)")
    ap.add_argument("--backup-dir", help="where to save the flash backup (default ./h6631-backup-<time>)")
    ap.add_argument("--rollback", action="store_true", help="switch back to the stock firmware")
    ap.add_argument("--yes", action="store_true", help="don't ask for confirmation")
    ap.add_argument("--force", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--build-only", metavar="STOCK_ROOTFS", help="just build the image from a rootfs dump (testing)")
    args = ap.parse_args()
    if args.build_only:
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")
        root = customize(squashfs.read(pathlib.Path(args.build_only).read_bytes()), ROOT / "firmware" / "overlay", stamp)
        img = squashfs.write(root)
        pathlib.Path("rootfs-custom.sqfs").write_bytes(img)
        say(f"rootfs-custom.sqfs {len(img)} bytes md5 {hashlib.md5(img).hexdigest()}")
        return
    try:
        rollback(args) if args.rollback else install(args)
    except KeyboardInterrupt:
        die("interrupted. If this happened before step 6 the display still boots its old slot.")


if __name__ == "__main__":
    main()
