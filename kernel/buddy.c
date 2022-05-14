#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

// Buddy allocator

static int nsizes;     // the number of entries in bd_sizes array； buddy数组的长度

#define LEAF_SIZE     16                         // The smallest block size
#define MAXSIZE       (nsizes-1)                 // Largest index in bd_sizes array
#define BLK_SIZE(k)   ((1L << (k)) * LEAF_SIZE)  // Size of block at size k
#define HEAP_SIZE     BLK_SIZE(MAXSIZE) 
#define NBLK(k)       (1 << (MAXSIZE-k))         // Number of block at size k
#define ROUNDUP(n,sz) (((((n)-1)/(sz))+1)*(sz))  // Round up to the next multiple of sz

typedef struct list Bd_list;

// The allocator has sz_info for each size k. Each sz_info has a free
// list, an array alloc to keep track which blocks have been
// allocated, and an split array to to keep track which blocks have
// been split.  The arrays are of type char (which is 1 byte), but the
// allocator uses 1 bit per block (thus, one char records the info of
// 8 blocks).
struct sz_info {
  Bd_list free;
  char *alloc;
  char *split;
};
typedef struct sz_info Sz_info;

static Sz_info *bd_sizes;  // 全局的buddy数据结构，每一项是一个链表
static void *bd_base;   // start address of memory managed by the buddy allocator； buddy 系统管理的内存起始地址
static struct spinlock lock;

// Return 1 if bit at position index in array is set to 1
int bit_isset(char *array, int index) {
  char b = array[index/8];
  char m = (1 << (index % 8));
  return (b & m) == m;
}

// Set bit at position index in array to 1
void bit_set(char *array, int index) {
  char b = array[index/8];
  char m = (1 << (index % 8));
  array[index/8] = (b | m);
}

// Clear bit at position index in array
void bit_clear(char *array, int index) {
  char b = array[index/8];
  char m = (1 << (index % 8));
  array[index/8] = (b & ~m);
}

// 翻转位
void bit_flip(char *array, int index){
  if(bit_isset(array, index)){
    bit_clear(array, index);
  }else{
    bit_set(array, index);
  }
}

// Print a bit vector as a list of ranges of 1 bits
void
bd_print_vector(char *vector, int len) {
  int last, lb;
  
  last = 1;
  lb = 0;
  for (int b = 0; b < len; b++) {
    if (last == bit_isset(vector, b))
      continue;
    if(last == 1)
      printf(" [%d, %d)", lb, b);
    lb = b;
    last = bit_isset(vector, b);
  }
  if(lb == 0 || last == 1) {
    printf(" [%d, %d)", lb, len);
  }
  printf("\n");
}

// Print buddy's data structures
void
bd_print() {
  for (int k = 0; k < nsizes; k++) {
    printf("size %d (blksz %d nblk %d): free list: ", k, BLK_SIZE(k), NBLK(k));
    lst_print(&bd_sizes[k].free);
    printf("  alloc:");
    bd_print_vector(bd_sizes[k].alloc, NBLK(k));
    if(k > 0) {
      printf("  split:");
      bd_print_vector(bd_sizes[k].split, NBLK(k));
    }
  }
}

// What is the first k such that 2^k >= n?
int
firstk(uint64 n) {
  int k = 0;
  uint64 size = LEAF_SIZE;

  while (size < n) {
    k++;
    size *= 2;
  }
  return k;
}

// Compute the block index for address p at size k
int
blk_index(int k, char *p) {
  int n = p - (char *) bd_base;
  return n / BLK_SIZE(k);
}

// Convert a block index at size k back into an address
void *addr(int k, int bi) {
  int n = bi * BLK_SIZE(k);
  return (char *) bd_base + n;
}

// allocate nbytes, but malloc won't return anything smaller than LEAF_SIZE
// 从buddy中分配内存
void *
bd_malloc(uint64 nbytes)
{
  int fk, k;

  acquire(&lock);

  // Find a free block >= nbytes, starting with smallest k possible
  fk = firstk(nbytes);
  // 首先对nbytes上取整为2的次幂大小

  // 从大小和fk相同的位置开始遍历链表
  for (k = fk; k < nsizes; k++) {
    if(!lst_empty(&bd_sizes[k].free))
      break;
  }
  // 找到bd_sizes最后了，意味着找完了，还没找到空闲内存
  if(k >= nsizes) { // No free blocks?
    release(&lock);
    return 0;
  }

  // Found a block; pop it and potentially split it.
  // 找到了，
  char *p = lst_pop(&bd_sizes[k].free);
  // 在这一级的alloc数组中更新这一块内存，
  bit_flip(bd_sizes[k].alloc, blk_index(k, p)/2);
  // 把未分配的伙伴放到下一级链表中
  for(; k > fk; k--) {
    // 在这个for循环中，p指针一直保持不变
    // split a block at size k and mark one half allocated at size k-1
    // and put the buddy on the free list at size k-1
    char *q = p + BLK_SIZE(k-1);   // p's buddy
    bit_set(bd_sizes[k].split, blk_index(k, p));
    bit_flip(bd_sizes[k-1].alloc, blk_index(k-1, p)/2);
    lst_push(&bd_sizes[k-1].free, q);
    // 在for循环中，p指针一直不变，只不过一直在试图把p的大小切分，知道满足malloc的大小要求。
    // 和bd_free对比的看
  }
  release(&lock);

  return p;
}

