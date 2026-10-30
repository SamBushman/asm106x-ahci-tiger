/*
 * AsmAHCIRoot.cpp - HBA-level bring-up (libahci.c: ahci_enable_ahci / ahci_reset_controller / ahci_init_controller).
 * GPL-2.0-or-later.
 */
#include "AsmAHCI.h"

#define super IOService
OSDefineMetaClassAndStructors(ASM106xAHCI, IOService)

IOWorkLoop *ASM106xAHCI::getWorkLoop() const { return _wl; }

extern "C" task_t current_task(void);

/* IORegistryEntrySetCFProperties() from a root process: {ReadOnly = true/false} switches write protection live */
IOReturn ASM106xAHCI::setProperties(OSObject *properties)
{
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    if (!d) return kIOReturnBadArgument;
    if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    OSBoolean *ro = OSDynamicCast(OSBoolean, d->getObject("ReadOnly"));
    if (ro) {
        for (int i = 0; i < 32; i++) if (_ports[i]) _ports[i]->setReadOnly(ro == kOSBooleanTrue);
        setProperty("ReadOnly", ro);
        IOLog("ASM106x: ReadOnly is now %s\n", ro == kOSBooleanTrue ? "ON (writes refused)" : "OFF (writes allowed)");
    }
    OSString *inj = OSDynamicCast(OSString, d->getObject("Inject"));
    if (inj) {
        UInt32 what = inj->isEqualTo("idnf") ? 1 : inj->isEqualTo("timeout") ? 2 : 0;
        for (int i = 0; i < 32; i++) if (_ports[i]) _ports[i]->setInject(what);
        IOLog("ASM106x: fault injection armed: %s\n", what == 1 ? "bad LBA on next read" : what == 2 ? "swallow next command" : "none");
    }
    return kIOReturnSuccess;
}

bool ASM106xAHCI::resetHBA()
{
    UInt32 tmp;
    int i;

    /* ahci_enable_ahci(): must be in AHCI mode before touching anything AHCI-specific */
    for (i = 0; i < 5; i++) {
        tmp = rd(HOST_CTL);
        if (tmp & HOST_AHCI_EN) break;
        wr(HOST_CTL, tmp | HOST_AHCI_EN);
        IOSleep(10);
    }
    if (!(rd(HOST_CTL) & HOST_AHCI_EN)) { IOLog("ASM106x: cannot enable AHCI mode\n"); return false; }

    /* global controller reset; must complete within 1 second */
    tmp = rd(HOST_CTL);
    if (!(tmp & HOST_RESET)) wr(HOST_CTL, tmp | HOST_RESET);
    for (i = 0; i < 1000; i++) {
        if (!(rd(HOST_CTL) & HOST_RESET)) break;
        IOSleep(1);
    }
    if (rd(HOST_CTL) & HOST_RESET) { IOLog("ASM106x: controller reset failed (GHC %08x)\n", (unsigned)rd(HOST_CTL)); return false; }

    for (i = 0; i < 5 && !(rd(HOST_CTL) & HOST_AHCI_EN); i++) { wr(HOST_CTL, HOST_AHCI_EN); IOSleep(10); }

    /* ahci_restore_initial_config(): registers that may have been cleared by the reset */
    if (rd(HOST_PORTS_IMPL) != _portMap) {
        IOLog("ASM106x: PI changed by reset (%08x -> %08x), restoring\n", (unsigned)rd(HOST_PORTS_IMPL), (unsigned)_portMap);
        wr(HOST_PORTS_IMPL, _portMap);
    }
    return true;
}

