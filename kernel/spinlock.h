// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
  uint n;
  uint nts;
};

// spinlock中额外的这两个参数n、nts就是在lock lab中统计lock的acquire次数的

// 查看acquire函数，可以知道:
// n: 表示的是acquire这个锁的次数
// nts: 表示的是在获取锁的时候自旋的次数 