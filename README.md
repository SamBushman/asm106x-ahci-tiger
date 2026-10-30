# ASM106xAHCI — ASMedia ASM106x SATA (AHCI) driver for Mac OS X 10.4 Tiger (PowerPC)

Tiger has no AHCI driver. This kext adds one for the ASMedia ASM1060/1061/1062 family by porting the
HBA/port logic of the Linux AHCI driver (`drivers/ata/ahci.c`, `libahci.c`, v6.6) onto Apple's own
**IOATAFamily**: each AHCI port becomes an `IOATAController`, so Tiger's stock `IOATABlockStorageDriver`,
Disk Utility, `fsck_hfs` etc. work on top of it unchanged. Developed and tested on a Power Mac G5 (10.4.11)
with an SU-SA3002.V1 card (ASM1062, PCI `1b21:0625`) and a WD 750 GB drive.

License: GPL-2.0-or-later (derived from Linux). See `LICENSE`.

## Status — what has actually been tested (Power Mac G5, Tiger 10.4.11)

| Area | Result |
|---|---|
| HBA reset, port spin-up, COMRESET, link at 3 Gbps, IDENTIFY | works |
| Reads (incl. LBA48, >128 GiB) | 407 raw checksums identical to the same drive on another controller |
| Writes | files 1 KB–512 MB verified from disk after remount, `fsck_hfs` clean, ~115 MB/s |
| Throughput | 125–135 MB/s sequential (drive-limited), ~14k IOPS at 512 B |
| Device error (ID-not-found) / command timeout | injected on real hardware; recovery works |
| 4 parallel readers + 2 parallel writers | consistent, `fsck` clean |
| Suspend/resume of the controller (see below) | 5 cycles mounted + write across a sleep: all data intact |

**Not implemented / not tested:** hotplug (a drive must be attached when the driver loads), SATA port
multipliers, NCQ (IOATAFamily issues one command at a time, so there is a single command slot), ATAPI/optical
drives (code path present, never exercised), MSI. Bootable-from-this-card is not possible (no Open Firmware driver).

System sleep itself was not tested: on the test machine the ATI X1900 does not survive sleep. The power-management
callback is exercised through `asmctl sleep|wake`, which runs the identical code.

## Install

```sh
sudo cp -R ASM106xAHCI.kext /System/Library/Extensions/
sudo chown -R root:wheel /System/Library/Extensions/ASM106xAHCI.kext
sudo chmod -R go-w /System/Library/Extensions/ASM106xAHCI.kext
sudo touch /System/Library/Extensions     # then reboot (kextd rebuilds its caches); or: sudo kextload <path>
```

`/Library/Extensions` does **not** work on Tiger: `kextd(8)` states that by default it only uses kexts in
`/System/Library/Extensions`, so a kext placed in `/Library/Extensions` is never matched or loaded.

The kext deliberately has no `OSBundleRequired`, so it is **not** part of the boot-critical set; `kextd` loads it
after boot when it sees the card (about a minute after power-on on the test machine). To remove it (e.g. from
single-user mode or another OS): delete `/System/Library/Extensions/ASM106xAHCI.kext` and `touch` the folder.

## Build (on a Tiger PPC machine, Apple gcc 4.0.1)

```sh
sh build.sh driver ASM106xAHCI      # -> $HOME/asm1062-kext/out/ASM106xAHCI/ASM106xAHCI.kext (override with OUT=)
gcc -o tools/asmctl tools/asmctl.c -framework IOKit -framework CoreFoundation
```

Needs the Tiger Kernel.framework headers (Xcode 2.x). Note: the headers shipped with Xcode declare `IODMACommand`
and `IOATABusCommand64`, which the Tiger 10.4.11 kernel and IOATAFamily binary do **not** contain — this driver
uses `IONaturalMemoryCursor` like Apple's `IOPCIATA`.

## Configuration (`Info.plist` personality)

* `IONameMatch` — ASM1060/1061/1062 PCI IDs (`pci1b21,601 … 625`).
* `PortMask` (default 3) — the ASM106x reports 12 ports in CAP/PI but boards have 2.
* `ReadOnly` (default false) — when true, all data-out ATA commands are refused (bring-up safety).

## `asmctl` (run as root)

```
asmctl ro | rw                 # write-protect / allow writes, live
asmctl sleep | wake            # run the power-management quiesce/resume path
asmctl inject idnf | timeout   # arm a one-shot fault: bad LBA on next read / swallow next command
```

A loaded driver cannot be unloaded while a disk is mounted on it (the ATA stack holds it), so these knobs exist
to avoid reboots while testing.

## Design notes

* `AsmAHCIRoot.cpp` — `ASM106xAHCI : IOService` on the `IOPCIDevice`: maps ABAR (BAR5), resets the HBA
  (`ahci_reset_controller`), owns the work loop and the interrupt, creates a port object per live link, registers
  with power management.
* `AsmAHCIPort.cpp` — `ASM106xAHCIPort : IOATAController`: command list / RX FIS / command table in one contiguous
  DMA block, `ahci_port_start`/`ahci_hardreset`/`ahci_error_intr` logic, taskfile → H2D FIS (incl. 48-bit LBA),
  PRD table from `IONaturalMemoryCursor`, polled completion for commands issued from reset callouts.
* AHCI structures are little-endian and the host is big-endian: MMIO uses `OSReadLittleInt32`, tables are written
  little-endian. IDENTIFY data arrives in wire order, which is what `ATADeviceNub` expects (it swaps the buffer itself).
* PIO-in commands (IDENTIFY) complete with a PIO Setup FIS, not a D2H FIS — the interrupt mask must include `PIOS`.
* Sleep: freeze dispatch → drain the in-flight command → stop the engine; wake: HBA reset, re-link,
  `kATAResetEvent` callout, release the held queue. (Aborting the in-flight command instead makes HFS+ lose a
  journal write.)

## Tests (`tests/`, written for the author's setup — read before running)

`sums.sh` (raw checksum set, compare two runs), `wtest.sh` (write/verify), `stress.sh`, `pmtest.sh`. They write to a
mounted volume and read raw devices as root.

`probe/` is a read-only kext that dumps PCI config space and AHCI registers.

## Credits

Logic from the Linux kernel AHCI driver (© its authors, GPL-2.0-or-later); structure from Apple's open-source
IOATAFamily (IOATAFamily-160.3.2 matches Tiger 10.4.11).
