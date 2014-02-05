/*!***************************************************************************
*!
*! FILE NAME  : vertex.c
*!
*! DESCRIPTION: Implements an interface towards virtex FPGA (mounted on one of our
*!              evaluation boards) from userspace using ioctl()'s
*!
*!              The FPGA can be programmed by copying the bit-file to /dev/fpga.
*!
*!                cat fpga.bit > /dev/fpga
*!
*!                Kernel log should look like:
*!                  69900 bytes written
*!                  FPGA-configuration completed, no errors
*!
*!              Number of bytes written depends on the FPGA
*!
*!                From Xilinx data sheet:
*!                XCV50    559,200 bits
*!                XCV100   781,216 bits
*!                XCV800 4,715,616 bits
*!
*!              Accepted file type is the design.bit generated by Alliance
*!              Design Manager.
*!              This software just sends the bitfile into the device without
*!              checking device type etc.
*!
*!              Sync-header 0xff 0xff 0xff 0xff defines the start for the
*!              byte stream, everything from that position is written to the FPGA.
*!
*!  
*! Jul 19 2002  Stefan Lundberg    Initial version.
*! $Log: virtex.c,v $
*! Revision 1.2  2003/02/24 07:50:30  fredriko
*! Bugfixes and cleanups.
*!
*! Revision 1.1  2002/06/25 09:58:58  stefanl
*! New FPGA driver for Platoon
*!
*!
*! ---------------------------------------------------------------------------
*!
*! (C) Copyright 2002 Axis Communications AB, LUND, SWEDEN
*!
*!***************************************************************************/
/* $Id: virtex.c,v 1.2 2003/02/24 07:50:30 fredriko Exp $ */
/****************** INCLUDE FILES SECTION ***********************************/

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/major.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/config.h>

#include <linux/hdreg.h>
#include <linux/mm.h>

#include <asm/etraxvirtex.h>

#include <asm/system.h>
#include <asm/svinto.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/delay.h>

#include "virtex.h"

/******************* VIRTEX DEFINITION SECTION **************************/

#define VIRTEX_DEBUG(x)

#define VIRTEX_MAJOR 246  /* EXPERIMENTAL */

static const char virtex_name[] = "virtex";

/****************** FUNCTION DEFINITION SECTION *************************/



//
// Read register interface for FPGA programming:
//
//   FPGA_DONE is connected to S1CD_N G28
//   FPGA_INIT_N is connected to S1IO_N G27
//

// Write register interface for FPGA programming:
//
//  Bit:       15         14      13    12      9   8   7   0
//        ____________________________________________________
//       | fpga_write | program | cclk | reserved | cs | data |
//       |____________|_________|______|__________|____|______|
//


// csp0 = cs_fpga1 = FPGA programming interface
// csr0 = cs_fpga2 = register interface towards FPGA construction

static volatile short *port_csp0_word_addr;
static volatile short *port_csr0_word_addr;

static volatile unsigned char open_count;
static volatile unsigned char bytes_written;
static volatile unsigned long bytes_written_cnt;
static volatile unsigned char sync_found;
static volatile unsigned char sync_count;
static volatile unsigned char sync_ff_count;

#define WRITE_FPGA_PROG_REG(data) *port_csp0_word_addr=(data)
#define SET_PROGRAM_BIT(data) (data)|(1<<14)

#define SET_WRITE_BIT(data) (data)|(1<<15)
#define CLR_WRITE_BIT(data) (data)&(~(1<<15))

#define SET_CS_BIT(data) (data)|(1<<8)
#define CLR_CS_BIT(data) (data)&(~(1<<8))

#define SET_CCLK_BIT(data) (data)|(1<<13)
#define CLR_CCLK_BIT(data) (data)&(~(1<<13))

// Bit in read port G (always inputs)
#define READ_INIT  (*R_PORT_G_DATA)&(1<<27)
#define READ_DONE  (*R_PORT_G_DATA)&(1<<28)


