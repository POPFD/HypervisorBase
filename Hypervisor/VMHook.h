#pragma once
#include <wdm.h>
#include "EPT.h"

/******************** Public Defines ********************/

/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
NTSTATUS VMHook_init(PEPT_CONFIG eptConfig);
void VMHook_queueHook(PVOID targetFunction, PVOID hookFunction, PVOID* origFunction);
