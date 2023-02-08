//
// network system calls.
//

#include <stdlib.h>
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

struct sock {
  struct sock *next; // the next socket in the list
  uint32 raddr;      // the remote IPv4 address
  uint16 lport;      // the local UDP port number
  uint16 rport;      // the remote UDP port number
  struct spinlock lock; // protects the rxq
  struct mbufq rxq;  // a queue of packets waiting to be received
};

static struct spinlock lock;
static struct sock *sockets;
// 套接字链表

void
sockinit(void)
{
  initlock(&lock, "socktbl");
}

int
sockalloc(struct file **f, uint32 raddr, uint16 lport, uint16 rport)
{
  struct sock *si, *pos;

  si = 0;
  *f = 0;
  if ((*f = filealloc()) == 0)
    goto bad;
  if ((si = (struct sock*)kalloc()) == 0)
    goto bad;

  // initialize objects
  si->raddr = raddr;
  si->lport = lport;
  si->rport = rport;
  initlock(&si->lock, "sock");
  mbufq_init(&si->rxq);
  (*f)->type = FD_SOCK;
  (*f)->readable = 1;
  (*f)->writable = 1;
  (*f)->sock = si;

  // add to list of sockets
  acquire(&lock);
  pos = sockets;
  while (pos) {
    if (pos->raddr == raddr &&
        pos->lport == lport &&
	pos->rport == rport) {
      release(&lock);
      goto bad;
    }
    pos = pos->next;
  }
  // 从这儿可以看出来，sockets链表是在头部插入最新数据的链表
  // 那就说明最新的sock连接在sockets链表的头部，所以在关闭sock连接的时候，应该从前往后遍历链表查找
  si->next = sockets;
  sockets = si;
  release(&lock);
  return 0;

bad:
  if (si)
    kfree((char*)si);
  if (*f)
    fileclose(*f);
  return -1;
}

//
// Your code here.
//
// Add and wire in methods to handle closing, reading,
// and writing for network sockets.
//
int sockread(struct file *f, uint64 addr, int n) { 

  // step1: 查看sock的mbufq是否为空，如果为空就sleep
  // 检查要读的套接字是不是空的，即mbuf是否为空
  // sock->rxq，rxq(receive queue接收队列，双向链表)
  struct sock *sock = f->sock;
  acquire(&sock->lock);
  while (mbufq_empty(&sock->rxq)) {
    // 就算因为别的情况被调度到，也还是要判断sock->rxq是否有数据
    sleep(&sock->rxq, &sock->lock);
  }

  // step2: 如果套接字上有缓冲数据，或者因为有数据到来而唤醒进程
  // 就从mbuf中把载荷取下来
  struct mbuf *mbuf = mbufq_pophead(&sock->rxq);

  // printf("sockread, recv mbuf payload = %s\n", mbuf->head);

  if (mbuf == 0) {
    printf("sockread empty\n");
  }

  // step3: 把载荷复制到用户地址空间
  copyout(myproc()->pagetable, addr, mbuf->head, mbuf->len);

  // step4: 释放掉mbuf的内存
  int len = mbuf->len;
  mbuffree(mbuf);
  release(&sock->lock);

  return len;
}

int sockwrite(struct file *f, uint64 addr, int n) {

  // step1: 首先分配一个mbuf
  struct mbuf *mbuf;
  mbuf = mbufalloc(MBUF_DEFAULT_HEADROOM);
  // step2:
  // 把用户空间的addr数据拷贝到mbuf中，mbufput设置mbuf的长度；copyin复制数据
  mbufput(mbuf, n);
  copyin(myproc()->pagetable, mbuf->head, addr, n);

  // printf("mbuf payload = %s\n", mbuf->head);

  // step3: 把mbuf这个数据结构交给底层网络设备发送
  net_tx_udp(mbuf, f->sock->raddr, f->sock->lport, f->sock->rport);

  return 0;
}

int sockclose(struct file *f) {

  // step1: 先从sockets链表中找到这个socket连接，然后从链表上取下来
  acquire(&lock);
  struct sock *pos = sockets;
  if (pos == f->sock) {
    // 当前sock在sockets链表头，让sockets链表往下移位，然后断开头结点和后面链表的关系
    // printf("sockclose, at head\n");
    sockets = pos->next;
    f->sock->next = 0;
  } else {
    // 当前sock不在sockets链表头
    // printf("sockclose, not at head\n");
    while (pos && pos->next) {
      if (pos->next == f->sock) {
        pos->next = pos->next->next;
        f->sock->next = 0;
      }
      pos = pos->next;
    }
    if (!pos || !pos->next) {
      release(&lock);
      return -1;
    }
  }
  release(&lock);

  // step2: 释放相关内存，包括struct sock，以及没有被读走的mbuf
  // step2.1: 先释放没有被读走的mbuf
  acquire(&f->sock->lock);
  struct mbuf *head = f->sock->rxq.head;
  struct mbuf *tmp;
  // 不懂：rxq既然是单向链表，有head、tail，为啥不能以head==tail判断其为空
  while (head) {
    tmp = head;
    head = head->next;
    tmp->next = 0;
    kfree(tmp);
  }
  release(&f->sock->lock);
  
  kfree(f->sock);

  
  return 0;
}

// 每个UDP数据包到来的时候，调用sockrecvudp
// called by protocol handler layer to deliver UDP packets
void sockrecvudp(struct mbuf *m, uint32 raddr, uint16 lport, uint16 rport) {
  //
  // Your code here.
  //
  // Find the socket that handles this mbuf and deliver it, waking
  // any sleeping reader. Free the mbuf if there are no sockets
  // registered to handle it.
  //

  // 这个函数的任务就是把来自udp的数据包放到某个socket连接的rxq链表里面，
  // 那么首先需要找到当前要处理的包来自哪个socket，根据ip协议的四元组确定(rip, rport, lip, lport)
  // 在插入数据之前，先查看一下目标socket的rxq是否是空的，如果是空的就在插入一个udp数据包之后awake这个进程

  // step1: 遍历sockets链表，找到匹配的soket
  acquire(&lock);
  struct sock *pos = sockets;
  while (pos) {
    if (pos->lport == lport && pos->rport == rport && pos->raddr == raddr)
      break;
    pos = pos->next;
  }

  release(&lock);
  // 如果不存在这个套接字，直接把这个mbuf释放掉
  if (pos == 0) {
    mbuffree(m);
    return;
  }

  // step2: 判断这个套接字是不是阻塞状态的
  acquire(&pos->lock);

  // step3: 把这个缓冲数据放在套接字的缓冲区
  mbufq_pushtail(&pos->rxq, m);

  // ste4: 如果套接字的缓冲区是空的，那就唤醒这个套接字；否则直接
  wakeup(&pos->rxq);

  release(&pos->lock);
}
