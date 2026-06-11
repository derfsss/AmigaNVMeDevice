"""Installer-script fixture for the nvme.device installer.

Produces an AmigaOS 4.1 FE `Installation Utility` script (Python 2.5)
that:

  1. asks where to install the driver -- a GUI choice page in the
     style of the OS Update installers (U3's SDK page idiom: a
     checkbox whose state an exithandler captures via GetUIAttr):

       - unchecked (default): nvme.device -> DEVS:   (runtime load --
         the driver loads on first OpenDevice() and namespaces
         auto-mount via mounter.library; no reboot needed)
       - checked: nvme.device -> SYS:Kickstart/  (boot-drive install;
         the script also appends `MODULE Kickstart/nvme.device` to
         SYS:Kickstart/Kicklayout with a Kicklayout.bak backup,
         LF-only line endings, and idempotency)

  2. copies nvme_stats into C: in both cases
  3. explains the remaining manual boot-drive step (diskboot.config)
     on the finish page, and offers an optional reboot (default off --
     only the Kickstart install needs it)

The destination choice is implemented exactly like the official
AmigaOS 4.1 Update installers implement choices (GUI page + checkbox +
packages registered dynamically inside the install page's
entryhandler) -- the Installation Utility also offers AddRadioButton,
but no shipping Hyperion installer uses it, so we stay on the
field-proven DSL surface.

install.py + NVMeInstallerLocale.py are emitted from this fixture by
an in-house installer-script generator and committed, so building the
distribution archive needs no extra tooling.  This fixture is the
authoritative description of the installer's pages, messages, and
behaviour.

Archive layout consumed by the script (see `make dist`):

    nvme/
      install.py
      NVMeInstallerLocale.py
      content/nvme.device
      content/nvme_stats

IMPORTANT: the installer must be launched with the drawer as the
current directory (double-clicking the install.py icon does this; from
a shell, CD into the drawer first) -- the package uses drawer-relative
content/ paths, exactly like the OS's own update installers.
"""

from installergen import (
    Project, Page, PageKind, Package, PackageKind, PostInstallAction,
    LocaleString, LocaleRef, GuiBlock, GuiWidget, WidgetKind,
    GroupOrientation,
)
from installergen.model import Handler


locale = [
    LocaleString(
        "MSG_WELCOME",
        "\nWelcome to the installation of the NVMe device driver.\n\n"
        "nvme.device exposes NVMe SSDs on PCIe to AmigaOS 4.1 Final "
        "Edition as standard block devices that can be partitioned, "
        "formatted, mounted -- and booted from.\n\n"
        "The following changes will be made to your system:\n\n"
        "    1.  nvme.device will be copied to \"DEVS:\" or "
        "\"SYS:Kickstart\" (your choice on the next page)\n\n"
        "    2.  the nvme_stats statistics monitor will be copied to "
        "\"C:\"\n\n\n"
        "Press \"Next\" to continue."),
    LocaleString(
        "MSG_DEST_INTRO",
        "\nPlease choose where to install the driver.\n\n"
        "Standard installation (to \"DEVS:\"): the driver loads on "
        "first use and NVMe partitions mount automatically.  No "
        "restart is needed.\n\n"
        "Kickstart module (to \"SYS:Kickstart\"): choose this to BOOT "
        "from an NVMe disk.  Kicklayout is updated automatically "
        "(backup: \"Kicklayout.bak\"); see nvme.readme for details."),
    LocaleString(
        "MSG_DEST_CHECKBOX",
        "Install as a Kickstart module (boot from NVMe)"),
    LocaleString(
        "MSG_FINISH",
        "\nThe installation has finished.\n\n"
        "nvme.device and the nvme_stats tool are now installed.  With "
        "the standard installation no restart is needed: the driver "
        "loads on first use and NVMe partitions mount "
        "automatically.\n\n"
        "If you chose the Kickstart module installation, one manual "
        "step remains: add the line \"nvme.device 1 1\" to "
        "\"SYS:Kickstart/diskboot.config\" (see diskboot.config.sample "
        "in this drawer), then restart.  Full details, including QEMU "
        "BBoot setups, are in the nvme.readme file.\n\n\n"
        "Press \"Finish\" to exit the installation."),
    LocaleString(
        "MSG_REBOOT",
        "Restart the system now (only needed for the Kickstart module install)"),
]


# Appends the MODULE line to Kicklayout, directly after the last
# existing device-driver MODULE entry so the new driver loads alongside
# the other disk drivers.  Pure Python 2.5; binary file modes keep the
# LF-only line endings Kickstart loaders require.  Returns an error
# string, or None on success (including the already-installed case).
update_kicklayout = Handler(
    name="updateKicklayout",
    params=[],
    body=(
        "kl = \"SYS:Kickstart/Kicklayout\"\n"
        "module_line = \"MODULE Kickstart/nvme.device\"\n"
        "try:\n"
        "    f = open(kl, \"rb\")\n"
        "    data = f.read()\n"
        "    f.close()\n"
        "except IOError:\n"
        "    return \"could not read \" + kl\n"
        "lines = data.split(\"\\n\")\n"
        "for ln in lines:\n"
        "    if ln.strip() == module_line:\n"
        "        return None        # already installed\n"
        "last_dev = -1\n"
        "last_mod = -1\n"
        "for i in range(len(lines)):\n"
        "    stripped = lines[i].strip()\n"
        "    if stripped.startswith(\"MODULE\"):\n"
        "        last_mod = i\n"
        "        if stripped.find(\".device\") != -1:\n"
        "            last_dev = i\n"
        "insert_at = last_dev\n"
        "if insert_at == -1:\n"
        "    insert_at = last_mod\n"
        "if insert_at == -1:\n"
        "    return \"no MODULE lines found in \" + kl\n"
        "out = lines[:insert_at + 1] + [module_line] + lines[insert_at + 1:]\n"
        "try:\n"
        "    b = open(kl + \".bak\", \"wb\")\n"
        "    b.write(data)\n"
        "    b.close()\n"
        "except IOError:\n"
        "    pass                   # backup is best-effort\n"
        "try:\n"
        "    f = open(kl, \"wb\")\n"
        "    f.write(\"\\n\".join(out))\n"
        "    f.close()\n"
        "except IOError:\n"
        "    return \"could not write \" + kl\n"
        "return None\n"
    ),
)


