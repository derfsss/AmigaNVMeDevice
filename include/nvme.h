#ifndef NVME_H
#define NVME_H

#include <exec/types.h>

/*
 * nvme.h — NVMe controller register offsets, field definitions,
 *           submission/completion queue entry structures, and opcodes.
 *
 * NVMe registers are 32-bit little-endian. On AmigaOne (Articia-S bridge)
 * they are accessed via pciDev->InLong()/OutLong() etc., which handle
 * the endian swap automatically (same pattern as legacy VirtIO).
 *
 * All offsets are relative to BAR0->Physical.
 */

/* ------------------------------------------------------------------ */
/* BAR0 register offsets                                               */
/* ------------------------------------------------------------------ */

#define NVME_REG_CAP_LO  0x00  /* Controller Capabilities [31:0] */
#define NVME_REG_CAP_HI  0x04  /* Controller Capabilities [63:32] */
#define NVME_REG_VS      0x08  /* Version */
#define NVME_REG_INTMS   0x0C  /* Interrupt Mask Set */
#define NVME_REG_INTMC   0x10  /* Interrupt Mask Clear */
#define NVME_REG_CC      0x14  /* Controller Configuration */
#define NVME_REG_CSTS    0x1C  /* Controller Status */
#define NVME_REG_NSSR    0x20  /* NVM Subsystem Reset */
#define NVME_REG_AQA     0x24  /* Admin Queue Attributes */
#define NVME_REG_ASQ_LO  0x28  /* Admin SQ Base Address [31:0] */
#define NVME_REG_ASQ_HI  0x2C  /* Admin SQ Base Address [63:32] */
#define NVME_REG_ACQ_LO  0x30  /* Admin CQ Base Address [31:0] */
#define NVME_REG_ACQ_HI  0x34  /* Admin CQ Base Address [63:32] */

/* Doorbell registers begin at offset 0x1000.
 * Stride = 4 << CAP.DSTRD bytes.
 * SQ tail doorbell for queue y = 0x1000 + (2*y)   * stride
 * CQ head doorbell for queue y = 0x1000 + (2*y+1) * stride
 */
#define NVME_DOORBELL_BASE  0x1000
#define NVME_SQ_TAIL_DB(qid, dstrd)  (NVME_DOORBELL_BASE + (2*(qid))   * (4 << (dstrd)))
#define NVME_CQ_HEAD_DB(qid, dstrd)  (NVME_DOORBELL_BASE + (2*(qid)+1) * (4 << (dstrd)))

/* ------------------------------------------------------------------ */
/* CAP register field extraction (from CAP_LO / CAP_HI)               */
/* ------------------------------------------------------------------ */

/* CAP_LO */
#define NVME_CAP_MQES(cap_lo)   ((cap_lo) & 0xFFFF)           /* max queue entries - 1 */
#define NVME_CAP_DSTRD(cap_lo)  (((cap_lo) >> 20) & 0xF)      /* doorbell stride */
#define NVME_CAP_MPSMIN(cap_lo) (((cap_lo) >> 21) & 0xF)      /* NOTE: in CAP_HI bits [7:4] */

/* CAP_HI */
#define NVME_CAP_MPSMIN_HI(cap_hi) (((cap_hi) >> 16) & 0xF)   /* min host page size (log2 - 12) */
#define NVME_CAP_MPSMAX_HI(cap_hi) (((cap_hi) >> 20) & 0xF)   /* max host page size (log2 - 12) */

/* ------------------------------------------------------------------ */
/* CC — Controller Configuration                                       */
/* ------------------------------------------------------------------ */

#define NVME_CC_EN       (1 << 0)          /* Enable */
#define NVME_CC_CSS_NVM  (0 << 4)          /* NVM command set */
#define NVME_CC_MPS(n)   ((n) << 7)        /* Host page size (log2 - 12) */
#define NVME_CC_AMS_RR   (0 << 11)         /* Round-robin arbitration */
#define NVME_CC_SHN_NONE (0 << 14)         /* No shutdown */
#define NVME_CC_SHN_NORM (1 << 14)         /* Normal shutdown */
#define NVME_CC_SHN_ABRT (2 << 14)         /* Abrupt shutdown */
#define NVME_CC_IOSQES(n) ((n) << 16)      /* I/O SQ entry size (log2) */
#define NVME_CC_IOCQES(n) ((n) << 20)      /* I/O CQ entry size (log2) */

