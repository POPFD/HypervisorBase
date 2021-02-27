#pragma once
#include <wdm.h>
#include "ia32.h"

/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
void EventLog_init(void);
NTSTATUS EventLog_logEvent(ULONG procIndex, CR0 guestCR0, CR3 guestCR3,
						   CR4 guestCR4, PCONTEXT guestContext, CHAR const* extraString);
NTSTATUS EventLog_retrieveAndClear(PUINT8 buffer, SIZE_T bufferSize, SIZE_T* eventCount);