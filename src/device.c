/*
 * device.c — Resident tag, manager interface vector, and exec entry stub.
 *
 * The Resident struct is scanned by exec at boot.  RTF_AUTOINIT tells
 * exec to follow CLT_InitFunc (== _manager_Init) and treat the library
 * as ready to use after that returns non-NULL.  RTF_COLDSTART + priority
 * 0 matches virtioscsi.device / disk.device, which lets diskboot.kmod
 * consider nvme.device as a boot-drive candidate.
 *
 * The _manager_Vectors table is the v1 "__device" interface: Obtain /
 * Release / [reserved] / [reserved] / Open / Close / Expunge /
 * [reserved] / BeginIO / AbortIO.  Entries we do not implement stay
 * NULL; the table is terminated with (APTR)-1.
 *
 * _start exists only so the binary can survive being double-clicked —
 * it returns 0 immediately rather than jumping into random driver code.
 */

#include "version.h"
#include "nvme_device.h"
#include <exec/exectags.h>
#include <exec/interfaces.h>
#include <exec/resident.h>

/* Obtain/Release refcount management for the device interface */
ULONG _manager_Obtain(struct DeviceManagerInterface *Self)
{
    Self->Data.RefCount++;
    return Self->Data.RefCount;
}

ULONG _manager_Release(struct DeviceManagerInterface *Self)
{
    Self->Data.RefCount--;
    return Self->Data.RefCount;
}

/* Interface vector table */
static const APTR _manager_Vectors[] = {
    (APTR)_manager_Obtain,
    (APTR)_manager_Release,
    (APTR)NULL,
    (APTR)NULL,
    (APTR)_manager_Open,
    (APTR)_manager_Close,
    (APTR)_manager_Expunge,
    (APTR)NULL,
    (APTR)_manager_BeginIO,
    (APTR)_manager_AbortIO,
    (APTR)-1
};

static const struct TagItem _manager_Tags[] = {
    {MIT_Name,        (ULONG)"__device"},
    {MIT_VectorTable, (ULONG)_manager_Vectors},
    {MIT_Version,     1},
    {TAG_END,         0}
};

const APTR devInterfaces[] = {(APTR)_manager_Tags, (APTR)NULL};

static const char verstag[] __attribute__((used)) = "\0$VER: " DEVVERSIONSTRING;

extern struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec);

static const struct TagItem dev_init_tags[] = {
    {CLT_DataSize,     sizeof(struct NVMeBase)},
    {CLT_Interfaces,   (ULONG)devInterfaces},
    {CLT_InitFunc,     (ULONG)_manager_Init},
    {CLT_NoLegacyIFace, TRUE},
    {TAG_END,          0}
};

static const struct Resident dev_res __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&dev_res,
    (struct Resident *)(&dev_res + 1),
    RTF_NATIVE | RTF_COLDSTART | RTF_AUTOINIT,
    DEVVER,
    NT_DEVICE,
    /* Resident priority 0 matches virtioscsi.device and disk.device: it
     * lets diskboot.kmod evaluate us as a boot-drive candidate.  mounter.library
     * is listed in diskboot.config ahead of us, so it is already resident by
     * the time we initialise. */
    0,
    DEVNAME,
    DEVVERSIONSTRING,
    (APTR)dev_init_tags
};

/* Prevent execution as a shell command */
int _start(char *argstring, int arglen, struct ExecBase *sysbase)
{
    (void)argstring; (void)arglen;
    struct ExecIFace *IExec = (struct ExecIFace *)sysbase->MainInterface;
    IExec->DebugPrintF("%s cannot be executed from a shell — install in "
                       "SYS:Kickstart/ and reboot.\n", DEVNAME);
    return 20; /* RETURN_FAIL */
}
