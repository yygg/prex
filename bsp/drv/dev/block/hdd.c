#include <sys/param.h>
#include <driver.h>
#include <pci.h>

typedef unsigned long long uint64_t; /* Hmm. */

/* Much of the content of this driver was worked out using
   wiki.osdev.org, the FreeBSD source code and the grub2 source code
   as references, though no code was copied from those sources. The
   Intel datasheets and the ATA spec drafts were also really useful.

   http://www.intel.com/design/chipsets/datashts/29054901.pdf
   http://www.intel.com/assets/pdf/datasheet/290562.pdf
   http://www.t13.org/Documents/UploadedDocuments/project/d0948r4c-ATA-2.pdf
   http://www.t13.org/Documents/UploadedDocuments/docs2007/D1699r4a-ATA8-ACS.pdf
   http://www.t13.org/documents/UploadedDocuments/docs2006/D1700r3-ATA8-AAM.pdf
   http://suif.stanford.edu/~csapuntz/specs/pciide.ps
   http://suif.stanford.edu/~csapuntz/specs/idems100.ps
*/

#define HDC_IRQ		14	/* Yeah, there are more than 16 these days.
				   Does prex support anything more than 16? */
/* In any case, we should be spreading the load around, and not
 * sharing one IRQ for all the IDE controllers in the system. For now,
 * sharing the IRQ is fine.
 *
 * FIXME: OK, sharing *would* be fine, but see the comment in the
 * implementation of irq_attach that says that sharing isn't supported
 * by prex. */

#define SECTOR_SIZE	512

/* These are the offsets to various IDE/ATA registers, relative to
 * ata_channel.base_port in I/O port space. */
typedef enum ata_port_register_t_ {
  ATA_REG_DATA = 0,
  ATA_REG_ERR = 1, /* osdev.org claims this is mostly for ATAPI? */
  ATA_REG_SECTOR_COUNT = 2,

  /* CHS addressing, which this driver doesn't use or support */
  ATA_REG_SECTOR_NUMBER = 3,
  ATA_REG_CYLINDER_LOW = 4,
  ATA_REG_CYLINDER_HIGH = 5,

  /* LBA addressing */
  ATA_REG_LBA_LOW = 3,
  ATA_REG_LBA_MID = 4,
  ATA_REG_LBA_HIGH = 5,

  ATA_REG_DISK_SELECT = 6, /* also contains head number for CHS addressing */
  ATA_REG_COMMAND_STATUS = 7
} ata_port_register_t;

/* Some controllers operate in "PCI native" mode, where they specify
   the I/O ports they want us to use. Others operate in
   "compatibility" (a.k.a. legacy) mode, where we just have to know
   ahead of time that the following two ports are used to communicate
   with the controller. */
#define ATA_LEGACY_PRIMARY_CONTROL_BASE		0x1f0
#define ATA_LEGACY_SECONDARY_CONTROL_BASE	0x170
/* In "legacy" mode, the control block base port number + the
   following offset is the port number of the control/altstatus
   register. In PCI native IDE mode, we look at the BARs instead: BAR1
   points to a 4-byte space, within which offset 2 is the
   control/altstatus register. */
#define ATA_LEGACY_CONTROL_ALTERNATE_STATUS_OFFSET 0x206

/* These flags appear in the contents of ATA_REG_ERR. These aren't the
   only flags that exist, there are others (see the various sources of
   information linked above), but these are the only ones this driver
   cares about for now. */
typedef enum ata_status_flag_t_ {
  ATA_STATUS_FLAG_ERROR = 0x01,
  ATA_STATUS_FLAG_DRQ = 0x08,
  ATA_STATUS_FLAG_DEVICE_FAILURE = 0x20,
  ATA_STATUS_FLAG_BUSY = 0x80
} ata_status_flag_t;

#define DEBUG_HDD 1

#if DEBUG_HDD
#define DPRINTF(a)	printf a
#else
#define DPRINTF(a)
#endif

