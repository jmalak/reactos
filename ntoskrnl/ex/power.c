/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/ex/power.c
 * PURPOSE:         Power managment
 * PROGRAMMER:      David Welch (welch@cwcom.net)
 * UPDATE HISTORY:
 *                  Created 22/05/98
 *                  Added reboot support 30/01/99
 */

/* INCLUDES *****************************************************************/

#include <ddk/ntddk.h>
#include <internal/hal/io.h>

#include <internal/debug.h>

/* FUNCTIONS *****************************************************************/

NTSTATUS STDCALL NtSetSystemPowerState(VOID)
{
   UNIMPLEMENTED;
}


NTSTATUS
STDCALL
NtShutdownSystem (
        IN   SHUTDOWN_ACTION Action
	)
{
    if (Action > ShutdownPowerOff)
        return STATUS_INVALID_PARAMETER;

    /* FIXME: notify all registered drivers (MJ_SHUTDOWN) */

    /* FIXME: notify configuration manager (Registry) */

    /* FIXME: notify memory manager */

    /* FIXME: notify all file system drivers (MJ_SHUTDOWN) */

    if (Action == ShutdownNoReboot)
    {
#if 0
        /* Switch off */
        HalReturnToFirmware (FIRMWARE_OFF);      
#endif
    }
    else if (Action == ShutdownReboot)
    {
        HalReturnToFirmware (FIRMWARE_REBOOT);
    }
    else
    {
        HalReturnToFirmware (FIRMWARE_HALT);
    }

    return STATUS_SUCCESS;
}

/* EOF */

