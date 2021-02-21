#pragma once
#include <wdm.h>
#include "MemManage.h"
#include "ia32.h"

/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
HOST_PHYS_ADDRESS GuestShim_GuestUVAToHPA(PMM_CONTEXT mmContext, CR3 userCR3, GUEST_VIRTUAL_ADDRESS guestAddress);
