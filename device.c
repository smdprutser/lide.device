// SPDX-License-Identifier: GPL-2.0-only
/* This file is part of lide.device
 * Copyright (C) 2023 Matthew Harlum <matt@harlum.net>
 */
#include <devices/scsidisk.h>
#include <devices/timer.h>
#include <devices/trackdisk.h>
#include <exec/errors.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <resources/filesysres.h>

#include <string.h>
#include <stdio.h>

#include "ata.h"
#include "atapi.h"
#include "device.h"
#include "idetask.h"
#include "newstyle.h"
#include "td64.h"
#include "mounter.h"
#include "debug.h"
#include "lide_alib.h"

#ifdef NO_AUTOCONFIG
extern UBYTE bootblock, bootblock_end;
#endif


/*-----------------------------------------------------------
A library or device with a romtag should start with moveq #-1,d0 (to
safely return an error if a user tries to execute the file), followed by a
Resident structure.
------------------------------------------------------------*/
int __attribute__((no_reorder)) _start()
{
    return -1;
}

asm("romtag:                                \n"
    "       dc.w    "XSTR(RTC_MATCHWORD)"   \n"
    "       dc.l    romtag                  \n"
    "       dc.l    _endskip                \n"
    "       dc.b    "XSTR(RTF_COLDSTART)"   \n"
    "       dc.b    "XSTR(DEVICE_VERSION)"  \n"
    "       dc.b    "XSTR(NT_DEVICE)"       \n"
    "       dc.b    "XSTR(DEVICE_PRIORITY)" \n"
    "       dc.l    _device_name            \n"
    "       dc.l    _device_id_string       \n"
    "       dc.l    _init                   \n");

const char device_name[] = DEVICE_NAME;
const char device_id_string[] = DEVICE_ID_STRING;

/**
 * set_dev_name
 *
 * Try to set a unique drive name
 * will prepend 2nd/3rd/4th. etc to the beginning of device_name
*/
char * set_dev_name(struct DeviceBase *dev) {
    struct ExecBase *SysBase = dev->SysBase;

    const ULONG device_prefix[] = {' nd.', ' rd.', ' th.'};
    char * devName = (char *)device_name;

    /* Prefix the device name if a device with the same name already exists */
    for (int i=0; i<8; i++) {
        if (FindName(&SysBase->DeviceList,devName)) {
            if (i == 0) {
                devName = AllocMem(sizeof(device_name)+4,MEMF_ANY|MEMF_CLEAR);
                if (devName == NULL) return NULL;
                strncpy(devName + 4,device_name,sizeof(device_name));
            }
            
            switch (i) {
                case 0:
                    *(ULONG *)devName = device_prefix[0];
                    break;
                case 1:
                    *(ULONG *)devName = device_prefix[1];
                    break;
                default:
                    *(ULONG *)devName = device_prefix[2];
                    break;
            }
            *(char *)devName = '2' + i;
        } else {
            Info("Device name: %s\n",devName);
            return devName;
        }
    }
    Info("Couldn't set device name.\n");
    return NULL;
}

#ifdef NO_AUTOCONFIG
/**
 * CreateFakeConfigDev
 * Create fake ConfigDev and DiagArea to support autoboot without requiring real autoconfig device.
 * Adapted from mounter.c by Toni Wilen
 * 
 * @param SysBase Pointer to SysBase
 * @param ExpansionBase Pointer to ExpansionBase
 * @returns Pointer to a ConfigDev struct
 */
struct ConfigDev *CreateFakeConfigDev(struct ExecBase *SysBase, struct Library *ExpansionBase)
{
	struct ConfigDev *cd;

	cd = AllocConfigDev();
	if (cd) {
		cd->cd_BoardAddr = NULL;
		cd->cd_BoardSize = 0;
		cd->cd_Rom.er_Type = ERTF_DIAGVALID;
		ULONG bbSize = &bootblock_end - &bootblock;
		ULONG daSize = sizeof(struct DiagArea) + bbSize;
		struct DiagArea *diagArea = AllocMem(daSize, MEMF_CLEAR | MEMF_PUBLIC);
		if (diagArea) {
			diagArea->da_Config     = DAC_CONFIGTIME;
			diagArea->da_BootPoint  = sizeof(struct DiagArea);
			diagArea->da_Size       = (UWORD)daSize;
            CopyMem(&bootblock, diagArea+1, bbSize);
			// cd_Rom.er_Reserved0c is used as a pointer to diagArea by strap
			ULONG *da_Pointer = (ULONG *)&cd->cd_Rom.er_Reserved0c;
			*da_Pointer = (ULONG)diagArea;
		}
	}
    return cd;
}
#endif

