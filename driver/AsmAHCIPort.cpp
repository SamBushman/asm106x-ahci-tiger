/*
 * AsmAHCIPort.cpp - one AHCI port exposed as an IOATAController (single device, unit 0).
 *
 * Port bring-up, COMRESET, error recovery and FIS construction follow Linux libahci.c
 * (ahci_port_start/ahci_hardreset/ahci_error_intr/ahci_qc_prep, GPL-2.0-or-later).  The command side
 * maps IOATAFamily's taskfile commands onto a single AHCI command slot (no NCQ: IOATAFamily
 * serialises commands per bus).  AHCI structures are little-endian; this is a big-endian host.
 */
#include "AsmAHCI.h"

#define super IOATAController
OSDefineMetaClassAndStructors(ASM106xAHCIPort, IOATAController)

#ifdef ASM_DEBUG
#define DBG(fmt, args...) IOLog("ASM106x: " fmt, ## args)
#else
#define DBG(fmt, args...)
#endif

static inline void dmaSync() { __asm__ volatile("sync" ::: "memory"); }

bool ASM106xAHCIPort::configure(ASM106xAHCI *hba, UInt32 portNo, bool readOnly)
{
    _readOnly = readOnly;
    _hba = hba;
    _no = portNo;
    _pm = hba->portBase(portNo);
    _pio = 0x10; _dmaMode = 0x04; _ultra = 0x40;
    _pktCfg = kATAPIDRQFast;
    return true;
}

bool ASM106xAHCIPort::waitReg(UInt32 off, UInt32 mask, UInt32 val, UInt32 timeoutMs)
{
    for (UInt32 i = 0; i <= timeoutMs; i++) {
        if ((rd(off) & mask) == val) return true;
        IOSleep(1);
    }
    return (rd(off) & mask) == val;
}

bool ASM106xAHCIPort::allocDma()
{
    if (_dma) return true;
    _dma = (UInt8 *)IOMallocContiguous(kDmaBlockSize, 4096, &_dmaPhys);
    if (!_dma) { IOLog("ASM106x: port %u: DMA block allocation failed\n", (unsigned)_no); return false; }
    bzero(_dma, kDmaBlockSize);
    return true;
}

/* ahci_stop_engine */
bool ASM106xAHCIPort::stopEngine()
{
    UInt32 t = rd(PORT_CMD);
    if (!(t & (PORT_CMD_START | PORT_CMD_LIST_ON))) return true;
    if (t == 0xffffffff) return false;
    wr(PORT_CMD, t & ~PORT_CMD_START);
    return waitReg(PORT_CMD, PORT_CMD_LIST_ON, 0, 500);
}

/* ahci_stop_fis_rx */
bool ASM106xAHCIPort::stopFisRx()
{
    wr(PORT_CMD, rd(PORT_CMD) & ~PORT_CMD_FIS_RX);
    return waitReg(PORT_CMD, PORT_CMD_FIS_ON, 0, 1000);
}

/* ahci_start_fis_rx */
void ASM106xAHCIPort::startFisRx()
{
    wr(PORT_LST_ADDR, (UInt32)_dmaPhys + kCmdListOff);
    wr(PORT_LST_ADDR_HI, 0);
    wr(PORT_FIS_ADDR, (UInt32)_dmaPhys + kRxFisOff);
    wr(PORT_FIS_ADDR_HI, 0);
    wr(PORT_CMD, rd(PORT_CMD) | PORT_CMD_FIS_RX);
    (void)rd(PORT_CMD);
}

void ASM106xAHCIPort::startEngine()
{
    wr(PORT_CMD, rd(PORT_CMD) | PORT_CMD_START);
    (void)rd(PORT_CMD);
}

/*
 * sata_link_hardreset + wait-ready: DET=1 for >=1ms, then DET=0, wait for a stable PHY link (DET=3)
 * and for BSY to clear.  Engine must be stopped, FIS receive running.  IPM is set to 3 (no
 * partial/slumber) throughout.
 */
