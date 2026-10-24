"""Bochs Graphics Array 与 framebuffer PNG 导出（FYXOS Unicorn 台架）。"""
from __future__ import annotations

import ctypes
import os
import struct
import zlib

from unicorn import UC_PROT_ALL
from unicorn.x86_const import UC_X86_REG_RAX


class BGADevice:
    INDEX_PORT = 0x1CE
    DATA_PORT = 0x1CF
    FB_PHYS = 0xE0000000
    WIDTH = 800
    HEIGHT = 600
    BPP = 32
    PITCH = WIDTH * 4
    VISIBLE_SIZE = PITCH * HEIGHT
    MAP_SIZE = (VISIBLE_SIZE + 0xFFF) & ~0xFFF

    def __init__(self, emu, quiet=False):
        self.emu = emu
        self.quiet = quiet
        self.index = 0
        self.regs = {
            0: 0xB0C5,
            1: 0,
            2: 0,
            3: 0,
            4: 0,
            6: self.WIDTH,
            7: self.HEIGHT,
            8: 0,
            9: 0,
            10: (self.MAP_SIZE + 65535) // 65536,
        }
        self.fb = (ctypes.c_ubyte * self.MAP_SIZE)()
        self.fb_addr = ctypes.addressof(self.fb)

    def map(self, uc):
        uc.mem_map_ptr(self.FB_PHYS, self.MAP_SIZE, UC_PROT_ALL, self.fb_addr)

    def handles(self, port):
        return port in (self.INDEX_PORT, self.DATA_PORT)

    def _io_in(self, uc, port, size, user_data=None):
        value = self.regs.get(self.index, 0) if port == self.DATA_PORT else self.index
        value &= (1 << (size * 8)) - 1
        uc.reg_write(UC_X86_REG_RAX, value)
        if getattr(self.emu, "trace_io", False):
            print("BGA  in%d  port=0x%04x -> 0x%x" % (size * 8, port, value))

    def _io_out(self, uc, port, size, value, user_data=None):
        value &= (1 << (size * 8)) - 1
        if port == self.INDEX_PORT:
            self.index = value
        else:
            self.regs[self.index] = value
        if getattr(self.emu, "trace_io", False):
            print("BGA  out%d port=0x%04x <- 0x%x" % (size * 8, port, value))

    def visible_bytes(self):
        return bytes(self.fb[:self.VISIBLE_SIZE])

    def checksum(self):
        hash_value = 2166136261
        for value in self.visible_bytes():
            hash_value ^= value
            hash_value = (hash_value * 16777619) & 0xFFFFFFFF
        return hash_value

    @staticmethod
    def _chunk(kind, data):
        return (struct.pack("!I", len(data)) + kind + data +
                struct.pack("!I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    def save_png(self, path):
        """设备是 BGRX little-endian；PNG 输出为 RGB888。"""
        raw = bytearray()
        pixels = self.visible_bytes()
        for row in range(self.HEIGHT):
            raw.append(0)                         # PNG filter: None
            start = row * self.PITCH
            for col in range(self.WIDTH):
                b, g, r, _ = pixels[start + col * 4:start + col * 4 + 4]
                raw.extend((r, g, b))
        header = struct.pack("!IIBBBBB", self.WIDTH, self.HEIGHT,
                             8, 2, 0, 0, 0)
        png = (b"\x89PNG\r\n\x1a\n" + self._chunk(b"IHDR", header) +
               self._chunk(b"IDAT", zlib.compress(bytes(raw), 6)) +
               self._chunk(b"IEND", b""))
        directory = os.path.dirname(os.path.abspath(path))
        os.makedirs(directory, exist_ok=True)
        with open(path, "wb") as output:
            output.write(png)
        print("[fyxemu] framebuffer 已保存到 %s（%dx%d，checksum=0x%08x）" %
              (path, self.WIDTH, self.HEIGHT, self.checksum()))
