#
# NVMeInstallerLocale - locale wrapper
# Auto-generated -- do not edit; regenerate from the fixture module.
#

import catalog


class NVMeInstallerLocale:
    strings = {}
    cat = None

    MSG_WELCOME = 1
    MSG_DEST_INTRO = 2
    MSG_DEST_CHECKBOX = 3
    MSG_FINISH = 4
    MSG_REBOOT = 5

    def __init__(self, language="", catalogName='NVMe.catalog', builtinLanguage='english'):
        self.strings[self.MSG_WELCOME] = '\nWelcome to the installation of the NVMe device driver.\n\nnvme.device exposes NVMe SSDs on PCIe to AmigaOS 4.1 Final Edition as standard block devices that can be partitioned, formatted, mounted -- and booted from.\n\nThe following changes will be made to your system:\n\n    1.  nvme.device will be copied to "DEVS:" or "SYS:Kickstart" (your choice on the next page)\n\n    2.  the nvme_stats statistics monitor will be copied to "C:"\n\n\nPress "Next" to continue.'
        self.strings[self.MSG_DEST_INTRO] = '\nPlease choose where to install the driver.\n\nStandard installation copies nvme.device to "DEVS:".  The driver loads on first use and NVMe partitions are mounted automatically.  No restart is required.  This is the right choice for using NVMe disks as additional storage.\n\nTo BOOT AmigaOS from an NVMe disk, the driver must instead load as a Kickstart module: nvme.device is then copied to "SYS:Kickstart" and "SYS:Kickstart/Kicklayout" is updated to load it during startup (the previous configuration is preserved as "Kicklayout.bak").  One further manual step, shown at the end of the installation, completes the boot-drive setup.'
        self.strings[self.MSG_DEST_CHECKBOX] = 'Install as a Kickstart module (boot from NVMe)'
        self.strings[self.MSG_FINISH] = '\nThe installation completed successfully.\n\nnvme_stats has been copied to "C:".\n\nIf you chose the standard installation, nvme.device is now in "DEVS:".  The driver loads on first use; NVMe namespaces are discovered and mounted automatically.  No restart is required.\n\nIf you chose the Kickstart module installation, nvme.device is in "SYS:Kickstart" and "SYS:Kickstart/Kicklayout" has been updated (backup: "Kicklayout.bak").  To complete the boot-drive setup, add this line to "SYS:Kickstart/diskboot.config" (see diskboot.config.sample in this drawer), then restart:\n\n    nvme.device 1 1\n\nQEMU users booting via BBoot: place nvme.device, the Kicklayout line, and the diskboot.config entry inside kickstart.zip\'s Kickstart/ drawer instead -- see the project documentation.\n\n\nPress "Finish" to exit the installation.'
        self.strings[self.MSG_REBOOT] = 'Restart the system now (only needed for the Kickstart module install)'

        try:
            self.cat = catalog.OpenCatalog(catalogName, language, builtinLanguage)
        except:
            self.cat = None

    def GetString(self, id):
        if self.cat != None:
            return self.cat.GetString(id, self.strings[id])
        return self.strings[id]
