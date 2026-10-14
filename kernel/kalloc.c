// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

// 空闲页链表的节点类型，只有一个字段 next，指向下一个空闲页
struct run {
  struct run *next;
};

// 物理内存分配的全部状态：一个空闲链表头 freelist，加上保护它的锁 lock
// 匿名结构体，同时定义变量
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void *)PHYSTOP);
}

// 将 [pa_start, pa_end) 这段物理内存按 4KB 切成一页一页，逐页调用 kfree()，交给空闲链表管理
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  // 起点向上对齐到页边界
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// 把一页 4KB 物理内存还给分配器
void
kfree(void *pa)
{
  struct run *r;

  // 检查 pa 是否是合法的页地址：4KB 对齐，位于内核结束地址和物理内存上限之间
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  // 将这一页当作链表节点，空闲链表的节点就是空闲页本身：每一页的前 8 字节存放指向下一个空闲页的
  // 指针，这样分配器自己就不需要额外的内存，空闲页本来也没人使用
  r = (struct run *)pa;

  // 将这一页插入空闲链表的头部
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// 从空闲链表头部取下一页 4KB 物理内存，用垃圾值填满后返回它的地址，返回类型是 void *，表示指向
// 某页内存，但不规定里面是什么类型；在 C 里，void * 不用强制转换就能赋值给任何对象指针（函数指针除外）
void *
kalloc(void)
{
  struct run *r;

  // 取下链表头（加锁部分）
  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}