/* At present, we limit individual transfers to a maximum of
   BUFFER_LENGTH bytes. TODO: once we have a request queue, this will
   probably become obsolete. Doubly so once we switch to DMA. */
#define BUFFER_LENGTH 65536 /* FIXME: proper caching please */
#define BUFFER_LENGTH_IN_SECTORS (BUFFER_LENGTH / SECTOR_SIZE)

/**
 * Represents the kernel's handle on some device object exposed
 * through this driver. */
struct ata_device_handle {
  enum {
    ATA_DEVICE_WHOLEDISK,
    ATA_DEVICE_PARTITION
  } kind;
  union {
    struct ata_disk *wholedisk;
    struct ata_partition *partition;
  } pointer;
};

/**
 * Represents a single partition on an ata_disk. */
struct ata_partition {
  struct list link; /* link to other partitions within this disk */
  struct ata_disk *disk; /* the disk this partition is part of */

  uint8_t system_id; /* the partition type, from the partition table */
  uint32_t start_lba; /* base block address of partition on the disk */
  uint32_t sector_count; /* total number of SECTORS within the partition */

  /* The name of this device as it is known to the kernel. The file
     system's "/dev" node for this device is named using this. */
  char devname[MAXDEVNAME]; /* "hdXdXpXX\0" */
  device_t dev; /* the PREX kernel's device handle for this device */
};

/**
 * Represents a detected ATA disk/device attached to a channel of a
 * controller. */
struct ata_disk {
  struct list link; /* link to other disks on this controller */
  struct ata_controller *controller; /* the controller for this device */

  int channel; /* 0 => primary, 1 => secondary */
  int slave; /* 0 => master, 1 => slave */

  /* Copy of the ATA identification space buffer sent back in response
     to the ATA IDENTIFY command for this device. Some of the fields
     in this buffer are extracted into fields below. */
  uint8_t identification_space[512];

  /* These fields are extracted from identification_space: */
  uint8_t serial_number[10];
  uint8_t firmware_revision[8];
  uint8_t model[40];
  int lba_supported;
  int dma_supported;
  uint32_t sector_capacity;
  uint64_t addressable_sector_count;

  /* The name of this device as it is known to the kernel. The file
     system's "/dev" node for this device is named using this. */
  char devname[MAXDEVNAME]; /* "hdXdX\0" */
  device_t dev; /* the PREX kernel's device handle for this device */

  struct list partition_list; /* all detected partitions on the disk */
};

/**
 * Represents a single channel within an IDE controller. IDE
 * controllers have *two* channels: primary and secondary. On each
 * channel, there can be up to two disks/devices. Each channel is
 * accessed via a different region of I/O port space. */
struct ata_channel {
  int base_port;
  int control_port;
  int dma_port;
};

/**
 * Represents a single IDE controller. */
struct ata_controller {
  char devname[MAXDEVNAME]; /* "hdX\0"; used for debugging etc. */
  struct pci_device *pci_dev; /* the PCI config for this device */
  struct irp irp; /* TODO: switch to a request queue */
  struct ata_disk *active_disk; /* disk using the irp right now TODO: switch to a request queue */
  irq_t irq; /* we registered an IRQ with the kernel; this is the handle we were given */
  struct ata_channel channel[2]; /* the two channels within the controller */
  struct list disk_list; /* all disks attached to this controller */
  uint8_t *buffer; /* TODO: switch to a request queue */
};

#if 0 /* we don't need this yet */
/* Append two strings, making sure not to read or write outside each
   string's allocated area. (I wish C had a unit test framework.) */
static char *strcat_limited(char *dest, size_t dest_max, const char *src, size_t src_max) {
  size_t dest_len = strnlen(dest, dest_max);
  size_t remaining_space = dest_max - dest_len;
  size_t i;
  for (i = 0 ; (i < remaining_space) && (i < src_max) && (src[i] != '\0') ; i++) {
    dest[dest_len + i] = src[i];
  }
  if (i < remaining_space) {
    dest[dest_len + i] = '\0';
  }
  return dest;
}
#endif

