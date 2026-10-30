/*
 * AsmAHCI.h - ASMedia ASM106x (AHCI) SATA driver for Mac OS X 10.4 Tiger PPC.
 *
 * Ported from the Linux AHCI driver (drivers/ata/ahci.c, libahci.c; GPL-2.0-or-later) onto
 * Apple's IOATAFamily: one root IOService per controller, one IOATAController subclass per port.
 */
#ifndef ASMAHCI_H
#define ASMAHCI_H

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOMemoryCursor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOUserClient.h>
#include <libkern/OSByteOrder.h>
#include <IOKit/ata/IOATATypes.h>
#include <IOKit/ata/IOATACommand.h>
#include <IOKit/ata/IOATAController.h>
#include <IOKit/ata/IOATABusCommand.h>
#include <IOKit/ata/IOATABusInfo.h>
#include <IOKit/ata/IOATADevConfig.h>
#include <IOKit/ata/ATADeviceNub.h>

/* ---- AHCI register map (ahci.h) ---- */
enum {
    HOST_CAP        = 0x00,
    HOST_CTL        = 0x04,
    HOST_IRQ_STAT   = 0x08,
    HOST_PORTS_IMPL = 0x0c,
    HOST_VERSION    = 0x10,
    HOST_CAP2       = 0x24,

    HOST_RESET      = 1u << 0,
    HOST_IRQ_EN     = 1u << 1,
    HOST_AHCI_EN    = 1u << 31,

    CAP_SSS         = 1u << 27,     /* staggered spin-up */
    CAP_SCLO        = 1u << 24,     /* command list override */
    CAP_S64A        = 1u << 31,

    PORT_BASE       = 0x100,
    PORT_STRIDE     = 0x80,

    PORT_LST_ADDR   = 0x00,
    PORT_LST_ADDR_HI= 0x04,
    PORT_FIS_ADDR   = 0x08,
    PORT_FIS_ADDR_HI= 0x0c,
    PORT_IRQ_STAT   = 0x10,
    PORT_IRQ_MASK   = 0x14,
    PORT_CMD        = 0x18,
    PORT_TFDATA     = 0x20,
    PORT_SIG        = 0x24,
    PORT_SCR_STAT   = 0x28,
    PORT_SCR_CTL    = 0x2c,
    PORT_SCR_ERR    = 0x30,
    PORT_SCR_ACT    = 0x34,
    PORT_CMD_ISSUE  = 0x38,

    PORT_IRQ_COLD_PRES   = 1u << 31,
    PORT_IRQ_TF_ERR      = 1u << 30,
    PORT_IRQ_HBUS_ERR    = 1u << 29,
    PORT_IRQ_HBUS_DATA_ERR = 1u << 28,
    PORT_IRQ_IF_ERR      = 1u << 27,
    PORT_IRQ_IF_NONFATAL = 1u << 26,
    PORT_IRQ_OVERFLOW    = 1u << 24,
    PORT_IRQ_BAD_PMP     = 1u << 23,
    PORT_IRQ_PHYRDY      = 1u << 22,
    PORT_IRQ_DEV_ILCK    = 1u << 7,
    PORT_IRQ_CONNECT     = 1u << 6,
    PORT_IRQ_SG_DONE     = 1u << 5,
    PORT_IRQ_UNK_FIS     = 1u << 4,
    PORT_IRQ_SDB_FIS     = 1u << 3,
    PORT_IRQ_DMAS_FIS    = 1u << 2,
    PORT_IRQ_PIOS_FIS    = 1u << 1,
    PORT_IRQ_D2H_REG_FIS = 1u << 0,

    PORT_IRQ_FATAL  = PORT_IRQ_HBUS_ERR | PORT_IRQ_HBUS_DATA_ERR | PORT_IRQ_IF_ERR,
    PORT_IRQ_ERROR  = PORT_IRQ_TF_ERR | PORT_IRQ_FATAL | PORT_IRQ_IF_NONFATAL | PORT_IRQ_OVERFLOW | PORT_IRQ_BAD_PMP,
    /* libahci.c DEF_PORT_IRQ: PIO-in commands complete with a PIO Setup FIS, DMA with D2H/SDB */
    DEF_PORT_IRQ    = PORT_IRQ_ERROR | PORT_IRQ_PHYRDY | PORT_IRQ_CONNECT | PORT_IRQ_SG_DONE | PORT_IRQ_UNK_FIS |
                      PORT_IRQ_SDB_FIS | PORT_IRQ_DMAS_FIS | PORT_IRQ_PIOS_FIS | PORT_IRQ_D2H_REG_FIS,

    PORT_CMD_ICC_ACTIVE = 1u << 28,
    PORT_CMD_LIST_ON    = 1u << 15,
    PORT_CMD_FIS_ON     = 1u << 14,
    PORT_CMD_FIS_RX     = 1u << 4,
    PORT_CMD_CLO        = 1u << 3,
    PORT_CMD_POWER_ON   = 1u << 2,
    PORT_CMD_SPIN_UP    = 1u << 1,
    PORT_CMD_START      = 1u << 0,

