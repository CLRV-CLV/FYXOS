#!/usr/bin/env python3
"""
FYXOS 仿真台架 (fyxemu)
========================

在没有 QEMU 的环境下，用 Unicorn 真实执行 FYXOS 的**完整启动链路**：

  BIOS 实模式 -> MBR(stage1) -> stage2 -> 32 位 -> 长模式 -> 内核 -> Shell

台架实现了最小 BIOS（INT 10h/13h/15h/16h）、8259 PIC、PIT 时钟、PS/2 键盘、
16550 串口，并把物理内存用 mem_map_ptr 在两个引擎间共享。

--------------------------------------------------------------------------
踩过的坑（改这个文件时请务必先读一遍，避免重复调试）
--------------------------------------------------------------------------
1. **寄存器宽度被"引擎创建时的模式"锁定**
   用 UC_MODE_16 创建的引擎，即便客户机已切到 64 位长模式，也只能读写
   EIP/ESP/EAX/RFLAGS（16/32 位名字）；读 RIP/RSP/RAX 恒为 0。
   => 因此必须**两阶段**：阶段 1 用 MODE_16 跑实模式引导，阶段 2 用 MODE_64
      跑 64 位内核。两阶段通过 mem_map_ptr 共享同一块宿主内存，无需快照拷贝。

2. **用 API 直接写 CR0/CR3/CR4/EFER 并不能真正启用分页**
   实测：API 写完 CR0.PG=1 后，虚拟地址会被当成物理地址直接使用。
   必须由客户机代码自己执行 `mov cr3/cr4/cr0` + `wrmsr` 才会真正启用 MMU。
   => 阶段 2 会注入一段 trampoline，让客户机自己重新打开分页，再跳内核入口。

3. **uc.mem_read / mem_write 走的是物理地址**
   访问高半核（0xFFFFFFFF8...）的虚拟地址会失败。
   => 台架自己实现了 translate() 做四级页表遍历。

4. **钩子里的寄存器读回"落后一条指令"**
   CODE 钩子在执行前触发，所以读到的是该指令执行前的状态 —— 这正是我们
   需要的语义（在 HLT 处注入中断时，RSP/RFLAGS 都是正确的）。
   INTR 钩子里 RIP 已经指向 `int` 之后，可直接作为中断返回地址。

5. **`int n` 在 Unicorn 2.1.4 的长模式下无法自行投递**
   不注册 INTR 钩子会直接抛 "Unhandled CPU exception"；注册了钩子则会被吞掉。
   => 台架在 INTR 钩子里**手工投递**：查 IDT 门、手工压 SS/RSP/RFLAGS/CS/RIP、
      清 IF、跳门里的入口。外部中断（PIT/键盘）用同一套代码注入。

6. **OUT 钩子的 value 参数不可靠**（偶发为 0，TCG 惰性同步）
   => 主输出不依赖端口，而是直接读内核日志环形缓冲（在物理内存里，稳定）。

用法：
    fyxemu.py --image build/fyxos.img --mode full-boot
    fyxemu.py --iso build/fyxos.iso --kernel build/fyxos.elf --mode full-boot
    fyxemu.py --kernel build/fyxos.elf --mode quick
    fyxemu.py --image build/fyxos.img --test
    fyxemu.py --image build/fyxos.img --keys "help;uptime;mem"
"""
import argparse
import ctypes
import os
import re
import struct
import sys
import time

try:
    from unicorn import (Uc, UcError, UC_ARCH_X86, UC_MODE_16, UC_MODE_64,
                         UC_PROT_ALL, UC_HOOK_INTR, UC_HOOK_INSN, UC_HOOK_CODE)
    from unicorn.x86_const import (
        UC_X86_REG_RIP, UC_X86_REG_EIP, UC_X86_REG_RSP, UC_X86_REG_ESP,
        UC_X86_REG_RFLAGS, UC_X86_REG_EFLAGS, UC_X86_REG_RAX, UC_X86_REG_EAX,
        UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI,
        UC_X86_REG_EDI, UC_X86_REG_EBP,
        UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_RBX, UC_X86_REG_RSI,
        UC_X86_REG_RDI, UC_X86_REG_RBP, UC_X86_REG_R8, UC_X86_REG_R9,
        UC_X86_REG_R10, UC_X86_REG_R11, UC_X86_REG_R12, UC_X86_REG_R13,
        UC_X86_REG_R14, UC_X86_REG_R15, UC_X86_REG_CS, UC_X86_REG_DS,
        UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_FS, UC_X86_REG_GS,
        UC_X86_REG_CR0, UC_X86_REG_CR2, UC_X86_REG_CR3, UC_X86_REG_CR4, UC_X86_REG_IDTR,
        UC_X86_REG_GDTR, UC_X86_REG_MSR,
        UC_X86_INS_IN, UC_X86_INS_OUT, UC_X86_INS_SYSCALL)
except ImportError:
    print("fyxemu: 需要 unicorn 引擎，请先执行： pip install unicorn",
          file=sys.stderr)
    sys.exit(2)

from rtl8139 import RTL8139Device
from bga import BGADevice
from isolive import IsoFormatError, read_live_iso

# --------------------------------------------------------------------------
# 常量
# --------------------------------------------------------------------------
KERNEL_VMA       = 0xFFFFFFFF80000000
KERNEL_LMA       = 0x0000000000200000
HHDM_BASE        = 0xFFFF800000000000
HANDOFF_THRESH   = 0xFFFF800000000000     # 进入高半核即认为阶段 1 完成

MBR_ADDR         = 0x7C00
TRAMPOLINE_ADDR  = 0x7000                 # 阶段 2 注入的长模式 trampoline
PARAMS_ADDR      = 0x6200                 # trampoline 参数（避开 stage2 的 0x6000）
# stage2 -> 长模式的暂存单元（boot/stage2_16.S 里也是这几个地址，务必一致）。
# 曾经写成 0x6000/0x6008/0x6010，与 stage2 实际使用的 0x0600/0x0608/0x0610 差一位，
# trampoline 读到全 0，于是跳到 RIP=0、RSP=0，第一条指令就 #PF。
SCRATCH_ENTRY    = 0x0600                 # stage2 留下的内核入口
SCRATCH_STACK    = 0x0608
SCRATCH_BOOTINFO = 0x0610

COM1             = 0x3F8
KB_DATA          = 0x60
KB_STATUS        = 0x64
DEBUG_PORT       = 0x501                  # 内核关机/崩溃端口

IRQ_TIMER        = 0
IRQ_KEYBOARD     = 1
IRQ_BASE_VECTOR  = 32

IF_BIT           = 0x200
PAGE_PRESENT     = 0x001
PAGE_PS          = 0x080
PTE_ADDR_MASK    = 0x000FFFFFFFFFF000

# 内核日志结构（include/fyxos/kernel.h struct fyx_log）的字段偏移
KLOG_OFF_HEAD    = 8
KLOG_OFF_PANIC   = 20
KLOG_OFF_BUF     = 32

# 调试端口约定
EXIT_OK          = 0x00
EXIT_EXCEPTION   = 0xFE
EXIT_PANIC       = 0xFF


def log(msg):
    print(msg, flush=True)


# --------------------------------------------------------------------------
# 极简 ELF64 解析器（用于定位内核符号）
# --------------------------------------------------------------------------
class Elf:
    def __init__(self, path=None, data=None):
        if data is None:
            with open(path, "rb") as f:
                data = f.read()
        self.data = data
        if self.data[:4] != b"\x7fELF":
            raise ValueError("不是 ELF 文件")
        (self.e_phoff, self.e_shoff) = struct.unpack_from("<QQ", self.data, 32)
        (self.e_phentsize, self.e_phnum, self.e_shentsize,
         self.e_shnum, self.e_shstrndx) = struct.unpack_from("<HHHHH",
                                                             self.data, 54)
        self.segments = []
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            p_type, p_flags = struct.unpack_from("<II", self.data, off)
            (p_offset, p_vaddr, p_paddr) = struct.unpack_from("<QQQ",
                                                              self.data, off + 8)
            (p_filesz, p_memsz, p_align) = struct.unpack_from("<QQQ",
                                                              self.data, off + 32)
            self.segments.append(dict(type=p_type, flags=p_flags,
                                      offset=p_offset, vaddr=p_vaddr,
                                      paddr=p_paddr, filesz=p_filesz,
                                      memsz=p_memsz, align=p_align))
        self.symbols = {}
        self._load_symbols()

    def _load_symbols(self):
        shs = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
             sh_link, sh_info, sh_align, sh_entsize) = struct.unpack_from(
                "<IIQQQQIIQQ", self.data, off)
            shs.append(dict(name=sh_name, type=sh_type, addr=sh_addr,
                            offset=sh_offset, size=sh_size, link=sh_link,
                            entsize=sh_entsize))
        if not shs:
            return
        strtab = shs[shs[self.e_shstrndx].get("link", 0)] if False else None
        for sh in shs:
            if sh["type"] != 2:          # SHT_SYMTAB
                continue
            strtab = shs[sh["link"]]
            strdata = self.data[strtab["offset"]:
                                strtab["offset"] + strtab["size"]]
            n = sh["size"] // sh["entsize"]
            for i in range(n):
                off = sh["offset"] + i * sh["entsize"]
                st_name, st_info, st_other, st_shndx, st_value, st_size = \
                    struct.unpack_from("<IBBHQQ", self.data, off)
                end = strdata.find(b"\0", st_name)
                if end < 0:
                    continue
                name = strdata[st_name:end].decode("utf-8", "replace")
                if name:
                    self.symbols[name] = (st_value, st_size)

    def vaddr_to_paddr(self, vaddr):
        for s in self.segments:
            if s["type"] != 1:           # PT_LOAD
                continue
            if s["vaddr"] <= vaddr < s["vaddr"] + s["memsz"]:
                return s["paddr"] + (vaddr - s["vaddr"])
        return None

    def symbol(self, name):
        return self.symbols.get(name)