void start_virtex_program(void)
{
  volatile unsigned int i=0;
  unsigned short reg_data=0;

  printk("Start writing to FPGA\n");
  reg_data = SET_CS_BIT(reg_data); // FPGA unselected, PROGRAM bit not set
  WRITE_FPGA_PROG_REG(reg_data);

  for(i=0;i<10;i++) { } // at least 300 ns loop
  
  reg_data = SET_PROGRAM_BIT(reg_data);
  WRITE_FPGA_PROG_REG(reg_data);

  for(i=0;i<10;i++) { } // at least 300 ns loop

  while(!READ_INIT); // Wait for init
  
  reg_data = SET_WRITE_BIT(reg_data);
  WRITE_FPGA_PROG_REG(reg_data);

  reg_data = CLR_CS_BIT(reg_data); // FPGA selected, CS is active low
  WRITE_FPGA_PROG_REG(reg_data);
  return;
}

// According to datasheet, bytes should be reversed, it is unknown to me why.
unsigned char bit_reverse(unsigned char data) 
{
  unsigned char in=data;
  unsigned short out=0;
  unsigned int i=0;

  for(i=0;i<8;i++) {
    if(in&0x1) {
      out|=0x1;
    }
    in=in>>1;
    out=out<<1;
  }

  return(out>>1);
  
}

void virtex_program(char* ptr,size_t count)
{
  int c;
  char *p;
  unsigned char data;  
  unsigned short reg_data=0;
//  short tmp_cnt;
  
  c=count;
  p=ptr;

  if(!sync_found) {
    c=count;
    p=ptr;
    while(c--) {
      data=(unsigned char)*p++;
      sync_count++;

      if(sync_count>=256) {
        printk("Sync not found, aborting\n");
        return;
      }

      if(data==0xff) {
        sync_ff_count++;
      } else {
        sync_ff_count=0;
      }
      if(sync_ff_count==4) {
        sync_found=1;
        VIRTEX_DEBUG(printk("Sync found at offset %d\n",sync_count));
        p--;p--;p--;p--;
        c++;c++;c++;c++;
        break;
      }
    }
  }

  if(sync_found) {
    if(bytes_written==0) {
      start_virtex_program();
    }
    bytes_written=1;
  
    reg_data = SET_PROGRAM_BIT(reg_data);
    reg_data = SET_WRITE_BIT(reg_data);
    reg_data = CLR_CS_BIT(reg_data);
  
//    tmp_cnt=0;
    
    printk("*");
    while(c--) {
      data=(unsigned char)*p++;
      data=bit_reverse(data);

/* debug
      tmp_cnt++;
      if(tmp_cnt<=32 || c<=32 ) {
        printk("0x%x ",data); 
      }
      if(tmp_cnt==32 || c==0 ) {
        printk("\n"); 
      }
*/
      bytes_written_cnt++;
      reg_data = CLR_CCLK_BIT(reg_data);
      WRITE_FPGA_PROG_REG(reg_data|(data&0xff));
      reg_data = SET_CCLK_BIT(reg_data);
      WRITE_FPGA_PROG_REG(reg_data|(data&0xff));
      reg_data = CLR_CCLK_BIT(reg_data);
      WRITE_FPGA_PROG_REG(reg_data|(data&0xff));
  
    }
  }
   
  return;
}

void stop_virtex_program(void)
{
  unsigned short reg_data=0;

  reg_data = SET_PROGRAM_BIT(reg_data);
  reg_data = SET_WRITE_BIT(reg_data);
  reg_data = CLR_CCLK_BIT(reg_data);

  reg_data = SET_CS_BIT(reg_data); // release CS
  WRITE_FPGA_PROG_REG(reg_data);

  reg_data = CLR_WRITE_BIT(reg_data); // release write, important to do!
  WRITE_FPGA_PROG_REG(reg_data);
  printk("%d bytes written\n",bytes_written_cnt);
  if(READ_DONE) {
    printk("FPGA-configuration completed, no errors\n");
  } else {
    printk("Error, FPGA-configuration failed\n");
  }
  return;
}

