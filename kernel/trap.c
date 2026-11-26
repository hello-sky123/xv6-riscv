#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// ticks 记录开机以来经过了多少次时钟中断（“滴答”），tickslock 是保护它的自旋锁。按 clockintr
// 里的设置，大约每 0.1 秒加 1，所以 ticks 就是以 0.1 秒为单位的开机时间
struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

// 初始化保护 ticks 的自旋锁 tickslock，启动时由 hart 0 调用一次
void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
// 设置当前 CPU 的 stvec 寄存器，让它指向 kernelvec。从这以后，这个 CPU 在内核态发生的
// 中断和异常都会跳到 kernelvec 去处理。每个 CPU 都要调用一次
void
trapinithart(void)
{
  // kernelvec 的实际定义在 kernelvec.S 里，这里取它的地址（C 和汇编通过符号名关联）。
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from, and returns to, trampoline.S
// return value is user satp for trampoline.S to switch to.
//
// 处理所有从用户态进入内核的陷入：系统调用、设备中断、缺页异常。它由 uservec 调用，返回值是
// 用户页表的 satp 值，交给 userret 用来切回用户页表
uint64
usertrap(void)
{
  int which_dev = 0;

  // 检查确实来自用户态
  if ((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  // 换掉 trap 向量，现在已经在内核里了，接下来如果再发生中断，就该走 kernelvec
  w_stvec((uint64)kernelvec); //DOC: kernelvec

  struct proc *p = myproc();

  // save user program counter.
  // 将中断前正在执行的那条指令的地址保存到该进程专属的 trapframe->epc 内存里
  p->trapframe->epc = r_sepc();

  // 按陷入原因分 4 种情况
  if (r_scause() == 8) {
    // system call

    // 检查这个进程是否已经被 kill
    if (killed(p))
      kexit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    // 触发 ecall 时，硬件存入的是 ecall 指令本身的地址，ecall 固定占用 4 个字节
    // 必须手动 +4，才能让 CPU 返回用户态后，去执行 ecall 后的下一条指令
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    // 这时，所有怕被覆盖的特权寄存器都已经用完/备份妥当，可以重新打开中断了
    intr_on();

    syscall();
    // 外部硬件中断（比如键盘敲了字、VirtIO 磁盘读写完成）。devintr() 内部已通过 PLIC（
    // 中断总机）查明了是谁，并调用 uartintr() 或 virtio_disk_intr() 处理完毕
  } else if ((which_dev = devintr()) != 0) {
    // ok
    // 懒加载缺页抢救，r_scause() == 13 读缺页异常，r_scause() == 15 写缺页异常
  } else if ((r_scause() == 15 || r_scause() == 13) &&
             vmfault(p->pagetable, p->sz, r_stval(),
                     (r_scause() == 13) ? 1 : 0) != 0) {
    // page fault on lazily-allocated page
  } else {
    // 用户程序自己闯了大祸（比如解引用野指针/段错误、执行非法指令、踩踏内核禁区内存等）
    printk("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printk("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  // 清理被 kill 的进程
  if (killed(p))
    kexit(-1);

  // give up the CPU if this is a timer interrupt.
  // 2 代表时钟中断，说明这个进程占用 CPU 的时间片用完了，需要交还 CPU 控制权
  if (which_dev == 2)
    yield();

  prepare_return();

  // the user page table to switch to, for trampoline.S
  // 将用户进程的页表地址拼装成 64 位的 satp 控制字
  uint64 satp = MAKE_SATP(p->pagetable);

  // return to trampoline.S; satp value in a0.
  return satp;
}

//
// set up trapframe and control registers for a return to user space
//
// 返回用户态之前的准备工作
void
prepare_return(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(). because a trap from kernel
  // code to usertrap would be a disaster, turn off interrupts.
  // 需要关掉中断，原因在于下一步要把 stvec 改成 uservec，但这时 CPU 还在内核里。如果之后
  // 来了一次中断，CPU 会跳到 uservec，而 uservec 假定自己是从用户态进来的：它会把当前
  // 寄存器当成用户寄存器存进 trapframe，再把 sp 换成内核栈的顶部，正在运行的内核代码的状态
  // 就被破坏了，所以从这里开始一直关着中断，直到 userret 最后执行 sret 回到用户态
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  // uservec 这个符号的地址是它在内核代码段里的位置，也就是 0x80006000 那一页里的某个地址，
  // 但用户页表里没有映射这个地址，只映射了高地址 TRAMPOLINE（0x3ffffff000）。从用户态
  // 陷入时，satp 还是用户页表，所以 stvec 里必须是用户页表能访问的地址。做法是先算出
  // uservec 在 trampoline 页里的偏移，再加上 TRAMPOLINE
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  // 填写 trapframe 里的内核字段
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp(); // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.

  // set S Previous Privilege mode to User.
  // 设置 sret 后的特权级和中断状态
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  // 设置返回地址
  w_sepc(p->trapframe->epc);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
// 处理内核态运行时发生的陷入
void
kerneltrap()
{
  // 先把三个陷入寄存器存进局部变量
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();

  // 陷入前必须处于 S 态
  if ((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  // 中断必须关着
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  // 只接受设备中断
  if ((which_dev = devintr()) == 0) {
    // interrupt or trap from an unknown source
    printk("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(),
           r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  // 时钟中断：内核里也会被抢占
  // 时钟中断也可能打断 scheduler 本身，scheduler 会在 intr_on() 和 intr_off() 之间
  // 短暂打开中断。这时 CPU 上没有进程，yield 里访问 p->lock 就是空指针解引用，要跳过
  if (which_dev == 2 && myproc() != 0)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  // 写回 sepc 和 sstatus
  w_sepc(sepc);
  w_sstatus(sstatus);
}

// 处理时钟中断，做两件事：如果当前是 hart 0，就把全局时钟 ticks 加 1，并唤醒等待时间的进程；
// 不管是哪个 CPU，都设定下一次时钟中断的时间。它不负责切换进程，让出 CPU 的 yield() 是调用者
// 在它返回后才调用的
void
clockintr()
{
  // 只有 hart 0 计时
  if (cpuid() == 0) {
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  // 设定下一次中断，每个 CPU 都必须做
  w_stimecmp(r_time() + 1000000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
// 判断这次陷入是不是设备中断。如果是，就把它处理掉
int
devintr()
{
  uint64 scause = r_scause();

  // scause 的最高位表示这次陷入是中断还是异常，低位是具体的编号：0x8000000000000009
  // 最高位是 1，编号 9，对应 S 态外部中断；0x8000000000000005 的最高位是 1，编号 5，
  // 对应 S 态时钟中断；8 最高位是 0，编号 8，对应用户态 ecall；13、15 最高位是 0，
  // 13 读缺页，15 写缺页
  if (scause == 0x8000000000000009L) {
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    // 外部中断只有一根线连到 CPU，CPU 只知道“有设备要求中断”，不知道是哪个设备，要问 PLIC
    int irq = plic_claim();

    // 按中断号分发
    if (irq == UART0_IRQ) {
      uartintr();
    } else if (irq == VIRTIO0_IRQ) {
      virtio_disk_intr();
    } else if (irq) {
      printk("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    // 通知处理完了
    if (irq)
      plic_complete(irq);

    return 1;
  } else if (scause == 0x8000000000000005L) {
    // timer interrupt.
    // 时钟中断，交给 clockintr()
    clockintr();
    return 2;
  } else {
    return 0;
  }
}
