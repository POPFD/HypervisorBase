#pragma once
#include <wdm.h>
#include "ia32.h"

/******************** Public Defines ********************/


/******************** Public Typedefs ********************/

typedef struct _MTRR_RANGE
{
	UINT32 Valid;
	UINT32 Type;
	UINT64 PhysicalAddressMin;
	UINT64 PhysicalAddressMax;
} MTRR_RANGE, *PMTRR_RANGE;

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/

void MTRR_readAll(MTRR_RANGE mtrrTable[IA32_MTRR_VARIABLE_COUNT]);