static struct ata_device_handle *get_handle(device_t dev) {
  return (struct ata_device_handle *) device_private(dev);
}

/* Writes to an ATA control register. */
static void ata_write(struct ata_controller *c, int channelnum, int reg, uint8_t val) {
  bus_write_8(c->channel[channelnum].base_port + reg, val);
}

/* Reads from an ATA control register. */
static uint8_t ata_read(struct ata_controller *c, int channelnum, int reg) {
  return bus_read_8(c->channel[channelnum].base_port + reg);
}

/* Writes to the special control/altstatus register. */
static void write_control(struct ata_controller *c, int channelnum, uint8_t val) {
  bus_write_8(c->channel[channelnum].control_port, val);
}

/* Reads from the special control/altstatus register. */
static uint8_t read_altstatus(struct ata_controller *c, int channelnum) {
  return bus_read_8(c->channel[channelnum].control_port);
}

/* A 400ns delay, used to wait for the device to start processing a
 * sent command and assert busy. */
static void ata_delay400(struct ata_controller *c, int channelnum) {
  read_altstatus(c, channelnum);
  read_altstatus(c, channelnum);
  read_altstatus(c, channelnum);
  read_altstatus(c, channelnum);
}

/* Poll until the BUSY flag clears. */
static void ata_wait(struct ata_controller *c, int channelnum) {
  unsigned int i;

  ata_delay400(c, channelnum);

  for (i = 0; i < 0x80000000; i++) {
    if (!(read_altstatus(c, channelnum) & ATA_STATUS_FLAG_BUSY)) {
      return;
    }
  }

  printf("ata_wait: busy never went away!!\n");
  /* TODO: reset device here, maybe? We'd have to retry or abort
     in-progress operations. */
}

/* Programmed I/O (PIO) read a buffer's worth of data from the controller. */
static void ata_pio_read(struct ata_controller *c,
			 int channelnum,
			 uint8_t *buffer,
			 size_t count)
{
  ASSERT((count & 3) == 0); /* multiple of 4 bytes. */
  while (count > 0) {
    uint32_t v = bus_read_32(c->channel[channelnum].base_port + ATA_REG_DATA);
    buffer[0] = v & 0xff;
    buffer[1] = (v >> 8) & 0xff;
    buffer[2] = (v >> 16) & 0xff;
    buffer[3] = (v >> 24) & 0xff;
    buffer += 4;
    count -= 4;
  }
}

/* interrupt service routine. Lowest-level responder to an interrupt -
   try to avoid doing "real work" here */
static int hdc_isr(void *arg) {
  struct ata_controller *c = arg;
  struct ata_disk *disk = c->active_disk;
  uint8_t status = read_altstatus(c, disk->channel);
  if (status & (ATA_STATUS_FLAG_DRQ | ATA_STATUS_FLAG_DEVICE_FAILURE | ATA_STATUS_FLAG_ERROR)) {
    return INT_CONTINUE;
  } else {
    return 0;
  }
}

/* interrupt service thread. The main workhorse for communicating with
   the device. */
static void hdc_ist(void *arg) {
  struct ata_controller *c = arg;
  struct ata_disk *disk = c->active_disk;
  struct irp *irp = &c->irp;
  uint8_t status = ata_read(c, disk->channel, ATA_REG_COMMAND_STATUS);

  c->active_disk = NULL;

  if (status & (ATA_STATUS_FLAG_ERROR | ATA_STATUS_FLAG_DEVICE_FAILURE)) {
    irp->error = 0x80000000 | (status << 16) | ata_read(c, disk->channel, ATA_REG_ERR);
    sched_wakeup(&irp->iocomp);
    return;
  }

  irp->error = 0;
  switch (irp->cmd) {
    case IO_READ:
      ata_pio_read(c, disk->channel, irp->buf, irp->blksz * SECTOR_SIZE);
      break;
    case IO_WRITE:
      panic("hdd_ist IO_WRITE not implemented"); /* TODO */
      /* TODO: add flush-to-disk ioctl? */
      break;
    default:
      panic("hdd_ist invalid irp->cmd");
      break;
  }
  sched_wakeup(&irp->iocomp);
}

