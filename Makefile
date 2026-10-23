# ============================================================================
#  FYXOS — 构建系统
#
#  目标一览：
#    make            -> build/fyxos.img（可引导磁盘镜像）
#    make kernel     -> build/fyxos.elf（ELF64 内核）
#    make boot       -> build/stage1.bin + build/stage2.bin
#    make emu        -> 在 Unicorn 台架上真实启动（BIOS 实模式全链路）
#    make emu-quick  -> 跳过引导扇区，直接把内核载入 64 位引擎启动
#    make test       -> 跑全部自动化测试
#    make clean
#
#  工具链要求：gcc / GNU as / GNU ld（freestanding），Python 3 + unicorn（仅测试）
# ============================================================================

ARCH        ?= x86_64
BUILD       ?= build
IMG         ?= $(BUILD)/fyxos.img
KERNEL_ELF  ?= $(BUILD)/fyxos.elf

CC          := gcc
AS          := as
LD          := ld
OBJCOPY     := objcopy
PYTHON      ?= python3
VENV_PY     := .venv/bin/python
ifeq ($(wildcard $(VENV_PY)),)
  PY := $(PYTHON)
else
  PY := $(VENV_PY)
endif

# ---- 通用警告 --------------------------------------------------------------
WARNINGS := -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
            -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes \
            -Wcast-align -Wwrite-strings -Wredundant-decls -Wnested-externs

# ---- 内核（64 位 freestanding） --------------------------------------------
KCFLAGS := -std=gnu11 -ffreestanding -fno-pie -no-pie -fno-stack-protector \
           -fno-builtin -fno-common -fno-asynchronous-unwind-tables \
           -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -mno-80387 -mno-avx -fno-omit-frame-pointer \
           -O2 -g -Wa,--noexecstack -Iinclude $(WARNINGS)
KLDFLAGS := -m elf_x86_64 -nostdlib -z max-page-size=0x1000

# ---- stage2 的 32 位 C 部分（freestanding） --------------------------------
SCFLAGS := -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
           -fno-builtin -fno-common -fno-asynchronous-unwind-tables \
           -m32 -fno-pic -fno-omit-frame-pointer -Wa,--noexecstack \
           -Os -g -Iinclude $(WARNINGS) -Iboot
SLDFLAGS := -m elf_i386

# 注意：kernel/arch/x86_64/ 是三层目录，两层的 wildcard 匹配不到，
#      因此统一用 find 递归收集源文件。
KERNEL_C_SRC := $(sort $(shell find kernel -name '*.c'))
KERNEL_S_SRC := $(sort $(shell find kernel -name '*.S'))
KERNEL_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_C_SRC)) \
                $(patsubst %.S,$(BUILD)/%.o,$(KERNEL_S_SRC))

STAGE2_OBJS := $(BUILD)/boot/stage2_16.o $(BUILD)/boot/stage2_32.o \
               $(BUILD)/boot/stage2_32c.o

# 头文件依赖：用 -MMD -MP 生成 .d 文件。
# 没有这个，改动 boot_info.h 之类的头文件时 .o 不会重编，
# 于是 stage2_32 还会去旧的暂存地址找 ELF，报出莫名其妙的 "not an ELF"。
DEPS := $(KERNEL_OBJS:.o=.d) $(STAGE2_OBJS:.o=.d)

DEPFLAGS := -MMD -MP

.PHONY: all kernel boot image emu emu-quick test clean dump

kernel: $(KERNEL_ELF)

all: image

# ---------------------------------------------------------------- 内核 ELF
$(KERNEL_ELF): $(KERNEL_OBJS) kernel/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(KLDFLAGS) -T kernel/linker.ld -o $@ $(KERNEL_OBJS)
	@echo "  LD      $@"

$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) $(DEPFLAGS) -c -o $@ $<
	@echo "  CC      $<"

$(BUILD)/kernel/%.o: kernel/%.S
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) $(DEPFLAGS) -c -o $@ $<
	@echo "  AS      $<"

# ------------------------------------------------------------------ 引导器
boot: $(BUILD)/stage1.bin $(BUILD)/stage2.bin

$(BUILD)/stage1.bin: boot/stage1.S boot/stage1.ld
	@mkdir -p $(dir $@)
	$(CC) -m32 -x assembler-with-cpp -I boot -c -o $(BUILD)/stage1.o $<
	$(LD) -m elf_i386 -T boot/stage1.ld --oformat=binary -o $@ $(BUILD)/stage1.o
	@s=`stat -c %s $@`; if [ "$$s" -ne 512 ]; then \
	    echo "ERROR: stage1 must be exactly 512 bytes (got $$s)"; exit 1; fi
	@echo "  AS+LD   $@ (512 bytes)"

$(BUILD)/stage2.bin: $(STAGE2_OBJS) boot/stage2.ld
	@mkdir -p $(dir $@)
	$(LD) $(SLDFLAGS) -T boot/stage2.ld --oformat=binary -o $@ $(STAGE2_OBJS)
	@echo "  LD      $@"

# 注意：必须用 gcc（而非直接 as）来汇编 .S —— as 不会做 C 预处理，
#      宏不会被展开，会被当成外部符号而报 "operand type mismatch"。
$(BUILD)/boot/stage2_16.o: boot/stage2_16.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -x assembler-with-cpp -I boot $(DEPFLAGS) -c -o $@ $<

$(BUILD)/boot/stage2_32.o: boot/stage2_32.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -x assembler-with-cpp -I boot $(DEPFLAGS) -c -o $@ $<

$(BUILD)/boot/stage2_32c.o: boot/stage2_32.c
	@mkdir -p $(dir $@)
	$(CC) $(SCFLAGS) $(DEPFLAGS) -c -o $@ $<

# 自动引入依赖描述文件（不存在时静默跳过）
-include $(DEPS)

# ------------------------------------------------------------------ 磁盘镜像
image: $(IMG)

$(IMG): $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(KERNEL_ELF) tools/mkimage.py
	$(PY) tools/mkimage.py --stage1 $(BUILD)/stage1.bin \
	                       --stage2 $(BUILD)/stage2.bin \
	                       --kernel $(KERNEL_ELF) \
	                       --out $@

# -------------------------------------------------------------------- 测试
emu: image
	$(PY) tools/fyxemu.py --image $(IMG) --mode full-boot $(EMU_ARGS)

emu-quick: $(KERNEL_ELF)
	$(PY) tools/fyxemu.py --kernel $(KERNEL_ELF) --mode quick $(EMU_ARGS)

test: image
	$(PY) tools/fyxemu.py --image $(IMG) --mode full-boot --test
	$(PY) tools/fyxemu.py --kernel $(KERNEL_ELF) --mode quick --test

# -------------------------------------------------------------------- 辅助
dump: $(KERNEL_ELF)
	readelf -h $(KERNEL_ELF)
	readelf -S $(KERNEL_ELF)
	nm -n $(KERNEL_ELF) | head -60

clean:
	rm -rf $(BUILD)

-include $(shell mkdir -p $(BUILD) 2>/dev/null) $(wildcard $(BUILD)/*.d)