/**
 * sleep
 *
 * @param seconds Seconds to wait
 * @param microseconds Microseconds to wait
*/
static void sleep(ULONG seconds, ULONG microseconds) {
    struct ExecBase *SysBase = *(struct ExecBase**)4UL;

    struct timerequest *tr = NULL;
    struct MsgPort     *iomp = NULL;


    if ((iomp = L_CreatePort(NULL,0))) {
        if ((tr = (struct timerequest *)L_CreateExtIO(iomp, sizeof(struct timerequest)))) {
            if ((OpenDevice("timer.device",UNIT_VBLANK,(struct IORequest *)tr,0)) == 0) {
                tr->tr_node.io_Command = TR_ADDREQUEST;
                tr->tr_time.tv_sec = seconds;
                tr->tr_time.tv_micro = microseconds;
                DoIO((struct IORequest *)tr);
                CloseDevice((struct IORequest *)tr);
            }
            L_DeleteExtIO((struct IORequest *)tr);
        }
        L_DeletePort(iomp);
    }
}

#if CDBOOT
/**
 * FindCDFS
 *
 * Look for a CD Filesystem in FileSystem.resource
 *
 * @return BOOL True if CDFS found
*/
static BOOL FindCDFS() {
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
    struct FileSysResource *fsr = OpenResource(FSRNAME);
    struct FileSysEntry *fse;

    if (fsr == NULL) return false;

    for (fse = (struct FileSysEntry *)fsr->fsr_FileSysEntries.lh_Head; fse->fse_Node.ln_Succ != NULL; fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
        if (fse->fse_DosType == 'CD01') return true;
    }

    return false;
}
#endif

static bool ioreq_is_valid(struct DeviceBase *dev, struct IORequest *ior) {
    struct ExecBase *SysBase = dev->SysBase;
    bool found = false;

    struct IDEUnit *unit;

    if (SysBase->LibNode.lib_Version >= 36) {
        ObtainSemaphoreShared(&dev->ulSem);
    } else {
        ObtainSemaphore(&dev->ulSem);
    }

    for (unit = (struct IDEUnit *)dev->units.mlh_Head;
         unit->mn_Node.mln_Succ != NULL;
         unit = (struct IDEUnit *)unit->mn_Node.mln_Succ) {
            if (unit == (struct IDEUnit *)ior->io_Unit) {
                found = true;
                break;
            }
         }

    ReleaseSemaphore(&dev->ulSem);

    if (!found) return false;

    if ((struct Device *)dev != ior->io_Device) return false;

    return true;
}

/**
 * Cleanup
 *
 * Free used resources back to the system
*/
static void Cleanup(struct DeviceBase *dev) {
    Info("Cleaning up...\n");
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;

    struct IDEUnit *unit;

    if (SysBase->LibNode.lib_Version >= 36) {
        ObtainSemaphoreShared(&dev->ulSem);
    } else {
        ObtainSemaphore(&dev->ulSem);
    }

    for (unit = (struct IDEUnit *)dev->units.mlh_Head;
         unit->mn_Node.mln_Succ != NULL;
         unit = (struct IDEUnit *)unit->mn_Node.mln_Succ)
        {
            unit->cd->cd_Flags |= CDF_CONFIGME;
        }

    ReleaseSemaphore(&dev->ulSem);

    if (dev->ExpansionBase) CloseLibrary((struct Library *)dev->ExpansionBase);

    struct IDETask *itask;

    for (itask = (struct IDETask *)dev->ideTasks.mlh_Head;
         itask->mn_Node.mln_Succ != NULL;
         itask = (struct IDETask *)itask->mn_Node.mln_Succ)
    {
        FreeMem(itask,sizeof(struct IDETask));
    }

    FreeMem((char *)dev - dev->lib.lib_NegSize, dev->lib.lib_NegSize + dev->lib.lib_PosSize);
}