bool ASM106xAHCIPort::comReset()
{
    UInt32 ctl = rd(PORT_SCR_CTL) & ~0xf0f;
    wr(PORT_SCR_CTL, ctl | 0x301);
    IOSleep(2);
    wr(PORT_SCR_CTL, ctl | 0x300);

    UInt32 stable = 0, waited = 0;
    while (waited < 2000) {
        if ((rd(PORT_SCR_STAT) & 0xf) == 3) { stable += 10; if (stable >= 100) break; }
        else stable = 0;
        IOSleep(10);
        waited += 10;
    }
    if (stable < 100) { DBG("port %u: no link after COMRESET (SSTS %08x)\n", (unsigned)_no, (unsigned)rd(PORT_SCR_STAT)); return false; }

    wr(PORT_SCR_ERR, rd(PORT_SCR_ERR));
    if (!waitReg(PORT_TFDATA, ATA_BUSY | ATA_DRQ, 0, 10000)) {
        IOLog("ASM106x: port %u: device busy after reset (TFD %08x)\n", (unsigned)_no, (unsigned)rd(PORT_TFDATA));
        return false;
    }
    wr(PORT_IRQ_STAT, rd(PORT_IRQ_STAT));
    return true;
}

/* ahci_port_start / ahci_power_up / initial hardreset */
bool ASM106xAHCIPort::bringUp()
{
    if (!allocDma()) return false;
    stopEngine();
    stopFisRx();
    wr(PORT_IRQ_MASK, 0);
    wr(PORT_IRQ_STAT, rd(PORT_IRQ_STAT));
    wr(PORT_SCR_ERR, rd(PORT_SCR_ERR));

    UInt32 cmd = rd(PORT_CMD) & ~(0xfu << 28);
    if (_hba->capabilities() & CAP_SSS) cmd |= PORT_CMD_SPIN_UP;
    wr(PORT_CMD, cmd | PORT_CMD_ICC_ACTIVE);

    startFisRx();
    _linkUp = comReset();
    if (_linkUp) {
        _sig = rd(PORT_SIG);
        startEngine();
    }
    IOLog("ASM106x: port %u: link %s, SSTS %08x SCTL %08x SERR %08x CMD %08x TFD %08x SIG %08x\n", (unsigned)_no, _linkUp ? "up" : "down",
          (unsigned)rd(PORT_SCR_STAT), (unsigned)rd(PORT_SCR_CTL), (unsigned)rd(PORT_SCR_ERR), (unsigned)rd(PORT_CMD),
          (unsigned)rd(PORT_TFDATA), (unsigned)_sig);
    return _linkUp;
}

/* ahci_error_intr / EH: recover the port, soft (CLO) when possible, otherwise COMRESET */
void ASM106xAHCIPort::recover(bool hard)
{
    stopEngine();
    wr(PORT_SCR_ERR, rd(PORT_SCR_ERR));
    if (!hard && (rd(PORT_TFDATA) & (ATA_BUSY | ATA_DRQ))) {
        if (_hba->capabilities() & CAP_SCLO) {
            wr(PORT_CMD, rd(PORT_CMD) | PORT_CMD_CLO);
            if (!waitReg(PORT_CMD, PORT_CMD_CLO, 0, 500)) hard = true;
        } else hard = true;
    }
    if (hard) _linkUp = comReset();
    wr(PORT_IRQ_STAT, rd(PORT_IRQ_STAT));
    if (_linkUp) startEngine();
}

void ASM106xAHCIPort::stopPort()
{
    if (!_pm) return;
    wr(PORT_IRQ_MASK, 0);
    stopEngine();
    stopFisRx();
}