/* Sends an I/O command to the disk, including the address of the
   block concerned, using LBA48 mode. Usable for setting up either
   interrupt-based or polling-based transfers. */
static void hdd_setup_io(struct ata_disk *disk,
			 int cmd,
			 uint64_t lba,
			 size_t sector_count)
{
  struct ata_controller *c = disk->controller;
  uint8_t final_cmd;

  c->active_disk = disk;

  switch (cmd) {
    case IO_READ:
      /* Send READ SECTORS EXT command. */
      ata_write(c, disk->channel, ATA_REG_DISK_SELECT, 0x40 | (disk->slave << 4));
      final_cmd = 0x24;
      break;
    case IO_WRITE:
      panic("hdd_setup_io IO_WRITE not implemented"); /* TODO */
      final_cmd = 0; /* TODO */
      break;
    default:
      panic("hdd_setup_io invalid cmd");
      return;
  }

  ata_write(c, disk->channel, ATA_REG_SECTOR_COUNT, (sector_count >> 8) & 0xff);
  ata_write(c, disk->channel, ATA_REG_LBA_LOW, (lba >> 24) & 0xff);
  ata_write(c, disk->channel, ATA_REG_LBA_MID, (lba >> 32) & 0xff);
  ata_write(c, disk->channel, ATA_REG_LBA_HIGH, (lba >> 40) & 0xff);
  ata_write(c, disk->channel, ATA_REG_SECTOR_COUNT, sector_count & 0xff);
  ata_write(c, disk->channel, ATA_REG_LBA_LOW, lba & 0xff);
  ata_write(c, disk->channel, ATA_REG_LBA_MID, (lba >> 8) & 0xff);
  ata_write(c, disk->channel, ATA_REG_LBA_HIGH, (lba >> 16) & 0xff);
  ata_write(c, disk->channel, ATA_REG_COMMAND_STATUS, final_cmd);

  /* We'll get an interrupt sometime, if interrupts aren't disabled;
     otherwise, we'll need to check the status register by polling. */
}

/* Useful only from within the kernel, while we're probing and setting
   everything up. */
static int read_during_setup(struct ata_disk *disk, uint64_t lba, uint8_t *buf, size_t count) {
  struct ata_controller *c = disk->controller;
  int status;
  hdd_setup_io(disk, IO_READ, lba, count);
  ata_wait(c, disk->channel);
  status = ata_read(c, disk->channel, ATA_REG_COMMAND_STATUS);
  if (status & (ATA_STATUS_FLAG_ERROR | ATA_STATUS_FLAG_DEVICE_FAILURE)) {
    printf("Couldn't read_during_setup %s (lba %d, count %d): 0x%02x, 0x%02x\n",
	   disk->devname,
	   lba, count,
	   status, ata_read(c, disk->channel, ATA_REG_ERR));
    return EIO;
  }

  ata_pio_read(c, disk->channel, buf, count * SECTOR_SIZE);
  return 0;
}

