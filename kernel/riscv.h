#ifndef __ASSEMBLER__

// which hart (core) is this?
static inline uint64
r_mhartid()
{
  uint64 x;
  // mhartid 是 CSR（控制状态寄存器），不是通用寄存器，要用 csrr/csrw 这类专门的指令读写
  asm volatile("csrr %0, mhartid" : "=r"(x));
  return x;
}

// Machine Status Register, mstatus
// 操作 mstatus 寄存器的 MPP 字段————记录并设置上一个特权级别，M=11, S=01, U=00

#define MSTATUS_MPP_MASK (3L << 11) // previous mode. 掩码：只抠出 MPP 两位
#define MSTATUS_MPP_M    (3L << 11) // 机器模式，(11₂ = 3)
#define MSTATUS_MPP_S    (1L << 11) // 监督模式，(01₂ = 1)
#define MSTATUS_MPP_U    (0L << 11) // 用户模式，(00₂ = 0)

static inline uint64
r_mstatus()
{
  uint64 x;
  asm volatile("csrr %0, mstatus" : "=r"(x));
  return x;
}

static inline void
w_mstatus(uint64 x)
{
  asm volatile("csrw mstatus, %0" : : "r"(x));
}

// machine exception program counter, holds the
// instruction address to which a return from
// exception will go.
// mepc 寄存器里存的是 mret 之后要跳转的地址
static inline void
w_mepc(uint64 x)
{
  asm volatile("csrw mepc, %0" : : "r"(x));
}

// Supervisor Status Register, sstatus
// 管 S 态的两件事：中断开没开（SIE/SPIE），以及陷入前处于哪个特权级（SPP）

#define SSTATUS_SPP  (1L << 8) // Previous mode, 1=Supervisor, 0=User
#define SSTATUS_SPIE (1L << 5) // Supervisor Previous Interrupt Enable
#define SSTATUS_UPIE (1L << 4) // User Previous Interrupt Enable
#define SSTATUS_SIE  (1L << 1) // Supervisor Interrupt Enable
#define SSTATUS_UIE  (1L << 0) // User Interrupt Enable

static inline uint64
r_sstatus()
{
  uint64 x;
  asm volatile("csrr %0, sstatus" : "=r"(x));
  return x;
}

static inline void
w_sstatus(uint64 x)
{
  asm volatile("csrw sstatus, %0" : : "r"(x));
}

static inline void
s_sstatus(uint64 x)
{
  // 把 sstatus 中掩码 x 为 1 的那些位置 1，其他位不变，比如 intr_on() 用它打开 SIE。"rK" 约束：
  // r 表示通用寄存器，K 表示 5 位立即数，如果 x 是 0~31 的常量，编译器会直接把它编码进指令（csrsi），
  // 而不是先放到寄存器里。memory：告诉编译器这条指令可能读写任意内存，所以不能把前后的内存访问挪到
  // 它的另一边；之前暂存在寄存器里、还没写回的内存数据要先写回，之后用到的内存数据要重新从内存读取
  __asm__ __volatile__("csrs sstatus, %0" ::"rK"(x) : "memory");
}

static inline void
c_sstatus(uint64 x)
{
  // 把 sstatus 中掩码 x 为 1 的那些位清 0，其他位不变，比如 intr_off() 用它关闭 SIE
  __asm__ __volatile__("csrc sstatus, %0" ::"rK"(x) : "memory");
}

static inline uint64
rc_sstatus(uint64 x)
{
  // 一条 csrrc 指令同时完成两件事：返回 sstatus 的旧值，并把掩码 x 为 1 的那些位清 0。读和清之间
  // 不会被中断插进来。push_off() 用它关中断，并从旧值里取出原来的 SIE，记到 mycpu()->intena
  __asm__ __volatile__("csrrc %0, sstatus, %1" : "=r"(x) : "rK"(x) : "memory");
  return x;
}

// Supervisor Interrupt Pending
// sip 寄存器记录哪些 S 模式可见的中断当前处于挂起状态，也就是哪些中断事件正在等待处理
static inline uint64
r_sip()
{
  uint64 x;
  asm volatile("csrr %0, sip" : "=r"(x));
  return x;
}

