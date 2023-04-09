// SPDX-License-Identifier: GPL-2.0-only
/* This file is part of liv2ride.device
 * Copyright (C) 2023 Matthew Harlum <matt@harlum.net>
 */
#include <devices/scsidisk.h>
#include <devices/trackdisk.h>
#include <exec/errors.h>
#include <proto/alib.h>
#include <proto/exec.h>
#include <string.h>

#include "ata.h"
#include "debug.h"
#include "device.h"
#include "idetask.h"
#include "newstyle.h"
#include "scsi.h"
#include "td64.h"

/**
 * scsi_sense
 * 
 * Populate sense data based on the error returned by the ATA functions
 * 
 * @param command A pointer to a SCSICmd
 * @param info Sense data Info long
 * @param specific Sense data Specific long
 * @param error Error code returned from ata_transfer
 * 
*/
void scsi_sense(struct SCSICmd* command, ULONG info, ULONG specific, BYTE error)
{
    struct SCSI_FIXED_SENSE *sense = (struct SCSI_FIXED_SENSE *)command->scsi_SenseData;
    if (!(command->scsi_Flags & SCSIF_AUTOSENSE) || error == 0 || sense == NULL || (command->scsi_SenseLength < sizeof(struct SCSI_FIXED_SENSE)))
    {
        command->scsi_SenseActual = 0;
        return;
    }

    command->scsi_SenseActual = sizeof(struct SCSI_FIXED_SENSE);

    sense->response   = 0x70; // Fixed format, current status
    sense->pad        = 0;
    sense->info       = info;
    sense->additional = (UBYTE)sizeof(sense) - 7;
    sense->specific   = specific;
    sense->fru        = error;
    switch (error) {
        case IOERR_UNITBUSY:
            sense->senseKey = 0x03;
            sense->asc      = 0x04; // Unit not ready, cause not reportable
            sense->asq      = 0x00;
            break;
        case IOERR_BADADDRESS:
            sense->senseKey = 0x03;
            sense->asc      = 0x21; // LBA Out of range
            sense->asq      = 0x00;
            break;
        case IOERR_NOCMD:
            sense->senseKey = 0x05; // Invalid Command operation code
            sense->asc      = 0x20;
            sense->asq      = 0x00;
            break;
        case TDERR_NotSpecified:
        case HFERR_SelTimeout:
            sense->senseKey = 0x0B; // Unit communication failure
            sense->asc      = 0x08;
            sense->asq      = 0x00;
        default:
            sense->senseKey = 0x00; // No sense
            sense->asc      = 0x00;
            sense->asq      = 0x00;
    }
}