/* Read a disk's partition table. */
static void setup_partitions(struct driver *self, struct ata_disk *disk) {
  uint8_t *sector0 = kmem_alloc(SECTOR_SIZE);

  if (read_during_setup(disk, 0, sector0, 1)) {
    kmem_free(sector0);
    return;
  }

  if (0xaa55 == (* (uint16_t *) (&sector0[SECTOR_SIZE - 2]))) {
    int partition;
    /* Valid DOS disklabel? */
    for (partition = 0; partition < 4; partition++) {
      struct {
	uint8_t flags;
	uint8_t start_chs[3];
	uint8_t system_id;
	uint8_t end_chs[3];
	uint32_t start_lba;
	uint32_t sector_count;
      } *p = (void *) (&sector0[0x1be + (partition * 16)]);
      struct ata_partition *part = NULL;

      if ((p->start_lba == 0) || (p->sector_count == 0) || (p->system_id == 0)) {
	/* No allocated partition in this slot. */
	continue;
      }

      part = kmem_alloc(sizeof(struct ata_partition));

      list_insert(list_last(&disk->partition_list), &part->link);
      part->disk = disk;
      part->system_id = p->system_id;
      part->start_lba = p->start_lba;
      part->sector_count = p->sector_count;
      /* TODO: sanity-check sector_count, to make sure it doesn't
	 reach past the addressable_sector_count known to the whole
	 disk. */

      strlcpy(part->devname, disk->devname, MAXDEVNAME);
      {
	char *p = part->devname + strnlen(part->devname, MAXDEVNAME);
	p[0] = 'p';
	p[1] = '0' + (partition / 10);
	p[2] = '0' + (partition % 10);
	p[3] = '\0';
      }
      part->dev = device_create(self, part->devname, D_BLK | D_PROT);
      get_handle(part->dev)->kind = ATA_DEVICE_PARTITION;
      get_handle(part->dev)->pointer.partition = part;

      printf(" - partition %s, type 0x%02x, 0x%08x size 0x%08x\n",
	     part->devname,
	     part->system_id,
	     part->start_lba,
	     part->sector_count);
    }
  }

  /* TODO: loop back around and add any partitions found in the table
     in an extended partition. */

  kmem_free(sector0);
}

/* Byteswap a "string" of 16-bit words. See comments near callers. */
static void fixup_string_endianness(uint8_t *p, size_t size) {
  while (size > 0) {
    uint8_t tmp = p[1];
    p[1] = p[0];
    p[0] = tmp;
    p += 2;
    size -= 2;
  }
}