// Find the size of the block that p points to.
int
size(char *p) {
  for (int k = 0; k < nsizes; k++) {
    if(bit_isset(bd_sizes[k+1].split, blk_index(k+1, p))) {
      return k;
    }
  }
  return 0;
}

// Free memory pointed to by p, which was earlier allocated using
// bd_malloc.
void
bd_free(void *p) {
  void *q;
  int k;

  acquire(&lock);
  for (k = size(p); k < MAXSIZE; k++) {
    int bi = blk_index(k, p);
    int buddy = (bi % 2 == 0) ? bi+1 : bi-1;
    // // 首先把这个块对应大小的alloc位给清0了，表示已经释放了这个块
    // bit_clear(bd_sizes[k].alloc, bi);  // free p at size k
    // // 然后看一下他的伙伴是否空闲，如果不空闲，啥也做不了；如果空闲，就可以合并
    // if (bit_isset(bd_sizes[k].alloc, buddy)) {  // is buddy allocated?
    //   break;   // break out of loop
    // }
    bit_flip(bd_sizes[k].alloc, bi/2);

    if(bit_isset(bd_sizes[k].alloc, bi/2)) {
      // 如果这一位是0，表示两个都分配出去了，现在只释放了一个伙伴，那就反转这个比特位，然后直接把释放的这一块加入到空闲链表，这种情况不能合并。
      // TODO:这儿感觉不太对，如果这一位是1，表示只有一个伙伴分配出去了，就是p,那现在释放p，应该是p和伙伴可以合并了？
      // bit_flip(bd_sizes[k].alloc, bi/2);
      break;
    }
    // Note:我之前写的是下面这样的,但是这个有个问题
    // 这种写法是说，如果两个都分配出去了，那么现在一个回来了，
    // 存在的问题就是，无法判断伙伴中哪一个是被分配出去用的，哪一个是现在释放的，导致从链表remove的时候可能出现空指针orz.
    // if(!bit_isset(bd_sizes[k].alloc, bi/2)) {
    //   bit_flip(bd_sizes[k].alloc, bi/2);
    //   break;
    // }

    
    // budy is free; merge with buddy
    q = addr(k, buddy);
    if(!q) continue;
    // 把伙伴的地址从当前块大小的free链表中删除
    lst_remove(q);    // remove buddy from free list
    // 找到伙伴中的第一个，(偶数，基数)算一个伙伴，第一个就是说偶数表示的那一个
    if(buddy % 2 == 0) {
      p = q;
    }
    // at size k+1, mark that the merged buddy pair isn't split
    // anymore
    // 把伙伴合并之后的上一级split清0
    bit_clear(bd_sizes[k+1].split, blk_index(k+1, p));

    // 在for循环中，指针p基本一直不变，只不过在遇到伙伴的时候可能指向第一个伙伴。通过循环扩大指针p指向的内存大小，最后加到空闲链表
  }
  // 最后把最大的空闲伙伴加入到空闲链表free
  lst_push(&bd_sizes[k].free, p);
  release(&lock);
}

// Compute the first block at size k that doesn't contain p
int
blk_index_next(int k, char *p) {
  int n = (p - (char *) bd_base) / BLK_SIZE(k);
  if((p - (char*) bd_base) % BLK_SIZE(k) != 0)
      n++;
  return n ;
}

int
log2(uint64 n) {
  int k = 0;
  while (n > 1) {
    k++;
    n = n >> 1;
  }
  return k;
}

// Mark memory from [start, stop), starting at size 0, as allocated. 
// 标记为已经使用
void
bd_mark(void *start, void *stop)
{
  int bi, bj;

  if (((uint64) start % LEAF_SIZE != 0) || ((uint64) stop % LEAF_SIZE != 0))
    panic("bd_mark");

  for (int k = 0; k < nsizes; k++) {
    // 可以看到，这儿也是按照块大小不同进行分的
    bi = blk_index(k, start);
    bj = blk_index_next(k, stop);
    for(; bi < bj; bi++) {
      if(k > 0) {
        // if a block is allocated at size k, mark it as split too.
        bit_set(bd_sizes[k].split, bi);
      }
      bit_flip(bd_sizes[k].alloc, bi/2);
      // update2. index除以2 
      // bd_mark是标记一块内存为已经使用.
      // 一共有两处，一处是元数据，一处是最后的margin
    }
  }
}