static inline void
w_sip(uint64 x)
{
  asm volatile("csrw sip, %0" : : "r"(x));
}

// Supervisor Interrupt Enable
#define SIE_SEIE (1L << 9) // external，允许 S 态外部中断
#define SIE_STIE (1L << 5) // timer，允许 S 态时钟中断
static inline uint64
r_sie()
{
  uint64 x;
  // sie 寄存器控制当前系统‘允许（Enable）’接收哪些特定类型的中断
  asm volatile("csrr %0, sie" : "=r"(x));
  return x;
}

static inline void
w_sie(uint64 x)
{
  asm volatile("csrw sie, %0" : : "r"(x));
}

// Machine-mode Interrupt Enable
#define MIE_STIE (1L << 5) // supervisor timer
// mie 寄存器是每一种中断源的使能位，M 态和 S 态的中断都在这里（比如 MIE_STIE 是 S 态时钟中断），
// sie 是它对 S 态可见的那部分
static inline uint64
r_mie()
{
  uint64 x;
  asm volatile("csrr %0, mie" : "=r"(x));
  return x;
}

static inline void
w_mie(uint64 x)
{
  asm volatile("csrw mie, %0" : : "r"(x));
}

// supervisor exception program counter, holds the
// instruction address to which a return from
// exception will go.
static inline void
w_sepc(uint64 x)
{
  asm volatile("csrw sepc, %0" : : "r"(x));
}

static inline uint64
r_sepc()
{
  uint64 x;
  asm volatile("csrr %0, sepc" : "=r"(x));
  return x;
}

// Machine Exception Delegation
// medeleg 寄存器决定哪些异常由 M 模式（Machine Mode）委托给 S 模式（Supervisor Mode）处理
static inline uint64
r_medeleg()
{
  uint64 x;
  asm volatile("csrr %0, medeleg" : "=r"(x));
  return x;
}

static inline void
w_medeleg(uint64 x)
{
  asm volatile("csrw medeleg, %0" : : "r"(x));
}

// Machine Interrupt Delegation
static inline uint64
r_mideleg()
{
  uint64 x;
  asm volatile("csrr %0, mideleg" : "=r"(x));
  return x;
}

static inline void
w_mideleg(uint64 x)
{
  asm volatile("csrw mideleg, %0" : : "r"(x));
}

// Supervisor Trap-Vector Base Address
// low two bits are mode.
// stvec 寄存器指定处理器进入 S 模式（Supervisor Mode）陷阱处理程序时的入口地址，以及陷阱入口的分发模式
static inline void
w_stvec(uint64 x)
{
  asm volatile("csrw stvec, %0" : : "r"(x));
}

static inline uint64
r_stvec()
{
  uint64 x;
  asm volatile("csrr %0, stvec" : "=r"(x));
  return x;
}

// Supervisor Timer Comparison Register
// 读取当前内核设定的下一次闹钟什么时候响
static inline uint64
r_stimecmp()
{
  uint64 x;
  // 在 RISC-V 主板上，有一个永远在匀速递增的全局时钟计数器（通常叫 time 寄存器）。
  // 而 stimecmp 寄存器，就是操作系统设定的“闹钟时间”
  // asm volatile("csrr %0, stimecmp" : "=r"(x));
  asm volatile("csrr %0, 0x14d" : "=r"(x)); // 0x14d 是 stimecmp 的 CSR 编号
  return x;
}

static inline void
w_stimecmp(uint64 x)
{
  // asm volatile("csrw stimecmp, %0" : : "r"(x));
  asm volatile("csrw 0x14d, %0" : : "r"(x));
}

// Machine Environment Configuration Register
// menvcfg 寄存器由 M 模式配置某些环境相关的硬件行为，并控制较低特权级能否使用部分可选的内存管理或其他架构功能

