#pragma once
#include <wdm.h>
#include "ia32.h"

/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
void EventLog_init(void);
DECLSPEC_NORETURN void EventLog_logAsGuestThenRestore(PCONTEXT context, ULONG procIndex, CHAR const* extraString);