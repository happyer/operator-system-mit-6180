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
} kmem;

#ifdef LAB_PGTBL
// Superpage (2MB) allocator: a handful of physically contiguous,
// 2MB-aligned chunks carved off the top of RAM at boot.
#define NSUPER 20
static struct {
  struct spinlock lock;
  uint64 base;            // start of reserved region (2MB-aligned)
  int used[NSUPER];
} supermem;
#endif

void
kinit()
{
  initlock(&kmem.lock, "kmem");
#ifdef LAB_PGTBL
  initlock(&supermem.lock, "supermem");
  // reserve the top NSUPER 2MB chunks of RAM for superpages;
  // PHYSTOP is 2MB-aligned, so supermem.base is too.
  supermem.base = PHYSTOP - (uint64)NSUPER * SUPERPGSIZE;
  freerange(end, (void*)supermem.base);
#else
  freerange(end, (void*)PHYSTOP);
#endif
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

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
#ifdef LAB_PGTBL
  if(is_super((uint64)pa))
    panic("kfree: superpage chunk");   // must go back via superfree()
#endif

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
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
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

#ifdef LAB_PGTBL
// Is pa inside the reserved superpage region?
int
is_super(uint64 pa)
{
  return pa >= supermem.base && pa < PHYSTOP;
}

// Allocate one zeroed 2MB-aligned chunk. Returns 0 if none free.
void *
superalloc(void)
{
  void *pa = 0;
  acquire(&supermem.lock);
  for(int i = 0; i < NSUPER; i++){
    if(supermem.used[i] == 0){
      supermem.used[i] = 1;
      pa = (void*)(supermem.base + (uint64)i * SUPERPGSIZE);
      break;
    }
  }
  release(&supermem.lock);
  if(pa)
    memset(pa, 0, SUPERPGSIZE);
  return pa;
}

// Return a chunk allocated by superalloc.
void
superfree(void *pa)
{
  uint64 a = (uint64)pa;
  if(a < supermem.base || a >= PHYSTOP || (a % SUPERPGSIZE) != 0)
    panic("superfree");
  int i = (a - supermem.base) / SUPERPGSIZE;
  acquire(&supermem.lock);
  if(supermem.used[i] == 0)
    panic("superfree: not in use");
  supermem.used[i] = 0;
  release(&supermem.lock);
}
#endif