bool ASM106xAHCI::start(IOService *provider)
{
    UInt32 portMask = 0xffffffff;
    OSNumber *pm;
    bool readOnly = true;
    OSBoolean *ro;

    for (int i = 0; i < 32; i++) _ports[i] = 0;
    if (!super::start(provider)) return false;
    _pci = OSDynamicCast(IOPCIDevice, provider);
    if (!_pci) return false;

    if ((pm = OSDynamicCast(OSNumber, getProperty("PortMask"))) != 0) portMask = pm->unsigned32BitValue();

    if ((ro = OSDynamicCast(OSBoolean, getProperty("ReadOnly"))) != 0) readOnly = (ro == kOSBooleanTrue);
    if (readOnly) IOLog("ASM106x: ReadOnly mode: data-out ATA commands are refused\n");

    _pci->setMemoryEnable(true);
    _pci->setBusMasterEnable(true);
    _pci->configWrite16(kIOPCIConfigCommand, _pci->configRead16(kIOPCIConfigCommand) & ~(1u << 10)); /* INTx enabled */

    _abar = _pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    if (!_abar) { IOLog("ASM106x: cannot map ABAR\n"); return false; }
    _mmio = (volatile UInt8 *)_abar->getVirtualAddress();

    _cap = rd(HOST_CAP);
    _portMap = rd(HOST_PORTS_IMPL);
    IOLog("ASM106x: AHCI %x.%x%x CAP %08x PI %08x, usable port mask %08x\n",
          (unsigned)(rd(HOST_VERSION) >> 16), (unsigned)((rd(HOST_VERSION) >> 8) & 0xff), (unsigned)(rd(HOST_VERSION) & 0xff),
          (unsigned)_cap, (unsigned)_portMap, (unsigned)portMask);

    _wl = IOWorkLoop::workLoop();
    if (!_wl) return false;

    if (!resetHBA()) return false;

    /* quiesce every implemented port (BIOS/OF may have left them running) before we own them */
    _irq = IOInterruptEventSource::interruptEventSource(this, (IOInterruptEventAction)&ASM106xAHCI::interruptAction, _pci, 0);
    if (!_irq || _wl->addEventSource(_irq) != kIOReturnSuccess) { IOLog("ASM106x: interrupt source failed\n"); return false; }
    _irq->enable();

    wr(HOST_IRQ_STAT, rd(HOST_IRQ_STAT));
    wr(HOST_CTL, rd(HOST_CTL) | HOST_IRQ_EN);

    int found = 0;
    for (UInt32 p = 0; p < 32; p++) {
        if (!(_portMap & portMask & (1u << p))) continue;
        ASM106xAHCIPort *port = new ASM106xAHCIPort;
        if (!port) continue;
        if (!port->init((OSDictionary *)0) || !port->configure(this, p, readOnly)) { port->release(); continue; }
        if (!port->attach(this)) { port->release(); continue; }
        _ports[p] = port;                       /* before start(): interrupts may arrive during device identify */
        if (!port->start(this)) {
            IOLog("ASM106x: port %u: nothing usable attached\n", (unsigned)p);
            _ports[p] = 0;
            port->stopPort();
            port->detach(this);
            port->release();
            continue;
        }
        found++;
    }
    IOLog("ASM106x: %d port(s) started\n", found);
    {
        static IOPMPowerState pmStates[2] = {
            { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
            { 1, IOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
        };
        PMinit();
        _pci->joinPMtree(this);
        registerPowerDriver(this, pmStates, 2);
    }
    registerService();
    return true;                                /* stay loaded so a later rescan/hotplug feature has a home */
}

void ASM106xAHCI::stop(IOService *provider)
{
    for (int i = 0; i < 32; i++) if (_ports[i]) _ports[i]->stopPort();
    if (_mmio) wr(HOST_CTL, rd(HOST_CTL) & ~HOST_IRQ_EN);
    if (_irq && _wl) { _irq->disable(); _wl->removeEventSource(_irq); _irq->release(); _irq = 0; }
    super::stop(provider);
}

void ASM106xAHCI::interruptAction(OSObject *owner, IOInterruptEventSource *, int)
{
    ((ASM106xAHCI *)owner)->handleInterrupt();
}

void ASM106xAHCI::handleInterrupt()
{
    UInt32 is = rd(HOST_IRQ_STAT);
    if (!is) return;
    for (UInt32 p = 0; p < 32; p++)
        if ((is & (1u << p)) && _ports[p]) _ports[p]->serviceInterrupt();
    wr(HOST_IRQ_STAT, is);                      /* after the per-port status has been cleared */
}

/* System sleep: stop the engines; wake: the card may have lost power, so redo the HBA reset and re-link every port. */
IOReturn ASM106xAHCI::setPowerState(unsigned long state, IOService *)
{
    if (!_mmio) return IOPMAckImplied;
    if (state == 0 && !_asleep) {
        _asleep = true;
        for (int i = 0; i < 32; i++) if (_ports[i]) _ports[i]->quiesceForSleep();
        wr(HOST_CTL, rd(HOST_CTL) & ~HOST_IRQ_EN);
        IOLog("ASM106x: sleeping\n");
    } else if (state == 1 && _asleep) {
        _asleep = false;
        IOLog("ASM106x: waking, GHC %08x\n", (unsigned)rd(HOST_CTL));
        if (!resetHBA()) { IOLog("ASM106x: HBA reset failed on wake\n"); return IOPMAckImplied; }
        wr(HOST_IRQ_STAT, rd(HOST_IRQ_STAT));
        wr(HOST_CTL, rd(HOST_CTL) | HOST_IRQ_EN);
        for (int i = 0; i < 32; i++) if (_ports[i]) _ports[i]->resumeFromSleep();
        IOLog("ASM106x: wake complete\n");
    }
    return IOPMAckImplied;
}