#define NVME_CC_DEFAULT  (NVME_CC_CSS_NVM | NVME_CC_AMS_RR | NVME_CC_SHN_NONE | \
                          NVME_CC_MPS(0) | NVME_CC_IOSQES(6) | NVME_CC_IOCQES(4))

/* ------------------------------------------------------------------ */
/* CSTS — Controller Status                                            */
/* ------------------------------------------------------------------ */

#define NVME_CSTS_RDY    (1 << 0)   /* Controller ready */
#define NVME_CSTS_CFS    (1 << 1)   /* Controller fatal status */
#define NVME_CSTS_SHST(s) (((s) >> 2) & 0x3) /* Shutdown status */

/* ------------------------------------------------------------------ */
/* AQA — Admin Queue Attributes                                        */
/* ------------------------------------------------------------------ */

#define NVME_AQA_ASQS(n)  (((n) - 1) & 0xFFF)         /* admin SQ size (entries-1) */
#define NVME_AQA_ACQS(n)  ((((n) - 1) & 0xFFF) << 16) /* admin CQ size (entries-1) */

/* ------------------------------------------------------------------ */
/* Submission Queue Entry (64 bytes)                                   */
/* ------------------------------------------------------------------ */

struct nvme_sqe {
    ULONG  cdw0;    /* opcode [7:0], fuse [9:8], psdt [15:14], cid [31:16] */
    ULONG  nsid;    /* namespace ID */
    ULONG  cdw2;
    ULONG  cdw3;
    ULONG  mptr_lo; /* metadata pointer [31:0] */
    ULONG  mptr_hi; /* metadata pointer [63:32] */
    ULONG  prp1_lo; /* PRP entry 1 [31:0] */
    ULONG  prp1_hi; /* PRP entry 1 [63:32] */
    ULONG  prp2_lo; /* PRP entry 2 / PRP list [31:0] */
    ULONG  prp2_hi; /* PRP entry 2 / PRP list [63:32] */
    ULONG  cdw10;
    ULONG  cdw11;
    ULONG  cdw12;
    ULONG  cdw13;
    ULONG  cdw14;
    ULONG  cdw15;
};

/* SQE CDW0 construction */
#define NVME_CDW0(opcode, cid)  ((ULONG)(opcode) | ((ULONG)(cid) << 16))

/* ------------------------------------------------------------------ */
/* Completion Queue Entry (16 bytes)                                   */
/* ------------------------------------------------------------------ */

struct nvme_cqe {
    ULONG  cdw0;    /* command-specific */
    ULONG  cdw1;    /* reserved */
    UWORD  sq_head; /* SQ head pointer */
    UWORD  sq_id;   /* SQ identifier */
    UWORD  cid;     /* command identifier */
    UWORD  status;  /* phase bit [0], status field [15:1] */
};

#define NVME_CQE_PHASE(status)   ((status) & 1)
#define NVME_CQE_STATUS(status)  (((status) >> 1) & 0x7FFF)
#define NVME_CQE_SC(status)      (((status) >> 1) & 0xFF)   /* status code */
#define NVME_CQE_SCT(status)     (((status) >> 9) & 0x7)    /* status code type */

#define NVME_STATUS_SUCCESS      0x0000

/* ------------------------------------------------------------------ */
/* Admin opcodes                                                        */
/* ------------------------------------------------------------------ */

#define NVME_ADMIN_DELETE_SQ     0x00
#define NVME_ADMIN_CREATE_SQ     0x01
#define NVME_ADMIN_DELETE_CQ     0x04
#define NVME_ADMIN_CREATE_CQ     0x05
#define NVME_ADMIN_IDENTIFY      0x06
#define NVME_ADMIN_ABORT         0x08
#define NVME_ADMIN_GET_LOG_PAGE  0x02
#define NVME_ADMIN_FIRMWARE_COMMIT    0x10
#define NVME_ADMIN_FIRMWARE_IMAGE_DL  0x11

/* Identify CNS values */
#define NVME_ID_CNS_NAMESPACE    0x00   /* Identify Namespace */
#define NVME_ID_CNS_CONTROLLER   0x01   /* Identify Controller */
#define NVME_ID_CNS_NS_LIST      0x02   /* Identify Active Namespace ID list */

/* Create I/O CQ CDW11 flags */
#define NVME_CQ_FLAGS_PC         (1 << 0)   /* physically contiguous */
#define NVME_CQ_FLAGS_IEN        (1 << 1)   /* interrupts enabled */