    ATA_BUSY = 0x80, ATA_DRQ = 0x08, ATA_ERR = 0x01, ATA_DF = 0x20,

    SIG_ATA   = 0x00000101,
    SIG_ATAPI = 0xEB140101,
    SIG_PMP   = 0x96690101,
    SIG_SEMB  = 0xC33C0101
};

/* ---- per-port DMA block layout (one 8K physically contiguous allocation) ---- */
enum {
    kCmdListOff   = 0x000,          /* 32 headers x 32 bytes, 1K aligned  */
    kRxFisOff     = 0x400,          /* 256 bytes, 256 aligned             */
    kCmdTableOff  = 0x800,          /* 128 aligned: CFIS(64) ACMD(16) rsvd, PRDT at +0x80 */
    kPrdtOff      = 0x80,
    kMaxPrd       = 255,
    kDmaBlockSize = 0x2000,
    kMaxBlocks    = 1024            /* sectors per command we advertise (512K) */
};

class ASM106xAHCIPort;

class ASM106xAHCI : public IOService
{
    OSDeclareDefaultStructors(ASM106xAHCI)
public:
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual IOWorkLoop *getWorkLoop() const;
    virtual IOReturn setProperties(OSObject *properties);   /* privileged runtime knobs: ReadOnly */

    inline UInt32 rd(UInt32 off) const { return OSReadLittleInt32((volatile void *)_mmio, off); }
    inline void   wr(UInt32 off, UInt32 v) const { OSWriteLittleInt32((volatile void *)_mmio, off, v); OSSynchronizeIO(); }
    volatile UInt8 *portBase(UInt32 p) const { return _mmio + PORT_BASE + p * PORT_STRIDE; }
    UInt32 capabilities() const { return _cap; }

private:
    IOPCIDevice            *_pci;
    IOMemoryMap            *_abar;
    volatile UInt8         *_mmio;
    IOWorkLoop             *_wl;
    IOInterruptEventSource *_irq;
    UInt32                  _cap, _portMap;
    ASM106xAHCIPort        *_ports[32];

    bool resetHBA();
    static void interruptAction(OSObject *owner, IOInterruptEventSource *src, int count);
    void handleInterrupt();
};

class ASM106xAHCIPort : public IOATAController
{
    OSDeclareDefaultStructors(ASM106xAHCIPort)
public:
    /* called by the root before attach/start */
    bool configure(ASM106xAHCI *hba, UInt32 portNo, bool readOnly);

    virtual bool start(IOService *provider);
    virtual void free();
    void serviceInterrupt();
    void stopPort();
    void setReadOnly(bool ro);

    /* IOATAController overrides */
    virtual IOReturn provideBusInfo(IOATABusInfo *infoOut);
    virtual IOReturn selectConfig(IOATADevConfig *configRequest, UInt32 unitNumber);
    virtual IOReturn getConfig(IOATADevConfig *configOut, UInt32 unitNumber);
protected:
    virtual bool configureTFPointers(void);
    virtual bool allocateDoubleBuffer(void);
    virtual UInt32 scanForDrives(void);
    virtual IOReturn handleExecIO(void);
    virtual IOReturn handleRegAccess(void);
    virtual IOReturn handleBusReset(void);
    virtual IOReturn handleQueueFlush(void);
    virtual IOReturn handleDeviceInterrupt(void);
    virtual void handleTimeout(void);
    virtual IOReturn selectDevice(ataUnitID unit);
    virtual IOReturn startDMA(void);
    virtual IOReturn stopDMA(void);
    virtual void selectIOTiming(ataUnitID unit);

private:
    ASM106xAHCI    *_hba;
    UInt32          _no;
    volatile UInt8 *_pm;                 /* this port's register block */
    UInt8          *_dma;                /* virtual base of the DMA block */
    IOPhysicalAddress _dmaPhys;
    IONaturalMemoryCursor *_cursor;
    bool            _linkUp;
    bool            _readOnly;           /* bring-up safety: refuse data-out ATA commands */
    UInt32          _sig;
    UInt8           _lastD2H[20];        /* copy of the last D2H register FIS */
    UInt8           _dummy[8];           /* target of the unused taskfile pointers */
    UInt8           _pio, _dmaMode, _ultra;
    atapiConfig     _pktCfg;

    inline UInt32 rd(UInt32 off) const { return OSReadLittleInt32((volatile void *)_pm, off); }
    inline void   wr(UInt32 off, UInt32 v) const { OSWriteLittleInt32((volatile void *)_pm, off, v); OSSynchronizeIO(); }
    bool waitReg(UInt32 off, UInt32 mask, UInt32 val, UInt32 timeoutMs);

    bool allocDma();
    bool stopEngine();
    bool stopFisRx();
    void startFisRx();
    void startEngine();
    bool comReset();
    bool bringUp();
    void recover(bool hard);
    IOReturn issueCurrent();
    void completeFromHW(IOReturn forced, UInt32 tfd);
    void fetchTaskfile(IOATABusCommand *c, bool isError);
    bool pollDone(UInt32 timeoutMs);
};

#endif
