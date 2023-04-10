// SPDX-License-Identifier: GPL-2.0-only
/* This file is part of liv2ride.device
 * Copyright (C) 2023 Matthew Harlum <matt@harlum.net>
 */
#ifndef _ATAPI_H
#define _ATAPI_H

#include <stdbool.h>
#include "device.h"
#include <exec/types.h>

#define atapi_flag_cd (1<<0)
#define atapi_flag_io (1<<1)

#define atapi_err_abort (1<<2)
#define atapi_err_eom   (1<<1)
#define atapi_err_len   (1<<0)

#define ATAPI_CMD_PACKET   0xA0
#define ATAPI_CMD_IDENTIFY 0xA1

bool atapi_identify(struct IDEUnit *unit, UWORD *buffer);
BYTE atapi_translate(APTR io_Data,ULONG lba, ULONG count, ULONG *io_Actual, struct IDEUnit *unit, enum xfer_dir direction);
BYTE atapi_packet(struct SCSICmd *cmd, struct IDEUnit *unit);
BYTE atapi_test_unit_ready(struct IDEUnit *unit);
BYTE atapi_get_capacity(struct IDEUnit *unit);
BYTE atapi_request_sense(struct IDEUnit *unit, UBYTE *buffer, int length);
BYTE atapi_mode_sense(struct IDEUnit *unit, BYTE page_code, UWORD *buffer, UWORD length, UWORD *actual);
BYTE atapi_scsi_mode_sense_6(struct SCSICmd *cmd, struct IDEUnit *unit);

#endif