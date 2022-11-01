// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define HASH_BUCKET_SZ 13


extern uint ticks;


// struct {
//   struct spinlock lock;
//   struct buf buf[NBUF];

//   // Linked list of all buffers, through prev/next.
//   // head.next is most recently used.
//   struct buf head;
// } bcache;

struct {
  struct buf buf[NBUF];
  struct spinlock lock;
} hash_table[HASH_BUCKET_SZ];



void
binit(void)
{
  // struct buf *b;

  // init hash_table
  uint now = ticks;
  for (int i = 0; i < HASH_BUCKET_SZ; i++) {
    for (int j = 0; j < NBUF; j++) {
      // set timestamp
      // printf("binit = %d\n", now);
      hash_table[i].buf[j].timestamp = now;
    }
    // set hash slot lock name
    initlock(&hash_table[i].lock, "bcache_");
  }

  // initlock(&bcache.lock, "bcache");

  // // Create linked list of buffers
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  // for(b = bcache.buf; b < bcache.buf+NBUF; b++){
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   initsleeplock(&b->lock, "buffer");
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }
}




//
// hash_function(dev,blockno) -> hash_bucket_id
// 初始化怎么设计？
// 如果找不到怎么设计?
// 还需要cache替换策略?
//
// 在最开始的时候，可以先把所有的block放在时间戳链表中，表示未使用的block
// 然后初始化hash table为空，
// bget从hash table中查找某个block，找到的话，直接返回
// 找不到的话，就从时间戳链表中拿一个(LRU 最久未被使用)，插入哈希表中，然后返回这个block。
// brelse直接把block从哈希表中取下来，插入到时间戳的链表中。
// 

// bget根据dev以及blockno在双向链表中查找对应的block，找不到的话，反向找一个空闲的block；
// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf* b;

  // step1: 计算得到hash key
  int slot = blockno % HASH_BUCKET_SZ;
  // step2: 得到hash slot的锁
  acquire(&hash_table[slot].lock);
  // step3: 遍历slot的block，看能不能找到
  for (b = hash_table[slot].buf; b < hash_table[slot].buf + NBUF; b++) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      b->timestamp = ticks;
      release(&hash_table[slot].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  // 如果在hash_table中找不到相应的缓存
  struct buf* p = 0;
  uint min_timestamp = ticks;
  for (b = hash_table[slot].buf; b < hash_table[slot].buf + NBUF; b++) {
    if (b->timestamp < min_timestamp && b->refcnt == 0) {
      p = b;
      min_timestamp = b->timestamp;
    }
  }
  if (p) {
    p->dev = dev;
    p->blockno = blockno;
    p->valid = 0;
    p->refcnt = 1;
    release(&hash_table[slot].lock);
    acquiresleep(&p->lock);
    return p;
  }
  else
    panic("bget: no buffers");
  // acquire(&bcache.lock);

  // // Is the block already cached?
  // for(b = bcache.head.next; b != &bcache.head; b = b->next){
  //   if(b->dev == dev && b->blockno == blockno){
  //     b->refcnt++;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }

  // // Not cached; recycle an unused buffer.
  // for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
  //   if(b->refcnt == 0) {
  //     b->dev = dev;
  //     b->blockno = blockno;
  //     b->valid = 0;
  //     b->refcnt = 1;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }
  // panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b->dev, b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b->dev, b, 1);
}

// Release a locked buffer.
// Move to the head of the MRU list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int slot = b->blockno % HASH_BUCKET_SZ;
  acquire(&hash_table[slot].lock);
  b->refcnt--;
  release(&hash_table[slot].lock);
  // acquire(&bcache.lock);
  // b->refcnt--;
  // if (b->refcnt == 0) {
  //   no one is waiting for it.
  //   b->next->prev = b->prev;
  //   b->prev->next = b->next;
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }
  
  // release(&bcache.lock);
}

void
bpin(struct buf *b) {
  int slot = b->blockno % HASH_BUCKET_SZ;
  acquire(&hash_table[slot].lock);
  b->refcnt++;
  release(&hash_table[slot].lock);
}

void
bunpin(struct buf *b) {
  int slot = b->blockno % HASH_BUCKET_SZ;
  acquire(&hash_table[slot].lock);
  b->refcnt--;
  release(&hash_table[slot].lock);
}