#define MENVCFG_STCE (1L << 63)  // 启用 Sstc 扩展，S 模式才能使用 stimecmp
#define MENVCFG_ADUE (1L << 61)  // 访问页面时由硬件自动设置 PTE 的 A/D 位

static inline uint64
r_menvcfg()
{
  uint64 x;
  // asm volatile("csrr %0, menvcfg" : "=r"(x));
  asm volatile("csrr %0, 0x30a" : "=r"(x));
  return x;
}

static inline void
w_menvcfg(uint64 x)
{
  // asm volatile("csrw menvcfg, %0" : : "r"(x));
  asm volatile("csrw 0x30a, %0" : : "r"(x));
}

// Physical Memory Protection
// pmpcfg0 寄存器决定“这块内存允许怎样访问”
static inline void
w_pmpcfg0(uint64 x)
{
  asm volatile("csrw pmpcfg0, %0" : : "r"(x));
}

// pmpaddr0 寄存器决定“保护哪块内存”
static inline void
w_pmpaddr0(uint64 x)
{
  asm volatile("csrw pmpaddr0, %0" : : "r"(x));
}

// 用这两个宏来构造写进 satp 寄存器的值，satp 决定 CPU 用哪张页表、用什么分页模式
// use riscv's sv39 page table scheme.
#define SATP_SV39 (8L << 60)

// pagetable 是物理内存地址，satp 的格式可划分为三段：1. 最高四位是模式位（MODE），决定分页模式；
// 0 是 Bare，不翻译地址；8 是 Sv39，39 位虚拟地址；9 是 Sv48，48 位虚拟地址。2. 中间 16 位是 ASID
// （地址空间 ID），xv6 不用，保持 0。3. 低 44 位是根页表的物理页号（PPN），即物理地址 >> 12
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12))

// supervisor address translation and protection;
// holds the address of the page table.
static inline void
w_satp(uint64 x)
{
  asm volatile("csrw satp, %0" : : "r"(x));
}

static inline uint64
r_satp()
{
  uint64 x;
  asm volatile("csrr %0, satp" : "=r"(x));
  return x;
}

// Supervisor Trap Cause
// 操作系统发生中断或发生异常时，它通过读取 scause 寄存器，来查明到底是谁、因为什么原因打断了 CPU 的执行
static inline uint64
r_scause()
{
  uint64 x;
  asm volatile("csrr %0, scause" : "=r"(x));
  return x;
}

// Supervisor Trap Value
// 当发生异常时，stval 寄存器会保存导致异常的地址或指令
static inline uint64
r_stval()
{
  uint64 x;
  asm volatile("csrr %0, stval" : "=r"(x));
  return x;
}

// Machine-mode Counter-Enable
// mcounteren 寄存器决定 S/U 模式能否访问某些计数器（cycle、time、instret）
static inline void
w_mcounteren(uint64 x)
{
  asm volatile("csrw mcounteren, %0" : : "r"(x));
}

static inline uint64
r_mcounteren()
{
  uint64 x;
  asm volatile("csrr %0, mcounteren" : "=r"(x));
  return x;
}

// machine-mode cycle counter
static inline uint64
r_time()
{
  uint64 x;
  asm volatile("csrr %0, time" : "=r"(x));
  return x;
}

// enable device interrupts，异步中断
static inline void
intr_on()
{
  s_sstatus(SSTATUS_SIE);
}

// disable device interrupts
static inline void
intr_off()
{
  c_sstatus(SSTATUS_SIE);
}

// are device interrupts enabled?
static inline int
intr_get()
{
  uint64 x = r_sstatus();
  return (x & SSTATUS_SIE) != 0;
}

static inline uint64
r_sp()
{
  uint64 x;
  asm volatile("mv %0, sp" : "=r"(x));
  return x;
}