// If a block is marked as allocated and the buddy is free, put the
// buddy on the free list at size k.
// 回过头看这个函数就比较清楚了，就是说按在bd_sizes数组中第k项的块大小计算的第bi个块加入到k的free链表中管理
int
bd_initfree_pair(int k, int bi) {
  // int buddy = (bi % 2 == 0) ? bi+1 : bi-1;
  // bi = (bi % 2 == 0) ? bi-1: bi;
  int free = 0;
  // 不论序号是基数还是偶数，除以2的余数就是在alloc位图中的标记，
  // 如果另一个伙伴已经被分配了(在前面的元数据以及不可用内存中标记为已经使用),那么位图中相应的比特位是1，同时可知，bi这个块肯定是未使用了，
  // 通过这两个条件可知，如果对应的位图中的比特位是1，那么这一对伙伴被拆开了，即把当前这一个加入到空闲链表中。
  if(bit_isset(bd_sizes[k].alloc, bi/2)) {
    // update3.第三处修改
    // 这儿bit_isset设置为1的话，说明一个伙伴已经被分配出去了，而调用此函数的参数bi肯定是空闲状态的，所以直接把bi加到空闲链表即可
    free = BLK_SIZE(k);
    lst_push(&bd_sizes[k].free, addr(k, bi));
  }
  // if(bit_isset(bd_sizes[k].alloc, bi) !=  bit_isset(bd_sizes[k].alloc, buddy)) {
  //   // 如果伙伴的状态不一样，那么肯定是一个在使用，一个空闲。找到空闲的哪个，加入和他块大小相同的链表
  //   // one of the pair is free
  //   free = BLK_SIZE(k);
  //   if(bit_isset(bd_sizes[k].alloc, bi))
  //     lst_push(&bd_sizes[k].free, addr(k, buddy));   // put buddy on free list
  //   else
  //     lst_push(&bd_sizes[k].free, addr(k, bi));      // put bi on free list
  // }
  return free;
}

// Initialize the free lists for each size k.  For each size k, there
// are only two pairs that may have a buddy that should be on free list:
// bd_left and bd_right.
// 这是buddy的另一个初始化函数。
// 从最两边开始分，然后一次扩大k值，太巧妙了woc
int
bd_initfree(void *bd_left, void *bd_right) {
  int free = 0;

  for (int k = 0; k < MAXSIZE; k++) {   // skip max size
    int left = blk_index_next(k, bd_left);         // blk_index_next(a, b)：计算b相对于bd_base的偏移序号，每个块大小为k，然后再上取整加1
    int right = blk_index(k, bd_right)-1;            // blk_index：基本和blk_index_next相同，不上取整加1
    // 根据debug，right对应的块在不可用内存中的第一页个块，因此right-1表示被buddy管理的最后一个块

    if(right <= left)                              // （因为左边是buddy的元数据占用，右边是不可用的部分地址）。所以需要在继续判断一次，看能不能扩充空闲块。
      continue;
    
    free += bd_initfree_pair(k, left);             // 对于每一种大小的块，在内部的块已经在bd_init中标记为0，即标记为空闲，而处于两边边界的块则还需要进一步判断
    free += bd_initfree_pair(k, right);
    // 判断右边界的块是否空闲；同上
    // 有可能在左右两侧都出现了这个问题，所以初始化之后链表最长可能为3
  }
  return free;
}

// Mark the range [bd_base,p) as allocated
int
bd_mark_data_structures(char *p) {
  int meta = p - (char*)bd_base;
  printf("bd: %d meta bytes for managing %d bytes of memory\n", meta, BLK_SIZE(MAXSIZE));
  bd_mark(bd_base, p);
  return meta;
}

// Mark the range [end, HEAPSIZE) as allocated
int
bd_mark_unavailable(void *end, void *left) {
  int unavailable = BLK_SIZE(MAXSIZE)-(end-bd_base);
  if(unavailable > 0)
    unavailable = ROUNDUP(unavailable, LEAF_SIZE);
  printf("bd: 0x%x bytes unavailable\n", unavailable);

  void *bd_end = bd_base+BLK_SIZE(MAXSIZE)-unavailable;
  bd_mark(bd_end, bd_base+BLK_SIZE(MAXSIZE));
  return unavailable;
}

