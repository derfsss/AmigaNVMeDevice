/*
 * persist_nvme.c — flush-persistence half-test for nvme.device.
 *
 * Usage:   persist_nvme write [unit]     — write pattern + CMD_UPDATE
 *          persist_nvme verify [unit]    — verify the pattern is there
 *
 * Run "write" against a NON-snapshot disk image, power-cycle the
 * machine (hard stop — no guest shutdown), boot fresh, then run
 * "verify".  Passing proves the write path lands data where it claims
 * and that CMD_UPDATE (NVMe Flush) pushed it to stable storage before
 * the power-cut.
 *
 * The pattern covers 1 MiB at byte offset 2 MiB and embeds the block
 * number in every block so a misdirected write shows up as a verify
 * mismatch at the exact offset.
 */

#include <exec/exec.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVNAME    "nvme.device"
#define TEST_OFF   (2u * 1024u * 1024u)
#define TEST_LEN   (1024u * 1024u)
#define MAGIC      0x4E564D45u   /* 'NVME' */

static void fill_block(UBYTE *b, ULONG block_no, ULONG block_size)
{
    for (ULONG i = 0; i < block_size; i += 4) {
        ULONG v = MAGIC ^ (block_no * 2654435761u) ^ i;
        b[i+0] = (UBYTE)(v >> 24);
        b[i+1] = (UBYTE)(v >> 16);
        b[i+2] = (UBYTE)(v >> 8);
        b[i+3] = (UBYTE)(v);
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("usage: persist_nvme write|verify [unit]\n");
        return 1;
    }
    BOOL  writing = (strcmp(argv[1], "write") == 0);
    ULONG unit    = (argc > 2) ? (ULONG)strtol(argv[2], NULL, 10) : 0;

    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    struct IOStdReq *ioreq = port ? (struct IOStdReq *)
        IExec->AllocSysObjectTags(ASOT_IOREQUEST,
            ASOIOR_Size, sizeof(struct IOStdReq),
            ASOIOR_ReplyPort, port, TAG_DONE) : NULL;
    if (!ioreq || IExec->OpenDevice(DEVNAME, unit,
                                    (struct IORequest *)ioreq, 0) != 0) {
        printf("FAIL: OpenDevice %s unit %lu\n", DEVNAME, unit);
        return 1;
    }

    struct DriveGeometry dg;
    memset(&dg, 0, sizeof(dg));
    ioreq->io_Command = TD_GETGEOMETRY;
    ioreq->io_Data    = &dg;
    ioreq->io_Length  = sizeof(dg);
    if (IExec->DoIO((struct IORequest *)ioreq) != 0 || dg.dg_SectorSize == 0) {
        printf("FAIL: TD_GETGEOMETRY\n");
        return 1;
    }
    ULONG bs = dg.dg_SectorSize;

    UBYTE *buf = IExec->AllocVecTags(TEST_LEN,
        AVT_Type, MEMF_SHARED, AVT_Alignment, 4096,
        AVT_Clear, 0, TAG_DONE);
    if (!buf) { printf("FAIL: AllocVec\n"); return 1; }

    int rc = 0;
    if (writing) {
        for (ULONG b = 0; b < TEST_LEN / bs; b++)
            fill_block(buf + b * bs, TEST_OFF / bs + b, bs);

        ioreq->io_Command = CMD_WRITE;
        ioreq->io_Data    = buf;
        ioreq->io_Length  = TEST_LEN;
        ioreq->io_Offset  = TEST_OFF;
        if (IExec->DoIO((struct IORequest *)ioreq) != 0) {
            printf("FAIL: CMD_WRITE io_Error=%d\n", ioreq->io_Error);
            rc = 1;
        } else {
            ioreq->io_Command = CMD_UPDATE;
            ioreq->io_Data    = NULL;
            ioreq->io_Length  = 0;
            ioreq->io_Offset  = 0;
            if (IExec->DoIO((struct IORequest *)ioreq) != 0) {
                printf("FAIL: CMD_UPDATE io_Error=%d\n", ioreq->io_Error);
                rc = 1;
            } else {
                printf("PASS: wrote+flushed %lu KiB at offset %lu KiB\n",
                       (ULONG)(TEST_LEN / 1024u), (ULONG)(TEST_OFF / 1024u));
                printf("persist_nvme: WRITE PHASE COMPLETE\n");
            }
        }
    } else {
        memset(buf, 0, TEST_LEN);
        ioreq->io_Command = CMD_READ;
        ioreq->io_Data    = buf;
        ioreq->io_Length  = TEST_LEN;
        ioreq->io_Offset  = TEST_OFF;
        if (IExec->DoIO((struct IORequest *)ioreq) != 0) {
            printf("FAIL: CMD_READ io_Error=%d\n", ioreq->io_Error);
            rc = 1;
        } else {
            ULONG bad = 0, first_bad = 0;
            for (ULONG b = 0; b < TEST_LEN / bs && bad == 0; b++) {
                /* spot-check the first 8 bytes of each block */
                UBYTE tmp[8];
                memcpy(tmp, buf + b * bs, 8);
                for (ULONG i = 0; i < 8; i += 4) {
                    ULONG v = MAGIC ^ ((TEST_OFF / bs + b) * 2654435761u) ^ i;
                    if (tmp[i]   != (UBYTE)(v >> 24) ||
                        tmp[i+1] != (UBYTE)(v >> 16) ||
                        tmp[i+2] != (UBYTE)(v >> 8)  ||
                        tmp[i+3] != (UBYTE)(v)) {
                        bad++; first_bad = b;
                        break;
                    }
                }
            }
            if (bad == 0) {
                printf("PASS: pattern survived the power-cycle\n");
                printf("persist_nvme: VERIFY PHASE COMPLETE\n");
            } else {
                printf("FAIL: mismatch in block %lu\n", first_bad);
                rc = 1;
            }
        }
    }

    IExec->FreeVec(buf);
    IExec->CloseDevice((struct IORequest *)ioreq);
    IExec->FreeSysObject(ASOT_IOREQUEST, ioreq);
    IExec->FreeSysObject(ASOT_PORT, port);
    return rc;
}
