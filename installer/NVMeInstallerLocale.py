#
# NVMeInstallerLocale - locale wrapper
# Auto-generated -- do not edit; regenerate from the fixture module.
#

import catalog


class NVMeInstallerLocale:
    strings = {}
    cat = None

    MSG_WELCOME = 1
    MSG_FINISH = 2

    def __init__(self, language="", catalogName='NVMe.catalog', builtinLanguage='english'):
        self.strings[self.MSG_WELCOME] = '\nWelcome to the installation of the NVMe device driver.\n\nnvme.device exposes NVMe SSDs on PCIe to AmigaOS 4.1 Final Edition as standard block devices that can be partitioned, formatted, and mounted.\n\nThe following changes will be made to your system:\n\n    1.  nvme.device will be copied to "DEVS:"\n\n    2.  the nvme_stats statistics monitor will be copied to "C:"\n\nNo system restart is required: the driver loads on first use and NVMe namespaces are mounted automatically.\n\n\nPress "Next" to continue.'
        self.strings[self.MSG_FINISH] = '\nThe installation completed successfully.\n\nnvme.device has been copied to "DEVS:" and nvme_stats to "C:".  The driver loads on first use; NVMe namespaces are discovered and mounted automatically.\n\nOPTIONAL -- to BOOT from an NVMe disk, the driver must load as a Kickstart module instead:\n\n    1.  copy DEVS:nvme.device SYS:Kickstart/nvme.device\n\n    2.  add this line to the Kicklayout in use:\n\n            MODULE Kickstart/nvme.device\n\n    3.  add this line to Kickstart/diskboot.config (see diskboot.config.sample in this drawer):\n\n            nvme.device 1 1\n\n    4.  restart the system.\n\nQEMU users: place nvme.device inside kickstart.zip\'s Kickstart/ drawer instead -- see the project documentation.\n\n\nPress "Finish" to exit the installation.'

        try:
            self.cat = catalog.OpenCatalog(catalogName, language, builtinLanguage)
        except:
            self.cat = None

    def GetString(self, id):
        if self.cat != None:
            return self.cat.GetString(id, self.strings[id])
        return self.strings[id]
