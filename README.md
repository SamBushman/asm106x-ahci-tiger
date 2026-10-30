# ASM106xAHCI - ASMedia ASM106x SATA (AHCI) driver for Mac OS X 10.4 Tiger PPC

Port of the Linux AHCI driver (drivers/ata/ahci.c + libahci.c, v6.6; GPL-2.0-or-later) to Apple's
IOATAFamily, for the SU-SA3002.V1 card (ASM1062, PCI 1b21:0625) in a Power Mac G5.

* `driver/AsmAHCIRoot.cpp` - HBA bring-up, one workloop + interrupt, creates one port object per live link
* `driver/AsmAHCIPort.cpp` - `IOATAController` subclass per port: AHCI command list/FIS/PRDT, COMRESET, error recovery
* `probe/` - read-only register dump kext used for hardware discovery
* `build.sh` - run ON the G5: `sh build.sh driver ASM106xAHCI` (Apple gcc 4.0.1, same recipe as the ATI kext project)

Info.plist `ReadOnly` (default true) refuses data-out ATA commands during bring-up; `PortMask` limits probed ports
(the ASM106x reports 12 ports in CAP/PI but the card has 2).

Status: loads, resets the HBA, brings both ports up (SUD/FRE/COMRESET), unloads cleanly.
Data path (IDENTIFY, DMA read/write, ATAPI) untested: needs a drive attached.
