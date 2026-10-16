#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
// 创建内核页表，返回根页表的地址。它分配一页作为根页表，把内核运行需要访问的每一块地址都映射进去
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  // 分配根页表，必须清零
  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext,
         PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
// 在一张 Sv39 三级页表里查虚拟地址 va，返回最后一级（level 0）PTE 的地址
// pagetable：根页表的物理地址，va：要查的虚拟地址，alloc：中间级页表不存在时要不要新建
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  // 用户/内核普通映射只用了下半区，检查地址范围
  if (va >= MAXVA)
    panic("walk");

  // 循环只处理 level 2 和 level 1，因为这两级的 PTE 指向的是下一级的页表
  for (int level = 2; level > 0; level--) {
    // 取出这一级的 PTE，PX(level, va) 取出 va 里这一级的 9 位索引，0~511
    // pagetable 当作有 512 项的数组用
    pte_t *pte = &pagetable[PX(level, va)];
    // 这一项有效：往下走一级
    if (*pte & PTE_V) {
      // Sv39 的 PTE 是 64 位，低 10 位是标志位（V/R/W/X/U/G/A/D 和 RSW），第 10~53 位是 44 位的 PPN（物理页号）
      // 右移 10 位去掉低 10 位的标志位，左移 12 位，补回 12 个 0 作为页内偏移
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      // 这一项无效：新建或者放弃；!alloc：调用者只是查询，不要求新建，直接返回 0
      // kalloc() 返回 0：内存耗尽，返回 0
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      // 拿到新的一页，清零
      memset(pagetable, 0, PGSIZE);
      // 把新页的物理地址写进当前这一项，只设 PTE_V，不设 R/W/X。按照 RISC-V 规范，V = 1，
      // 而 R/W/X 全为 0，表示这是指向下一级页表的非叶子 PTE
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  // 返回 level 0 的 PTE 地址
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
// 查询用户虚拟地址 va 所在页的物理地址。没有映射或者不是用户页时，返回 0
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  // 地址越界时返回 0，不 panic，walkaddr 处理的 va 往往来自用户程序，传进来一个非法地址是
  // 正常情况，系统调用返回 -1 就行，不能让整个内核 panic
  if (va >= MAXVA)
    return 0;

  // 这里只是查询，路上缺少中间级页表，就说明这个地址没有映射，没有理由为了查一下就新建页表
  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  // 检查 level 0 的 PTE 是否有效，walk 只检查 level 2/1
  if ((*pte & PTE_V) == 0)
    return 0;
  // 只接受用户页
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  // 返回页的起始物理地址
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
// 在页表里建立一段连续的映射：把虚拟地址 [va, va + size) 一页一页映射到从 pa 开始的物理内存，
// 权限是 perm。成功返回 0，中间级页表分配失败返回 -1，只负责填写 PTE，不刷新 TLB，也不打开分页
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  // 检查 va 是否按页对齐，页表只能按整页映射
  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  // last 记下最后一页的起始地址
  last = va + size - PGSIZE;
  for (;;) {
    // 找到这一页的 PTE
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    // 检查是否重复映射
    if (*pte & PTE_V)
      panic("mappages: remap");
    // 写入 PTE
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    // 移到下一页
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
// 创建一张空的用户页表：分配一页作为根页表，清零后返回它的地址，内存不够时返回 0，不做映射
pagetable_t
uvmcreate()
{
  // 分配根页表
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
// 删掉从 va 开始的 npages 页映射：把这些页的 level 0 PTE 清零。do_free 为 1 时，还会把这些
// PTE 指向的物理页 kfree 掉，没有映射的页直接跳过，不报错
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    // 跳过没有映射的页，第一种：中间级页表不存在，第二种：中间级页表存在，但这一页没有映射
    if ((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;
    if ((*pte & PTE_V) == 0) // has physical page been allocated?
      continue;
    // 可选地释放物理页
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// 把用户内存从 oldsz 扩大到 newsz，中间缺的每一页，它都会分配物理页、清零，再映射进页表，成功
// 返回 newsz，失败返回 0，并撤销这次已经分配的所有页
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  // 缩小时什么都不做
  if (newsz < oldsz)
    return oldsz;

  // oldsz 所在的那一页已经属于进程（懒分配时可能还没真正映射，访问时由 vmfault 补上），
  // 所以向上取整，从下一页开始分配
  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    // mem 是 0，没什么要释放的，撤销 [oldsz, a) 之间这次已经映射好的页
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) !=
        0) {
      // mem 已经分配，但还没有映射进页表，uvmdealloc 找不到它，所以要先 kfree
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
// 把用户内存从 oldsz 缩小到 newsz，完全落在 newsz 之后的页会被取消映射，并释放对应的物理页，
// newsz 所在的那一页会保留下来，返回 newsz
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
// 递归地释放一张页表的所有页表页：根页表，加上所有中间级和最后一级的页表页。它不释放叶子 PTE 指向
// 的物理页，并且要求调用前所有叶子映射都已经删掉了，否则直接 panic
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    // V = 1 时，R、W、X 至少有一个是 1 就是叶子，全为 0 就是指向下一级页表的 PTE
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      // 先把下一级页表整个释放掉，再处理当前这一项
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
// 释放一整张用户页表：先释放 [0, sz) 范围内所有用户页对应的物理页，再用 freewalk 释放页表页本身
// 调用之后，这张页表就不能再用了
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    // 删掉叶子映射，并释放物理页
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  // 释放页表页
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0)
      continue; // page table entry hasn't been allocated
    if ((*pte & PTE_V) == 0)
      continue; // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 psz, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if ((*pte & PTE_W) == 0)
      return -1;

    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva,
          uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null) {
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 psz, uint64 va, int read)
{
  uint64 mem;

  if (va >= psz)
    return 0;
  va = PGROUNDDOWN(va);
  if (ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64)kalloc();
  if (mem == 0)
    return 0;
  memset((void *)mem, 0, PGSIZE);
  if (mappages(pagetable, va, PGSIZE, mem, PTE_W | PTE_U | PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V) {
    return 1;
  }
  return 0;
}