/**
 * detectChannels
 *
 * Detect how many IDE Channels this board has
 * @param cd Pointer to the ConfigDev struct for this board
 * @returns number of channels
*/
static BYTE detectChannels(struct ConfigDev *cd) {
    if ((cd->cd_Rom.er_Manufacturer == OAHR_MANUF_ID) && (cd->cd_Rom.er_Product == RIPPLE_PROD_ID))
        return 2;

    UBYTE *drvsel = cd->cd_BoardAddr + CHANNEL_0 + ata_reg_devHead;

    *drvsel = 0xE0; // Select the primary drive + poke IDE to turn off ROM

    if (cd->cd_Rom.er_Manufacturer == BSC_MANUF_ID || cd->cd_Rom.er_Manufacturer == A1K_MANUF_ID) {
        // On the AT-Bus 2008 (Clone) the ROM is selected on the lower byte when IDE_CS1 is asserted
        // Not a problem in single channel mode - the drive registers there only use the upper byte
        // If Status == Alt Status or it's an AT-Bus card then only one channel is supported.
        //
        // Check for the ROM Footer
        // On the AT-Bus 2008 clone it will still be there
        // On a Matze TK the ROM goes away and the board can do 2 channels
        ULONG signature = 0;
        char *romFooter = (char *)cd->cd_BoardAddr + 0xFFF8;

        if (cd->cd_Rom.er_InitDiagVec & 1 ||      // If the board has an odd offset then add it
            cd->cd_Rom.er_InitDiagVec == 0x100) { // WinUAE AT-Bus 2008 has DiagVec @ 0x100 but Driver is on odd offset
                romFooter++;
            }

        for (int i=0; i<4; i++) {
            signature <<= 8;
            signature |= *romFooter;
            romFooter += 2;
        }

        if (signature == 'LIDE') {
            Info("Channel detection: Saw ROM footer - assuming AT-Bus single channel mode\n");
            return 1;
        }
    }

    // Detect if there are 1 or 2 IDE channels on this board
    // 2 channel boards use the CS2 decode for the second channel
    volatile UBYTE *status     = cd->cd_BoardAddr + CHANNEL_0 + ata_reg_status;
    volatile UBYTE *alt_status = cd->cd_BoardAddr + CHANNEL_1 + ata_reg_altStatus;

    // Try a couple of times with a small delay
    // Some drives have been seen to be quite slow at resetting, and interfere with the channel detection unless there's a delay
    for (int i=0; i<4; i++) {
        sleep(0,250000); // 250ms
        if (*status != *alt_status) {
            return 2;
        }
    }

    return 1;

}