/* Create I/O SQ CDW11 flags */
#define NVME_SQ_FLAGS_PC         (1 << 0)   /* physically contiguous */
#define NVME_SQ_PRIO_URG         (0 << 1)
#define NVME_SQ_PRIO_HIGH        (1 << 1)
#define NVME_SQ_PRIO_MED         (2 << 1)
#define NVME_SQ_PRIO_LOW         (3 << 1)

/* ------------------------------------------------------------------ */
/* I/O opcodes                                                          */
/* ------------------------------------------------------------------ */

#define NVME_CMD_FLUSH           0x00
#define NVME_CMD_WRITE           0x01
#define NVME_CMD_READ            0x02

/* ------------------------------------------------------------------ */
/* Identify Namespace data (4096 bytes)                                */
/* ------------------------------------------------------------------ */

struct nvme_id_ns {
    UQUAD  nsze;         /* namespace size in LBAs */
    UQUAD  ncap;         /* namespace capacity */
    UQUAD  nuse;         /* namespace utilization */
    UBYTE  nsfeat;
    UBYTE  nlbaf;        /* number of LBA formats - 1 */
    UBYTE  flbas;        /* formatted LBA size [3:0] = index into lbaf[] */
    UBYTE  mc;
    UBYTE  dpc;
    UBYTE  dps;
    UBYTE  nmic;
    UBYTE  rescap;
    UBYTE  fpi;
    UBYTE  dlfeat;
    UWORD  nawun;
    UWORD  nawupf;
    UWORD  nacwu;
    UWORD  nabsn;
    UWORD  nabo;
    UWORD  nabspf;
    UWORD  noiob;
    UBYTE  nvmcap[16];
    UWORD  npwg;
    UWORD  npwa;
    UWORD  npdg;
    UWORD  npda;
    UWORD  nows;
    UBYTE  reserved72[18];
    ULONG  anagrpid;
    UBYTE  reserved96[3];
    UBYTE  nsattr;
    UWORD  nvmsetid;
    UWORD  endgid;
    UBYTE  nguid[16];
    UBYTE  eui64[8];
    struct {
        ULONG ds;   /* [3:0] = data shift (log2 LBA size - 9); [19:16] = relative perf */
    } lbaf[16];
    UBYTE  reserved192[192];
    UBYTE  vs[3712];
};

/* LBA format data shift: lba_size = 512 << lbaf_ds */
#define NVME_LBAF_DS(lbaf_ds_field)  ((lbaf_ds_field) & 0xF)

/* ------------------------------------------------------------------ */
/* Identify Controller data (4096 bytes, partial)                      */
/* ------------------------------------------------------------------ */

struct nvme_id_ctrl {
    UWORD  vid;         /* PCI vendor ID */
    UWORD  ssvid;
    UBYTE  sn[20];      /* serial number */
    UBYTE  mn[40];      /* model number */
    UBYTE  fr[8];       /* firmware revision */
    UBYTE  rab;
    UBYTE  ieee[3];
    UBYTE  cmic;
    UBYTE  mdts;        /* max data transfer size (log2 of pages, 0=unlimited) */
    UWORD  cntlid;
    ULONG  ver;
    ULONG  rtd3r;
    ULONG  rtd3e;
    ULONG  oaes;
    ULONG  ctratt;
    /* ... many more fields, only first part needed at init */
    UBYTE  reserved100[156];
    UWORD  oacs;
    UBYTE  acl;
    UBYTE  aerl;
    UBYTE  frmw;
    UBYTE  lpa;
    UBYTE  elpe;
    UBYTE  npss;
    UBYTE  avscc;
    UBYTE  apsta;
    UWORD  wctemp;
    UWORD  cctemp;
    UBYTE  reserved270[3826];
};

/* ------------------------------------------------------------------ */
/* Queue sizing constants                                              */
/* ------------------------------------------------------------------ */

#define NVME_ADMIN_QUEUE_DEPTH  64   /* entries in admin SQ and CQ */
#define NVME_IO_QUEUE_DEPTH     64   /* entries in each I/O SQ and CQ */
#define NVME_MAX_INFLIGHT       16   /* max pipelined I/O requests per unit */
#define NVME_BOUNCE_SIZE        4096 /* max transfer size for bounce buffer path */

#define NVME_SQE_SIZE  64            /* bytes per SQ entry */
#define NVME_CQE_SIZE  16            /* bytes per CQ entry */

#endif /* NVME_H */