bool ASM106xAHCIPort::start(IOService *provider)
{
    if (!bringUp()) return false;
    if (_sig != SIG_ATA && _sig != SIG_ATAPI) {
        IOLog("ASM106x: port %u: unsupported device signature %08x (port multiplier?)\n", (unsigned)_no, (unsigned)_sig);
        return false;
    }
    _cursor = IONaturalMemoryCursor::withSpecification(0x100000, 0x100000);
    if (!_cursor) return false;

    if (!super::start(provider)) return false;

    setProperty("ASM106x Port", (UInt64)_no, 32);
    wr(PORT_IRQ_STAT, rd(PORT_IRQ_STAT));
    wr(PORT_IRQ_MASK, DEF_PORT_IRQ);

    _nub[0] = ATADeviceNub::ataDeviceNub((IOATAController *)this, kATADevice0DeviceID, _devInfo[0].type);
    if (!_nub[0]) {
        IOLog("ASM106x: port %u: device identify failed\n", (unsigned)_no);
        return false;
    }
    _nub[0]->attach(this);
    _nub[0]->registerService();
    IOLog("ASM106x: port %u: %s attached\n", (unsigned)_no, _devInfo[0].type == kATAPIDeviceType ? "ATAPI device" : "ATA disk");
    return true;
}

void ASM106xAHCIPort::free()
{
    stopPort();
    if (_cursor) { _cursor->release(); _cursor = 0; }
    if (_dma) { IOFreeContiguous(_dma, kDmaBlockSize); _dma = 0; }
    super::free();
}

/* ---- IOATAController glue ---- */

bool ASM106xAHCIPort::configureTFPointers(void)
{
    /* No legacy taskfile registers exist under AHCI.  Point every register at scratch storage so that any
     * base-class path we failed to override reads/writes harmlessly instead of faulting. */
    _tfFeatureReg = _tfSCountReg = _tfSectorNReg = _tfCylLoReg = _tfCylHiReg = _tfSDHReg = _tfStatusCmdReg = _tfAltSDevCReg = &_dummy[0];
    _tfDataReg = (IOATARegPtr16)&_dummy[2];
    return true;
}

bool ASM106xAHCIPort::allocateDoubleBuffer(void) { return true; }

UInt32 ASM106xAHCIPort::scanForDrives(void)
{
    _devInfo[0].type = kUnknownATADeviceType;
    _devInfo[1].type = kUnknownATADeviceType;
    _devInfo[0].packetSend = _devInfo[1].packetSend = kATAPIUnknown;
    if (!_linkUp) return 0;
    if (_sig == SIG_ATAPI) { _devInfo[0].type = kATAPIDeviceType; _devInfo[0].packetSend = kATAPIDRQFast; return 1; }
    if (_sig == SIG_ATA)   { _devInfo[0].type = kATADeviceType; return 1; }
    return 0;
}

IOReturn ASM106xAHCIPort::provideBusInfo(IOATABusInfo *info)
{
    if (!info) return -1;
    info->zeroData();
    info->setSocketType(kInternalSATA);
    info->setPIOModes(0x1f);
    info->setDMAModes(0x07);
    info->setUltraModes(0x7f);
    info->setExtendedLBA(true);
    info->setMaxBlocksExtended(kMaxBlocks);
    info->setUnits(1);
    info->setDMAQueued(false);
    info->setOverlapped(false);
    return kATANoErr;
}

IOReturn ASM106xAHCIPort::selectConfig(IOATADevConfig *cfg, UInt32 unit)
{
    if (unit != 0 || !cfg) return kATAInvalidDevID;
    _pio = cfg->getPIOMode(); _dmaMode = cfg->getDMAMode(); _ultra = cfg->getUltraMode(); _pktCfg = cfg->getPacketConfig();
    return kATANoErr;       /* the SATA link speed is negotiated by the PHY; nothing to program */
}

IOReturn ASM106xAHCIPort::getConfig(IOATADevConfig *cfg, UInt32 unit)
{
    if (unit != 0 || !cfg) return kATAInvalidDevID;
    cfg->setPIOMode(_pio); cfg->setDMAMode(_dmaMode); cfg->setUltraMode(_ultra); cfg->setPacketConfig(_pktCfg);
    return kATANoErr;
}

IOReturn ASM106xAHCIPort::selectDevice(ataUnitID unit) { return unit == kATADevice0DeviceID ? kATANoErr : kATAInvalidDevID; }
IOReturn ASM106xAHCIPort::startDMA(void) { return kATANoErr; }
IOReturn ASM106xAHCIPort::stopDMA(void) { return kATANoErr; }
void ASM106xAHCIPort::selectIOTiming(ataUnitID) {}
IOReturn ASM106xAHCIPort::handleQueueFlush(void) { return kATANoErr; }
IOReturn ASM106xAHCIPort::handleDeviceInterrupt(void) { serviceInterrupt(); return kATANoErr; }