/**
 * init_device
 *
 * Scan for drives and initialize the driver if any are found
*/
struct Library __attribute__((used, saveds)) * init_device(struct ExecBase *SysBase asm("a6"), BPTR seg_list asm("a0"), struct DeviceBase *dev asm("d0"))
//struct Library __attribute__((used)) * init_device(struct ExecBase *SysBase, BPTR seg_list, struct DeviceBase *dev)
{
    dev->SysBase = SysBase;
    Trace("Init dev, base: %08lx\n",dev);
    struct Library *ExpansionBase = NULL;

    char *devName;

    UBYTE numBoards = 0;

    if (!(devName = set_dev_name(dev))) return NULL;
    /* save pointer to our loaded code (the SegList) */
    dev->saved_seg_list       = seg_list;
    dev->lib.lib_Node.ln_Type = NT_DEVICE;
    dev->lib.lib_Node.ln_Name = devName;
    dev->lib.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    dev->lib.lib_Version      = DEVICE_VERSION;
    dev->lib.lib_Revision     = DEVICE_REVISION;
    dev->lib.lib_IdString     = (APTR)device_id_string;

    dev->isOpen        = FALSE;
    dev->numUnits      = 0;
    dev->numTasks      = 0;
    dev->hasRemovables = false;

    L_NewList((struct List *)&dev->units);
    InitSemaphore(&dev->ulSem);

    L_NewList((struct List *)&dev->ideTasks);

    if (!(ExpansionBase = (struct Library *)OpenLibrary("expansion.library",0))) {
        Cleanup(dev);
        return NULL;
    } else {
        dev->ExpansionBase = ExpansionBase;
    }

    struct IDETask *itask;
    struct ConfigDev *cd;
    struct Task *self = FindTask(NULL);

#ifndef NO_AUTOCONFIG

    struct CurrentBinding cb;

    if (GetCurrentBinding(&cb,sizeof(struct CurrentBinding)) == 0) {
        Cleanup(dev);
        return NULL;
    }

    cd = cb.cb_ConfigDev;

    // Add an IDE Task for each board
    // When loaded from Autoconfig ROM this will still only attach to one board.
    // If the driver is loaded by BindDrivers though then this should attach to multiple boards.
    for (cd = cb.cb_ConfigDev; cd != NULL; cd = cd->cd_NextCD) {

        if (!cd) {
            break;
        }

        if (!(cd->cd_Flags & CDF_CONFIGME)) {
            continue;
        }

        Trace("Claiming board %08lx\n",(ULONG)cd->cd_BoardAddr);
        cd->cd_Flags &= ~(CDF_CONFIGME); // Claim the board
        cd->cd_Driver = dev;

        numBoards++;
#else
        /**
         * A ConfigDev is needed for Autoboot
         * If we are booting a non-autoconfig device we need to create a ConfigDev struct
         * This could also be done in mounter.c but that would create a new ConfigDev for each unit
         */
        cd = CreateFakeConfigDev(SysBase,ExpansionBase);
        cd->cd_BoardAddr = (APTR)BOARD_BASE;
        cd->cd_BoardSize = 0x1000;
        numBoards = 1;
#endif
        UBYTE channels = detectChannels(cd);

        for (int c=0; c < channels; c++) {

            Trace("Starting IDE Task %ld\n",numBoards);

            itask = AllocMem(sizeof(struct IDETask), MEMF_ANY|MEMF_CLEAR);

            if (itask == NULL) {
                Info("Couldn't allocate memory\n");
                break;
            }

            itask->dev      = dev;
            itask->cd       = cd;
            itask->channel  = c;
            itask->taskNum  = dev->numTasks;
            itask->parent   = self;
            itask->boardNum = (numBoards - 1);

            SetSignal(0,SIGF_SINGLE);

            // Start the IDE Task
            itask->task = L_CreateTask(ATA_TASK_NAME,TASK_PRIORITY,ide_task,TASK_STACK_SIZE,itask);
            if (itask->task == NULL) {
                Info("IDE Task %ld failed\n",itask->taskNum);
                continue;
            } else {
                Trace("IDE Task %ld created!, waiting for init\n",itask->taskNum);
            }

            // Wait for task to init
            Wait(SIGF_SINGLE);

            // If itask->active has been set to false it means the task exited
            if (itask->active == false) {
                Info("IDE Task %ld exited.\n",itask->taskNum);
                continue;
            }

            // Add the task to the list
            AddTail((struct List *)&dev->ideTasks,(struct Node *)&itask->mn_Node);
            dev->numTasks++;
        }
#ifndef NO_AUTOCONFIG
    }
#endif
    Info("Detected %ld drives, %ld boards\n",((volatile struct DeviceBase *)dev)->numUnits, numBoards);

    if (dev->numTasks == 0) {
        Cleanup(dev);
        return NULL;
    }

    if (dev->hasRemovables) dev->ChangeTask = L_CreateTask(CHANGE_TASK_NAME,0,diskchange_task,TASK_STACK_SIZE,dev);

    Info("Startup finished.\n");
    return (struct Library *)dev;

}