// read and write tp, the thread pointer, which xv6 uses to hold
// this core's hartid (core number), the index into cpus[].
// 使用内联汇编的原因是因为 C 语言没法直接访问某个特定的寄存器，只能借助内联汇编
static inline uint64
r_tp()
{
  uint64 x;
  // asm 告诉编译器后面是一条汇编指令，volatile 告诉编译器这条汇编有副作用，不能删除或合并
  // mv 把 tp 寄存器里的值，赋值给 %0 这个占位符，: 后面是输出操作数，r：让编译器随便挑一个通用寄存器
  // 来存放 %0，= 表示这个操作数只写（汇编只写入、不读取它原来的值），(x)：汇编执行完后，把这个寄存器的值赋值给变量 x
  asm volatile("mv %0, tp" : "=r"(x));
  return x;
}

static inline void
w_tp(uint64 x)
{
  // 第一个冒号后面是输出操作数，第二个冒号后面是输入操作数，
  // 执行汇编前编译器要先把 x 的值放到一个通用寄存器里，然后汇编指令把这个寄存器的值写入 tp 寄存器
  asm volatile("mv tp, %0" : : "r"(x));
}

// ra 是 return address 寄存器，保存函数调用返回的地址
static inline uint64
r_ra()
{
  uint64 x;
  asm volatile("mv %0, ra" : "=r"(x));
  return x;
}

// flush the TLB.
// 清空 CPU 内部的 TLB（地址转换缓存），强制 CPU 重新去物理内存中读取最新的页表映射关系
static inline void
sfence_vma()
{
  // the zero, zero means flush all TLB entries.
  // 页表存放在普通物理内存里，需要 TLB 来加速访问，切换页表时，旧的页表映射关系可能还在 TLB 里，
  // 所以要清空 TLB，zero 是通用寄存器 x0，值恒为 0，sfence.vma 指令的两个参数都是 zero，表示清空所有 TLB 条目
  asm volatile("sfence.vma zero, zero" ::: "memory");
}

// fence for memory-mapped IO
// 内存屏障：保证屏障之前的内存和设备 I/O 访问，在其他 CPU 和设备看来，都先于屏障之后的访问发生。
// virtio_disk.c 用它确保描述符写进内存之后，才去通知设备
static inline void
io_fence()
{
  // iorw 的四个字母分别是：i 设备输入（I/O 读），o 设备输出（I/O 写），r 内存读，w 内存写。
  // 前后都写 iorw，表示屏障前的这四类访问，都要排在屏障后的这四类访问之前
  asm volatile("fence iorw, iorw" ::: "memory");
}

// fence for icache
// 指令屏障：让本 CPU 之后的取指令看到之前写进内存的代码（比如刚加载进来的程序），
// 避免执行指令缓存里的旧内容
static inline void
icache_fence()
{
  asm volatile("fence.i" ::: "memory");
}

// page table entry，指针天然可以作为数组来使用，把一个指向 4KB 内存页首地址的指针转换成 pagetable_t 后
// 它自然而然就变成了一个拥有 512 个元素的数组 pagetable[0] 到 pagetable[511]
typedef uint64 pte_t;
typedef uint64 *pagetable_t; // 512 PTEs

#endif // __ASSEMBLER__

#define PGSIZE  4096 // bytes per page
#define PGSHIFT 12   // bits of offset within a page

#define PGROUNDUP(sz)  (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE - 1))

#define PTE_V (1L << 0) // valid
#define PTE_R (1L << 1)
#define PTE_W (1L << 2)
#define PTE_X (1L << 3)
#define PTE_U (1L << 4) // user can access

// shift a physical address to the right place for a PTE.
#define PA2PTE(pa) ((((uint64)pa) >> 12) << 10)

#define PTE2PA(pte) (((pte) >> 10) << 12)

#define PTE_FLAGS(pte) ((pte) & 0x3FF)

// extract the three 9-bit page table indices from a virtual address.
#define PXMASK         0x1FF // 9 bits
#define PXSHIFT(level) (PGSHIFT + (9 * (level)))
#define PX(level, va)  ((((uint64)(va)) >> PXSHIFT(level)) & PXMASK)

// one beyond the highest possible virtual address.
// MAXVA is actually one bit less than the max allowed by
// Sv39, to avoid having to sign-extend virtual addresses
// that have the high bit set.
#define MAXVA (1L << (9 + 9 + 9 + 12 - 1))
