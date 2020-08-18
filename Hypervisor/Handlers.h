#pragma once
#include <wdm.h>

/******************** Public Defines ********************/

/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/

DECLSPEC_NORETURN VOID Handlers_hostToGuest(void);
DECLSPEC_NORETURN VOID Handlers_guestToHost(PCONTEXT guestContext);
DECLSPEC_NORETURN void Handlers_VMResume(void);
