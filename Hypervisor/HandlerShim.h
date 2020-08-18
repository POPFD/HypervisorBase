#pragma once
#include <wdm.h>

/******************** Public Defines ********************/


/******************** Public Typedefs ********************/


/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/

void HandlerShim_hostToGuest(void);
void HandlerShim_guestToHost(void);
NTSTATUS HandlerShim_VMCALL(UINT64 key, void* params);