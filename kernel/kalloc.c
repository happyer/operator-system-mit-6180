// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers.  Allocates whole 4096-byte pages.
//
// Lock lab: one free list (and one lock) PER CPU, so alloc/free on
// different CPUs run in parallel. When a CPU's list is empty it
// "steals" half of another CPU's list. We never hold two kmem locks
// at the same time, so no lock ordering (and no deadlock) is needed.
//
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
} kmem[NCPU];

void
kinit()
{
  // All locks are named "kmem..." so kalloctest's stats pick them up.
  for(int c = 0; c < NCPU; c++)
    initlock(&kmem[c].lock, "kmem");
  // freerange() -> kfree() -> cpuid() runs on the boot CPU, so all
  // free memory lands in that CPU's list; others steal as needed.
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

// Steal another CPU's ENTIRE free list in O(1), atomically:
// we hold BOTH locks (always acquired in ascending CPU order, so
// no deadlock cycle) while the chain hops from the victim's list to
// ours -- it is never "in transit", so no concurrent kalloc can see
// every list empty and wrongly fail. Stealing whole lists also makes
// steals rare (once per ~30k pages in test4's sbrk storms), and the
// O(1) lock window means victims never spin behind a list walk.
static void
steal(int me)
{
  for(int other = 0; other < NCPU; other++){
    if(other == me)
      continue;
    struct spinlock *l0 = me < other ? &kmem[me].lock : &kmem[other].lock;
    struct spinlock *l1 = me < other ? &kmem[other].lock : &kmem[me].lock;
    acquire(l0);
    acquire(l1);
    if(kmem[me].freelist == 0 && kmem[other].freelist != 0){
      kmem[me].freelist = kmem[other].freelist;   // O(1) atomic transfer
      kmem[other].freelist = 0;
    }
    release(l1);
    release(l0);
    // Success check under our own lock (someone may have stolen from us,
    // or a kfree may have refilled us -- either way, stop stealing).
    acquire(&kmem[me].lock);
    int ok = (kmem[me].freelist != 0);
    release(&kmem[me].lock);
    if(ok)
      return;
  }
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

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();                      // pin this CPU: cpuid must not go stale
  int c = cpuid();                 // between here and the acquire below
  acquire(&kmem[c].lock);
  r->next = kmem[c].freelist;
  kmem[c].freelist = r;
  release(&kmem[c].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();                      // pin this CPU (see kfree)
  int c = cpuid();

  acquire(&kmem[c].lock);
  r = kmem[c].freelist;
  if(r)
    kmem[c].freelist = r->next;
  release(&kmem[c].lock);

  if(r == 0){
    // Local list empty: steal a batch from some other CPU, then retry.
    steal(c);
    acquire(&kmem[c].lock);
    r = kmem[c].freelist;
    if(r)
      kmem[c].freelist = r->next;
    release(&kmem[c].lock);
  }
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk

  return (void*)r;
}
