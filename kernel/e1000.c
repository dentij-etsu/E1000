#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_tlock;
struct spinlock e1000_rlock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_tlock, "e1000");
  initlock(&e1000_rlock, "e1000");

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
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
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


int
e1000_transmit(struct mbuf *m)
{ 
  // Acquire a lock (e1000_tlock)
  // Read the current position of the transmit descriptor ring (position) from the E1000_TDT register
  // Check if the transmit descriptor at the current position indicates that the previous transmission has finished.
      // Do this by checking if the E1000_TXD_STAT_DD flag is set in the status field
        // If it's not set, it means the E1000 hasn't finished transmitting the previous packet, so it returns an error (-1)
        // If the previous transmission has finished, it frees mbuf
  // Set the RS and EOP flags
  // Save a pointer to the current mbuf
  // Set the address and length fields in the transmit descriptor
  // Update the ring position (E1000_TDT) to point to the next one
  // Release the lock
  // Return 0 if the mbuf was successfully added to the transmit ring or -1 if there was an error
 
  acquire(&e1000_tlock); // lock
  int position = regs[E1000_TDT]; // ring index
  // Check to see if the previous transmission request was successful (bitwise & these together - should both be 1)
  if(!(tx_ring[position].status & E1000_TXD_STAT_DD)) {
    release(&e1000_tlock);
    return -1; // Error
  }
  // Otherwise, use mbuffree() to free the last mbuf that was transmitted from that descriptor
  struct mbuf *temp = tx_mbufs[position]; // get the current element in the buffer
  if(temp){
  mbuffree(temp);
  }
  // set flags
  tx_ring[position].cmd |= E1000_TXD_CMD_RS;
  tx_ring[position].cmd |= E1000_TXD_CMD_EOP;
  tx_mbufs[position] = m; // save the pointer 
  // m is a param for the buffer
  // location and size of the packet data to be transmitted 
  tx_ring[position].addr = (uint64) m->head;
  tx_ring[position].length = (uint64) m->len;
  regs[E1000_TDT] = (position + 1) % TX_RING_SIZE; // update ring position 
  release(&e1000_tlock);
  return 0;

  //First, ask the E1000 for the TX ring index at which it's expecting the next packet, by reading the E1000_TDT control register.  
  // Then, check if the ring is overflowing. If E1000_TXD_STAT_DD is not set in the descriptor indexed by E1000_TDT, the E1000 hasn't finished the corresponding previous transmission request, so return an error. Otherwise, use mbuffree() to free the last mbuf that was transmitted from that descriptor (if there was one).
  // if E1000_TXD_STAT_DD is not set (== 0),
    // then the e1000 hasn't finish the previous transmission request (return error)
  //else 
    //use mbuffree to free the last mbuf
  // e1000_txd_stat_dd should be & with something, it is a bit map 
  // Otherwise, use mbuffree() to free the last mbuf that was transmitted from that descriptor (if there was one).
   
  // if (tx_ring[position] !empty)
    // free mbuf

  //Then, fill in the descriptor.
    //m->head points to the packet's content in memory and m->len is the packet length.
    //Set the necessary cmd flags (look at Section 3.3 in the E1000 manual) and save a pointer to the
    //mbuf for later freeing.
    //only 2 flags in book and in header
  // set flags
  // save the pointer  

  // Finally, update the ring position by adding one to E1000_TDT modulo TX_RING_SIZE.

  // If e1000_transmit() added the mbuf successfully to the ring, return 0. On failure (e.g., there is no
  // descriptor available to transmit the mbuf), return -1 so that the caller knows to free the mbuf
  
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //
}

static void
e1000_recv(void)
{
  //
  // Chris and Joe were here.

  // Get the lock so we can do stuff and things.
	//printf("recv pre\n");
  acquire(&e1000_rlock);
  //printf("recv post\n");
  // First, ask the E1000 for the ring index at which the next waiting received packet (if any) is located,
  // by fetching the E1000_RDT control register and adding one modulo RX_RING_SIZE.

  int index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  
  // Then, check if a new packet is available by checking for the E1000_RXD_STAT_DD bit in the status portion
  // of the descriptor. If not, stop.
  while ((rx_ring[index].status & E1000_RXD_STAT_DD) != 0) {
    //if the bit is not set then stop bcz there is nothing there.  No if statement is necessary since condition is checked each loop

    //Otherwise, update the mbuf's m->len to the length reported in the descriptor.

    //mbuf.length = rx_ring[index].length;

    mbufput(rx_mbufs[index], rx_ring[index].length);


    //Deliver the mbuf to the network stack using net_rx().
    net_rx(rx_mbufs[index]);

    // Then, allocate a new mbuf using mbufalloc() to replace the one just given to net_rx(). Program its data
    // pointer (m->head) into the descriptor. Clear the descriptor's status bits to zero.
    
    rx_mbufs[index] = mbufalloc(0); 
    // Added (Uint64) to convert the type over.
    rx_ring[index].addr = (uint64) rx_mbufs[index]->head;
    rx_ring[index].status = 0;

  // Finally, update the E1000_RDT register to be the index of the last ring descriptor processed.

  regs[E1000_RDT] = index;
  index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  }

  // e1000_init() initializes the RX ring with mbufs, and you'll want to look at how it does that (and, perhaps,
  // “borrow” code).

  // At some point, the total number of packets that have ever arrived will exceed the ring size (16); make sure
  // your code can handle that.
  
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //
  
  
  release(&e1000_rlock);
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