static int setup_disk(struct driver *self, struct ata_controller *c, int disknum) {
  struct ata_disk *disk = kmem_alloc(sizeof(struct ata_disk));

  /* disk->link will be initialised once we insert into our controller's disk_list. */
  disk->controller = c;

  disk->channel = disknum >> 1;
  disk->slave = disknum & 1;

  /* Send IDENTIFY command (0xEC). */

  ata_write(c, disk->channel, ATA_REG_DISK_SELECT, 0xA0 | (disk->slave << 4));
  ata_delay400(c, disk->channel);

  ata_write(c, disk->channel, ATA_REG_SECTOR_COUNT, 0);
  ata_write(c, disk->channel, ATA_REG_LBA_LOW, 0);
  ata_write(c, disk->channel, ATA_REG_LBA_MID, 0);
  ata_write(c, disk->channel, ATA_REG_LBA_HIGH, 0);

  ata_write(c, disk->channel, ATA_REG_COMMAND_STATUS, 0xEC);
  ata_delay400(c, disk->channel);

  if (ata_read(c, disk->channel, ATA_REG_COMMAND_STATUS) == 0) {
    printf("Disk %d absent (wouldn't accept command).\n", disknum);
    goto cancel_setup;
  }

  ata_wait(c, disk->channel);
  if (read_altstatus(c, disk->channel) & ATA_STATUS_FLAG_ERROR) {
    printf("Disk %d absent (wouldn't identify).\n", disknum);
    goto cancel_setup;
  }

  /* ATAPI devices return special values in LBA_MID and LBA_HIGH. We
     don't check those here. (TODO) */

  ata_pio_read(c, disk->channel, disk->identification_space, sizeof(disk->identification_space));

  memcpy(disk->serial_number, &disk->identification_space[20], sizeof(disk->serial_number));
  memcpy(disk->firmware_revision, &disk->identification_space[46], sizeof(disk->firmware_revision));
  memcpy(disk->model, &disk->identification_space[54], sizeof(disk->model));
  disk->lba_supported = ((disk->identification_space[99] & 2) != 0);
  disk->dma_supported = ((disk->identification_space[99] & 1) != 0);
  memcpy(&disk->sector_capacity, &disk->identification_space[114], sizeof(disk->sector_capacity));

  if (!disk->lba_supported) {
    printf("Disk %d doesn't support LBA.\n", disknum);
    goto cancel_setup;
  }

  if (!disk->dma_supported) {
    printf("Disk %d doesn't support DMA.\n", disknum);
    goto cancel_setup;
  }

  /* Decide how many sectors this physical disk supports. If the
     lba28_count is the maximum possible, the convention is that the
     lba48_count is valid and should be used. */
  {
    uint32_t lba28_count;
    memcpy(&lba28_count, &disk->identification_space[120], sizeof(lba28_count));
    if (lba28_count == 0x0fffffff) {
      uint64_t lba48_count;
      /* More than 28 bits' worth of sectors - read the lba48 area */
      memcpy(&lba48_count, &disk->identification_space[200], sizeof(lba48_count));
      disk->addressable_sector_count = lba48_count;
    } else {
      disk->addressable_sector_count = lba28_count;
    }
  }

  /* Weirdly, the ASCII strings in the identification_space are
     byte-swapped, because it was originally defined as a region of
     16-bit words (!) */
  fixup_string_endianness(disk->serial_number, sizeof(disk->serial_number));
  fixup_string_endianness(disk->firmware_revision, sizeof(disk->firmware_revision));
  fixup_string_endianness(disk->model, sizeof(disk->model));

  /* At this point, the disk has identified itself, and it looks
     more-or-less like the kind of thing we might be able to use. Add
     it to the list in our controller. */
  list_insert(list_last(&c->disk_list), &disk->link);

  memcpy(disk->devname, c->devname, 3);
  disk->devname[3] = 'd';
  disk->devname[4] = '0' + disknum;
  disk->devname[5] = '\0';
  disk->dev = device_create(self, disk->devname, D_BLK | D_PROT);
  get_handle(disk->dev)->kind = ATA_DEVICE_WHOLEDISK;
  get_handle(disk->dev)->pointer.wholedisk = disk;

  list_init(&disk->partition_list);

  printf("Disk %d/%s:\n", disknum, disk->devname);
  printf(" - serial %.*s\n", sizeof(disk->serial_number), disk->serial_number);
  printf(" - firmware %.*s\n", sizeof(disk->firmware_revision), disk->firmware_revision);
  printf(" - model %.*s\n", sizeof(disk->model), disk->model);
  printf(" - sector count %d (0x%08x%08x)\n",
	 (uint32_t) disk->addressable_sector_count,
	 (uint32_t) (disk->addressable_sector_count >> 32),
	 (uint32_t) disk->addressable_sector_count);

  setup_partitions(self, disk);
  return 0;

 cancel_setup:
  kmem_free(disk);
  return -1;
}

