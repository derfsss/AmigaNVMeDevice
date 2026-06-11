#
# NVMe Device Driver Install install.py
# $VER: NVMe Device Driver Install 1.68 (11.06.2026)
# Auto-generated -- do not edit; regenerate from the fixture module.
#

from installer import *
from NVMeInstallerLocale import *
import amiga
import os

loc = NVMeInstallerLocale()

##############################################
# welcomePage
welcomePage = NewPage(WELCOME)
SetString(welcomePage, 'message', loc.GetString(loc.MSG_WELCOME))

##############################################
# installPage
installPage = NewPage(INSTALL)

##############################################
# finishPage
finishPage = NewPage(FINISH)
SetString(finishPage, 'message', loc.GetString(loc.MSG_FINISH))

##############################################
# Top-level packages (always registered)

_pkg = AddPackage(FILEPACKAGE,
    name='NVMe device driver',
    files=['content/nvme.device'],
    alternatepath="DEVS:"
    )

_pkg = AddPackage(FILEPACKAGE,
    name='nvme_stats statistics monitor',
    files=['content/nvme_stats'],
    alternatepath="C:"
    )

##############################################
# Run the installer
RunInstaller()