/* ---- command issue ---- */

IOReturn ASM106xAHCIPort::issueCurrent()
{
    IOATABusCommand *c = _currentCommand;
    UInt32 flags = c->getFlags();
    volatile UInt8 *tbl = _dma + kCmdTableOff;
    UInt8 *fis = (UInt8 *)tbl;
    UInt32 *hdr = (UInt32 *)(_dma + kCmdListOff);

    bzero((void *)tbl, kPrdtOff);
    fis[0] = 0x27;                              /* Register - Host to Device */
    fis[1] = 0x80;                              /* C bit: command register update */
    if (flags & mATAFlag48BitLBA) {
        IOExtendedLBA *e = c->getExtendedLBA();
        UInt16 f = e->getFeatures16(), cnt = e->getSectorCount16(), lo = e->getLBALow16(), mid = e->getLBAMid16(), hi = e->getLBAHigh16();
        fis[2] = e->getCommand();
        fis[3] = f & 0xff;  fis[11] = f >> 8;
        fis[4] = lo & 0xff; fis[8] = lo >> 8;
        fis[5] = mid & 0xff; fis[9] = mid >> 8;
        fis[6] = hi & 0xff; fis[10] = hi >> 8;
        fis[7] = e->getDevice();
        fis[12] = cnt & 0xff; fis[13] = cnt >> 8;
    } else {
        ataTaskFile *tf = c->getTaskFilePtr();
        fis[2] = tf->ataTFCommand;
        fis[3] = tf->ataTFFeatures;
        fis[4] = tf->ataTFSector;
        fis[5] = tf->ataTFCylLo;
        fis[6] = tf->ataTFCylHigh;
        fis[7] = tf->ataTFSDH;
        fis[12] = tf->ataTFCount;
    }

    UInt32 hdr0 = 5;                            /* CFL: 5 dwords */
    if (flags & mATAFlagIOWrite) hdr0 |= 1u << 6;   /* W */
    if ((flags & mATAFlagProtocolATAPI) && c->getPacketSize() > 0) {
        hdr0 |= 1u << 5;                        /* A */
        UInt32 n = c->getPacketSize() > 12 ? 16 : 12;
        bzero((void *)(tbl + 0x40), 16);
        bcopy(c->getPacketData(), (void *)(tbl + 0x40), n);
    }

    /* PRDT */
    UInt32 nprd = 0;
    IOMemoryDescriptor *d = c->getBuffer();
    IOByteCount remaining = c->getByteCount();
    if (d && remaining) {
        IOByteCount pos = c->getPosition();
        volatile UInt32 *prd = (volatile UInt32 *)(tbl + kPrdtOff);
        IOMemoryCursor::PhysicalSegment seg[16];
        while (remaining) {
            IOByteCount got = 0;
            UInt32 n = _cursor->getPhysicalSegments(d, pos, seg, 16, remaining, &got);
            if (!n || !got) break;
            for (UInt32 i = 0; i < n; i++) {
                if (nprd >= kMaxPrd) { IOLog("ASM106x: scatter list too long\n"); return kATADMAErr; }
                OSWriteLittleInt32(prd, nprd * 16 + 0, (UInt32)seg[i].location);
                OSWriteLittleInt32(prd, nprd * 16 + 4, 0);
                OSWriteLittleInt32(prd, nprd * 16 + 8, 0);
                OSWriteLittleInt32(prd, nprd * 16 + 12, (UInt32)seg[i].length - 1);
                nprd++;
            }
            pos += got;
            remaining = remaining > got ? remaining - got : 0;
        }
        if (remaining) { IOLog("ASM106x: could not map %lu bytes of buffer\n", (unsigned long)remaining); return kATADMAErr; }
    }

    OSWriteLittleInt32(hdr, 0, hdr0 | (nprd << 16));
    OSWriteLittleInt32(hdr, 4, 0);              /* PRDBC */
    OSWriteLittleInt32(hdr, 8, (UInt32)_dmaPhys + kCmdTableOff);
    OSWriteLittleInt32(hdr, 12, 0);
    dmaSync();

    wr(PORT_SCR_ERR, rd(PORT_SCR_ERR));
    wr(PORT_CMD_ISSUE, 1);
    (void)rd(PORT_CMD_ISSUE);
    return kATANoErr;
}