static void setup_controller(struct driver *self, struct pci_device *v) {
  static char which_device = '0';
  char devname_tmp[MAXDEVNAME];
  int primary_native;
  int secondary_native;

  struct ata_controller *c;
  struct irp *irp;

  /* According to the "PCI IDE Controller Specification Revision 1.0",
     which I retrieved from
     http://suif.stanford.edu/~csapuntz/specs/pciide.ps, the prog_if
     value contains bits describing whether the IDE controller is in
     PCI native or compatibility mode:

     76543210
     |   ||||
     |   |||\--	0 => primary channel in compatibility mode, 1 => native
     |   ||\---	0 => primary channel can't switch modes, 1 => it can
     |   |\----	0 => secondary channel in compatibility mode, 1 => native
     |   \-----	0 => secondary channel can't switch modes, 1 => it can
     \---------	0 => can't bus-master-dma, 1 => can

     (That last one, bit 7 (0x80), is implied by the PIIX3
     documentation for the embedded IDE controller. I don't have a
     better source than that.)

     Therefore, looking at bits 0 and 2 will tell us which programming
     interface to use for the IDE controller we have in the system.
  */

  primary_native = ((v->prog_if & 0x01) != 0);
  secondary_native = ((v->prog_if & 0x04) != 0);

  {
    char *n = &devname_tmp[0];
    n[0] = 'h';
    n[1] = 'd';
    n[2] = which_device++; /* barrrrrrrrrrrrrrf */
    n[3] = '\0';
    /* Why is there a vsprintf but no vsnprintf or snprintf? */
  }

  printf("device %d.%d.%d = %s\n", v->bus, v->slot, v->function, devname_tmp);

  c = kmem_alloc(sizeof(struct ata_controller));
  memcpy(&c->devname[0], &devname_tmp[0], sizeof(c->devname));
  c->pci_dev = v;

  irp = &c->irp;
  irp->cmd = IO_NONE;
  event_init(&irp->iocomp, &c->devname[0]);

  /* TODO: if we're operating in compatibility/legacy mode, we are
     behaving like an old school IDE adapter, which wants to use IRQ14
     for the primary and IRQ15 for the secondary controller. We
     currently only take one IRQ, so secondary controllers won't
     work. */

  /* TODO: claiming an IRQ more than once causes, um, issues, so don't do that. Ever. */
  c->irq = irq_attach(HDC_IRQ, IPL_BLOCK, 0, hdc_isr, hdc_ist, c);

  if (primary_native || secondary_native) {
    /* Tell the controller which IRQ to use, if we're in native mode. */
    write_pci_interrupt_line(v, HDC_IRQ);
  }

  list_init(&c->disk_list); /* no disks yet; will scan in a moment */

  c->buffer = ptokv(page_alloc(BUFFER_LENGTH));

  /* TODO: It is unclear whether, in native mode, the BARs contain
     port numbers directly, or whether they should be masked with
     ~0x03. The low two bits might be used as flags?? */

  if (primary_native) {
    c->channel[0].base_port = read_pci_bar(v, 0);
    c->channel[0].control_port = read_pci_bar(v, 1) + 2;
  } else {
    c->channel[0].base_port = ATA_LEGACY_PRIMARY_CONTROL_BASE;
    c->channel[0].control_port =
      ATA_LEGACY_PRIMARY_CONTROL_BASE + ATA_LEGACY_CONTROL_ALTERNATE_STATUS_OFFSET;
  }

  if (secondary_native) {
    c->channel[1].base_port = read_pci_bar(v, 2);
    c->channel[1].control_port = read_pci_bar(v, 3) + 2;
  } else {
    c->channel[1].base_port = ATA_LEGACY_SECONDARY_CONTROL_BASE;
    c->channel[1].control_port =
      ATA_LEGACY_SECONDARY_CONTROL_BASE + ATA_LEGACY_CONTROL_ALTERNATE_STATUS_OFFSET;
  }

  /* BAR4 points to a 16-byte block of I/O port space, the low 8 bytes
     of which are for the primary and the high 8 bytes for the
     secondary controller. */
  c->channel[0].dma_port = read_pci_bar(v, 4);
  c->channel[1].dma_port = c->channel[0].dma_port + 8;

  printf(" - pri 0x%04x/0x%04x/0x%04x, sec 0x%04x/0x%04x/0x%04x\n",
	 c->channel[0].base_port, c->channel[0].control_port, c->channel[0].dma_port,
	 c->channel[1].base_port, c->channel[1].control_port, c->channel[1].dma_port);

  /* Disable interrupts from the two channels. */
  write_control(c, 0, 2);
  write_control(c, 1, 2);

  {
    int disk;
    for (disk = 0; disk < 4; disk++) {
      setup_disk(self, c, disk);
    }
  }

  /* Reenable interrupts from the two channels. */
  write_control(c, 0, 0);
  write_control(c, 1, 0);
}

static int hdd_init(struct driver *self) {
  int i;

  for (i = 0; i < pci_device_count; i++) {
    struct pci_device *v = &pci_devices[i];
    if (v->class_code == PCI_CLASS_STORAGE &&
	v->subclass == 1 /* IDE */)
    {
      setup_controller(self, v);
    }
  }

  return 0;
}

static int hdd_open(device_t dev, int mode) {
  /* There's nothing needs doing here. The device tree is static after
     the probe, and we no longer use locking at the device level
     (locking a request queue, instead). This applies to close etc as
     well, at least until we get asynchronous requests in Prex. */
  return 0;
}

