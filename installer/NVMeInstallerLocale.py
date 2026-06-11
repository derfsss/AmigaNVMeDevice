#
# NVMeInstallerLocale - locale wrapper
# Auto-generated -- do not edit; regenerate from the fixture module.
#

import catalog


class NVMeInstallerLocale:
    strings = {}
    cat = None

    MSG_WELCOME = 1
    MSG_README_BUTTON = 2
    MSG_DEST_INTRO = 3
    MSG_DEST_OPT_DEVS = 4
    MSG_DEST_OPT_KICKSTART = 5
    MSG_FINISH = 6
    MSG_REBOOT = 7

    def __init__(self, language="", catalogName='NVMe.catalog', builtinLanguage='english'):
        self.strings[self.MSG_WELCOME] = '\nWelcome to the installation of the NVMe device driver.\n\nnvme.device exposes NVMe SSDs on PCIe to AmigaOS 4.1 Final Edition as standard block devices that can be partitioned, formatted, mounted -- and booted from.\n\nThe following changes will be made to your system:\n\n    1.  nvme.device will be copied to "DEVS:" or "SYS:Kickstart" (your choice on the next page)\n\n    2.  the nvme_stats statistics monitor will be copied to "C:"\n\nClick "View Readme" below for manual installation details and general instructions on use.\n\n\nPress "Next" to continue.'
        self.strings[self.MSG_README_BUTTON] = 'View Readme...'
        self.strings[self.MSG_DEST_INTRO] = '\nPlease choose where to install the driver.\n\nStandard installation (to "DEVS:"): the driver loads on first use and NVMe partitions mount automatically.  No restart is needed.\n\nKickstart module (to "SYS:Kickstart"): choose this to BOOT from an NVMe disk.  Kicklayout is updated automatically (backup: "Kicklayout.bak"); see nvme.readme for details.'
        self.strings[self.MSG_DEST_OPT_DEVS] = 'Standard installation (DEVS:)'
        self.strings[self.MSG_DEST_OPT_KICKSTART] = 'Kickstart module (boot from NVMe)'
        self.strings[self.MSG_FINISH] = '\nThe installation has finished.\n\nnvme.device and the nvme_stats tool are now installed.  With the standard installation no restart is needed: the driver loads on first use and NVMe partitions mount automatically.\n\nIf you chose the Kickstart module installation, one manual step remains: add the line "nvme.device 1 1" to "SYS:Kickstart/diskboot.config" (see diskboot.config.sample in this drawer), then restart.  Full details, including QEMU BBoot setups, are in the nvme.readme file.\n\n\nPress "Finish" to exit the installation.'
        self.strings[self.MSG_REBOOT] = 'Restart the system now (only needed for the Kickstart module install)'

        try:
            self.cat = catalog.OpenCatalog(catalogName, language, builtinLanguage)
        except:
            self.cat = None

    def GetString(self, id):
        if self.cat != None:
            return self.cat.GetString(id, self.strings[id])
        return self.strings[id]