/* device dependent expunge function
!!! CAUTION: This function runs in a forbidden state !!!
This call is guaranteed to be single-threaded; only one task
will execute your Expunge at a time. */
static BPTR __attribute__((used, saveds)) expunge(struct DeviceBase *dev asm("a6"))
{
    Trace((CONST_STRPTR) "running expunge()\n");

    /**
     * Don't expunge
     *
     * If expunged the driver will be gone until reboot
     *
     * Also need to figure out how to kill the disk change int task cleanly before this can be enabled
    */

    dev->lib.lib_Flags |= LIBF_DELEXP;
    return 0;

    // if (dev->lib.lib_OpenCnt != 0)
    // {
    //     dev->lib.lib_Flags |= LIBF_DELEXP;
    //     return 0;
    // }

    // if (dev->IDETask != NULL && dev->IDETaskActive == true) {
    //     // Shut down ide_task

    //     struct MsgPort *mp = NULL;
    //     struct IOStdReq *ioreq = NULL;

    //     if ((mp = CreatePort(NULL,0)) == NULL)
    //         return 0;
    //     if ((ioreq = CreateStdIO(mp)) == NULL) {
    //         DeletePort(mp);
    //         return 0;
    //     }

    //     ioreq->io_Command = CMD_DIE; // Tell ide_task to shut down

    //     PutMsg(dev->IDETaskMP,(struct Message *)ioreq);
    //     WaitPort(mp);                // Wait for ide_task to signal that it is shutting down

    //     if (ioreq) DeleteStdIO(ioreq);
    //     if (mp) DeletePort(mp);
    // }

    // Cleanup(dev);
    // BPTR seg_list = dev->saved_seg_list;
    // Remove(&dev->lib.lib_Node);
    // FreeMem((char *)dev - dev->lib.lib_NegSize, dev->lib.lib_NegSize + dev->lib.lib_PosSize);
    // return seg_list;
}

/* device dependent open function
!!! CAUTION: This function runs in a forbidden state !!!
This call is guaranteed to be single-threaded; only one task
will execute your Open at a time. */
static void __attribute__((used, saveds)) open(struct DeviceBase *dev asm("a6"), struct IORequest *ioreq asm("a1"), ULONG unitnum asm("d0"), ULONG flags asm("d1"))
{
    struct ExecBase *SysBase = dev->SysBase;
    struct IDEUnit *unit = NULL;
    BYTE error = 0;
    bool found = false;

    Trace((CONST_STRPTR) "running open() for unitnum %ld\n",unitnum);

    /* IMPORTANT: Must return TDERR_BadUnitNum when lun > 0
     * SCSI Unit encoding places the LUN in the 10s column of the unit number
     * HDToolbox scans each LUN of a unit and stops searching if it sees an error other than TDERR_BadUnitNum
     * So if this is not returned, only one drive will ever be detected
    */
    UBYTE lun = unitnum / 10;
    unitnum = (unitnum % 10);

    if (lun != 0) {
        // No LUNs for IDE drives
        error = TDERR_BadUnitNum;
        goto exit;
    }

    if (unitnum > dev->highestUnit) {
        error = IOERR_OPENFAIL;
        goto exit;
    }

    if (SysBase->LibNode.lib_Version >= 36) {
        ObtainSemaphoreShared(&dev->ulSem);
    } else {
        ObtainSemaphore(&dev->ulSem);
    }

    for (unit = (struct IDEUnit *)dev->units.mlh_Head;
         unit->mn_Node.mln_Succ != NULL;
         unit = (struct IDEUnit *)unit->mn_Node.mln_Succ)
        {
            if (unit->unitNum == unitnum) {
                found = true;
                break;
            }
        }

    ReleaseSemaphore(&dev->ulSem);

    if (found == false || unit->present == false) {
        error = TDERR_BadUnitNum;
        goto exit;
    }

    if (unit->itask->task == NULL || unit->itask->active == false) {
        error = IOERR_OPENFAIL;
        goto exit;
    }

    ioreq->io_Unit = (struct Unit *)unit;

    /* IMPORTANT: Mark IORequest as "complete" or otherwise CheckIO() may
     *            consider it as "in use" in spite of never having been
     *            used at all. This also avoids that WaitIO() will hang
     *            on an IORequest which has never been used.
     *
     *            Source: Olaf Barthel's Trackfile.device
     *            https://github.com/obarthel/trackfile-device
    */
    ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;

    // Send a TD_CHANGESTATE ioreq for the unit if it is ATAPI and not already open
    // This will update the media presence & geometry
    if (unit->atapi && unit->openCount == 0) direct_changestate(unit,dev);

    unit->openCount++;

    dev->lib.lib_Flags &= ~LIBF_DELEXP;

    if (!dev->isOpen)
    {
        dev->isOpen = TRUE;
    }
exit:
    if (error != 0) {
        /* IMPORTANT: Invalidate io_Device and io_Unit on open failure. */
        ioreq->io_Unit   = NULL;
        ioreq->io_Device = NULL;
    }
    dev->lib.lib_OpenCnt++;
    ioreq->io_Error = error;
}