IOReturn ASM106xAHCIPort::handleExecIO(void)
{
    IOATABusCommand *c = _currentCommand;
    if (!_linkUp) {
        c->state = kATAComplete;
        completeIO(kIOReturnOffline);
        return kIOReturnOffline;
    }
    if (c->getUnit() != kATADevice0DeviceID) {
        c->state = kATAComplete;
        completeIO(kATAInvalidDevID);
        return kATAInvalidDevID;
    }
    if (_readOnly && (c->getFlags() & mATAFlagIOWrite) && !(c->getFlags() & mATAFlagProtocolATAPI)) {
        IOLog("ASM106x: port %u: ReadOnly: refused write command %02x\n", (unsigned)_no, c->getTaskFilePtr()->ataTFCommand);
        c->state = kATAComplete;
        completeIO(kIOReturnNotWritable);
        return kIOReturnNotWritable;
    }
    bool polled = (c->getFlags() & mATAFlagUseNoIRQ) != 0;
    if (!polled) startTimer(c->getTimeoutMS());
    c->state = kATAStarted;

    IOReturn err = issueCurrent();
    if (err) {
        c->state = kATAComplete;
        completeIO(err);
        return err;
    }
    if (polled) {
        /* commands issued from event callouts re-enter the gate, so the interrupt can't be delivered: poll */
        UInt32 limit = c->getTimeoutMS() ? c->getTimeoutMS() : 30000;
        for (UInt32 t = 0; _currentCommand == c && t < limit; t++) {
            serviceInterrupt();
            if (_currentCommand != c) break;
            IOSleep(1);
        }
        if (_currentCommand == c) handleTimeout();
    }
    return kATANoErr;
}

/* The ATA taskfile registers don't exist on AHCI; synthesise reads from PxTFD and the last D2H FIS. */
IOReturn ASM106xAHCIPort::handleRegAccess(void)
{
    IOATABusCommand *c = _currentCommand;
    UInt32 mask = c->getRegMask(), tfd = rd(PORT_TFDATA);
    if (!(c->getFlags() & mATAFlagIOWrite)) {
        if (mask & mATAErrFeaturesValid) c->setFeatures((tfd >> 8) & 0xff);
        if (mask & mATASectorCntValid)   c->setSectorCount(_lastD2H[12]);
        if (mask & mATASectorNumValid)   c->setSectorNumber(_lastD2H[4]);
        if (mask & mATACylinderLoValid)  c->setCylLo(_lastD2H[5]);
        if (mask & mATACylinderHiValid)  c->setCylHi(_lastD2H[6]);
        if (mask & mATASDHValid)         c->setDevice_Head(_lastD2H[7]);
        if (mask & mATAAltSDevCValid)    c->setControl(tfd & 0xff);
        if (mask & mATAStatusCmdValid)   c->setCommand(tfd & 0xff);
    }
    c->state = kATAComplete;
    completeIO(kATANoErr);
    return kATANoErr;
}

IOReturn ASM106xAHCIPort::handleBusReset(void)
{
    bool atapi = (_currentCommand->getFlags() & mATAFlagProtocolATAPI) != 0;
    recover(true);
    if (atapi) executeEventCallouts(kATAPIResetEvent, _currentCommand->getUnit());
    else       executeEventCallouts(kATAResetEvent, kATAInvalidDeviceID);
    _currentCommand->state = kATAComplete;
    IOReturn r = _linkUp ? kATANoErr : kIOReturnOffline;
    completeIO(r);
    return r;
}

/* ---- completion ---- */