static int
virtex_open(struct inode *inode, struct file *filp)
{
  port_csp0_word_addr = port_csp0_addr;
  if(open_count>=1) {
    printk("FPGA Device busy, aborting\n");
    return(-EBUSY);
  }
  open_count++;
  bytes_written=0;
  sync_found=0;
  sync_count=0;
  sync_ff_count=0;
  bytes_written_cnt=0;
  return(0);
}

static int
virtex_release(struct inode *inode, struct file *filp)
{
  open_count--;
  if(bytes_written!=0)stop_virtex_program();
  return 0;
}

// FPGA programming interface

static ssize_t virtex_write(struct file * file, const char * buf, 
                                 size_t count, loff_t *ppos)
{
  char *ptr;
  VIRTEX_DEBUG(printk("Write FPGA count %d\n", count));
  
  ptr=kmalloc(count, GFP_KERNEL);
  if(!ptr) {
    printk("FPGA device, kernel malloc failed (%d) bytes\n",count);
    return -EFAULT;
  }
  if(copy_from_user(ptr, buf, count)) {
    printk("copy_from_user failed\n");
    kfree(ptr);
    return -EFAULT;
  }
  
  virtex_program(ptr,count);
  
  kfree(ptr);
  return count;
}

/* Main device API. ioctl's to write or read to/from registers.
 */

int virtex_writereg(unsigned short theReg, unsigned short theValue)
{
  port_csr0_word_addr[theReg]=theValue;
  return(0);
}

unsigned short virtex_readreg(unsigned short theReg)
{
  return(port_csr0_word_addr[theReg]);
}


static int
virtex_ioctl(struct inode *inode, struct file *file,
	  unsigned int cmd, unsigned long arg)
{
  if(_IOC_TYPE(cmd) != ETRAXVIRTEX_FPGA_IOCTYPE) {
    return -EINVAL;
  }
  
  switch (_IOC_NR(cmd)) {
    case VIRTEX_FPGA_WRITEREG:
      /* write to an FPGA register */
      VIRTEX_DEBUG(printk("virtex wr 0x%x = 0x%x\n", 
               VIRTEX_FPGA_ARGREG(arg),
               VIRTEX_FPGA_ARGVALUE(arg)));
      
      return virtex_writereg(VIRTEX_FPGA_ARGREG(arg),
                             VIRTEX_FPGA_ARGVALUE(arg));
    case VIRTEX_FPGA_READREG:
    {
      unsigned short val;
      /* read from an FPGA register */
      VIRTEX_DEBUG(printk("virtex rd 0x%x ", 
              VIRTEX_FPGA_ARGREG(arg)));
      val = virtex_readreg(VIRTEX_FPGA_ARGREG(arg));
      VIRTEX_DEBUG(printk("= 0x%x\n", val));
      return val;
    }					    
    default:
      return -EINVAL;
    }

return 0;
}

static struct file_operations virtex_fops = {
	owner:    THIS_MODULE,
	ioctl:    virtex_ioctl,
	open:     virtex_open,
        write:    virtex_write,
	release:  virtex_release,
};

static int __init
virtex_init(void)
{
  int res;
  
  /* register char device */
  res = register_chrdev(VIRTEX_MAJOR, virtex_name, &virtex_fops);
  if(res < 0) {
          printk(KERN_ERR "virtex: couldn't get a major number.\n");
          return res;
  }
  
  port_csr0_word_addr = (volatile unsigned short *) 
                        ioremap((unsigned long)(MEM_CSR0_START |
                                                MEM_NON_CACHEABLE), 16);
     
// see ~/platoon/rel2/platoon/os/linux/arch/cris/mm/init.c
//              port_csp0_addr = (volatile unsigned long *)
//                 ioremap((unsigned long)(MEM_CSP0_START |
//                                         MEM_NON_CACHEABLE), 16);
  
  open_count=0;
  
  printk("VIRTEX(TM) FPGA driver v1.0, (c) 2002 Axis Communications AB\n");
  
  return 0;
}

/* this makes sure that virtex_init is called during boot */

module_init(virtex_init);

/****************** END OF FILE virtex.c ********************************/