static void td_get_geometry(struct IOStdReq *ioreq) {
    struct DriveGeometry *geometry = (struct DriveGeometry *)ioreq->io_Data;
    struct IDEUnit *unit = (struct IDEUnit *)ioreq->io_Unit;

    // if (unit->atapi && unit->mediumPresent == false) {
    //     ioreq->io_Error = TDERR_DiskChanged;
    //     return;
    // }

    // Clear the geometry struct beforehand to make sure reserved / unused parts are zero
    memset(geometry,0,sizeof(struct DriveGeometry));

    geometry->dg_SectorSize   = unit->blockSize;
    geometry->dg_TotalSectors = unit->logicalSectors;
    geometry->dg_Cylinders    = unit->cylinders;
    geometry->dg_CylSectors   = (unit->sectorsPerTrack * unit->heads);
    geometry->dg_Heads        = unit->heads;
    geometry->dg_TrackSectors = unit->sectorsPerTrack;
    geometry->dg_BufMemType   = MEMF_PUBLIC;
    geometry->dg_DeviceType   = unit->deviceType;
    geometry->dg_Flags        = (unit->atapi) ? DGF_REMOVABLE : 0;

    ioreq->io_Actual = sizeof(struct DriveGeometry);
}


/* device dependent close function
!!! CAUTION: This function runs in a forbidden state !!!
This call is guaranteed to be single-threaded; only one task
will execute your Close at a time. */
static BPTR __attribute__((used, saveds)) close(struct DeviceBase *dev asm("a6"), struct IORequest *ioreq asm("a1"))
{
    if (ioreq_is_valid(dev,ioreq)) {
        struct IDEUnit *unit = (struct IDEUnit *)ioreq->io_Unit;
        Trace((CONST_STRPTR) "running close()\n");

        if (dev->lib.lib_OpenCnt) dev->lib.lib_OpenCnt--;

        if (unit->openCount > 0) unit->openCount--;

        if (dev->lib.lib_OpenCnt == 0 && (dev->lib.lib_Flags & LIBF_DELEXP))
            return expunge(dev);

    }

    ioreq->io_Unit   = NULL;
    ioreq->io_Device = NULL;
    return 0;
}

const UWORD supported_commands[] =
{
    CMD_CLEAR,
    CMD_UPDATE,
    CMD_READ,
    CMD_WRITE,
    TD_ADDCHANGEINT,
    TD_REMCHANGEINT,
    TD_REMOVE,
    TD_PROTSTATUS,
    TD_CHANGENUM,
    TD_CHANGESTATE,
    TD_EJECT,
    TD_GETDRIVETYPE,
    TD_GETGEOMETRY,
    TD_MOTOR,
    TD_PROTSTATUS,
    TD_READ64,
    TD_WRITE64,
    TD_FORMAT64,
    ETD_READ,
    ETD_WRITE,
    ETD_FORMAT,
    NSCMD_ETD_READ64,
    NSCMD_ETD_WRITE64,
    NSCMD_ETD_FORMAT64,
    NSCMD_DEVICEQUERY,
    NSCMD_TD_READ64,
    NSCMD_TD_WRITE64,
    NSCMD_TD_FORMAT64,
    HD_SCSICMD,
    0
};

