"""Installer-script fixture for the nvme.device installer.

Produces an AmigaOS 4.1 FE `Installation Utility` script (Python 2.5)
that:

  1. copies nvme.device into DEVS:   (runtime load -- the driver loads
     on first OpenDevice() and namespaces auto-mount via
     mounter.library; no reboot needed)
  2. copies nvme_stats into C:
  3. explains the OPTIONAL boot-drive (Kickstart module) steps on the
     finish page -- those edits (Kicklayout + diskboot.config) are
     deliberately left manual

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
    Project, Page, PageKind, Package, PackageKind,
    LocaleString, LocaleRef,
)


locale = [
    LocaleString(
        "MSG_WELCOME",
        "\nWelcome to the installation of the NVMe device driver.\n\n"
        "nvme.device exposes NVMe SSDs on PCIe to AmigaOS 4.1 Final "
        "Edition as standard block devices that can be partitioned, "
        "formatted, and mounted.\n\n"
        "The following changes will be made to your system:\n\n"
        "    1.  nvme.device will be copied to \"DEVS:\"\n\n"
        "    2.  the nvme_stats statistics monitor will be copied to "
        "\"C:\"\n\n"
        "No system restart is required: the driver loads on first "
        "use and NVMe namespaces are mounted automatically.\n\n\n"
        "Press \"Next\" to continue."),
    LocaleString(
        "MSG_FINISH",
        "\nThe installation completed successfully.\n\n"
        "nvme.device has been copied to \"DEVS:\" and nvme_stats to "
        "\"C:\".  The driver loads on first use; NVMe namespaces are "
        "discovered and mounted automatically.\n\n"
        "OPTIONAL -- to BOOT from an NVMe disk, the driver must load "
        "as a Kickstart module instead:\n\n"
        "    1.  copy DEVS:nvme.device SYS:Kickstart/nvme.device\n\n"
        "    2.  add this line to the Kicklayout in use:\n\n"
        "            MODULE Kickstart/nvme.device\n\n"
        "    3.  add this line to Kickstart/diskboot.config (see "
        "diskboot.config.sample in this drawer):\n\n"
        "            nvme.device 1 1\n\n"
        "    4.  restart the system.\n\n"
        "QEMU users: place nvme.device inside kickstart.zip's "
        "Kickstart/ drawer instead -- see the project "
        "documentation.\n\n\n"
        "Press \"Finish\" to exit the installation."),
]


welcome_page = Page(
    var_name="welcomePage",
    kind=PageKind.WELCOME,
    strings={"message": LocaleRef("MSG_WELCOME")},
)

install_page = Page(
    var_name="installPage",
    kind=PageKind.INSTALL,
)

finish_page = Page(
    var_name="finishPage",
    kind=PageKind.FINISH,
    strings={"message": LocaleRef("MSG_FINISH")},
)


driver_package = Package(
    name="NVMe device driver",
    files=["content/nvme.device"],
    kind=PackageKind.FILEPACKAGE,
    # Fixed destinations: the runtime driver belongs in DEVS: and the
    # CLI tool in C: regardless of any user preference, so there is no
    # DESTINATION page.  Emitted verbatim, hence the embedded quotes.
    alternatepath="\"DEVS:\"",
    register_in="top",
)

stats_package = Package(
    name="nvme_stats statistics monitor",
    files=["content/nvme_stats"],
    kind=PackageKind.FILEPACKAGE,
    alternatepath="\"C:\"",
    register_in="top",
)


project = Project(
    name="NVMe Device Driver Install",
    short_name="NVMe",
    version="1.68",
    date="11.06.2026",
    locale_strings=locale,
    pages=[welcome_page, install_page, finish_page],
    packages=[driver_package, stats_package],
)
