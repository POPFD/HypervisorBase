#pragma once
#include <wdm.h>

/******************** Public Defines ********************/

/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/

void MSR_readXMSR(PLARGE_INTEGER msrData, SIZE_T count, UINT32 msrBase);
UINT32 MSR_adjustMSR(LARGE_INTEGER ControlValue, UINT32 DesiredValue);
