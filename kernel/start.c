#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

// 声明两个 start() 里会用到的函数
void main();
void timerinit();

// entry.S needs one stack per CPU.
// 指定变量的内存对齐要求为 16 字节
__attribute__((aligned(16))) char stack0[4096 * NCPU];

// entry.S jumps here in machine mode on stack0.
// 配置好那些只有 M 态才能设置的寄存器，然后切换到 S 态，跳转到 main() 里去
void
start()
{
  // set M Previous Privilege mode to Supervisor, for mret.
  unsigned long x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  w_mstatus(x);

  // set M Exception Program Counter to main, for mret.
  // requires gcc -mcmodel=medany
  // 函数名和数组名一样，可以当作指针
  w_mepc((uint64)main);

  // disable paging for now.
  w_satp(0);

  // delegate all interrupts and exceptions to supervisor mode.
  // 默认情况下，所有中断和异常都交给 M 态处理，这两个寄存器按 cause 编号逐位决定
  // 哪些 trap 改为交给 S 态处理，0xffff 覆盖全部常见的异常和中断（低 16 位的异常和中断）
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  w_sie(r_sie() | SIE_SEIE | SIE_STIE);

  // configure Physical Memory Protection to give supervisor mode
  // access to all of physical memory.
  // xv6 只用了条目 0，pmpaddr0 给范围，pmpcfg0 的最低字节给访问权限
  // 0x3fffffffffffff 是 2^54-1，pmpaddr0 的值是物理地址右移 2 位的结果，
  // 所以它表示了所有的物理地址。0xf 是 0b01111：低 3 位允许 S/U 态读、写、执行，
  // 第 3~4 位 A=01 是 TOR 模式，表示条目 0 覆盖 [0, pmpaddr0 << 2)
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  // enable hardware updates of page table A and D bits
  // 允许 MMU 在页表遍历时自动设置 PTE 的 A（Accessed，已访问）和 D（Dirty，已脏）
  w_menvcfg(r_menvcfg() | MENVCFG_ADUE);

  // ask for clock interrupts.
  timerinit();

  // keep each CPU's hartid in its tp register, for cpuid().
  // mhartid 只有 M 态能读，趁现在把它复制到 tp
  int id = r_mhartid();
  w_tp(id);

  // switch to supervisor mode and jump to main().
  // 跳转到 mepc 指向的地址，并将特权级切换为 mstatus.MPP 字段记录的模式
  asm volatile("mret");
}

// ask each hart to generate timer interrupts.
void
timerinit()
{
  // enable the sstc extension (i.e. stimecmp).
  // RISC-V 有一个一直匀速递增的 64 位计数器 time，在 QEMU virt 上，它的频率是 10MHz
  // Sstc 扩展给 S 态提供了一个比较寄存器 stimecmp，硬件一直在比较 time 和 stimecmp 的值
  // 一旦 time >= stimecmp，就会把 S 态时钟中断置为挂起（sip.STIP = 1），中断打开后才会被响应
  w_menvcfg(r_menvcfg() | MENVCFG_STCE);

  // allow supervisor to use stimecmp and time.
  w_mcounteren(r_mcounteren() | 2);

  // ask for the very first timer interrupt.
  w_stimecmp(r_time() + 1000000);
}