welcome_page = Page(
    var_name="welcomePage",
    kind=PageKind.WELCOME,
    strings={"message": LocaleRef("MSG_WELCOME")},
)

# Destination choice page -- official Update-installer idiom (U3 SDK
# page): a GUI page with one checkbox; the exithandler captures its
# state into a global that the install entryhandler acts on.
destination_page = Page(
    var_name="destChoicePage",
    kind=PageKind.GUI,
    exit_handler=Handler(
        name="destChoiceExitHandler",
        params=["page_nr", "direction"],
        body=(
            "global installKickstart\n"
            "global KSCheckBoxID\n"
            "installKickstart = GetUIAttr(page_nr, KSCheckBoxID, GUI_CHECKED)\n"
            "return True\n"
        ),
    ),
    gui=GuiBlock(
        orientation=GroupOrientation.VERTICAL,
        children=[
            GuiWidget(kind=WidgetKind.LABEL,
                      label=LocaleRef("MSG_DEST_INTRO")),
            GuiBlock(
                orientation=GroupOrientation.VERTICAL,
                children=[
                    GuiWidget(kind=WidgetKind.SPACE, weight=1),
                    GuiWidget(
                        kind=WidgetKind.CHECKBOX,
                        label=LocaleRef("MSG_DEST_CHECKBOX"),
                        checked=0,
                        weight=0,
                        capture_id_as="KSCheckBoxID",
                    ),
                    GuiWidget(kind=WidgetKind.SPACE, weight=1),
                ],
            ),
            GuiWidget(kind=WidgetKind.SPACE),
        ],
    ),
)

# The driver package is registered dynamically so its destination
# follows the choice; the Kicklayout edit runs on forward exit, only
# for the Kickstart install.  Errors are reported via asl.MessageBox
# with manual-fix instructions; the wizard still completes so the
# copied driver isn't left half-installed silently.
install_page = Page(
    var_name="installPage",
    kind=PageKind.INSTALL,
    entry_handler=Handler(
        name="installEntryHandler",
        params=["page"],
        body=(
            "global installKickstart\n"
            "\n"
            "if installKickstart:\n"
            "    driverPackage = AddPackage(FILEPACKAGE,\n"
            "        name=\"NVMe device driver (Kickstart module)\",\n"
            "        files=[\"content/nvme.device\"],\n"
            "        alternatepath=\"SYS:Kickstart\"\n"
            "        )\n"
            "else:\n"
            "    driverPackage = AddPackage(FILEPACKAGE,\n"
            "        name=\"NVMe device driver\",\n"
            "        files=[\"content/nvme.device\"],\n"
            "        alternatepath=\"DEVS:\"\n"
            "        )\n"
        ),
    ),
    exit_handler=Handler(
        name="installExitHandler",
        params=["page_nr", "direction"],
        body=(
            "global installKickstart\n"
            "if direction != 1:\n"
            "    return True\n"
            "if not installKickstart:\n"
            "    return True\n"
            "err = updateKicklayout()\n"
            "if err:\n"
            "    try:\n"
            "        import asl\n"
            "        asl.MessageBox(\"nvme.device installer\",\n"
            "            \"Kicklayout update failed: \" + err + \"\\n\\n\"\n"
            "            \"Please add this line to SYS:Kickstart/Kicklayout\\n\"\n"
            "            \"manually, after the existing device driver lines:\\n\\n\"\n"
            "            \"MODULE Kickstart/nvme.device\",\n"
            "            \"OK\")\n"
            "    except StandardError:\n"
            "        pass\n"
            "return True\n"
        ),
    ),
)

finish_page = Page(
    var_name="finishPage",
    kind=PageKind.FINISH,
    strings={"message": LocaleRef("MSG_FINISH")},
)


stats_package = Package(
    name="nvme_stats statistics monitor",
    files=["content/nvme_stats"],
    kind=PackageKind.FILEPACKAGE,
    # Fixed destination: the CLI tool belongs in C: regardless of the
    # driver-destination choice.  Emitted verbatim, hence the quotes.
    alternatepath="\"C:\"",
    register_in="top",
)

reboot_action = PostInstallAction(
    name="Reboot",
    description=LocaleRef("MSG_REBOOT"),
    visible=True,
    default=False,
    callback=Handler(
        name="rebootHandler",
        params=[],
        body="amiga.system(\"reboot SYNC\")\nreturn True\n",
    ),
)


project = Project(
    name="NVMe Device Driver Install",
    short_name="NVMe",
    version="1.68",
    date="11.06.2026",
    locale_strings=locale,
    preamble=[
        "installKickstart = 0",
        "KSCheckBoxID = None",
    ],
    helpers=[update_kicklayout],
    pages=[welcome_page, destination_page, install_page, finish_page],
    packages=[stats_package],
    post_install_actions=[reboot_action],
)
