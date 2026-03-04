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
    -60,              /* init after mounter.library */
    DEVNAME,
    DEVVERSIONSTRING,
    (APTR)dev_init_tags
};

/* Prevent execution as a shell command */
int _start(char *argstring, int arglen, struct ExecBase *sysbase)
{
    (void)argstring; (void)arglen; (void)sysbase;
    return 0;
}
