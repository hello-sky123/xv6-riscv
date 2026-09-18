K=kernel  # 内核态代码
U=user  # 用户态代码

# 第一部分：启动与底层硬件 (Boot & Hardware)
#   . $K/entry.o: 内核的绝对入口（汇编代码）。机器启动后执行的第一条指令就在这里，主要负责设置好最初的 C 语言运行堆栈。
#   . $K/start.o: 机器模式（Machine Mode）下的初始化。配置好硬件特权级后，跳入 Supervisor Mode（操作系统内核所在的特权级）。
#   . $K/main.o: 内核的 main() 函数所在处。负责依次调用各个子系统的初始化函数。
#   . $K/plic.o: 平台级中断控制器（Platform-Level Interrupt Controller）。负责管理外部硬件设备（如键盘、网卡）发来的中断信号。
# 第二部分：内存管理 (Memory Management)
#   . $K/kalloc.o: 物理内存分配器。管理物理内存页的分配和释放（把内存按 4KB 划分并用链表管理）。
#   . $K/vm.o: 虚拟内存（Virtual Memory）。操作系统中最烧脑的部分之一，负责建立和管理页表 (Page Table)，将虚拟地址映射到物理地址。
# 第三部分：进程与调度 (Process & Scheduling)
#   . $K/proc.o: 进程管理与 CPU 调度器。管理进程的状态（就绪、运行、休眠），并决定下一个让哪个进程使用 CPU。
#   . $K/swtch.o: 上下文切换（Context Switch，纯汇编代码）。负责在两个进程之间切换 CPU 的寄存器状态。
# 第四部分：中断与异常 (Traps & Exceptions)
#   . $K/trampoline.o: 蹦床代码（汇编代码）。用户态和内核态之间切换时，极其精巧的过渡代码，因为必须映射在所有进程页表的最高地址，所以被称为蹦床。
#   . $K/trap.o: C 语言写的中断处理核心逻辑。当发生系统调用、缺页、除零错误或硬件中断时，都会陷入这里。
#   . $K/kernelvec.o: 内核态下发生中断时的处理入口（汇编）。
# 第五部分：系统调用 (System Calls)
#   . $K/syscall.o: 系统调用分发器。从用户态收到系统调用号后，在这里查表，路由到具体的处理函数。
#   . $K/sysproc.o: 进程相关的系统调用实现（比如 fork, exit, kill, sleep 等）。
# 第六部分：并发与同步 (Concurrency)
#   . $K/spinlock.o: 自旋锁。用于多核 CPU 下保护短时间访问的共享数据（拿不到锁就一直死循环等）。
#   . $K/sleeplock.o: 睡眠锁。用于长时间操作（如读写磁盘）时的同步（拿不到锁就让出 CPU 去睡觉）。
# 第七部分：文件系统与存储 (File System & Storage)
#   . $K/virtio_disk.o: 磁盘驱动（基于 VirtIO 标准）。告诉内核如何跟虚拟机的硬盘通信。
#   . $K/bio.o: 块缓存（Block I/O）。磁盘太慢了，这里用内存缓存磁盘的块数据，提高读写速度。
#   . $K/log.o: 崩溃恢复与日志（Journaling）。保证文件系统如果在写到一半时突然断电，重启后数据不会损坏。
#   . $K/fs.o: 文件系统核心。管理 inode（文件索引节点）、目录、以及磁盘的布局。
#   . $K/file.o: 文件描述符层。在 Linux/UNIX 中“一切皆文件”，这个文件统一了控制台、管道、实际文件的读写接口。
#   . $K/pipe.o: 管道。用于两个进程之间通信（比如 shell 里的 ls | grep）。
#   . $K/exec.o: exec 机制。负责从磁盘读取一个可执行文件（ELF格式），丢弃进程原来的旧内存，装载新代码并执行。
#   . $K/sysfile.o: 文件相关的系统调用实现（比如 read, write, open, close 等）。
# 第八部分：工具与杂项 (Utilities)
#   . $K/console.o: 控制台。处理键盘的输入字符和屏幕的输出字符。
#   . $K/uart.o: 串口驱动。与底层 UART 芯片交互，完成实际的字符收发。
#   . $K/printk.o: 内核专属的打印函数 printf。
#   . $K/string.o: 内核自己实现的 C 语言字符串和内存处理函数（如 memset, memmove 等，因为内核不能用 C 标准库）。
OBJS = \
  $K/entry.o \
  $K/start.o \
  $K/console.o \
  $K/printk.o \
  $K/uart.o \
  $K/kalloc.o \
  $K/spinlock.o \
  $K/string.o \
  $K/main.o \
  $K/vm.o \
  $K/proc.o \
  $K/swtch.o \
  $K/trampoline.o \
  $K/trap.o \
  $K/syscall.o \
  $K/sysproc.o \
  $K/bio.o \
  $K/fs.o \
  $K/log.o \
  $K/sleeplock.o \
  $K/file.o \
  $K/pipe.o \
  $K/exec.o \
  $K/sysfile.o \
  $K/kernelvec.o \
  $K/plic.o \
  $K/virtio_disk.o

# riscv64-unknown-elf- or riscv64-linux-gnu-
# perhaps in /opt/riscv/bin
#TOOLPREFIX = 