# --------------------------------------------------------------------------
# 台架
# --------------------------------------------------------------------------
class FyxEmu:
    def __init__(self, image=None, iso=None, kernel=None, ram_mb=64, mode="full-boot",
                 keys=None, show_bios=False, max_seconds=120, quiet=False,
                 tick_every=20000, chunk=20000, trace_syscalls=False,
                 keys_after=None, writeback=False, disk_output=None,
                 power_fail_after=None, power_fail_write=1, no_net=False,
                 no_gfx=False, fb_output=None):
        self.image_path = image
        self.iso_path = iso
        self.kernel_path = kernel
        self.ram_bytes = ram_mb * 1024 * 1024
        self.mode = mode
        self.key_script = keys or ""
        self.keys_after = keys_after
        self.keys_armed = not keys_after
        self.show_bios = show_bios
        self.max_seconds = max_seconds
        self.quiet = quiet
        self.tick_every = tick_every
        self.chunk = chunk

        # 共享物理内存：一块宿主缓冲区，两个引擎都映射它
        self.ram = (ctypes.c_ubyte * self.ram_bytes)()
        self.ram_addr = ctypes.addressof(self.ram)

        self.writeback = writeback
        self.disk_output = disk_output
        self.disk_dirty = False
        self.disk = bytearray()
        self.cd = bytearray()
        self.cd_info = None
        if image:
            with open(image, "rb") as f:
                self.disk = bytearray(f.read())
        elif iso:
            # 先从 ISO9660/El Torito 描述定位启动载荷，再模拟 BIOS 的
            # hard-disk emulation。内核阶段则通过下面的 ATAPI 模型直接读 CD。
            with open(iso, "rb") as f:
                self.cd = bytearray(f.read())
            self.cd_info = read_live_iso(self.cd)
            self.disk = bytearray(self.cd_info.image)
        elif mode == "quick":
            # quick 模式也提供一块带合法系统分区的 32 MiB ATA RAM 盘，
            # 因而文件系统与 exec 路径不再依赖 full-boot 的镜像输入。
            self.disk = bytearray(32 * 1024 * 1024)
            total = len(self.disk) // 512
            part = bytearray(16)
            part[4] = 0x83
            struct.pack_into("<II", part, 8, 2048, total - 2048)
            self.disk[0x1BE:0x1BE + 16] = part
            self.disk[510:512] = b"\x55\xAA"

        self.elf = None
        if kernel:
            self.elf = Elf(kernel)
        elif image or iso:
            # full-boot 也允许只传 --image/--iso：从 MBR 的 FYXB 描述符取出
            # 内核 ELF（布局与 tools/mkimage.py 完全一致），用于符号解析
            # 以及用户态 iretq 仿真。尾部扇区填充不影响 ELF 解析。
            try:
                desc = self.disk[0x1B0:0x1B0 + 14]
                if desc[:4] == b"FYXB":
                    kernel_lba, kernel_sectors = struct.unpack_from("<II", desc, 6)
                    start = kernel_lba * 512
                    end = start + kernel_sectors * 512
                    self.elf = Elf(data=self.disk[start:end])
            except (ValueError, struct.error, IndexError):
                self.elf = None

        # 运行状态
        self.uc = None
        self.phase = 0
        self.handoff = None
        self.instructions = 0
        self.exit_code = None
        self.shutdown_code = None
        self.pending_irqs = []
        self.key_queue = []
        self.key_index = 0
        self.bios_text = bytearray()
        self.serial_out = bytearray()
        self.klog_pos = 0
        self.console_out = bytearray()
        self.stopped = False
        self.error = None
        self.start_wall = 0
        self.ticks_injected = 0
        self.irqs_injected = 0
        self.fast_syscalls = 0
        self._syscall_fixup = False
        self._syscall_target = 0
        self._user_irq_grace = False
        self.iret_addr = 0
        self.HAS_ERR = frozenset((8, 10, 11, 12, 13, 14, 17, 21, 29, 30))
        self.tlb = {}
        self.tlb_cr3 = None
        self.last_cr3 = None
        self.trace_io = False
        self.net = None if no_net else RTL8139Device(self, quiet)
        self.bga = None if no_gfx else BGADevice(self, quiet)
        self.fb_output = fb_output

        # ATA PIO 模型（端口 0x1F0..0x1F7 / 0x3F6）。与真机一样，命令先
        # 设置 LBA/计数寄存器，再通过 DATA 端口逐字传输 256 个 word。
        self.ata_regs = {0x1F1: 0, 0x1F2: 0, 0x1F3: 0,
                         0x1F4: 0, 0x1F5: 0, 0x1F6: 0xA0}
        self.ata_status = 0x50           # DRDY | DSC
        self.ata_buffer = bytearray()
        self.ata_pos = 0
        self.ata_write_lba = None
        self.atapi_receiving = False
        self.atapi_packet = bytearray()
        self.ata_writes = 0
        self.power_fail_after = power_fail_after
        self.power_fail_write = power_fail_write
        self.power_fail_armed = False
        self.power_fail_writes_seen = 0
        self.power_failed = False

        # 阶段 2 的 trampoline（让客户机自己打开分页，见文件头说明第 2 条）
        self.trampoline = bytes([
            # mov rax,[PARAMS+0]  ; mov cr3,rax
            0x48, 0x8B, 0x04, 0x25, PARAMS_ADDR & 0xFF, (PARAMS_ADDR >> 8) & 0xFF,
            (PARAMS_ADDR >> 16) & 0xFF, (PARAMS_ADDR >> 24) & 0xFF,
            0x0F, 0x22, 0xD8,
            # mov rax,[PARAMS+8]  ; mov cr4,rax
            0x48, 0x8B, 0x04, 0x25, (PARAMS_ADDR + 8) & 0xFF,
            ((PARAMS_ADDR + 8) >> 8) & 0xFF, ((PARAMS_ADDR + 8) >> 16) & 0xFF,
            ((PARAMS_ADDR + 8) >> 24) & 0xFF,
            0x0F, 0x22, 0xE0,
            # mov ecx,0xC0000080 ; mov eax,[PARAMS+16] ; xor edx,edx ; wrmsr
            0xB9, 0x80, 0x00, 0x00, 0xC0,
            0x8B, 0x04, 0x25, (PARAMS_ADDR + 16) & 0xFF,
            ((PARAMS_ADDR + 16) >> 8) & 0xFF, ((PARAMS_ADDR + 16) >> 16) & 0xFF,
            ((PARAMS_ADDR + 16) >> 24) & 0xFF,
            0x31, 0xD2,
            0x0F, 0x30,
            # mov rax,[PARAMS+24] ; mov cr0,rax
            0x48, 0x8B, 0x04, 0x25, (PARAMS_ADDR + 24) & 0xFF,
            ((PARAMS_ADDR + 24) >> 8) & 0xFF, ((PARAMS_ADDR + 24) >> 16) & 0xFF,
            ((PARAMS_ADDR + 24) >> 24) & 0xFF,
            0x0F, 0x22, 0xC0,
            # 复刻 stage2 的 lm_trampoline：载入 RSP/RDI，再跳内核入口
            0x48, 0x8B, 0x24, 0x25, SCRATCH_STACK & 0xFF,
            (SCRATCH_STACK >> 8) & 0xFF, (SCRATCH_STACK >> 16) & 0xFF,
            (SCRATCH_STACK >> 24) & 0xFF,
            0x48, 0x8B, 0x3C, 0x25, SCRATCH_BOOTINFO & 0xFF,
            (SCRATCH_BOOTINFO >> 8) & 0xFF, (SCRATCH_BOOTINFO >> 16) & 0xFF,
            (SCRATCH_BOOTINFO >> 24) & 0xFF,
            0x48, 0x8B, 0x04, 0x25, SCRATCH_ENTRY & 0xFF,
            (SCRATCH_ENTRY >> 8) & 0xFF, (SCRATCH_ENTRY >> 16) & 0xFF,
            (SCRATCH_ENTRY >> 24) & 0xFF,
            0xFF, 0xE0,
        ])

    # ------------------------- 寄存器访问（按阶段选宽度） ------------------
    def _rip(self):
        return UC_X86_REG_RIP if self.phase == 2 else UC_X86_REG_EIP

    def _sp(self):
        return UC_X86_REG_RSP if self.phase == 2 else UC_X86_REG_ESP

    def _flags(self):
        return UC_X86_REG_RFLAGS if self.phase == 2 else UC_X86_REG_EFLAGS

    def get_rip(self):
        return self.uc.reg_read(self._rip())

    def set_rip(self, v):
        self.uc.reg_write(self._rip(), v)

    # ---------------------------- 内存与地址转换 ---------------------------
    def phys_read(self, addr, size):
        return bytes(self.uc.mem_read(addr, size))

    def read_at(self, addr, size):
        """
        按地址性质读取：高半核地址是虚拟地址（需走页表），低地址当作物理地址。
        IDTR/GDTR 里存的基址是虚拟地址，早期引导阶段又往往是物理地址，
        所以两种都要支持。
        """
        if addr >= HANDOFF_THRESH:
            data = self.virt_read(addr, size)
            if data is None:
                raise UcError(6)          # UC_ERR_READ_UNMAPPED
            return data
        return self.phys_read(addr, size)

    def phys_write(self, addr, data):
        self.uc.mem_write(addr, bytes(data))

    def translate(self, vaddr):
        """四级页表遍历：虚拟地址 -> 物理地址；失败返回 None"""
        cr3 = self.uc.reg_read(UC_X86_REG_CR3) & PTE_ADDR_MASK
        if self.tlb_cr3 != cr3:
            # 用户进程可复用同一虚拟地址；CR3 改变后旧翻译绝不能命中。
            self.tlb.clear()
            self.tlb_cr3 = cr3
        cached = self.tlb.get(vaddr >> 12)
        if cached is not None:
            return cached | (vaddr & 0xFFF)
        try:
            level = cr3
            for shift in (39, 30, 21):
                entry = struct.unpack("<Q", self.phys_read(
                    (level & PTE_ADDR_MASK) + ((vaddr >> shift) & 0x1FF) * 8,
                    8))[0]
                if not (entry & PAGE_PRESENT):
                    return None
                if (entry & PAGE_PS) and shift != 39:
                    phys = (entry & 0x000FFFFFFFE00000) | (vaddr & 0x1FFFFF)
                    return phys
                level = entry
            entry = struct.unpack("<Q", self.phys_read(
                (level & PTE_ADDR_MASK) + ((vaddr >> 12) & 0x1FF) * 8, 8))[0]
            if not (entry & PAGE_PRESENT):
                return None
            phys = (entry & PTE_ADDR_MASK) | (vaddr & 0xFFF)
            self.tlb[vaddr >> 12] = entry & PTE_ADDR_MASK
            return phys
        except UcError:
            return None

    def virt_read(self, vaddr, size):
        phys = self.translate(vaddr)
        if phys is None:
            return None
        # 跨页读取时逐字节处理（日志缓冲读取会跨页）
        if (phys & 0xFFF) + size > 0x1000:
            out = bytearray()
            for i in range(size):
                p = self.translate(vaddr + i)
                if p is None:
                    return None
                out += self.phys_read(p, 1)
            return bytes(out)
        return self.phys_read(phys, size)

    def virt_write(self, vaddr, data):
        phys = self.translate(vaddr)
        if phys is None:
            return False
        self.phys_write(phys, data)
        return True

    # ------------------------------ 中断投递 -------------------------------
    def gate_entry(self, vector):
        idtr = self.uc.reg_read(UC_X86_REG_IDTR)
        base = idtr[1] if isinstance(idtr, tuple) else 0
        g = self.read_at(base + vector * 16, 16)
        lo, sel, attr, mid, hi = struct.unpack("<HHHHI", g[:12])
        # 64 位中断门的属性字：bit15 = P（存在），bit8..11 = 类型，bit13..14 = DPL。
        # 注意不是 bit7 —— 早期版本判成 0x80，导致所有门都被当成"不存在"。
        if not (attr & 0x8000):
            return None
        return (lo | (mid << 16) | (hi << 32), sel, attr)

    # ------------------------------------------------- TSS（ring3 陷入取内核栈）
    def tss_rsp0(self):
        """
        从 GDTR 指向的 GDT 里找出 TSS 描述符，读它的 RSP0。
        ring 3 执行时来了中断，CPU 会无条件用 TSS.RSP0 当内核栈；台架必须照做，
        否则中断帧会被压到用户栈上 —— 而用户栈对内核代码是不可信的。
        """
        uc = self.uc
        try:
            gdtr = uc.reg_read(UC_X86_REG_GDTR)
            base, limit = gdtr[1], gdtr[2]
        except Exception:
            return 0
        idx = 0x28 >> 3                       # GDT_SEL_TSS
        off = base + idx * 8
        if off + 8 > base + limit:
            return 0
        # 注意：uc.mem_read 走的是物理地址，而 GDTR 基址与 TSS 基址都是内核
        # 高半核的虚拟地址，必须用走页表的 read_at。
        desc = self.read_at(off, 16)          # 64 位 TSS 描述符占 16 字节
        tss_base = (desc[2] | (desc[3] << 8) | (desc[4] << 16) | (desc[7] << 24)
                    | (desc[8] << 32) | (desc[9] << 40)
                    | (desc[10] << 48) | (desc[11] << 56))
        if tss_base == 0:
            return 0
        return struct.unpack('<Q', self.read_at(tss_base + 4, 8))[0]

    # -------------------------------------------- SYSCALL / SYSRET 手工仿真
    MSR_STAR   = 0xC0000081
    MSR_LSTAR  = 0xC0000082
    MSR_SFMASK = 0xC0000084

    def msr_read(self, index, default=0):
        try:
            v = self.uc.reg_read(UC_X86_REG_MSR, index)
            if isinstance(v, (tuple, list)):
                v = v[0]
            return int(v)
        except Exception:
            return default

    def write_at(self, addr, data):
        """按当前页表写入；跨页时逐字节处理。返回是否全部写成功"""
        data = bytes(data)
        # 阶段 2 的地址都按当前 CR3 解释；逐字节写可同时处理高半核
        # 虚拟地址与跨页帧，不能退化成 uc.mem_write 的物理写入。
        for i in range(len(data)):
            phys = self.translate(addr + i)
            if phys is None:
                return False
            self.phys_write(phys, data[i:i + 1])
        return True

    def exception_error_code(self, vector, cs):
        """
        估算异常错误码。Unicorn 不会把错误码交给钩子，只能按现场还原：
        真机的 #PF 错误码最低三位是 P / W-R / U-S，bit4 表示取指。
        """
        if vector != 14:
            return 0
        cr2 = self.uc.reg_read(UC_X86_REG_CR2)
        err = 0
        if self.translate(cr2) is not None:
            err |= 0x01                     # 页在 -> 是权限违例而非缺页
        if (cs & 3) == 3:
            err |= 0x04                     # 用户态访问
        if cr2 == self.get_rip():
            err |= 0x10                     # 取指令导致的缺页
        return err

    def emulate_fast_syscall(self, rip):
        """
        按硬件语义模拟 syscall / sysret，返回 True 表示这条指令已处理。

        内核用的是真机上的 SYSCALL/SYSRET 快速路径；Unicorn 2.1.4 没有实现
        SYSRET（syscall 也不完整），遇到就抛 UC_ERR_INSN_INVALID。这是台架的
        缺口，不是内核的问题，所以在这里补齐，内核侧保持真机语义不动。
        """
        uc = self.uc
        # 阶段 2 里 RIP 永远是虚拟地址：内核在高半核，用户程序在低半核。
        # read_at() 为了兼容实模式，把低地址当物理地址，不能直接拿来取用户
        # 指令；必须先按当前 CR3 走页表。
        try:
            code = self.virt_read(rip, 3)
            if code is None:
                code = self.read_at(rip, 3)
        except Exception:
            return False

        if code[0] == 0x0F and code[1] == 0x05:
            kind, length = 'syscall', 2
        elif code[0] == 0x48 and code[1] == 0x0F and code[2] == 0x07:
            kind, length = 'sysret', 3
        elif code[0] == 0x0F and code[1] == 0x07:
            kind, length = 'sysret', 2
        else:
            return False

        star = self.msr_read(self.MSR_STAR)
        star_user = (star >> 48) & 0xFFFF
        kernel_cs = (star >> 32) & 0xFFFF

        if kind == 'syscall':
            lstar = self.msr_read(self.MSR_LSTAR)
            if lstar == 0 or kernel_cs == 0:
                return False
            sfmask = self.msr_read(self.MSR_SFMASK)
            rflags = uc.reg_read(UC_X86_REG_RFLAGS)
            uc.reg_write(UC_X86_REG_RCX, rip + length)     # 返回地址
            uc.reg_write(UC_X86_REG_R11, rflags)           # 保存 RFLAGS
            uc.reg_write(UC_X86_REG_RFLAGS, rflags & ~sfmask)
            uc.reg_write(UC_X86_REG_RIP, lstar)
            uc.reg_write(UC_X86_REG_CS, kernel_cs)
            uc.reg_write(UC_X86_REG_SS, (kernel_cs + 8) & 0xFFFF)
        else:
            # sysret：RIP = RCX，RFLAGS = R11，CS/SS 由 STAR[63:48] 算出
            rcx = uc.reg_read(UC_X86_REG_RCX)
            r11 = uc.reg_read(UC_X86_REG_R11)
            uc.reg_write(UC_X86_REG_RIP, rcx)
            uc.reg_write(UC_X86_REG_RFLAGS, r11)
            if star_user:
                uc.reg_write(UC_X86_REG_CS, (star_user + 16) & 0xFFFF)
                uc.reg_write(UC_X86_REG_SS, (star_user + 8) & 0xFFFF)
            # Unicorn 在“手工 sysret + 立刻手工投递中断”的边界上会偶尔
            # 对用户代码页报假的 #PF。先让用户态执行一条指令再开中断窗口；
            # 由 phase2_loop 用 count=1 实现，真机不需要也不执行这段代码。
            self._user_irq_grace = True
        self.fast_syscalls += 1
        return True

    def syscall_insn_hook(self, uc, user_data):
        """
        UC_HOOK_INSN 的 SYSCALL 回调。

        Unicorn 在没有该钩子时会把这个指令当成普通两字节指令跳过，既不
        跳 LSTAR 也不保存 RCX/R11。这里手工完成硬件语义，并停止当前仿真块；
        回调返回后 Unicorn 还会把 RIP 前进 2 字节，所以目标地址由
        phase2_loop 在 emu_start 返回后重新写回，不能只在回调里设置。
        """
        if not self.emulate_fast_syscall(self.get_rip()):
            return
        self._syscall_target = self.get_rip()
        self._syscall_fixup = True
        uc.emu_stop()

    def deliver(self, vector, return_rip, clear_if=True):
        """
        手工投递一次中断：压栈构造中断帧，再跳到 IDT 门里的入口。

        真机语义：只有特权级切换（ring3 -> ring0）时 CPU 才压 SS/RSP，
        同级中断只压 RFLAGS/CS/RIP 三项。

        但 Unicorn 2.1.4 的 64 位 iretq **无论是否同级返回都会弹出 5 槽并
        校验 SS**（实测：帧后第 5 槽是垃圾值 0x3f9f000 时直接 #GP，而全零
        或 0x10 则正常）。所以这里必须压满 5 槽：
          - 压入的 RSP 就是中断前的 RSP，iretq 弹出来等于原值，不会漂移；
          - SS 压入当前 SS，是合法的 0x10。
        内核侧仍按真机语义编写（isr_common 里 add rsp,16 再 iretq），
        两者在本台架上自洽。
        """
        g = self.gate_entry(vector)
        if g is None:
            return False
        entry, sel, _attr = g
        rip = return_rip
        rsp = self.uc.reg_read(self._sp())
        flags = self.uc.reg_read(self._flags())
        cs = self.uc.reg_read(UC_X86_REG_CS)
        ss = self.uc.reg_read(UC_X86_REG_SS)

        # 中断帧里保存的必须是**中断发生前**的 SS:RSP。换栈时若直接用内核
        # 栈顶当"被保存的 RSP"，iretq 返回后用户态的 RSP 就停在内核栈上了，
        # 用户程序再碰一次栈就立刻二次异常 —— 早期版本正是踩了这个坑。
        saved_rsp = rsp
        saved_ss = ss

        # ring3 -> ring0：CPU 会换到 TSS.RSP0 指向的内核栈。
        if (cs & 3) == 3:
            rsp0 = self.tss_rsp0()
            if rsp0:
                rsp = rsp0
                self.uc.reg_write(UC_X86_REG_SS, 0x10)

        # 真机语义：CPU 从 IDT 门里取出目标代码段装入 CS。不切 CS 的话，
        # 内核 ISR 会以 CPL=3 运行，一碰内核数据（无 US 位）就 #PF。
        self.uc.reg_write(UC_X86_REG_CS, sel)

        # 帧从低地址到高地址：RIP、CS、RFLAGS、RSP、SS（错误码压在最前面，
        # 也就是地址最高处 —— 与 kernel/arch/x86_64/isr.S 的约定一致）。
        # 错误码压在返回地址**之下**（最低地址）：x86 的顺序是
        # ERR、RIP、CS、RFLAGS、RSP、SS，与 struct interrupt_frame 一致。
        frame = struct.pack("<QQQQQ", rip, cs, flags, saved_rsp, saved_ss)
        if vector in self.HAS_ERR:
            frame = struct.pack("<Q", self.exception_error_code(vector, cs)) + frame

        rsp -= len(frame)
        if not self.write_at(rsp, frame):
            return False

        self.uc.reg_write(self._sp(), rsp)
        if clear_if:
            self.uc.reg_write(self._flags(), flags & ~IF_BIT)
        self.set_rip(entry)
        self.irqs_injected += 1
        return True

    # ------------------------------ 内核日志 -------------------------------
    def klog_addr(self):
        if not self.elf:
            return None
        sym = self.elf.symbol("g_klog")
        if not sym:
            return None
        return self.elf.vaddr_to_paddr(sym[0])

    def poll_klog(self):
        addr = self.klog_addr()
        if addr is None:
            return
        head = struct.unpack("<I", self.phys_read(addr + KLOG_OFF_HEAD, 4))[0]
        if head <= self.klog_pos:
            return
        size = struct.unpack("<I", self.phys_read(addr + 4, 4))[0] or 16384
        start = head - self.klog_pos
        if start > size:
            start = size
        base = addr + KLOG_OFF_BUF
        begin = (head - start) % size
        chunk = min(start, size - begin)
        data = self.phys_read(base + begin, chunk)
        if start > chunk:
            data += self.phys_read(base, start - chunk)
        self.klog_pos = head
        self.console_out += data
        if (self.power_fail_after and not self.power_fail_armed and
                self.power_fail_after.encode("utf-8") in self.console_out):
            self.power_fail_armed = True
            self.power_fail_writes_seen = 0
            log(f"[fyxemu] 已看到断电标记，将在第 {self.power_fail_write} 次 ATA 写后断电")
        if (not self.keys_armed and self.keys_after and
                self.keys_after.encode("utf-8") in self.console_out):
            self.keys_armed = True
        if not self.quiet:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()

    def panic_happened(self):
        addr = self.klog_addr()
        if addr is None:
            return False
        return struct.unpack("<I", self.phys_read(addr + KLOG_OFF_PANIC, 4))[0] != 0

    # -------------------------------- BIOS --------------------------------
    def reg16(self, uc, name):
        """阶段 1 里读 16/32 位通用寄存器"""
        table = {
            "ax": UC_X86_REG_EAX, "bx": UC_X86_REG_RAX if False else UC_X86_REG_EAX,
        }
        raise NotImplementedError

    def bios_int(self, uc, intno):
        if intno == 0x10:
            ah = (uc.reg_read(UC_X86_REG_EAX) >> 8) & 0xFF
            if ah == 0x0E:
                ch = uc.reg_read(UC_X86_REG_EAX) & 0xFF
                self.bios_text.append(ch)
                if self.show_bios:
                    sys.stdout.write(chr(ch) if 32 <= ch < 127 else ".")
                    sys.stdout.flush()
            return
        if intno == 0x13:
            self.bios_disk(uc)
            return
        if intno == 0x15:
            self.bios_system(uc)
            return
        if intno == 0x16:
            ah = (uc.reg_read(UC_X86_REG_EAX) >> 8) & 0xFF
            if ah == 0x00 and self.keys_armed and self.key_queue:
                sc = self.key_queue.pop(0)
                uc.reg_write(UC_X86_REG_EAX, (uc.reg_read(UC_X86_REG_EAX)
                                              & 0xFFFFFF00) | sc)
            return
        if intno == 0x1A:
            return

    def _set_carry(self, uc, carry):
        flags = uc.reg_read(UC_X86_REG_EFLAGS)
        if carry:
            flags |= 0x01
        else:
            flags &= ~0x01
        uc.reg_write(UC_X86_REG_EFLAGS, flags)

    def bios_disk(self, uc):
        eax = uc.reg_read(UC_X86_REG_EAX)
        ah = (eax >> 8) & 0xFF
        if ah == 0x42:
            si = uc.reg_read(UC_X86_REG_ESI) & 0xFFFF
            ds = uc.reg_read(UC_X86_REG_DS)
            dap_addr = (ds << 4) + si
            dap = self.phys_read(dap_addr, 16)
            count = struct.unpack_from("<H", dap, 2)[0]
            offset = struct.unpack_from("<H", dap, 4)[0]
            segment = struct.unpack_from("<H", dap, 6)[0]
            lba = struct.unpack_from("<Q", dap, 8)[0]
            dest = (segment << 4) + offset
            src = lba * 512
            data = self.disk[src:src + count * 512]
            if len(data) != count * 512:
                self._set_carry(uc, True)
                uc.reg_write(UC_X86_REG_EAX, (eax & 0x00FF) | (0x01 << 8))
                return
            self.phys_write(dest, data)
            self._set_carry(uc, False)
            uc.reg_write(UC_X86_REG_EAX, eax & 0x00FF)   # AH = 0
            return
        if ah == 0x02:
            eax = uc.reg_read(UC_X86_REG_EAX)
            ecx = uc.reg_read(UC_X86_REG_ECX)
            edx = uc.reg_read(UC_X86_REG_EDX)
            ebx = uc.reg_read(UC_X86_REG_EBX)
            es = uc.reg_read(UC_X86_REG_ES)
            count = eax & 0xFF
            cyl = (ecx >> 8) & 0xFF
            sector = ecx & 0x3F
            head = (edx >> 8) & 0xFF
            dest = (es << 4) + (ebx & 0xFFFF)
            lba = ((cyl * 16 + head) * 63) + (sector - 1)
            data = self.disk[lba * 512:(lba + count) * 512]
            if len(data) != count * 512:
                self._set_carry(uc, True)
                return
            self.phys_write(dest, data)
            self._set_carry(uc, False)
            uc.reg_write(UC_X86_REG_EAX, eax & 0x00FF)
            return
        if ah == 0x08:
            # 返回几何参数：16 磁头 / 63 扇区每磁道
            total = len(self.disk) // 512
            max_cyl = min(total // (16 * 63), 1023)
            uc.reg_write(UC_X86_REG_ECX, ((max_cyl & 0xFF) << 8) | 63)
            uc.reg_write(UC_X86_REG_EDX, ((16 - 1) << 8))
            uc.reg_write(UC_X86_REG_EAX, 0)
            self._set_carry(uc, False)
            return
        if ah == 0x41:
            uc.reg_write(UC_X86_REG_EAX, ((0x21) << 8) | (eax & 0xFF))
            uc.reg_write(UC_X86_REG_EBX, 0xAA55)
            uc.reg_write(UC_X86_REG_ECX, 0x0007)
            self._set_carry(uc, False)
            return
        self._set_carry(uc, True)

    def bios_system(self, uc):
        eax = uc.reg_read(UC_X86_REG_EAX)
        ax = eax & 0xFFFF
        if ax == 0x2401 or ax == 0x2402 or ax == 0x2400:
            # A20：直接报告成功
            uc.reg_write(UC_X86_REG_EAX, (eax & 0xFFFF0000) | 0x0001)
            self._set_carry(uc, False)
            return
        if ax == 0xE820:
            edi = uc.reg_read(UC_X86_REG_EDI) & 0xFFFF
            es = uc.reg_read(UC_X86_REG_ES)
            buf = (es << 4) + edi
            cont = uc.reg_read(UC_X86_REG_EBX)
            regions = [
                (0x00000000, 0x0009FC00, 1),
                (0x0009FC00, 0x00000400, 2),
                (0x000F0000, 0x00010000, 2),
                (0x00100000, self.ram_bytes - 0x00100000, 1),
            ]
            idx = cont
            if idx >= len(regions):
                self._set_carry(uc, True)
                return
            base, length, kind = regions[idx]
            entry = struct.pack("<QQII", base, length, kind, 1)
            self.phys_write(buf, entry)
            uc.reg_write(UC_X86_REG_EAX, 0x534D4150)
            uc.reg_write(UC_X86_REG_ECX, 24)
            uc.reg_write(UC_X86_REG_EBX, idx + 1 if idx + 1 < len(regions) else 0)
            self._set_carry(uc, False)
            return
        if (eax >> 16) == 0x0000 and ax == 0x0000:
            return
        self._set_carry(uc, True)

    # ------------------------------ ATA PIO --------------------------------
    def ata_lba(self):
        return (self.ata_regs[0x1F3] |
                (self.ata_regs[0x1F4] << 8) |
                (self.ata_regs[0x1F5] << 16) |
                ((self.ata_regs[0x1F6] & 0x0F) << 24))

    @staticmethod
    def ata_identify_sector(sectors):
        words = [0] * 256
        words[0] = 0x0040
        words[49] = 0x0200             # LBA supported
        words[60] = sectors & 0xFFFF
        words[61] = (sectors >> 16) & 0xFFFF
        words[80] = 0x0070
        words[83] = 0x4000

        def string_field(first, count, text):
            raw = text.encode("ascii", "replace")[:count * 2].ljust(count * 2)
            for i in range(count):
                words[first + i] = (raw[2 * i] << 8) | raw[2 * i + 1]

        string_field(10, 10, "FYXOS-ATA-0001")
        string_field(27, 20, "FYXOS PERSISTENT ATA DISK")
        data = bytearray()
        for word in words:
            data += struct.pack("<H", word)
        return data

    @staticmethod
    def atapi_identify_sector():
        words = [0] * 256
        words[0] = 0x8580             # removable ATAPI, 12-byte command packet
        words[80] = 0x0070
        words[83] = 0x4000

        def string_field(first, count, text):
            raw = text.encode("ascii", "replace")[:count * 2].ljust(count * 2)
            for i in range(count):
                words[first + i] = (raw[2 * i] << 8) | raw[2 * i + 1]

        string_field(10, 10, "FYXOS-CD-0001")
        string_field(27, 20, "FYXOS LIVE ATAPI CD")
        data = bytearray()
        for word in words:
            data += struct.pack("<H", word)
        return data

    def atapi_execute_packet(self):
        self.ata_pos = 0
        self.ata_buffer = bytearray()
        self.ata_regs[0x1F1] = 0
        if len(self.atapi_packet) != 12:
            self.ata_status = 0x51
            self.ata_regs[0x1F1] = 0x04
            return
        cdb = self.atapi_packet
        opcode = cdb[0]
        if opcode == 0x00:             # TEST UNIT READY
            self.ata_status = 0x50
            return
        if opcode == 0x25:             # READ CAPACITY(10)
            last_lba = len(self.cd) // 2048 - 1
            self.ata_buffer = bytearray(struct.pack(">II", last_lba, 2048))
            self.ata_status = 0x58
            return
        if opcode == 0x28:             # READ(10), 2048-byte logical blocks
            lba = int.from_bytes(cdb[2:6], "big")
            count = int.from_bytes(cdb[7:9], "big")
            start = lba * 2048
            data = self.cd[start:start + count * 2048]
            if count == 0 or len(data) != count * 2048:
                self.ata_status = 0x51
                self.ata_regs[0x1F1] = 0x04
                return
            self.ata_buffer = bytearray(data)
            self.ata_status = 0x58
            return
        self.ata_status = 0x51         # unsupported SCSI operation
        self.ata_regs[0x1F1] = 0x04

    def ata_command(self, command):
        self.ata_pos = 0
        self.ata_write_lba = None
        self.atapi_receiving = False
        if self.cd and command == 0xEC:  # ATA IDENTIFY is aborted by ATAPI
            self.ata_buffer = bytearray()
            self.ata_status = 0x51
            self.ata_regs[0x1F1] = 0x04
            self.ata_regs[0x1F4] = 0x14
            self.ata_regs[0x1F5] = 0xEB
            return
        if self.cd and command == 0xA1:  # IDENTIFY PACKET DEVICE
            self.ata_buffer = self.atapi_identify_sector()
            self.ata_regs[0x1F1] = 0
            self.ata_status = 0x58
            return
        if self.cd and command == 0xA0:  # PACKET: receive a 12-byte CDB
            self.atapi_packet = bytearray(12)
            self.atapi_receiving = True
            self.ata_buffer = bytearray()
            self.ata_regs[0x1F1] = 0
            self.ata_status = 0x58
            return
        if command == 0xEC:              # IDENTIFY DEVICE
            self.ata_buffer = self.ata_identify_sector(len(self.disk) // 512)
            self.ata_status = 0x58       # DRDY | DSC | DRQ
            return
        if command == 0x20:              # READ SECTORS (one sector)
            lba = self.ata_lba()
            start = lba * 512
            data = self.disk[start:start + 512]
            if len(data) != 512:
                self.ata_buffer = bytearray()
                self.ata_status = 0x51   # DRDY | ERR
                self.ata_regs[0x1F1] = 0x04
                return
            self.ata_buffer = bytearray(data)
            self.ata_status = 0x58
            return
        if command == 0x30:              # WRITE SECTORS
            self.ata_buffer = bytearray(512)
            self.ata_write_lba = self.ata_lba()
            self.ata_status = 0x58
            return
        if command == 0xE7:              # FLUSH CACHE
            self.ata_buffer = bytearray()
            self.ata_status = 0x50
            return
        self.ata_buffer = bytearray()
        self.ata_status = 0x51
        self.ata_regs[0x1F1] = 0x04

    def ata_data_in(self, size):
        if self.ata_pos + size > len(self.ata_buffer):
            return 0
        value = int.from_bytes(self.ata_buffer[self.ata_pos:self.ata_pos + size],
                               "little")
        self.ata_pos += size
        if self.ata_pos >= len(self.ata_buffer):
            self.ata_status = 0x50
        return value

    def ata_data_out(self, value, size):
        if self.atapi_receiving:
            if self.ata_pos + size > len(self.atapi_packet):
                self.atapi_receiving = False
                self.ata_status = 0x51
                return
            self.atapi_packet[self.ata_pos:self.ata_pos + size] = \
                int(value & ((1 << (size * 8)) - 1)).to_bytes(size, "little")
            self.ata_pos += size
            if self.ata_pos == len(self.atapi_packet):
                self.atapi_receiving = False
                self.atapi_execute_packet()
            return
        if self.ata_write_lba is None or self.ata_pos + size > len(self.ata_buffer):
            self.ata_status = 0x51
            return
        self.ata_buffer[self.ata_pos:self.ata_pos + size] = \
            int(value & ((1 << (size * 8)) - 1)).to_bytes(size, "little")
        self.ata_pos += size
        if self.ata_pos < len(self.ata_buffer):
            return

        lba = self.ata_write_lba
        start = lba * 512
        if start + 512 > len(self.disk):
            self.ata_status = 0x51
            self.ata_regs[0x1F1] = 0x04
            return
        self.disk[start:start + 512] = self.ata_buffer
        self.ata_write_lba = None
        self.ata_status = 0x50
        self.disk_dirty = True
        self.ata_writes += 1

        if self.power_fail_armed and not self.power_failed:
            self.power_fail_writes_seen += 1
            if self.power_fail_writes_seen >= self.power_fail_write:
                self.power_failed = True
                self.stopped = True
                self.uc.emu_stop()
                log(f"[fyxemu] 模拟断电：ATA 写点 {self.power_fail_writes_seen}，"
                    f"LBA={lba}")

    # ------------------------------ 端口 I/O -------------------------------
    def port_in(self, uc, port, size):
        if self.bga and self.bga.handles(port):
            self.bga._io_in(uc, port, size)
            return uc.reg_read(UC_X86_REG_RAX) & ((1 << (size * 8)) - 1)
        if self.net and self.net.handles(port):
            self.net._io_in(uc, port, size)
            return uc.reg_read(UC_X86_REG_RAX) & ((1 << (size * 8)) - 1)
        if 0x1F0 <= port <= 0x1F7 or port == 0x3F6:
            if port == 0x1F0:
                return self.ata_data_in(size)
            if port in (0x1F7, 0x3F6):
                return self.ata_status
            return self.ata_regs.get(port, 0)
        if port == COM1 + 5:            # 串口线路状态：发送保持寄存器空
            return 0x20
        if port == COM1:
            return 0
        if port == KB_STATUS:
            return 0x01 if (self.keys_armed and
                            self.key_index < len(self.key_queue)) else 0x00
        if port == KB_DATA:
            if self.keys_armed and self.key_index < len(self.key_queue):
                sc = self.key_queue[self.key_index]
                self.key_index += 1
                if IRQ_KEYBOARD not in self.pending_irqs:
                    self.pending_irqs.append(IRQ_KEYBOARD)
                return sc
            return 0x00
        if port == 0x92:                 # 快速 A20 门
            return 0x02
        return 0x00

    def port_out(self, uc, port, size, value):
        raw = uc.reg_read(UC_X86_REG_RAX if self.phase == 2
                          else UC_X86_REG_EAX)
        value = raw & ((1 << (size * 8)) - 1)
        if self.bga and self.bga.handles(port):
            self.bga._io_out(uc, port, size, value)
            return
        if self.net and self.net.handles(port):
            self.net._io_out(uc, port, size, value)
            return
        if 0x1F0 <= port <= 0x1F7 or port == 0x3F6:
            if port == 0x1F0:
                self.ata_data_out(value, size)
            elif port == 0x1F7:
                self.ata_command(value & 0xFF)
            elif port in self.ata_regs:
                self.ata_regs[port] = value & 0xFF
            return
        if port == COM1:
            self.serial_out.append(value & 0xFF)
            return
        if port == DEBUG_PORT:
            self.shutdown_code = value & 0xFF
            self.stopped = True
            uc.emu_stop()
            return
        if port == 0x92:
            return

    # ------------------------------- 阶段 1 --------------------------------
    def run_phase1(self):
        self.phase = 1
        uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc = uc
        uc.mem_map_ptr(0, self.ram_bytes, UC_PROT_ALL, self.ram_addr)
        uc.mem_write(MBR_ADDR, bytes(self.disk[0:512]))

        uc.reg_write(UC_X86_REG_CS, 0)
        uc.reg_write(UC_X86_REG_DS, 0)
        uc.reg_write(UC_X86_REG_ES, 0)
        uc.reg_write(UC_X86_REG_SS, 0)
        uc.reg_write(UC_X86_REG_ESP, 0x7C00)
        uc.reg_write(UC_X86_REG_EAX, 0)
        uc.reg_write(UC_X86_REG_EDX, 0x0080)      # DL = 引导驱动器
        uc.reg_write(UC_X86_REG_EFLAGS, 0x0002)

        uc.hook_add(UC_HOOK_INTR, lambda u, i, d: self.bios_int(u, i))
        uc.hook_add(UC_HOOK_INSN, lambda u, p, s, d: self.port_in(u, p, s),
                    None, 1, 0, UC_X86_INS_IN)
        uc.hook_add(UC_HOOK_INSN,
                    lambda u, p, s, v, d: self.port_out(u, p, s, v),
                    None, 1, 0, UC_X86_INS_OUT)
        uc.hook_add(UC_HOOK_CODE, self.phase1_code)

        try:
            uc.emu_start(MBR_ADDR, 0, count=20_000_000)
        except UcError as e:
            self.error = f"阶段 1 仿真错误: {e} (EIP={self.get_rip():#x})"
            return False
        if self.handoff is None:
            self.error = (f"阶段 1 未能进入高半核（停在 EIP={self.get_rip():#x}）\n"
                          f"  BIOS/串口输出: {bytes(self.bios_text)!r}\n"
                          f"  VGA 屏幕内容:\n{self.vga_text()}")
            return False
        return True

    def vga_text(self):
        """读出 VGA 文本缓冲（物理 0xB8000，80x25）供诊断用。

        stage2 的 die() 只往 VGA 写、不走串口，遇到致命错误时串口输出会
        停在最后一条成功消息上，必须靠这个才能看到真正的失败原因。
        """
        try:
            raw = self.phys_read(0xB8000, 80 * 25 * 2)
        except UcError:
            return "    <无法显示>"
        lines = []
        for r in range(25):
            row = "".join(chr(raw[(r * 80 + c) * 2])
                          if 32 <= raw[(r * 80 + c) * 2] < 127 else " "
                          for c in range(80)).rstrip()
            if row:
                lines.append("    |" + row)
        return "\n".join(lines) if lines else "    <空>"

    def phase1_code(self, uc, addr, size, ud):
        if addr >= HANDOFF_THRESH:
            self.handoff = {
                "rip": uc.reg_read(UC_X86_REG_EIP),
                "cr0": uc.reg_read(UC_X86_REG_CR0),
                "cr3": uc.reg_read(UC_X86_REG_CR3),
                "cr4": uc.reg_read(UC_X86_REG_CR4),
                "efer": uc.reg_read(UC_X86_REG_MSR, 0xC0000080),
                "gdtr": uc.reg_read(UC_X86_REG_GDTR),
                "idtr": uc.reg_read(UC_X86_REG_IDTR),
                "cs": uc.reg_read(UC_X86_REG_CS),
                "ds": uc.reg_read(UC_X86_REG_DS),
                "es": uc.reg_read(UC_X86_REG_ES),
                "ss": uc.reg_read(UC_X86_REG_SS),
                "fs": uc.reg_read(UC_X86_REG_FS),
                "gs": uc.reg_read(UC_X86_REG_GS),
            }
            uc.emu_stop()
            return
        self.instructions += 1
        if self.instructions > 30_000_000:
            uc.emu_stop()

    # ------------------------------- 阶段 2 --------------------------------
    def run_phase2(self, state):
        self.phase = 2
        uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self.uc = uc
        # 关键：映射同一块宿主内存，阶段 1 的成果直接继承
        uc.mem_map_ptr(0, self.ram_bytes, UC_PROT_ALL, self.ram_addr)
        if self.bga:
            self.bga.map(uc)

        # 注入 trampoline 与参数（让客户机自己打开分页）
        self.phys_write(TRAMPOLINE_ADDR, self.trampoline)
        self.phys_write(PARAMS_ADDR, struct.pack(
            "<QQQQ", state["cr3"], state["cr4"], state["efer"], state["cr0"]))

        # 分页尚未打开：只置 PE，其余由 trampoline 完成
        uc.reg_write(UC_X86_REG_CR0, 0x00000011)
        uc.reg_write(UC_X86_REG_CR4, 0)
        uc.reg_write(UC_X86_REG_MSR, (0xC0000080, 0))
        if state["gdtr"]:
            uc.reg_write(UC_X86_REG_GDTR, state["gdtr"])
        if state["idtr"]:
            uc.reg_write(UC_X86_REG_IDTR, state["idtr"])
        uc.reg_write(UC_X86_REG_CS, state["cs"] if state["cs"] else 0x08)
        uc.reg_write(UC_X86_REG_DS, state["ds"] if state["ds"] else 0x10)
        uc.reg_write(UC_X86_REG_ES, state["es"] if state["es"] else 0x10)
        uc.reg_write(UC_X86_REG_SS, state["ss"] if state["ss"] else 0x10)
        uc.reg_write(UC_X86_REG_FS, 0x10)
        uc.reg_write(UC_X86_REG_GS, 0x10)
        uc.reg_write(UC_X86_REG_RFLAGS, 0x0002)
        uc.reg_write(UC_X86_REG_RSP, 0x70000)
        uc.reg_write(UC_X86_REG_RIP, TRAMPOLINE_ADDR)

        uc.hook_add(UC_HOOK_INTR, self.phase2_intr)
        uc.hook_add(UC_HOOK_INSN,
                    lambda u, p, s, d: self.port_in(u, p, s),
                    None, 1, 0, UC_X86_INS_IN)
        uc.hook_add(UC_HOOK_INSN,
                    lambda u, p, s, v, d: self.port_out(u, p, s, v),
                    None, 1, 0, UC_X86_INS_OUT)
        return self.phase2_loop()

    EXC_NAMES = {0: "#DE 除零", 1: "#DB 调试", 2: "NMI", 3: "#BP 断点",
                 4: "#OF 溢出", 5: "#BR 越界", 6: "#UD 非法指令", 7: "#NM 无协处理器",
                 8: "#DF 双重故障", 10: "#TS 无效 TSS", 11: "#NP 段不存在",
                 12: "#SS 栈段故障", 13: "#GP 通用保护", 14: "#PF 缺页",
                 16: "#MF 浮点错", 17: "#AC 对齐检查", 18: "#MC 机器检查",
                 19: "#XM SIMD 异常"}

    def phase2_intr(self, uc, intno, ud):
        """Unicorn 无法自行投递 int n，这里手工完成（见文件头说明第 5 条）"""
        rip = self.get_rip()            # 已指向 int 之后，可直接作为返回地址
        if not self.deliver(intno, rip):
            if not self.quiet:
                name = self.EXC_NAMES.get(intno, "外部中断/软中断")
                print(f"[fyxemu] 无法投递中断 {intno:#x}（{name}）："
                      f"RIP={rip:#x} RSP={uc.reg_read(UC_X86_REG_RSP):#x} "
                      f"CR2={uc.reg_read(UC_X86_REG_CR2):#x} "
                      f"CR3={uc.reg_read(UC_X86_REG_CR3):#x}"
                      + ("  <- 内核还没装好 IDT？" if intno < 32 else ""),
                      flush=True)

    def iret_code_hook(self, uc, addr, size, user_data):
        """
        Unicorn 2.1.4 的 iretq 在返回 ring 3 时会把一个合法用户页误报为
        #PF。帧本身由内核按真机格式准备好，这里仅对 CS.RPL=3 的返回手工
        完成同样的五槽弹帧；内核态 iretq 仍交给引擎执行。
        """
        if not self.iret_addr or addr != self.iret_addr:
            return
        rsp = uc.reg_read(UC_X86_REG_RSP)
        raw = self.virt_read(rsp, 40)
        if raw is None or len(raw) != 40:
            return
        rip, cs, rflags, user_rsp, user_ss = struct.unpack('<5Q', raw)
        if (cs & 3) != 3:
            return
        uc.reg_write(UC_X86_REG_RIP, rip)
        uc.reg_write(UC_X86_REG_CS, cs)
        uc.reg_write(UC_X86_REG_RFLAGS, rflags)
        uc.reg_write(UC_X86_REG_RSP, user_rsp)
        uc.reg_write(UC_X86_REG_SS, user_ss)
        self._user_irq_grace = True
        uc.emu_stop()

    def phase2_loop(self):
        """分块执行 + 在块之间注入外部中断（避免逐指令钩子的开销）"""
        uc = self.uc
        # isr_return_point 到 iretq 的距离由 isr.S 的固定 pop 序列决定。
        if self.elf:
            sym = self.elf.symbols.get('isr_return_point')
            if sym:
                self.iret_addr = sym[0] + 0x1B
                uc.hook_add(UC_HOOK_CODE, self.iret_code_hook,
                            None, self.iret_addr, self.iret_addr + 1)
        # SYSCALL 必须主动挂钩：否则 Unicorn 2.1.4 会把它当空操作跳过。
        # SYSRET 没有可注册的 INSN 钩子，仍在下面的异常处理里补齐。
        uc.hook_add(UC_HOOK_INSN, self.syscall_insn_hook,
                    None, 1, 0, UC_X86_INS_SYSCALL)
        self.start_wall = time.time()
        while not self.stopped:
            if time.time() - self.start_wall > self.max_seconds:
                self.error = f"超过 {self.max_seconds} 秒的时间预算"
                break
            # sysret 后的第一条用户指令单独执行，详见 emulate_fast_syscall。
            run_count = self.chunk
            # 断电测试需要先及时读到普通内核日志标记，再开始数 ATA 写；
            # 只在武装前缩短执行块，不影响正常仿真性能。
            if (self.power_fail_after and not self.power_fail_armed and
                    self.keys_armed):
                run_count = min(run_count, 200)
            if self._user_irq_grace:
                run_count = 1
                self._user_irq_grace = False
            current_cr3 = uc.reg_read(UC_X86_REG_CR3) & PTE_ADDR_MASK
            if self.last_cr3 != current_cr3:
                # CR3 切换不仅使 Python 侧 TLB 失效，也要丢弃 Unicorn 已
                # 翻译的基本块；否则同虚拟页可能继续执行上一个进程的代码。
                self.tlb.clear()
                self.tlb_cr3 = current_cr3
                uc.ctl_flush_tb()
                self.last_cr3 = current_cr3
            try:
                uc.emu_start(self.get_rip(), 0, count=run_count)
            except UcError as e:
                # Unicorn 不实现 SYSRET：按硬件约定补上。不能在这里 continue，
                # 否则会跳过下面的时钟节拍与中断投递，用户态频繁调用
                # get_ticks 时就会永远没有定时器中断。
                if not self.emulate_fast_syscall(self.get_rip()):
                    self.error = f"阶段 2 仿真错误: {e} (RIP={self.get_rip():#x})"
                    break
            # SYSCALL 钩子会在停止仿真块后被 Unicorn 额外前进 2 字节；
            # 恢复成真正的 LSTAR 目标，再继续执行内核入口。
            if self._syscall_fixup:
                self._syscall_fixup = False
                self.set_rip(self._syscall_target)
            self.instructions += self.chunk
            self.poll_klog()
            if self.stopped:
                break

            # 时钟节拍
            if self.instructions % self.tick_every < self.chunk:
                if IRQ_TIMER not in self.pending_irqs:
                    self.pending_irqs.append(IRQ_TIMER)

            # 注入挂起的外部中断。sysret 刚完成时先留一条用户指令的
            # 缓冲窗口，避免 Unicorn 在特权级边界上报假的用户页故障。
            if self.pending_irqs and not self._user_irq_grace:
                flags = uc.reg_read(UC_X86_REG_RFLAGS)
                if flags & IF_BIT:
                    irq = self.pending_irqs.pop(0)
                    if irq == IRQ_TIMER:
                        self.ticks_injected += 1
                    self.deliver(IRQ_BASE_VECTOR + irq, self.get_rip())
        return self.error is None

    # ------------------------------- 键盘输入 ------------------------------
    # 扫描码集 1（Set 1，US QWERTY）。
    # 注意：按键扫描码是按**键盘布局**排列的，不是按字母表顺序
    # （q=0x10 w=0x11 e=0x12 ... 而不是 a=0x1E b=0x1F ...）。
    # 早期版本写成 0x1E + 字母下标，结果 help 被识别成 kg`x。
    SCANCODES = {
        'q': 0x10, 'w': 0x11, 'e': 0x12, 'r': 0x13, 't': 0x14, 'y': 0x15,
        'u': 0x16, 'i': 0x17, 'o': 0x18, 'p': 0x19,
        'a': 0x1E, 's': 0x1F, 'd': 0x20, 'f': 0x21, 'g': 0x22, 'h': 0x23,
        'j': 0x24, 'k': 0x25, 'l': 0x26,
        'z': 0x2C, 'x': 0x2D, 'c': 0x2E, 'v': 0x2F, 'b': 0x30, 'n': 0x31,
        'm': 0x32,
        '1': 0x02, '2': 0x03, '3': 0x04, '4': 0x05, '5': 0x06, '6': 0x07,
        '7': 0x08, '8': 0x09, '9': 0x0A, '0': 0x0B,
        ' ': 0x39, '-': 0x0C, '=': 0x0D, '[': 0x1A, ']': 0x1B, '\\': 0x2B,
        ';': 0x27, "'": 0x28, '`': 0x29, ',': 0x33, '.': 0x34, '/': 0x35,
        '\n': 0x1C, '\r': 0x1C, '\t': 0x0F, '\b': 0x0E,
    }
    # 需要按住 Shift 才能打出的字符 -> 对应的基础键
    SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6',
               '&': '7', '*': '8', '(': '9', ')': '0', '_': '-', '+': '=',
               '{': '[', '}': ']', '|': '\\', ':': ';', '"': "'", '~': '`',
               '<': ',', '>': '.', '?': '/'}

    def build_key_queue(self, script):
        """脚本文本转 PS/2；支持 Shift、Ctrl 和 \\cX 控制键转义。"""
        i = 0
        while i < len(script):
            # \cS / \cB / \cR / \cQ 等用于 IDE 自动化，不需要改变文本协议。
            if script[i] == "\\" and i + 1 < len(script) and script[i + 1] == "e":
                self.key_queue.extend([0x01, 0x81])
                i += 2
                continue
            if (script[i] == "\\" and i + 2 < len(script) and
                    script[i + 1] == "c" and script[i + 2].isalpha()):
                sc = self.SCANCODES.get(script[i + 2].lower())
                if sc is not None:
                    self.key_queue.extend([0x1D, sc, sc | 0x80, 0x9D])
                i += 3
                continue
            ch = script[i]
            i += 1
            if ch in self.SHIFTED:
                base = self.SCANCODES[self.SHIFTED[ch]]
                self.key_queue.extend([0x2A, base, base | 0x80, 0xAA])
            elif ch.isupper() and ch.lower() in self.SCANCODES:
                sc = self.SCANCODES[ch.lower()]
                self.key_queue.extend([0x2A, sc, sc | 0x80, 0xAA])
            elif ch in self.SCANCODES:
                sc = self.SCANCODES[ch]
                self.key_queue.extend([sc, sc | 0x80])
            elif "\x01" <= ch <= "\x1a":
                # 也接受已经解码的 Ctrl+A .. Ctrl+Z 字符。
                sc = self.SCANCODES.get(chr(ord("a") + ord(ch) - 1))
                if sc is not None:
                    self.key_queue.extend([0x1D, sc, sc | 0x80, 0x9D])
            # 其余字符（如分号分隔符已被替换）忽略

    # --------------------------------- 运行 --------------------------------
    def save_disk(self):
        """把 ATA 盘当前介质状态落盘；正常关机与模拟断电都调用。"""
        target = self.disk_output
        if target is None and self.writeback:
            target = self.image_path
        if target is None or not self.disk_dirty:
            return
        directory = os.path.dirname(os.path.abspath(target))
        os.makedirs(directory, exist_ok=True)
        with open(target, "wb") as f:
            f.write(self.disk)
        log(f"[fyxemu] ATA 介质已保存到 {target}（{self.ata_writes} 次写扇区）")

    def save_framebuffer(self):
        if self.bga and self.fb_output:
            self.bga.save_png(self.fb_output)

    def run(self):
        if self.mode == "full-boot":
            if not self.image_path and not self.iso_path:
                print("fyxemu: full-boot 模式需要 --image 或 --iso", file=sys.stderr)
                return 2
            ok = self.run_phase1()
            if not ok:
                log(f"[fyxemu] {self.error}")
                return 1
            if self.elf is None and self.kernel_path:
                self.elf = Elf(self.kernel_path)
            state = self.handoff
            # 重建高 32 位：MODE_16 引擎只能读到 EIP
            entry = None
            if self.elf:
                entry = self.elf.symbols.get("_kernel_entry", (None,))[0]
            if entry:
                state["rip"] = (entry & 0xFFFFFFFF00000000) | (state["rip"]
                                                               & 0xFFFFFFFF)
            log(f"[fyxemu] 阶段 1 完成：BIOS 引导 -> 长模式，"
                f"入口 RIP={state['rip']:#x}, CR3={state['cr3']:#x}")
            ok = self.run_phase2(state)
        else:
            ok = self.run_quick()

        self.poll_klog()
        return 0 if self.error is None else 1

    def run_quick(self):
        """跳过引导扇区，直接把内核映像放进内存并用 trampoline 启动"""
        if not self.kernel_path:
            print("fyxemu: quick 模式需要 --kernel", file=sys.stderr)
            return False
        self.elf = Elf(self.kernel_path)
        self.phase = 1
        uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self.uc = uc
        uc.mem_map_ptr(0, self.ram_bytes, UC_PROT_ALL, self.ram_addr)

        # 1) 装载内核段到物理地址
        for s in self.elf.segments:
            if s["type"] != 1:
                continue
            data = self.elf.data[s["offset"]:s["offset"] + s["filesz"]]
            self.phys_write(s["paddr"], data)
            if s["memsz"] > s["filesz"]:
                self.phys_write(s["paddr"] + s["filesz"],
                                b"\0" * (s["memsz"] - s["filesz"]))

        # 2) 构造引导期页表（PML4/PDPT/PD，2 MiB 大页，恒等 + HHDM + 内核窗口）
        PML4, PDPT, PD = 0x20000, 0x21000, 0x22000
        self.phys_write(PML4, b"\0" * 0x1000)
        self.phys_write(PDPT, b"\0" * 0x1000)
        self.phys_write(PD, struct.pack("<Q", 0) * 512)
        self.phys_write(PML4, struct.pack("<Q", PDPT | 3))
        self.phys_write(PDPT, struct.pack("<Q", PD | 3))
        for i in range(512):
            self.phys_write(PD + i * 8,
                            struct.pack("<Q", i * 0x200000 | 0x83))
        # HHDM 与高半核映射到内核物理基址
        self.phys_write(PML4 + 256 * 8, struct.pack("<Q", 0x23000 | 3))
        self.phys_write(0x23000, b"\0" * 0x1000)
        self.phys_write(0x23000, struct.pack("<Q", 0x24000 | 3))
        self.phys_write(0x24000, b"\0" * 0x1000)
        for i in range(512):
            self.phys_write(0x24000 + i * 8,
                            struct.pack("<Q", i * 0x200000 | 0x83))
        self.phys_write(PML4 + 511 * 8, struct.pack("<Q", 0x25000 | 3))
        self.phys_write(0x25000, b"\0" * 0x1000)
        self.phys_write(0x25000 + 510 * 8, struct.pack("<Q", 0x26000 | 3))
        self.phys_write(0x26000, b"\0" * 0x1000)
        self.phys_write(0x26000, struct.pack("<Q", KERNEL_LMA | 0x83))

        # 3) 伪造 boot_info（放在 0x5000）
        bi = 0x5000
        self.phys_write(bi, b"\0" * 0x700)
        self.phys_write(bi, struct.pack("<I", 0x31585946))       # magic
        self.phys_write(bi + 4, struct.pack("<I", 1))            # version
        self.phys_write(bi + 0x0C, struct.pack("<I", 0x80))      # boot_drive
        regions = [(0, 0x9FC00, 1), (0x100000, self.ram_bytes - 0x100000, 1)]
        for i, (b, l, t) in enumerate(regions):
            self.phys_write(bi + 0x50 + i * 24, struct.pack("<QQII", b, l, t, 1))
        self.phys_write(bi + 8, struct.pack("<I", len(regions)))
        self.phys_write(bi + 0x30, struct.pack("<Q", self.ram_bytes))
        self.phys_write(bi + 0x10, struct.pack("<Q", KERNEL_LMA))
        self.phys_write(bi + 0x20, struct.pack("<Q", KERNEL_VMA))
        self.phys_write(bi + 0x28, struct.pack("<Q", HHDM_BASE))

        # 4) trampoline 参数
        entry = self.elf.symbols.get("_kernel_entry", (None,))[0]
        if entry is None:
            self.error = "内核缺少 _kernel_entry 符号"
            return False
        stack_top = self.elf.symbols.get("_boot_stack_top", (None,))[0]
        self.phys_write(SCRATCH_ENTRY, struct.pack("<Q", entry))
        self.phys_write(SCRATCH_STACK,
                        struct.pack("<Q", stack_top or (KERNEL_VMA + 0x27000)))
        self.phys_write(SCRATCH_BOOTINFO, struct.pack("<Q", HHDM_BASE + bi))

        self.phase = 2
        uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self.uc = uc
        uc.mem_map_ptr(0, self.ram_bytes, UC_PROT_ALL, self.ram_addr)
        if self.bga:
            self.bga.map(uc)
        self.phys_write(TRAMPOLINE_ADDR, self.trampoline)
        self.phys_write(PARAMS_ADDR, struct.pack(
            "<QQQQ", PML4, 0x20, 0x500, 0x80000011))

        uc.reg_write(UC_X86_REG_CR0, 0x00000011)
        uc.reg_write(UC_X86_REG_CR4, 0)
        uc.reg_write(UC_X86_REG_MSR, (0xC0000080, 0))
        # 忠实复刻 stage2 的 GDT 布局（含 0x18 = 64 位代码段），并让 CPU 以
        # CS=0x18 进入内核 —— 与真实引导完全一致。早期版本直接用 0x08，
        # 结果掩盖了"内核加载自己的 GDT 后不重载 CS"这个 bug。
        GDT = 0x13000
        self.phys_write(GDT, struct.pack("<QQQQQ", 0,
                                         0x00CF9A000000FFFF,   # 0x08 32位代码
                                         0x00CF92000000FFFF,   # 0x10 32位数据
                                         0x00AF9A000000FFFF,   # 0x18 64位代码
                                         0x00AF92000000FFFF))  # 0x20 64位数据
        uc.reg_write(UC_X86_REG_GDTR, (0, GDT, 39, 0))
        uc.reg_write(UC_X86_REG_CS, 0x18)
        uc.reg_write(UC_X86_REG_DS, 0x10)
        uc.reg_write(UC_X86_REG_ES, 0x10)
        uc.reg_write(UC_X86_REG_SS, 0x10)
        uc.reg_write(UC_X86_REG_FS, 0x10)
        uc.reg_write(UC_X86_REG_GS, 0x10)
        uc.reg_write(UC_X86_REG_RFLAGS, 0x0002)
        uc.reg_write(UC_X86_REG_RSP, 0x70000)
        uc.reg_write(UC_X86_REG_RIP, TRAMPOLINE_ADDR)

        uc.hook_add(UC_HOOK_INTR, self.phase2_intr)
        uc.hook_add(UC_HOOK_INSN,
                    lambda u, p, s, d: self.port_in(u, p, s),
                    None, 1, 0, UC_X86_INS_IN)
        uc.hook_add(UC_HOOK_INSN,
                    lambda u, p, s, v, d: self.port_out(u, p, s, v),
                    None, 1, 0, UC_X86_INS_OUT)
        return self.phase2_loop()


# --------------------------------------------------------------------------
# 测试模式
# --------------------------------------------------------------------------
def run_selfcheck(emu: FyxEmu) -> int:
    out = emu.console_out.decode("utf-8", "replace")
    checks = [
        ("内核启动横幅", "FYXOS"),
        ("自检全部通过", "自检全部通过"),
        ("调度演示完成", "demo-a 计数="),
        ("ATA/ATAPI 与 FYXFS 挂载",
         "Live RAM overlay 文件系统就绪" if emu.iso_path else "持久化文件系统就绪"),
        ("DHCP 地址配置", "[net] DHCP 完成"),
        ("ARP 下一跳解析", "[net] ARP 网关"),
        ("ICMP 网关往返", "[net] ICMP 网关往返成功"),
        ("从磁盘加载 init", "从 /bin/init 读取可执行映像"),
        ("ring3 UDP socket", "[init] ring3 UDP socket 通过"),
        ("ring3 TCP socket", "[init] ring3 TCP socket 通过"),
        ("Shell ifconfig", "eth0: UP"),
        ("Shell ping", "ICMP echo 回复"),
        ("用户态启动", "[init] === FYXOS 用户态已启动"),
        ("用户态文件系统调用", "FYXFS 文件读写与 seek"),
        ("fork 子进程返回 0", "fork 返回 0"),
        ("exec 启动新程序", "[exec] 新程序已启动"),
        ("wait 回收退出码", "wait 回收 pid=2 退出码=7"),
        ("管道通信", "pipe 收到 pipe-ok"),
        ("用户态时钟与堆", "堆已增长"),
        ("未发生 panic", not emu.panic_happened()),
    ]
    if "desktop" in emu.key_script or "[desktop] frame=" in out:
        frames = re.findall(
            r"\[desktop\] frame=(\d+) focus=([A-Z]+) theme=(dark|light) "
            r"checksum=0x([0-9a-fA-F]{8})", out)
        checks.extend([
            ("BGA 800x600 LFB", "[gfx] BGA LFB 800x600x32 phys=0xe0000000" in out),
            ("软件背 buffer", "软件背 buffer phys=" in out),
            ("桌面首帧", len(frames) >= 1 and frames[0][0] == "1" and
             frames[0][1] == "WELCOME" and frames[0][2] == "dark"),
            ("桌面焦点与主题重绘", len(frames) >= 3 and
             frames[1][1] == "NETWORK" and frames[2][2] == "light"),
            ("guest/host framebuffer 校验一致", bool(frames) and emu.bga is not None and
             int(frames[-1][3], 16) == emu.bga.checksum()),
        ])
    failures = 0
    log("")
    log("================ 自动化检查 ================")
    for name, cond in checks:
        ok = (cond in out) if isinstance(cond, str) else cond
        log(f"  [{'PASS' if ok else 'FAIL'}] {name}")
        if not ok:
            failures += 1
    log(f"  注入时钟节拍 {emu.ticks_injected} 次，中断共 {emu.irqs_injected} 次，"
        f"执行约 {emu.instructions} 条指令")
    log("============================================")
    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description="FYXOS 仿真台架")
    ap.add_argument("--image", help="可引导磁盘镜像（full-boot 模式）")
    ap.add_argument("--iso", help="FYXOS Live ISO（El Torito + ATAPI full-boot）")
    ap.add_argument("--kernel", help="内核 ELF（quick 模式 / 符号表）")
    ap.add_argument("--mode", choices=["full-boot", "quick"], default="full-boot")
    ap.add_argument("--ram-mb", type=int, default=64)
    ap.add_argument("--keys", default="", help="注入的键盘输入脚本")
    ap.add_argument("--keys-after", default=None,
                    help="看到指定内核日志后才开始注入 --keys")
    ap.add_argument("--show-bios", action="store_true", help="显示 BIOS 屏幕输出")
    ap.add_argument("--quiet", action="store_true", help="不实时回显内核输出")
    ap.add_argument("--test", action="store_true", help="跑自动化检查")
    ap.add_argument("--max-seconds", type=int, default=120)
    ap.add_argument("--writeback", action="store_true",
                    help="把 ATA 写修改保存回 --image")
    ap.add_argument("--disk-output", default=None,
                    help="把运行后的 ATA 介质保存到另一镜像")
    ap.add_argument("--power-fail-after", default=None,
                    help="看到指定日志后，开始倒数 ATA 写入以模拟断电")
    ap.add_argument("--power-fail-write", type=int, default=1,
                    help="标记之后第几次 ATA 扇区写完成时断电（默认 1）")
    ap.add_argument("--tick-every", type=int, default=20000,
                    help="每多少条指令注入一次时钟中断")
    ap.add_argument("--chunk", type=int, default=20000)
    ap.add_argument("--trace-io", action="store_true",
                    help="跟踪 RTL8139 等端口 I/O")
    ap.add_argument("--no-net", action="store_true",
                    help="不安装 RTL8139，用于离线诊断")
    ap.add_argument("--no-gfx", action="store_true",
                    help="不安装 BGA，用于纯文本诊断")
    ap.add_argument("--fb-output", default=None,
                    help="把 BGA 物理帧缓冲导出为 PNG")
    args = ap.parse_args()
    if args.image and args.iso:
        ap.error("--image 与 --iso 只能选择一个")
    if args.iso and args.mode != "full-boot":
        ap.error("--iso 仅支持 full-boot 模式")
    if args.iso and (args.writeback or args.disk_output or args.power_fail_after):
        ap.error("Live ISO 不写回介质，也不支持 ATA 断电注入")

    try:
        emu = FyxEmu(image=args.image, iso=args.iso, kernel=args.kernel, ram_mb=args.ram_mb,
                 mode=args.mode, keys=args.keys, show_bios=args.show_bios,
                 max_seconds=args.max_seconds, quiet=args.quiet,
                 tick_every=args.tick_every, chunk=args.chunk,
                 keys_after=args.keys_after, writeback=args.writeback,
                 disk_output=args.disk_output,
                 power_fail_after=args.power_fail_after,
                 power_fail_write=args.power_fail_write, no_net=args.no_net,
                 no_gfx=args.no_gfx, fb_output=args.fb_output)
    except (OSError, IsoFormatError) as exc:
        print(f"fyxemu: 无法加载输入介质：{exc}", file=sys.stderr)
        return 2
    emu.trace_io = args.trace_io
    if args.keys:
        # 命令分隔符仍是分号；\; 允许把 C/脚本源码中的字面分号送进 Shell。
        script = args.keys.replace("\\;", "\x01")
        script = script.replace(";", "\n").replace("\\n", "\n")
        script = script.replace("\x01", ";")
        if not script.endswith("\n"):
            script += "\n"          # 最后一条命令也要回车才会执行
        emu.build_key_queue(script)

    rc = emu.run()
    emu.save_disk()
    emu.save_framebuffer()

    if emu.power_failed:
        log("[fyxemu] 本次运行按请求在 ATA 写点断电，未执行客户机关机流程")
    if emu.error:
        log(f"[fyxemu] 失败：{emu.error}")
    if emu.shutdown_code is not None:
        code = emu.shutdown_code
        label = {EXIT_OK: "正常关机", EXIT_PANIC: "内核 panic",
                 EXIT_EXCEPTION: "CPU 异常"}.get(code, f"代码 {code:#x}")
        log(f"[fyxemu] 内核请求停机：{label}")

    if args.test:
        failures = run_selfcheck(emu)
        rc = 0 if (rc == 0 and failures == 0 and emu.shutdown_code == EXIT_OK) else 1
        log(f"[fyxemu] 测试{'通过' if rc == 0 else '失败'}")
    elif emu.shutdown_code not in (None, EXIT_OK):
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