/**
 * begin_io
 *
 * Handle immediate requests and send any others to ide_task
*/
static void __attribute__((used, saveds)) begin_io(struct DeviceBase *dev asm("a6"), struct IOStdReq *ioreq asm("a1"))
{
    struct ExecBase *SysBase = dev->SysBase;

    BYTE error = TDERR_NotSpecified;

    // Check that the IOReq has a sane Device / Unit pointer first
    if (ioreq_is_valid(dev,(struct IORequest *)ioreq)) {
        /* This makes sure that WaitIO() is guaranteed to work and
        * will not hang.
        *
        * Source: Olaf Barthel's Trackfile.device
        * https://github.com/obarthel/trackfile-device
        */
        ioreq->io_Message.mn_Node.ln_Type = NT_MESSAGE;

        struct IDEUnit *unit = (struct IDEUnit *)ioreq->io_Unit;

        Trace((CONST_STRPTR) "running begin_io()\n");
        if (ioreq == NULL || ioreq->io_Unit == 0) return;

        if (unit->itask == NULL || unit->itask->active == false) {

            // If the IDE task is dead then we can only throw an error and reply now.
            ioreq->io_Error = IOERR_OPENFAIL;

            if (!(ioreq->io_Flags & IOF_QUICK)) {
                ReplyMsg(&ioreq->io_Message);
            }

            return;

        }

        Trace("Command %lx\n",ioreq->io_Command);
        switch (ioreq->io_Command) {
            case TD_MOTOR:
            case CMD_CLEAR:
            case CMD_UPDATE:
                ioreq->io_Actual = 0;
                error            = 0;
                break;

            case TD_CHANGENUM:
                ioreq->io_Actual = unit->changeCount;
                error            = 0;
                break;

            case TD_GETDRIVETYPE:
                ioreq->io_Actual = unit->deviceType;
                error            = 0;
                break;

            case TD_GETGEOMETRY:
                td_get_geometry(ioreq);
                error = 0;
                break;

            case TD_REMOVE:
                unit->changeInt = ioreq->io_Data;
                error           = 0;
                break;


            case TD_ADDCHANGEINT:
                Info("Addchangeint\n");

                ioreq->io_Flags |= IOF_QUICK; // Must not Reply to this request
                error = 0;

                Disable();
                AddHead((struct List *)&unit->changeInts,(struct Node *)&ioreq->io_Message.mn_Node);
                Enable();
                break;

            case TD_REMCHANGEINT:
                error = 0;
                struct MinNode *changeint;
                // Must Disable() rather than Forbid()!
                Disable();
                for (changeint = unit->changeInts.mlh_Head; changeint->mln_Succ != NULL; changeint = changeint->mln_Succ) {
                    if (ioreq == (struct IOStdReq *)changeint) {
                        Remove(&ioreq->io_Message.mn_Node);
                        break;
                    }
                }
                Enable();
                break;


            case TD_CHANGESTATE:
            case CMD_READ:
            case ETD_READ:
            case CMD_WRITE:
            case ETD_WRITE:
                ioreq->io_Actual = 0; // Clear high offset for 32-bit commands
            case TD_PROTSTATUS:
            case TD_EJECT:
            case TD_FORMAT:
            case TD_READ64:
            case TD_WRITE64:
            case TD_FORMAT64:
            case NSCMD_TD_READ64:
            case NSCMD_TD_WRITE64:
            case NSCMD_TD_FORMAT64:
            case NSCMD_ETD_READ64:
            case NSCMD_ETD_WRITE64:
            case NSCMD_ETD_FORMAT64:
            case CMD_XFER:
            case CMD_PIO:
            case HD_SCSICMD:
                // Send all of these to ide_task
                ioreq->io_Flags &= ~IOF_QUICK;
                PutMsg(unit->itask->iomp,&ioreq->io_Message);
                Trace((CONST_STRPTR) "IO queued\n");
                return;

            case NSCMD_DEVICEQUERY:
                if (ioreq->io_Length >= sizeof(struct NSDeviceQueryResult))
                {
                    struct NSDeviceQueryResult *result = ioreq->io_Data;

                    result->DevQueryFormat    = 0;
                    result->SizeAvailable     = sizeof(struct NSDeviceQueryResult);
                    result->DeviceType        = NSDEVTYPE_TRACKDISK;
                    result->DeviceSubType     = 0;
                    result->SupportedCommands = (UWORD *)supported_commands;

                    ioreq->io_Actual = sizeof(struct NSDeviceQueryResult);
                    error = 0;
                }
                else {
                    error = IOERR_BADLENGTH;
                }
                break;

            default:
                Warn("Unknown command %d\n", ioreq->io_Command);
                error = IOERR_NOCMD;
        }
    }

#if DEBUG & DBG_CMD
    traceCommand(ioreq);
#endif
    ioreq->io_Error = error;

    if (ioreq && !(ioreq->io_Flags & IOF_QUICK)) {
        ReplyMsg(&ioreq->io_Message);
    }
}

