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

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  // COW: number of user page-table references to each physical page,
  // indexed by (pa - KERNBASE) / PGSIZE. uchar suffices: at most
  // NPROC(64) processes can share one page.
  uchar refcnt[(PHYSTOP - KERNBASE) / PGSIZE];
} kmem;

// Serializes COW fault copying: two CPUs may fault on the same
// shared page simultaneously (forkfork test hunts exactly this race).
// Lock order: cow_lock -> kmem.lock (never the reverse).
struct spinlock cow_lock;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&cow_lock, "cow");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int idx = ((uint64)pa - KERNBASE) / PGSIZE;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem.lock);
  // Drop one reference. Note: freerange() at boot calls kfree on pages
  // whose refcnt is 0 -- never decrement below zero (uchar would wrap
  // to 255 and the page would look shared forever).
  if(kmem.refcnt[idx] > 0)
    kmem.refcnt[idx]--;
  if(kmem.refcnt[idx] > 0){
    // Other page tables still map this page: just detach, don't recycle.
    release(&kmem.lock);
    return;
  }
  // Fill with junk to catch dangling refs -- only when truly recycled
  // (doing this earlier would scribble on a page someone still uses).
  memset(pa, 1, PGSIZE);
  r = (struct run*)pa;
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// COW: one more page table now maps this physical page.
void
ref_incr(void *pa)
{
  int idx = ((uint64)pa - KERNBASE) / PGSIZE;
  acquire(&kmem.lock);
  kmem.refcnt[idx]++;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
    kmem.refcnt[((uint64)r - KERNBASE) / PGSIZE] = 1;  // fresh page: one user
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
