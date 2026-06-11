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

installKickstart = 0
DestRadioID = None

def updateKicklayout():
    kl = "SYS:Kickstart/Kicklayout"
    module_line = "MODULE Kickstart/nvme.device"
    try:
        f = open(kl, "rb")
        data = f.read()
        f.close()
    except IOError:
        return "could not read " + kl
    lines = data.split("\n")
    for ln in lines:
        if ln.strip() == module_line:
            return None        # already installed
    last_dev = -1
    last_mod = -1
    for i in range(len(lines)):
        stripped = lines[i].strip()
        if stripped.startswith("MODULE"):
            last_mod = i
            if stripped.find(".device") != -1:
                last_dev = i
    insert_at = last_dev
    if insert_at == -1:
        insert_at = last_mod
    if insert_at == -1:
        return "no MODULE lines found in " + kl
    out = lines[:insert_at + 1] + [module_line] + lines[insert_at + 1:]
    try:
        b = open(kl + ".bak", "wb")
        b.write(data)
        b.close()
    except IOError:
        pass                   # backup is best-effort
    try:
        f = open(kl, "wb")
        f.write("\n".join(out))
        f.close()
    except IOError:
        return "could not write " + kl
    return None

##############################################
# welcomePage
welcomePage = NewPage(GUI)

def readmeLaunch(page, id):
    amiga.system('notepad *>NIL: "nvme.readme"')
    return True

StartGUI(welcomePage)
BeginGroup(GROUP_VERTICAL)
AddLabel(label=loc.GetString(loc.MSG_WELCOME))
BeginGroup(GROUP_HORIZONTAL, weight=0)
AddSpace(weight=1)
AddButton(label=loc.GetString(loc.MSG_README_BUTTON), frame=BUTTON_FRAME, onclick=readmeLaunch, weight=10)
AddSpace(weight=1)
EndGroup()
AddSpace()
EndGroup()
EndGUI()

##############################################
# destChoicePage
destChoicePage = NewPage(GUI)

def destChoiceExitHandler(page_nr, direction):
    global installKickstart
    global DestRadioID
    installKickstart = GetUIAttr(page_nr, DestRadioID, GUI_SELECTED)
    return True
SetObject(destChoicePage, "exithandler", destChoiceExitHandler)

StartGUI(destChoicePage)
BeginGroup(GROUP_VERTICAL)
AddLabel(label=loc.GetString(loc.MSG_DEST_INTRO))
BeginGroup(GROUP_VERTICAL)
AddSpace(weight=1)
DestRadioID = AddRadioButton(choices=[loc.GetString(loc.MSG_DEST_OPT_DEVS), loc.GetString(loc.MSG_DEST_OPT_KICKSTART)], weight=0)
AddSpace(weight=1)
EndGroup()
AddSpace()
EndGroup()
EndGUI()

##############################################
# installPage
installPage = NewPage(INSTALL)

def installEntryHandler(page):
    global installKickstart

    if installKickstart:
        driverPackage = AddPackage(FILEPACKAGE,
            name="NVMe device driver (Kickstart module)",
            files=["content/nvme.device"],
            alternatepath="SYS:Kickstart"
            )
    else:
        driverPackage = AddPackage(FILEPACKAGE,
            name="NVMe device driver",
            files=["content/nvme.device"],
            alternatepath="DEVS:"
            )
SetObject(installPage, "entryhandler", installEntryHandler)

def installExitHandler(page_nr, direction):
    global installKickstart
    if direction != 1:
        return True
    if not installKickstart:
        return True
    err = updateKicklayout()
    if err:
        try:
            import asl
            asl.MessageBox("nvme.device installer",
                "Kicklayout update failed: " + err + "\n\n"
                "Please add this line to SYS:Kickstart/Kicklayout\n"
                "manually, after the existing device driver lines:\n\n"
                "MODULE Kickstart/nvme.device",
                "OK")
        except StandardError:
            pass
    return True
SetObject(installPage, "exithandler", installExitHandler)

##############################################
# Post-install actions

def rebootHandler():
    amiga.system("reboot SYNC")
    return True

AddPostInstallAction(
    name='Reboot',
    description=loc.GetString(loc.MSG_REBOOT),
    visible=True,
    default=False,
    callback=rebootHandler,
    )

##############################################
# finishPage
finishPage = NewPage(FINISH)
SetString(finishPage, 'message', loc.GetString(loc.MSG_FINISH))

##############################################
# Top-level packages (always registered)

_pkg = AddPackage(FILEPACKAGE,
    name='nvme_stats statistics monitor',
    files=['content/nvme_stats'],
    alternatepath="C:"
    )

##############################################
# Run the installer
RunInstaller()