# Try to infer the correct TOOLPREFIX if not set
# xxx-objdump -i 列出支持的所有目标文件格式，2>&1：将标准错误输出（stderr，代号 2）重定向到标准输出（stdout，代号 1）
# 如果系统没有安装 objdump，执行上述命令会报 "command not found" 的错误，通过 2>&1，这个报错信息可以顺着管道流给下一个
# 命令，而不是打印到屏幕上干扰用户，if 判断命令的退出状态（0 成功，判断为真），>/dev/null 2>&1，只关心 grep 成功还是失败，不需要
# 它把匹配内容打印出来，>/dev/null 等价于 1>/dev/null，将 stdout 丢掉，2>&1 将 stderr 也丢掉，elf64-big 几个通用基础格式之一
ifndef TOOLPREFIX
TOOLPREFIX := $(shell if riscv64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-elf-'; \
	elif riscv64-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-elf-'; \
	elif riscv64-none-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-none-elf-'; \
	elif riscv64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-linux-gnu-'; \
	elif riscv64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-linux-gnu-'; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find a riscv64 version of GCC/binutils." 1>&2; \
	echo "*** To turn off this error, run 'gmake TOOLPREFIX= ...'." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
endif

QEMU = qemu-system-riscv64
MIN_QEMU_VERSION = 7.2

CC = $(TOOLPREFIX)gcc
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

# Deterministic builds.
DETFLAGS = -ffile-prefix-map=$(CURDIR)=.

CFLAGS = -Wall -Werror -Wno-unknown-attributes -O -fno-omit-frame-pointer -ggdb -gdwarf-2
CFLAGS += $(DETFLAGS)
CFLAGS += -march=rv64gc
CFLAGS += -std=gnu99
CFLAGS += -MD
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding
CFLAGS += -fno-common -nostdlib
CFLAGS += -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen -fno-builtin-memset
CFLAGS += -fno-builtin-memmove -fno-builtin-memcmp -fno-builtin-log -fno-builtin-bzero
CFLAGS += -fno-builtin-strchr -fno-builtin-exit -fno-builtin-malloc -fno-builtin-putc
CFLAGS += -fno-builtin-free
CFLAGS += -fno-builtin-memcpy -Wno-main
CFLAGS += -fno-builtin-printf -fno-builtin-fprintf -fno-builtin-vprintf
CFLAGS += -I.
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

LDFLAGS = -z max-page-size=4096

$K/kernel: $(OBJS) $K/kernel.ld
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o $K/kernel $(OBJS) 
	$(OBJDUMP) -S $K/kernel > $K/kernel.asm
	$(OBJDUMP) -t $K/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $K/kernel.sym

$K/%.o: $K/%.S
	$(CC) -march=rv64gc -g $(DETFLAGS) -c -o $@ $<

tags: $(OBJS)
	etags kernel/*.S kernel/*.c

ULIB = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o

_%: %.o $(ULIB) $U/user.ld
	$(LD) $(LDFLAGS) -T $U/user.ld -o $@ $< $(ULIB)
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

$U/usys.S : $U/usys.pl
	perl $U/usys.pl > $U/usys.S

$U/usys.o : $U/usys.S
	$(CC) $(CFLAGS) -c -o $U/usys.o $U/usys.S

$U/_forktest: $U/forktest.o $(ULIB)
	# forktest has less library code linked in - needs to be small
	# in order to be able to max out the proc table.
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $U/_forktest $U/forktest.o $U/ulib.o $U/usys.o
	$(OBJDUMP) -S $U/_forktest > $U/forktest.asm

mkfs/mkfs: mkfs/mkfs.c $K/fs.h $K/param.h
	gcc -Wno-unknown-attributes -I. -o mkfs/mkfs mkfs/mkfs.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: %.o

UPROGS=\
	$U/_cat\
	$U/_echo\
	$U/_forktest\
	$U/_grep\
	$U/_init\
	$U/_kill\
	$U/_ln\
	$U/_ls\
	$U/_mkdir\
	$U/_rm\
	$U/_sh\
	$U/_stressfs\
	$U/_usertests\
	$U/_grind\
	$U/_wc\
	$U/_zombie\
	$U/_logstress\
	$U/_forphan\
	$U/_dorphan\
	$U/_sync\

fs.img: mkfs/mkfs README $(UPROGS)
	mkfs/mkfs fs.img README $(UPROGS)

-include kernel/*.d user/*.d

clean: 
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*/*.o */*.d */*.asm */*.sym \
	$K/kernel fs.img \
	mkfs/mkfs .gdbinit \
        $U/usys.S \
	$(UPROGS)

# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)
ifndef CPUS
CPUS := 3
endif

QEMUOPTS = -machine virt -bios none -kernel $K/kernel -m 128M -smp $(CPUS) -nographic
QEMUOPTS += -global virtio-mmio.force-legacy=false
QEMUOPTS += -drive file=fs.img,if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

qemu: check-qemu-version $K/kernel fs.img
	$(QEMU) $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl-riscv
	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

qemu-gdb: $K/kernel .gdbinit fs.img
	@echo "*** Now run 'gdb' in another window." 1>&2
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

print-gdbport:
	@echo $(GDBPORT)

QEMU_VERSION := $(shell $(QEMU) --version | head -n 1 | sed -E 's/^QEMU emulator version ([0-9]+\.[0-9]+)\..*/\1/')
check-qemu-version:
	@if [ "$(shell echo "$(QEMU_VERSION) >= $(MIN_QEMU_VERSION)" | bc)" -eq 0 ]; then \
		echo "ERROR: Need qemu version >= $(MIN_QEMU_VERSION)"; \
		exit 1; \
	fi

.PHONY: fmt
fmt:
	clang-format -i $(wildcard kernel/*.[ch] user/*.[ch] mkfs/*.c)