/**
 * abort_io
 *
 * Abort io request
*/
static ULONG __attribute__((used, saveds)) abort_io(struct DeviceBase *dev asm("a6"), struct IOStdReq *ioreq asm("a1"))
{
    struct ExecBase *SysBase = dev->SysBase;

    Trace((CONST_STRPTR) "running abort_io()\n");

    struct IORequest *io;

    BYTE error = 0; // 0 indicates that the IO was *NOT* aborted

    /* If the IO is still in queue then we can remove it
     * MUST be done inside a Disable()!
     *
     * Copied from Olaf Barthels Trackfile.device
     * https://github.com/obarthel/trackfile-device
     */
    if (ioreq_is_valid(dev,(struct IORequest *)ioreq)) {
        struct IDEUnit *unit = (struct IDEUnit *)ioreq->io_Unit;
        Disable();
        for (io = (struct IORequest *)unit->itask->iomp->mp_MsgList.lh_Head;
             io->io_Message.mn_Node.ln_Succ != NULL;
             io = (struct IORequest *)io->io_Message.mn_Node.ln_Succ)
        {
            if (io == (struct IORequest *)ioreq) {
                Remove(&io->io_Message.mn_Node);
                error = io->io_Error = IOERR_ABORTED;
                ReplyMsg(&io->io_Message);
                break;
            }
        }
        Enable();
    }
    return error;
}


static const ULONG device_vectors[] =
    {
        (ULONG)open,
        (ULONG)close,
        (ULONG)expunge,
        0, //extFunc not used here
        (ULONG)begin_io,
        (ULONG)abort_io,
        -1 //function table end marker
    };

/**
 * init
 *
 * Create the device and add it to the system if init_device succeeds
*/
static struct Library __attribute__((used)) * init(BPTR seg_list asm("a0"))
{
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
    Info("Init driver.\n");
    struct MountStruct *ms = NULL;
    struct DeviceBase *mydev = (struct DeviceBase *)MakeLibrary((ULONG *)&device_vectors,  // Vectors
                                                                NULL,                      // InitStruct data
                                                                (APTR)init_device,         // Init function
                                                                sizeof(struct DeviceBase), // Library data size
                                                                seg_list);                 // Segment list

    if (mydev != NULL) {
        ULONG ms_size = (sizeof(struct MountStruct) + (MAX_UNITS * sizeof(struct UnitStruct)));
        Info("Add Device.\n");
        AddDevice((struct Device *)mydev);

        if ((ms = AllocMem(ms_size,MEMF_ANY|MEMF_PUBLIC)) == NULL) goto done;

        ms->deviceName  = mydev->lib.lib_Node.ln_Name;
        ms->creatorName = NULL;
        ms->numUnits    = 0;
        ms->SysBase     = SysBase;

        UWORD index = 0;
#if CDBOOT
        BOOL CDBoot = FindCDFS();
#endif
        struct IDEUnit *unit;

        if (SysBase->LibNode.lib_Version >= 36) {
            ObtainSemaphoreShared(&mydev->ulSem);
        } else {
            ObtainSemaphore(&mydev->ulSem);
        }

        for (unit = (struct IDEUnit *)mydev->units.mlh_Head;
             unit->mn_Node.mln_Succ != NULL;
             unit = (struct IDEUnit *)unit->mn_Node.mln_Succ)
        {
            if (unit->present == true) {
#if CDBOOT
                // If CDFS not resident don't bother adding the CDROM to the mountlist
                if (unit->deviceType == DG_CDROM && !CDBoot) continue;
#endif
                ms->Units[index].unitNum    = unit->unitNum;
                ms->Units[index].configDev  = unit->cd;
                index++;
            }
        }

        ms->numUnits = index;

        ReleaseSemaphore(&mydev->ulSem);
        if (ms->numUnits > 0) {
            MountDrive(ms);
        }

        FreeMem(ms,ms_size);
    }
done:
    return (struct Library *)mydev;
}