void ASM106xAHCIPort::fetchTaskfile(IOATABusCommand *c, bool)
{
    UInt8 *f = _lastD2H;
    if (c->getFlags() & mATAFlag48BitLBA) {
        IOExtendedLBA *e = c->getExtendedLBA();
        e->setFeatures16((UInt16)f[3]);
        e->setSectorCount16((UInt16)(f[13] << 8 | f[12]));
        e->setLBALow16((UInt16)(f[8] << 8 | f[4]));
        e->setLBAMid16((UInt16)(f[9] << 8 | f[5]));
        e->setLBAHigh16((UInt16)(f[10] << 8 | f[6]));
        e->setDevice(f[7]);
    } else {
        c->setFeatures(f[3]);
        c->setSectorCount(f[12]);
        c->setSectorNumber(f[4]);
        c->setCylLo(f[5]);
        c->setCylHi(f[6]);
        c->setDevice_Head(f[7]);
    }
}

void ASM106xAHCIPort::completeFromHW(IOReturn forced, UInt32 tfd)
{
    IOATABusCommand *c = _currentCommand;
    if (!c) return;
    UInt8 status = tfd & 0xff, error = (tfd >> 8) & 0xff;
    UInt32 prdbc = OSReadLittleInt32((volatile void *)(_dma + kCmdListOff), 4);
    bcopy((const void *)(_dma + kRxFisOff + 0x40), _lastD2H, sizeof(_lastD2H));

    IOReturn r = forced;
    if (!r && (status & (ATA_ERR | ATA_DF))) r = kATADeviceError;
    if (c->getByteCount()) c->setActualTransfer(prdbc);
    c->setEndResult(status, error);
    UInt32 flags = c->getFlags();
    if ((flags & mATAFlagTFAccessResult) || (r && (flags & mATAFlagTFAccess))) fetchTaskfile(c, r != 0);
    DBG("port %u: cmd %02x done r=%ld status %02x err %02x prdbc %lu\n", (unsigned)_no, c->getTaskFilePtr()->ataTFCommand, (long)r,
        status, error, (unsigned long)prdbc);
    c->state = kATAComplete;
    completeIO(r);
}

void ASM106xAHCIPort::serviceInterrupt()
{
    UInt32 is = rd(PORT_IRQ_STAT);
    if (!is) return;
    wr(PORT_IRQ_STAT, is);

    if (is & (PORT_IRQ_PHYRDY | PORT_IRQ_CONNECT)) {
        DBG("port %u: link event IS %08x SSTS %08x SERR %08x\n", (unsigned)_no, (unsigned)is, (unsigned)rd(PORT_SCR_STAT), (unsigned)rd(PORT_SCR_ERR));
        wr(PORT_SCR_ERR, rd(PORT_SCR_ERR));
    }
    if (!_currentCommand) {
        if (is & PORT_IRQ_ERROR) recover(false);
        return;
    }
    if (is & PORT_IRQ_ERROR) {
        UInt32 tfd = rd(PORT_TFDATA);
        bool fatal = (is & (PORT_IRQ_FATAL | PORT_IRQ_OVERFLOW | PORT_IRQ_BAD_PMP)) != 0;
        IOLog("ASM106x: port %u: error IS %08x TFD %08x SERR %08x\n", (unsigned)_no, (unsigned)is, (unsigned)tfd, (unsigned)rd(PORT_SCR_ERR));
        recover(fatal);
        completeFromHW(fatal ? kIOReturnIOError : kATADeviceError, tfd);
        return;
    }
    if (!(rd(PORT_CMD_ISSUE) & 1)) completeFromHW(kATANoErr, rd(PORT_TFDATA));
}

void ASM106xAHCIPort::handleTimeout(void)
{
    if (_currentCommand) {
        UInt32 tfd = rd(PORT_TFDATA);
        IOLog("ASM106x: port %u: command %02x timed out (TFD %08x CI %08x IS %08x)\n", (unsigned)_no,
              _currentCommand->getTaskFilePtr()->ataTFCommand, (unsigned)tfd, (unsigned)rd(PORT_CMD_ISSUE), (unsigned)rd(PORT_IRQ_STAT));
        recover(true);
        completeFromHW(kATATimeoutErr, tfd);
    } else {
        dispatchNext();
    }
}
