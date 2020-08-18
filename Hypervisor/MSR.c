#include "MSR.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/


/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/


/******************** Public Code ********************/

void MSR_readXMSR(PLARGE_INTEGER msrData, SIZE_T count, UINT32 msrBase)
{
	/* Read X MSR's starting from the base, into the MSR array. */
	for (UINT32 i = 0; i < count; i++)
	{
		msrData[i].QuadPart = __readmsr(msrBase + i);
	}
}

UINT32 MSR_adjustMSR(LARGE_INTEGER ControlValue, UINT32 DesiredValue)
{
	/*
	* VMX feature/capability MSRs encode the "must be 0" bits in the high word
	* of their value, and the "must be 1" bits in the low word of their value.
	* Adjust any requested capability/feature based on these requirements.
	*/
	DesiredValue &= ControlValue.HighPart;
	DesiredValue |= ControlValue.LowPart;
	return DesiredValue;
}

/******************** Module Code ********************/