// Initialize the buddy allocator: it manages memory from [base, end).
// 1、先看这儿，buddy系统的初始化，给出buddy系统维护的物理内存区间
void
bd_init(void *base, void *end) {
  char *p = (char *) ROUNDUP((uint64)base, LEAF_SIZE);
  // 首先计算buddy分配器所管理物理内存的起始地址，计算内存对齐;即物理内存起始地址不约定按照最小的buddy块对齐(即设置base增加到最小的16字节的倍数)
  int sz;

  initlock(&lock, "buddy");
  bd_base = (void *) p;
  // buddy alloctor全局变量bd_base

  // compute the number of sizes we need to manage [base, end)
  // 这儿似乎是说，nsizes的大小是在LEAF_SIZE这一个链表中管理所有内存区域所对应的数组大小？
  // bd: memory sz is 134045696 bytes; allocate an size array of length 24，
  // 总的内存大小接近128MB，16B * 2^24 = 128MB
  nsizes = log2(((char *)end-p)/LEAF_SIZE) + 1;
  if((char*)end-p > BLK_SIZE(MAXSIZE)) {
    nsizes++;  // round up to the next power of 2
  }
  // nsizes 对应在buddy数组的长度，这样算出来的nsizes大小表示，最大快一块可以表示所有内存
  // 通过debug可知，nsize=24，即2^(24-1) * 16B = 128MB.基本等于buddy管理的物理内存大小

  printf("bd: memory sz is %d bytes; allocate an size array of length %d\n",
         (char*) end - p, nsizes);

  // allocate bd_sizes array
  bd_sizes = (Sz_info *) p;   // bd_sizes 就是buddy的数组，全局变量，记录物理内存块分配的元数据bd_sizes
  p += sizeof(Sz_info) * nsizes;   // p指针后移，空出的内存用来初始化buddy长度为nsizes的数组bd_sizes
  memset(bd_sizes, 0, sizeof(Sz_info) * nsizes);  // 把相应的空间置为0

  // initialize free list and allocate the alloc array for each size k
  for (int k = 0; k < nsizes; k++) {    // 初始化每个数组的双向链表，以及alloc这一块作用是bitmap的内存
    lst_init(&bd_sizes[k].free);  // 初始化内核双向链表
    sz = sizeof(char)* ROUNDUP(NBLK(k)/2, 8)/8; // update1. 在这儿再多除一个2，表示一对伙伴只用一个bit
    // sz 是alloc数组的长度，用字节表示

    // sz就是计算每中大小的块最多有多少个，即对于每个bd_sizes[k],其free链表的长度，
    // 因为把整个物理分成单一的块大小都可以管理，即对应每种块大小，sz = 当前块大小对应块数量 / sizeof(char)，bitmap单位是位
    // 当k==0的时候，sz = 1M ,即128MB/16B = 8M， 8M/8 = 1M  
    // 然后在挨着bd_sizes数组的地方分配内存
    // 在最后一层，每个块大小128MB(16B * 2^23),一共需要一个bit表示使用情况。
    // 所以这个alloc数组大小是个等比数列，从1M到1bit，
      bd_sizes[k].alloc = p;
    memset(bd_sizes[k].alloc, 0, sz); 
    p += sz;
  }
  // 初始化bd_sizes这个数组，初始化其中的free链表，以及alloc数组，这个alloc数组就是对应某种块大小的bitmap
  // 下面初始化split也是一样的，记录split的bitmap

  // allocate the split array for each size k, except for k = 0, since
  // we will not split blocks of size k = 0, the smallest size.
  for (int k = 1; k < nsizes; k++) {   // 继续分配这个数组中split的bitmap这块内存
    sz = sizeof(char)* (ROUNDUP(NBLK(k), 8))/8;
    bd_sizes[k].split = p;
    memset(bd_sizes[k].split, 0, sz);
    // split数组和alloc数组完全相同
    p += sz;
  }
  p = (char *) ROUNDUP((uint64) p, LEAF_SIZE);
  // 在这儿，p指针再次内存对齐

  // done allocating; mark the memory range [base, p) as allocated, so
  // that buddy will not hand out that memory.
  // 把buddy元数据使用的内存在buddy数组中标记为已经使用
  // 元数据起始地址为bd_base，终止地址为参数p。
  int meta = bd_mark_data_structures(p);    
  
  // mark the unavailable memory range [end, HEAP_SIZE) as allocated,
  // so that buddy will not hand out that memory.
  int unavailable = bd_mark_unavailable(end, p);    // 把base到end这段内存中向上取整的部分设置为unavailable，即标记为1，已经被使用
  // unavailable是整个物理内存最大处内存对齐差额
  void *bd_end = bd_base+BLK_SIZE(MAXSIZE)-unavailable;
  // 然后设置b_end为buddy实际管理物理内存的最大地址
  
  // initialize free lists for each size k
  int free = bd_initfree(p, bd_end);   // 初始化空闲内存区域
  printf("free=%d\n", free);

  // check if the amount that is free is what we expect
  if(free != BLK_SIZE(MAXSIZE)-meta-unavailable) {
    printf("free %d %d\n", free, BLK_SIZE(MAXSIZE)-meta-unavailable);
    panic("bd_init: free mem");
  }
}

