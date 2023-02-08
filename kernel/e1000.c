#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

// 发送数据包队列
#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

// 接收数据包队列
#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  // 初始化e1000网卡，此处是初始化网卡的发送数据的ring buffer.
  // 这个ring buffer是内核和驱动通过DMA共享的
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    // init初始化的时候，设置ring buffer与mbuf之间建立关系
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  // 初始化e1000网卡，此处是初始化网卡的接收数据的ring buffer.
  // 这个ring buffer是内核和驱动通过DMA共享的
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

// 发送数据包
int e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //

  // step1: 首先使用E1000_TDT得到当前发送ring的位置
  acquire(&e1000_lock);
  uint32 pos = regs[E1000_TDT];

  // step2: 检查ring是否溢出。如果当前descriptor的E1000_TXD_STAT_DD没有置位
  // 说明上一个发送数据包的过程还在继续，所以直接返回错误，注意释放锁
  if (!(tx_ring[pos].status & E1000_TXD_STAT_DD)) {
    release(&e1000_lock);
    return -1;
  }

  // step3：首先使用mbuffree()把当前descriptor所指向的mbuf释放掉
  // 先誊出来空位置，然后把当前descriptor的addr设置为传入的要发送的mbuf
  if (tx_mbufs[pos]) {
    mbuffree(tx_mbufs[pos]);
    tx_mbufs[pos] = 0;
  }

  // step4: 把传入的mbuf的属性写入到当前的descriptor中，包括mbuf.head以及length
  // 还有必要的cmd参数(看e1000的manual)以及desc.addr
  tx_ring[pos].addr = (uint64) m->head;
  tx_ring[pos].length = m->len;
  // TODO: tx_ring[pos].cmd怎么设置呢
  tx_ring[pos].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  tx_mbufs[pos] = m;

  // step5: 最后给E1000_TDT加一
  regs[E1000_TDT] = (pos + 1) % TX_RING_SIZE;

  release(&e1000_lock);
  return 0;
}

// 接收数据包
static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).

  // 在网卡有数据到来的时候
  // step1：网卡中的PHY芯片首先根据高低电平信号转换为比特流信号
  // step2：然后MAC芯片根据比特流解码出数据帧
  // step3：然后把数据帧存储在rx FIFO中
  // step4：然后发出硬件中断说明有硬件中断到来
  // step5：驱动需要把这些数据帧从rx FIFO中搬到rx ring buffer中
  // step6：然后发出软中断，剩下的交给网卡驱动处理
  // =========================================
  //

  // e1000网卡在接收数据的时候，需要一次性的把所有环形缓冲区中的数据全部交给上层协议栈
  while (1) {
    // step1: 计算得到第一个数据包在网卡ring buffer中的位置
    uint32 pos = (regs[E1000_RDT] + 1) % RX_RING_SIZE;

    // step2: 如果这个位置上的标志位无效，直接退出，说明到来的数据是无效的
    if (!(rx_ring[pos].status & E1000_RXD_STAT_DD)) return;

    // 每收到一个数据包，打印出来
    // printf("e1000 recv packages, %d\n", pos);

    // step3: 设置内存中的环形缓冲区中对应项的数据长度，这个长度是从网卡的ring buffer上直接读过来的
    mbufput(rx_mbufs[pos], rx_ring[pos].length);
    // 交给上层网络栈处理
    net_rx(rx_mbufs[pos]);

    // step4: 分配一个新的mbuf数据结构，并且设置网卡的rx_ring的同一个位置的addr指向这块内存区域。
    struct mbuf *m;
    m = mbufalloc(0);
    // 把新分配的mbuf缓冲区挂到内存的环形缓冲区上
    rx_mbufs[pos] = m;
    // 把新分配的mbuf数据结构和rxring的当前descriptor建立关系
    rx_ring[pos].addr = (uint64)m->head;
    // 恢复当前descriptor缓冲区位置的状态寄存器
    rx_ring[pos].status = 0;

    // step5：最后将网卡的下一个数据包的位置后移一位
    regs[E1000_RDT] = pos;
  }
}

// 网卡工作在物理层和数据链路层，主要由PHY/MAC芯片、Tx/Rx FIFO以及DMA的内存ring
// buffer组成。其中网线通过变压器接PHY芯片、PHY芯片通过MII接MAC芯片、MAC芯片接PCI总线。
// 1. PHY芯片：PHY芯片主要负责数模转换，也就是电信号转换为比特流
// 2. MAC芯片：根据PHY芯片生成的比特流封装成数据帧
// 3. 网卡驱动将数据帧写入到Rx FIFO
// 4. 网卡驱动找到Rx decsriptor ring中下一个将要使用的decsriptor
// 5. 网卡驱动使用DMA的方式，通过PCI总线，把Rx FIFO中的数据复制到Rx
// decsriptor指向的数据缓冲区中，其实也就是复制到mbuf中。因为是以DMA的方式把数据写到Rx
// ring buffer上。
//
// 内核并不知道数据帧的写入情况，所以在复制完之后，由网卡发起硬中断通知CPU数据缓冲区中已经有新的数据帧了。
// 硬件中断的处理程序会首先屏蔽网卡硬件中断，意思是网卡先不要发硬件中断了，有新的数据帧到来的时候直接把数据帧从Rx
// FIFO中写到Rx ring buffer上。

// e1000网卡接收数据包
void
e1000_intr(void)
{
  e1000_recv();
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR];
}
