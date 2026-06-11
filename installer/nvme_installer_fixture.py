"""Installer-script fixture for the nvme.device installer.

Produces an AmigaOS 4.1 FE `Installation Utility` script (Python 2.5)
that:

  1. asks where to install the driver -- a mutually-exclusive radio
     (MX) gadget; the first option is selected by default:

       - "Standard installation (DEVS:)"  [default]: runtime load --
         the driver loads on first OpenDevice() and namespaces
         auto-mount via mounter.library; no reboot needed
       - "Kickstart module (boot from NVMe)": nvme.device goes to
         SYS:Kickstart/ and `MODULE Kickstart/nvme.device` is appended
         to SYS:Kickstart/Kicklayout (Kicklayout.bak backup, LF-only
         line endings, idempotent)

  2. copies nvme_stats into C: in both cases
  3. explains the remaining manual boot-drive step (diskboot.config)
     on the finish page, and offers an optional reboot (default off --
     only the Kickstart install needs it)

install.py + NVMeInstallerLocale.py are emitted from this fixture by
an in-house installer-script generator and committed, so building the
distribution archive needs no extra tooling.  This fixture is the
authoritative description of the installer's pages, messages, and
behaviour.  The page idioms and the Kicklayout edit are expanded from
`installergen.presets` -- the field-tested templates shared by all of
this author's driver installers.

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
    LocaleString, LocaleRef, Handler,
)
from installergen.presets import (
    README_BUTTON_LOCALE, InsertAfterLast, welcome_with_readme,
    destination_choice, finish_page, system_edit_helper,
    system_edit_exit_handler,
)


# NOTE: the Installation Utility's page text is PLAIN TEXT only and the
# label does NOT scroll -- keep pages inside the ~20-rendered-line lint
# budget and defer detail to the bundled readme.  Formatting follows
# Hyperion's own Update installers: leading blank line, paragraph
# spacing, quoted file and button names, explicit navigation cue.
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
        "\"C:\"\n\n"
        "Click \"View Readme\" below for manual installation details "
        "and general instructions on use.\n\n\n"
        "Press \"Next\" to continue."),
    README_BUTTON_LOCALE,
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
        "MSG_DEST_OPT_DEVS",
        "Standard installation (DEVS:)"),
    LocaleString(
        "MSG_DEST_OPT_KICKSTART",
        "Kickstart module (boot from NVMe)"),
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


# Welcome page with the View Readme button (proven preset).
welcome_page = welcome_with_readme(LocaleRef("MSG_WELCOME"), "nvme.readme")

# Destination choice page (proven preset): radio gadget, DEVS: default,
# selected index captured into the installKickstart global.
dest_page, dest_preamble = destination_choice(
    intro=LocaleRef("MSG_DEST_INTRO"),
    choices=[LocaleRef("MSG_DEST_OPT_DEVS"),
             LocaleRef("MSG_DEST_OPT_KICKSTART")],
    capture_global="installKickstart",
)

# Kicklayout edit (proven preset), run only for the Kickstart install.
update_kicklayout = system_edit_helper(
    "SYS:Kickstart/Kicklayout",
    "MODULE Kickstart/nvme.device",
    InsertAfterLast(contains=".device"),
)

# The driver package is registered dynamically so its destination
# follows the choice; the Kicklayout edit runs on forward exit, gated
# on the Kickstart selection.
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
    exit_handler=system_edit_exit_handler(
        "SYS:Kickstart/Kicklayout",
        "MODULE Kickstart/nvme.device",
        "nvme.device installer",
        "manually, after the existing device driver lines:",
        gate_global="installKickstart",
    ),
)

finish = finish_page(LocaleRef("MSG_FINISH"))


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
    preamble=dest_preamble,
    helpers=[update_kicklayout],
    pages=[welcome_page, dest_page, install_page, finish],
    packages=[stats_package],
    post_install_actions=[reboot_action],
)
