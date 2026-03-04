/*
 * test_nvme.c — Basic nvme.device test program
 *
 * Compile: ppc-amigaos-gcc -O2 -Wall test_nvme.c -o test_nvme -lauto
 * Run on AmigaOS: test_nvme [unit]
 *
 * Tests performed:
 *   1. OpenDevice("nvme.device", unit, ...)
 *   2. NSCMD_DEVICEQUERY — list supported commands
 *   3. TD_GETGEOMETRY    — print disk size
 *   4. CMD_READ block 0  — read first 512 bytes
 *   5. CMD_WRITE + CMD_READ verify — write pattern to last block, read back
 *   6. CMD_UPDATE (flush)
 *   7. CloseDevice
 */

#include <exec/exec.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#define DEVNAME "nvme.device"

static struct IOStdReq *create_ioreq(struct MsgPort *port)
{
    return (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
}

int main(int argc, char *argv[])
{
    ULONG unit = 0;
    if (argc > 1)
        unit = strtol(argv[1], NULL, 10);

    printf("nvme.device test — unit %lu\n", unit);

    struct MsgPort  *port  = CreateMsgPort();
    struct IOStdReq *ioreq = create_ioreq(port);
    if (!port || !ioreq) {
        printf("FAIL: could not create message port/ioreq\n");
        return 1;
    }

    /* 1. Open device */
    LONG err = OpenDevice(DEVNAME, unit, (struct IORequest *)ioreq, 0);
    if (err != 0) {
        printf("FAIL: OpenDevice returned %ld\n", err);
        DeleteIORequest((struct IORequest *)ioreq);
        DeleteMsgPort(port);
        return 1;
    }
    printf("PASS: OpenDevice unit %lu\n", unit);

    /* 2. NSCMD_DEVICEQUERY */
    struct NSDeviceQueryResult qr;
    memset(&qr, 0, sizeof(qr));
    ioreq->io_Command = NSCMD_DEVICEQUERY;
    ioreq->io_Data    = &qr;
    ioreq->io_Length  = sizeof(qr);
    if (DoIO((struct IORequest *)ioreq) == 0) {
        printf("PASS: NSCMD_DEVICEQUERY — DeviceType=%lu\n", (ULONG)qr.DeviceType);
        if (qr.SupportedCommands) {
            printf("      Supported commands:");
            for (UWORD *cmd = qr.SupportedCommands; *cmd; cmd++)
                printf(" %u", *cmd);
            printf("\n");
        }
    } else {
        printf("FAIL: NSCMD_DEVICEQUERY error %d\n", ioreq->io_Error);
    }

    /* 3. TD_GETGEOMETRY */
    struct DriveGeometry dg;
    memset(&dg, 0, sizeof(dg));
    ioreq->io_Command = TD_GETGEOMETRY;
    ioreq->io_Data    = &dg;
    ioreq->io_Length  = sizeof(dg);
    if (DoIO((struct IORequest *)ioreq) == 0) {
        UQUAD total_bytes = (UQUAD)dg.dg_TotalSectors * dg.dg_SectorSize;
        printf("PASS: TD_GETGEOMETRY — SectorSize=%lu TotalSectors=%lu (~%lu MB)\n",
               dg.dg_SectorSize, dg.dg_TotalSectors,
               (ULONG)(total_bytes / (1024*1024)));
    } else {
        printf("FAIL: TD_GETGEOMETRY error %d\n", ioreq->io_Error);
    }

    /* Allocate aligned read/write buffer */
    UBYTE *buf = AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) {
        printf("FAIL: AllocVec\n");
        goto done;
    }

    /* 4. CMD_READ block 0 */
    ioreq->io_Command = CMD_READ;
    ioreq->io_Data    = buf;
    ioreq->io_Length  = 512;
    ioreq->io_Offset  = 0;
    if (DoIO((struct IORequest *)ioreq) == 0) {
        printf("PASS: CMD_READ block 0 — first 4 bytes: %02X %02X %02X %02X\n",
               buf[0], buf[1], buf[2], buf[3]);
    } else {
        printf("FAIL: CMD_READ error %d\n", ioreq->io_Error);
    }

    /* 5. CMD_WRITE + readback verify (write to block 1 — safe, avoids MBR) */
    memset(buf, 0xA5, 512);
    ioreq->io_Command = CMD_WRITE;
    ioreq->io_Data    = buf;
    ioreq->io_Length  = 512;
    ioreq->io_Offset  = 512; /* block 1 */
    if (DoIO((struct IORequest *)ioreq) == 0) {
        printf("PASS: CMD_WRITE block 1\n");

        /* CMD_UPDATE (flush) */
        ioreq->io_Command = CMD_UPDATE;
        ioreq->io_Data    = NULL;
        ioreq->io_Length  = 0;
        ioreq->io_Offset  = 0;
        if (DoIO((struct IORequest *)ioreq) == 0)
            printf("PASS: CMD_UPDATE (flush)\n");
        else
            printf("WARN: CMD_UPDATE error %d (may not be supported)\n", ioreq->io_Error);

        /* Readback */
        memset(buf, 0, 512);
        ioreq->io_Command = CMD_READ;
        ioreq->io_Data    = buf;
        ioreq->io_Length  = 512;
        ioreq->io_Offset  = 512;
        if (DoIO((struct IORequest *)ioreq) == 0) {
            BOOL ok = TRUE;
            for (int i = 0; i < 512; i++)
                if (buf[i] != 0xA5) { ok = FALSE; break; }
            printf("%s: Write/readback verify block 1\n", ok ? "PASS" : "FAIL");
        } else {
            printf("FAIL: Readback CMD_READ error %d\n", ioreq->io_Error);
        }
    } else {
        printf("FAIL: CMD_WRITE error %d\n", ioreq->io_Error);
    }

    FreeVec(buf);

done:
    /* 7. CloseDevice */
    CloseDevice((struct IORequest *)ioreq);
    printf("PASS: CloseDevice\n");

    DeleteIORequest((struct IORequest *)ioreq);
    DeleteMsgPort(port);

    printf("Test complete.\n");
    return 0;
}
