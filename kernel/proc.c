#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
// 给 64 个进程槽各分配一页物理内存作为内核栈，并把它映射到内核页表中，权限是可读写。每个栈下面
// 都留一页不映射，作为保护页
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    // 启动阶段，内存一定够用，分配失败说明出了大问题，所以直接 panic，这一页没有清零，因为栈
    // 不需要初始值，使用前函数会先压栈写入，再读出来
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    // 每个进程占 2 页虚拟地址：上面一页映射成栈，下面一页不映射
    uint64 va = KSTACK((int)(p - proc));
    // 建立虚拟地址和物理地址的映射关系（不是恒等映射）
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
// 初始化进程表 proc[]，在启动时由 hart0 调用一次。主要做三件事：初始化两把全局锁，把 64 个
// 进程槽都标记为空闲，并给每个槽记下它的内核栈地址。它不分配任何内存，也不建立映射
void
procinit(void)
{
  struct proc *p;

  // pid_lock 保护 nextpid。多个 CPU 同时创建进程时，靠它保证每个进程拿到不同的 pid
  initlock(&pid_lock, "nextpid");
  // wait_lock 保护所有进程的 parent 字段，必须先拿到 wait_lock 锁，再拿任何 p->lock
  initlock(&wait_lock, "wait_lock");
  // 遍历 64 个进程槽
  for (p = proc; p < &proc[NPROC]; p++) {
    // 每个进程有自己的锁，保护 state、chan、killed、xstate、pid 这些会被其他 CPU 访问的字段，
    // 锁的名称都相同
    initlock(&p->lock, "proc");
    // 表示这个槽空着，allocproc 可以拿来用
    p->state = UNUSED;
    // 记下这个槽的内核栈虚拟地址
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
// 返回当前 CPU（hart）的编号，也就是 tp 寄存器的值。调用时必须关着中断，否则拿到的编号可能
// 已经过时
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
// 返回当前 CPU 对应的 struct cpu 指针，也需要关着中断
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
// 返回当前 CPU 上正在运行的进程，如果当前 CPU 没有正在运行的进程，返回 0
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

// 分配一个新的进程号
int
allocpid()
{
  int pid;

  // 获取自旋锁
  acquire(&pid_lock);
  // nextpid 是全局变量，初始值是 1，所以 pid 从 1 开始
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
// 从进程表里找一个空闲的槽（UNUSED），把它初始化成“能在内核态里运行”的状态，然后持有 p->lock
// 返回，找不到槽，或者内存分配失败时，返回 0
static struct proc *
allocproc(void)
{
  struct proc *p;

  // 找空槽：检查和占用必须在锁里完成
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) {
      // 找到以后用 goto found 跳出循环，这时锁还没有释放
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  // 分配 pid，标记为 USED
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  // 分配 trapframe，单独占一页物理内存，保存用户态和内核态之间切换时的用户寄存器现场
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  // 建用户页表
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  // 设置 context，新进程从 forkret 开始运行
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  // 返回时还持有锁，原因是调用者还要继续初始化：kfork 要复制内存、复制打开的文件、设置 parent，
  // userinit 要设置 cwd。全部做完后，才把 state 修改为 RUNNABLE，再释放锁。这样，其他 CPU
  // 就不会看到一个初始化到一半的进程
  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
// 回收一个进程槽：释放 trapframe 页和整张用户页表（连同用户内存），把记录进程状态的字段清零，
// 最后把 state 设回 UNUSED，让 allocproc 可以再次使用这个槽。调用时需要持有 p->lock
static void
freeproc(struct proc *p)
{
  // 释放 trapframe，之前分配时，trapframe 是单独分配的一页，所以这里也要单独释放
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0; // 指针置空
  // 释放页表和用户内存
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
// 给进程 p 创建一张新的用户页表。这张页表里还没有任何用户内存，只映射了两页：trampoline 和
// 这个进程的 trapframe，而且都放在用户地址空间的最顶端。成功时返回页表，失败时先释放已分配的资源，再返回 0
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  // 映射 trampoline
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline,
               PTE_R | PTE_X) < 0) {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  // 映射 trapframe
  // 这两页都是给 trampoline.S 用的，用于用户态和内核态之间切换
  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe),
               PTE_R | PTE_W) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
// 创建第一个进程，也就是之后的 init 进程（pid 1）。它只做最少的准备：分配一个进程槽、把当前目录
// 设为根目录、标记为可运行。这时这个进程还没有任何用户程序，加载 /init 是它第一次被调度、运行到
// forkret 时才做的。它在 main() 里由 hart 0 调用，是全局初始化的最后一步
void
userinit(void)
{
  struct proc *p;

  // 拿到一个进程槽，这时进程表全是空的，所以拿到的一定是 proc[0]，pid 是 1
  p = allocproc();
  initproc = p; // 记下这个进程，其他进程退出时，会把子进程过继给它（见 reparent）

  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
// 将当前进程的用户内存扩大或缩小 n 个字节：n > 0 时用 uvmalloc 立即分配并映射新页，n < 0 时
// 用 uvmdealloc 释放末尾的页，最后更新 p->sz。成功返回 0，失败返回 -1
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0) {
    // 检查上限，用户内存从 0 开始往上长，最顶端是 TRAPFRAME 和 TRAMPOLINE
    if (sz + n > TRAPFRAME) {
      return -1;
    }
    // uvmalloc 映射的页固定有 R | U，这里再加上 W，堆是数据，所以没有 X
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if (n < 0) {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
// 创建当前进程的子进程，子进程是父进程的一份副本：用户内存、用户寄存器、打开的文件、当前目录、
// 进程名都复制过去。父进程拿到的是子进程的 pid，子进程从 fork 返回时拿到的是 0
int
kfork(void)
{
  int i, pid;
  // p 是父进程，np 是子进程
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  // 分配进程槽
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // Copy user memory from parent to child.
  // 复制用户内存
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  // 复制用户寄存器：结构体整体赋值，把父进程 trapframe 的 288 字节全部复制过来，所以子进程回到
  // 用户态时，所有寄存器都和父进程一样
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0; // a0 存放返回值，所以子进程看到的 fork 返回值是 0

  // increment reference counts on open file descriptors.
  // 共享打开的文件和当前目录
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  // 复制进程名
  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid; // pid 必须要在锁释放前读出来

  release(&np->lock);

  // 设置父进程，parent 字段受 wait_lock 保护，先获取锁
  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
// 在进程 p 退出时，把 p 的所有子进程过继给 init 进程：凡是 parent == p 的进程，都把 parent 改
// 成 initproc，并唤醒 init。调用者必须持有 wait_lock
// 进程退出以后不会马上消失，而是变成 ZOMBIE，等父进程调用 wait 来回收。回收时 kwait 才会调用
// freeproc，把槽设为 UNUSED
void
reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->parent == p) {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
// 让当前进程退出，并且永远不会返回。它先关闭所有打开的文件、放掉当前目录，再把子进程过继给 init，
// 唤醒父进程，最后把自己设成 ZOMBIE，切换到 scheduler，从此不再运行。它不释放用户内存、页表
// 和 trapframe。这些要等到父进程调用 wait 时，由 kwait 里的 freeproc 来回收
void
kexit(int status)
{
  struct proc *p = myproc();

  // init 不能退出
  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  // 关闭文件、放掉当前目录（不持有任何锁）
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  // iput 可能要写磁盘（释放 inode），文件系统的修改必须要放在一次日志事务里
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  // 持有 wait_lock，过继子进程、唤醒父进程
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  // 记录退出码，变成 ZOMBIE
  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
// 让当前进程等待任意一个子进程退出。找到一个已经是 ZOMBIE 的子进程后，它会把子进程的退出码写到
// 用户地址 addr，用 freeproc 回收这个子进程，然后返回子进程的 pid。如果没有任何子进程，或者
// 当前进程已经被 kill，就返回 -1。如果有子进程但还没有退出，它就睡眠，等子进程在 kexit 里将
// 它唤醒后再查一遍
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  // 先拿 wait_lock，它保护所有进程的 parent 字段。持有它，扫描期间子进程就无法在 kexit 里
  // 完成退出，也不会有 reparent 改动父子关系
  acquire(&wait_lock);

  for (;;) {
    // Scan through table looking for exited children.
    havekids = 0;
    // struct proc 里没有子进程列表，只能把 64 个槽全扫一遍
    for (pp = proc; pp < &proc[NPROC]; pp++) {
      // 如果 pp 是 p 的子进程，就检查它的状态
      if (pp->parent == p) {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE) {
          // Found one.
          pid = pp->pid;
          // 将子进程的退出码写到用户地址 addr
          if (addr != 0 &&
              copyout(p->pagetable, p->sz, addr, (char *)&pp->xstate,
                      sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          pp->parent = 0;
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    // 没有子进程或者自己已经被 kill，就返回 -1
    if (!havekids || killed(p)) {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    // 睡眠：等子进程退出
    sleep_prepare(p); //DOC: wait-sleep
    release(&wait_lock);
    sleep();
    acquire(&wait_lock);
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
// scheduler() 是每个 CPU 上的调度循环，永远不会返回。它不停地扫描进程表，找到一个 RUNNABLE
// 的进程，就用 swtch 切过去运行。那个进程让出 CPU 时，会再 swtch 回到这里，循环接着往下找。一轮
// 扫描下来一个可运行的进程都没有，就执行 wfi 停下来等中断
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  // 每个 CPU 核心（struct cpu）都有一个专属的调度器线程
  c->proc = 0;
  for (;;) {
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    // 为什么要开：刚切回来时中断是关着的，如果所有进程都在睡眠、等磁盘或时钟中断来唤醒，而 scheduler
    // 一直关着中断空转，就没有人能够唤醒它们。开一下，让已经挂起的中断有机会被处理
    intr_on();
    // 为什么又关：后面找不到进程时要执行 wfi。如果中断开着，中断可能正好在“扫描完、发现没有进程”和“
    // 执行 wfi”之间到来，被处理掉以后，wfi 就要一直等到下一个中断才醒，白白多等一轮
    intr_off();

    int found = 0;
    // 找到一个 RUNNABLE 的进程
    for (p = proc; p < &proc[NPROC]; p++) {
      // state 受 p->lock 保护，必须先拿锁再检查。多个 CPU 的 scheduler 同时扫描同一张进程表，
      // 有了锁，同一个进程只会被一个 CPU 选中。这是最简单的轮转调度：没有优先级，从 proc[0] 扫到
      // proc[63]，遇到 RUNNABLE 就运行它。等它让出 CPU 后，从下一个槽接着扫
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        // scheduler 也是一个线程，swtch 把 scheduler 当前的寄存器存进 c->context，再从
        // p->context 恢复进程的寄存器，然后 ret，带着 p->lock 切换
        swtch(&c->context, &p->context);

        // Don't re-enable interrupts on release.
        // 切回来以后把 intena 设为 0，这样下面 release(&p->lock) 时不会打开中断
        mycpu()->intena = 0;

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if (found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      // 让 CPU 暂停执行，直到有中断挂起
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
// 让当前进程的内核线程主动让出 CPU，切换到这个 CPU 的 scheduler。它只做三件事：检查调用条件、调用
// swtch、在切换前后保存和恢复 intena。调用者要先把 p->state 改成某个不是 RUNNING 的状态，sched()
// 本身不决定进程接下来是什么状态
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  // 检查是否持有 p->lock
  if (!holding(&p->lock))
    panic("sched p->lock");
  // 只能持有这一把锁
  if (mycpu()->noff != 1)
    panic("sched locks");
  // 状态不能是 RUNNING
  if (p->state == RUNNING)
    panic("sched RUNNING");
  // 中断必须是关着的
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
// yield() 让当前进程主动让出一轮 CPU：把自己设成 RUNNABLE，调用 sched() 切到 scheduler。它不睡眠，
// 不等待任何条件，scheduler 下次扫描到它时就会让它接着运行
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
// 每个新进程的内核线程第一次运行时执行的函数。allocproc 把 context.ra 设成了 forkret，所以
// scheduler 第一次 swtch 到这个进程时，ret 会跳到这里。它做三件事：释放 scheduler 持有的
// p->lock；如果是系统的第一个进程，就初始化文件系统并加载 /init；最后跳进 trampoline 里的
// userret，返回到用户态。它不会返回，因为最后跳进的 userret 以 sret 结束
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  // 以原子且带有获取内存序的方式读取变量的值
  if (__atomic_load_n(&first, __ATOMIC_ACQUIRE)) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    // ensure other cores see first=0.
    __atomic_store_n(&first, 0, __ATOMIC_RELEASE);

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Register current process as waiting for wakeups on chan.
// 把当前进程登记为正在等待 chan 上的唤醒，也就是在持有 p->lock 时把 p->chan 设为 chan
void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (chan == 0)
    panic("sleep_prepare: zero chan");
  p->chan = chan;
  release(&p->lock);
}

// Put the thread to sleep.  Assumes sleep_prepare() was called before.
// If the channel registered by sleep_prepare() has been woken up in
// the meantime, do not go to sleep, and instead return immediately.
// sleep() 让当前进程真正睡下去：如果 p->chan 还不是 0，也就是登记以后还没有被唤醒，就把状态
// 设成 SLEEPING，调用 sched() 切到 scheduler。如果 p->chan 已经是 0，说明登记以后、睡下
// 之前，唤醒已经发生了，就直接返回，不睡
void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->chan != 0) {
    p->state = SLEEPING;
    sched();
  }
  release(&p->lock);
}

// Wake up all processes sleeping on channel chan.
// 唤醒所有在 chan 上等待的进程。它扫描整个进程表，凡是 p->chan == chan 的进程，都把 chan 清零，
// 表示“唤醒已经发生”。如果那个进程已经睡下了（SLEEPING），再把它改成 RUNNABLE。它不会切换 CPU，
// 被唤醒的进程要等 scheduler 下次选中它才会运行
void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->chan == chan) {
      // If the process is waiting for wakeups on this channel,
      // signal that the wakeup happened by clearing p->chan.
      p->chan = 0;

      // If this waiting process has gotten so far as to actually
      // go to sleep, also set it back to RUNNING.
      if (p->state == SLEEPING) {
        p->state = RUNNABLE;
      }
    }
    release(&p->lock);
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
// 在进程表里找到 pid 等于参数的进程，把它的 killed 标志设置为 1。如果它正在睡眠，就改成 RUNNABLE，
// 让它醒过来。找到返回 0，找不到返回 -1。它不会立刻杀死进程，只是做一个标记
int
kkill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      p->killed = 1;
      if (p->state == SLEEPING) {
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
// 把内核里 src 开始的 len 个字节复制到 dst。dst 可能是用户地址，也可能是内核地址，由 user_dst
// 决定：为 1 时 dst 是当前进程的用户虚拟地址，走 copyout；为 0 时 dst 是内核地址，直接 memmove
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  // dst 是用户虚拟地址，在内核页表里没有映射，不能直接当指针用
  if (user_dst) {
    return copyout(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
// 把进程表里所有正在使用的槽打印到控制台，每行一个进程，格式是 pid 状态 进程名。它只用来调试：在 xv6
// 控制台按 Ctrl-P 就会调用它。它故意不加锁，这样在系统卡死时也能用它看看各个进程停在什么状态
void
procdump(void)
{
  // C99 引入的数组指定初始化器，[SLEEPING] = "sleep " 的意思是：直接将数组中下标为 SLEEPING 的
  // 元素初始化为 "sleep"
  static char *states[] = {
    // clang-format off
    [UNUSED]    = "unused",
    [USED]      = "used",
    [SLEEPING]  = "sleep ",
    [RUNNABLE]  = "runble",
    [RUNNING]   = "run   ",
    [ZOMBIE]    = "zombie"
    // clang-format on
  };
  struct proc *p;
  char *state;

  printk("\n");
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printk("%d %s %s", p->pid, state, p->name);
    printk("\n");
  }
}
