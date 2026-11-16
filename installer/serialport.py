"""Tiny serial port wrapper (Python standard library only): Windows via ctypes, Linux/macOS via termios.

Opens the port raw 8N1 with DTR/RTS left alone and no flow control. read() returns whatever arrived within
about 50 ms (possibly b"").
"""
import os
import sys


def list_ports():
    if sys.platform == "win32":
        import winreg
        out = []
        try:
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
        except OSError:
            return out
        i = 0
        while True:
            try:
                _, val, _ = winreg.EnumValue(k, i)
            except OSError:
                break
            out.append(val)
            i += 1
        return sorted(out, key=lambda s: (len(s), s))
    import glob
    pats = ["/dev/ttyUSB*", "/dev/ttyACM*"] if sys.platform.startswith("linux") else ["/dev/cu.usb*", "/dev/cu.SLAB*", "/dev/cu.wch*"]
    return sorted(p for pat in pats for p in glob.glob(pat))


if sys.platform == "win32":
    import ctypes
    from ctypes import wintypes

    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class _DCB(ctypes.Structure):
        _fields_ = [("DCBlength", wintypes.DWORD), ("BaudRate", wintypes.DWORD), ("flags", wintypes.DWORD),
                    ("wReserved", wintypes.WORD), ("XonLim", wintypes.WORD), ("XoffLim", wintypes.WORD),
                    ("ByteSize", ctypes.c_ubyte), ("Parity", ctypes.c_ubyte), ("StopBits", ctypes.c_ubyte),
                    ("XonChar", ctypes.c_char), ("XoffChar", ctypes.c_char), ("ErrorChar", ctypes.c_char),
                    ("EofChar", ctypes.c_char), ("EvtChar", ctypes.c_char), ("wReserved1", wintypes.WORD)]

    class _TIMEOUTS(ctypes.Structure):
        _fields_ = [("ReadIntervalTimeout", wintypes.DWORD), ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
                    ("ReadTotalTimeoutConstant", wintypes.DWORD), ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
                    ("WriteTotalTimeoutConstant", wintypes.DWORD)]

    _k32.CreateFileW.restype = wintypes.HANDLE
    _INVALID = wintypes.HANDLE(-1).value

    class Serial:
        def __init__(self, port, baud):
            name = port if port.startswith("\\\\.\\") else "\\\\.\\" + port
            h = _k32.CreateFileW(name, 0xC0000000, 0, None, 3, 0, None)   # GENERIC_READ|WRITE, OPEN_EXISTING
            if h == _INVALID or h is None:
                raise OSError(f"can't open {port} (in use by another program?)")
            self.h = h
            _k32.SetupComm(h, 1 << 16, 1 << 16)
            dcb = _DCB()
            dcb.DCBlength = ctypes.sizeof(_DCB)
            if not _k32.GetCommState(h, ctypes.byref(dcb)):
                raise OSError("GetCommState failed")
            dcb.BaudRate = baud
            dcb.flags = 1                      # fBinary only: no parity check, no flow control, DTR/RTS disabled
            dcb.ByteSize, dcb.Parity, dcb.StopBits = 8, 0, 0
            if not _k32.SetCommState(h, ctypes.byref(dcb)):
                raise OSError(f"{port}: baud rate {baud} not supported by this adapter")
            t = _TIMEOUTS(0xFFFFFFFF, 0xFFFFFFFF, 50, 0, 5000)   # return as soon as data arrives, or after 50 ms
            _k32.SetCommTimeouts(h, ctypes.byref(t))
            _k32.PurgeComm(h, 0x0F)

        def read(self, n=65536):
            buf = ctypes.create_string_buffer(n)
            got = wintypes.DWORD()
            if not _k32.ReadFile(self.h, buf, n, ctypes.byref(got), None):
                raise OSError("serial read failed")
            return buf.raw[:got.value]

        def write(self, data):
            got = wintypes.DWORD()
            if not _k32.WriteFile(self.h, data, len(data), ctypes.byref(got), None):
                raise OSError("serial write failed")

        def close(self):
            if self.h:
                _k32.CloseHandle(self.h)
                self.h = None
else:
    import select
    import termios

    class Serial:
        def __init__(self, port, baud):
            self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            a = termios.tcgetattr(self.fd)
            a[0] = 0                                       # iflag: raw
            a[1] = 0                                       # oflag
            a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            a[3] = 0                                       # lflag: no echo, non-canonical
            a[6][termios.VMIN], a[6][termios.VTIME] = 0, 0
            const = getattr(termios, f"B{baud}", None)
            if const is not None:
                a[4] = a[5] = const
                termios.tcsetattr(self.fd, termios.TCSANOW, a)
            else:                                          # macOS: set the speed with IOSSIOSPEED
                termios.tcsetattr(self.fd, termios.TCSANOW, a)
                import fcntl
                import struct
                fcntl.ioctl(self.fd, 0x80085402, struct.pack("L", baud))
            termios.tcflush(self.fd, termios.TCIOFLUSH)

        def read(self, n=65536):
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                return b""
            try:
                return os.read(self.fd, n)
            except BlockingIOError:
                return b""

        def write(self, data):
            while data:
                select.select([], [self.fd], [], 5)
                n = os.write(self.fd, data)
                data = data[n:]

        def close(self):
            if self.fd is not None:
                os.close(self.fd)
                self.fd = None
