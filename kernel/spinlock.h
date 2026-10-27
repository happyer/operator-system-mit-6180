// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
#ifdef LAB_LOCK
  int nts;
  int n;
#endif
};

#ifdef LAB_LOCK
// Reader-writer lock.
struct rwspinlock {
  // Replace this with your implementation.
  struct spinlock l;    // protects the three counters below (critical sections are O(1))
  int readers;          // number of active readers inside
  int writing;          // 1 while a writer holds the lock exclusively
  int waiting_writers;  // queued writers; >0 blocks new readers (writer priority)
};
#endif
