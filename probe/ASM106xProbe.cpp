/*
 * ASM106xProbe.cpp - read-only probe of an ASMedia ASM106x AHCI controller on Tiger PPC.
 *
 * Maps ABAR (BAR5), dumps PCI config space and the AHCI HBA + per-port registers to the system log.
 * Never writes any HBA/port register (only the PCI command register's memory-enable bit if OF left
 * it clear).  GPL-2.0-or-later (the follow-on driver is derived from Linux libahci/ahci).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSByteOrder.h>

class ASM106xProbe : public IOService {
    OSDeclareDefaultStructors(ASM106xProbe)
public:
    virtual bool start(IOService *provider);
};

#define super IOService
OSDefineMetaClassAndStructors(ASM106xProbe, IOService)

static inline UInt32 rd(volatile UInt8 *base, UInt32 off) { return OSReadLittleInt32((volatile void *)base, off); }

bool ASM106xProbe::start(IOService *provider)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!super::start(provider) || !pci) return false;

    IOLog("ASM106xProbe: start\n");
    UInt16 cmd = pci->configRead16(kIOPCIConfigCommand);
    IOLog("ASM106xProbe: vid:did %04x:%04x subsys %04x:%04x rev %02x class %06x cmd %04x status %04x\n",
          pci->configRead16(0x00), pci->configRead16(0x02), pci->configRead16(0x2c), pci->configRead16(0x2e),
          pci->configRead8(0x08), (unsigned)(pci->configRead32(0x08) >> 8), cmd, pci->configRead16(0x06));
    for (UInt32 o = 0; o < 0x100; o += 16)
        IOLog("ASM106xProbe: cfg[%02x] %08x %08x %08x %08x\n", (unsigned)o, (unsigned)pci->configRead32(o),
              (unsigned)pci->configRead32(o + 4), (unsigned)pci->configRead32(o + 8), (unsigned)pci->configRead32(o + 12));
    for (int b = 0; b < 6; b++)
        IOLog("ASM106xProbe: BAR%d = %08x\n", b, (unsigned)pci->configRead32(0x10 + 4 * b));

    if (!(cmd & kIOPCICommandMemorySpace)) { IOLog("ASM106xProbe: enabling memory space\n"); pci->setMemoryEnable(true); }

    IOMemoryMap *map = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    if (!map) { IOLog("ASM106xProbe: BAR5 map failed\n"); return true; }
    volatile UInt8 *h = (volatile UInt8 *)map->getVirtualAddress();
    IOLog("ASM106xProbe: ABAR phys %08x len %u virt %p\n", (unsigned)map->getPhysicalAddress(), (unsigned)map->getLength(), h);

    UInt32 cap = rd(h, 0x00), ghc = rd(h, 0x04), pi = rd(h, 0x0c), vs = rd(h, 0x10);
    IOLog("ASM106xProbe: CAP %08x GHC %08x IS %08x PI %08x VS %08x CCC_CTL %08x CCC_PORTS %08x EM_LOC %08x EM_CTL %08x CAP2 %08x BOHC %08x\n",
          (unsigned)cap, (unsigned)ghc, (unsigned)rd(h, 0x08), (unsigned)pi, (unsigned)vs, (unsigned)rd(h, 0x14), (unsigned)rd(h, 0x18),
          (unsigned)rd(h, 0x1c), (unsigned)rd(h, 0x20), (unsigned)rd(h, 0x24), (unsigned)rd(h, 0x28));
    IOLog("ASM106xProbe: NP=%u NCS=%u S64A=%d SNCQ=%d SSS=%d SALP=%d SAL=%d SCLO=%d ISS=%u SPM=%d PMD=%d SSC=%d PSC=%d CCCS=%d EMS=%d SXS=%d\n",
          (unsigned)(cap & 0x1f) + 1, (unsigned)((cap >> 8) & 0x1f) + 1, (cap >> 31) & 1, (cap >> 30) & 1, (cap >> 27) & 1, (cap >> 26) & 1,
          (cap >> 25) & 1, (cap >> 24) & 1, (unsigned)((cap >> 20) & 0xf), (cap >> 17) & 1, (cap >> 15) & 1, (cap >> 14) & 1, (cap >> 13) & 1,
          (cap >> 7) & 1, (cap >> 6) & 1, (cap >> 5) & 1);

    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;
        UInt32 o = 0x100 + p * 0x80;
        if (o + 0x80 > map->getLength()) { IOLog("ASM106xProbe: port %d beyond ABAR length\n", p); break; }
        UInt32 ssts = rd(h, o + 0x28);
        IOLog("ASM106xProbe: P%d CLB %08x CLBU %08x FB %08x FBU %08x IS %08x IE %08x CMD %08x TFD %08x SIG %08x SSTS %08x SCTL %08x SERR %08x SACT %08x CI %08x SNTF %08x FBS %08x\n",
              p, (unsigned)rd(h, o), (unsigned)rd(h, o + 4), (unsigned)rd(h, o + 8), (unsigned)rd(h, o + 12), (unsigned)rd(h, o + 0x10),
              (unsigned)rd(h, o + 0x14), (unsigned)rd(h, o + 0x18), (unsigned)rd(h, o + 0x20), (unsigned)rd(h, o + 0x24), (unsigned)ssts,
              (unsigned)rd(h, o + 0x2c), (unsigned)rd(h, o + 0x30), (unsigned)rd(h, o + 0x34), (unsigned)rd(h, o + 0x38),
              (unsigned)rd(h, o + 0x3c), (unsigned)rd(h, o + 0x40));
        IOLog("ASM106xProbe: P%d link: DET=%u SPD=%u IPM=%u  (DET 3 = device present + PHY up)\n", p, (unsigned)(ssts & 0xf),
              (unsigned)((ssts >> 4) & 0xf), (unsigned)((ssts >> 8) & 0xf));
    }
    map->release();
    IOLog("ASM106xProbe: done\n");
    return true;
}