static int hdd_close(device_t dev) {
  /* See hdd_open's comment. */
  return 0;
}

static int hdd_rw(struct ata_disk *disk, struct irp *irp, int cmd,
		  uint8_t *buf, size_t block_count, int blkno)
{
  int err;

  irp->cmd = cmd;
  irp->ntries = 0;
  irp->error = 0;
  irp->blkno = blkno;
  irp->blksz = block_count;
  irp->buf = buf;

  sched_lock();

  hdd_setup_io(disk, irp->cmd, irp->blkno, irp->blksz); /* TODO: 64 bit irp->blkno? */

  if (sched_sleep(&irp->iocomp) == SLP_INTR) {
    err = EINTR;
  } else {
    err = irp->error;
  }
  sched_unlock();

  return err;
}

static void adjust_blkno(device_t dev,
			 struct ata_disk **disk_p,
			 int *blkno_p,
			 size_t *limit_p)
{
  struct ata_device_handle *handle = get_handle(dev);
  switch (handle->kind) {
    case ATA_DEVICE_WHOLEDISK:
      *disk_p = handle->pointer.wholedisk;
      /* No adjustment to blkno required. */
      *limit_p = handle->pointer.wholedisk->addressable_sector_count;
      break;

    case ATA_DEVICE_PARTITION:
      *disk_p = handle->pointer.partition->disk;
      *blkno_p += handle->pointer.partition->start_lba;
      *limit_p = handle->pointer.partition->sector_count;
      break;

    default:
      panic("Unknown ata_device_handle kind");
  }
}

static int hdd_read(device_t dev, char *buf, size_t *nbyte, int blkno) {
  struct ata_disk *disk = NULL;
  uint8_t *kbuf;
  size_t sector_count = *nbyte / SECTOR_SIZE;
  size_t transferred_total = 0;
  size_t sector_limit = 0;

  adjust_blkno(dev, &disk, &blkno, &sector_limit);
  if ((blkno < 0) || (blkno + sector_count >= sector_limit))
    return EIO;

  kbuf = kmem_map(buf, *nbyte);
  if (kbuf == NULL)
    return EFAULT;
  /* TODO: could it be possible that noncontiguous physical pages are
     backing this portion of virtual address space? The code here (and
     in fdd.c, which it is based on) assumes not, I think... */

  while (sector_count > 0) {
    size_t transfer_sector_count =
      (sector_count > BUFFER_LENGTH_IN_SECTORS) ? BUFFER_LENGTH_IN_SECTORS : sector_count;
    size_t transfer_byte_count = SECTOR_SIZE * transfer_sector_count;
    int err;

    err = hdd_rw(disk, &disk->controller->irp, IO_READ,
		 disk->controller->buffer, transfer_sector_count, blkno);
    if (err) {
      printf("hdd_read error: %d\n", err);
      *nbyte = transferred_total;
      return EIO;
    }

    memcpy(kbuf, disk->controller->buffer, transfer_byte_count);

    transferred_total += transfer_byte_count;
    kbuf += transfer_byte_count;
    blkno += transfer_sector_count;
    sector_count -= transfer_sector_count;
  }

  *nbyte = transferred_total;
  return 0;
}

static int hdd_write(device_t dev, char *buf, size_t *nbyte, int blkno) {
  /* TODO: this */
  return EINVAL;
}

static struct devops hdd_devops = {
	/* open */	hdd_open,
	/* close */	hdd_close,
	/* read */	hdd_read,
	/* write */	hdd_write,
	/* ioctl */	no_ioctl,
	/* devctl */	no_devctl,
};

struct driver hdd_driver = {
	/* name */	"hdd",
	/* devsops */	&hdd_devops,
	/* devsz */	sizeof(struct ata_device_handle),
	/* flags */	0,
	/* probe */	NULL,
	/* init */	hdd_init,
	/* shutdown */	NULL,
};
