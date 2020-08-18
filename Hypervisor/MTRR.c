#include "MTRR.h"
#include "Debug.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/


/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/


/******************** Public Code ********************/

void MTRR_readAll(MTRR_RANGE mtrrTable[IA32_MTRR_VARIABLE_COUNT])
{
	IA32_MTRR_CAPABILITIES_REGISTER mtrrCapabilities;
	IA32_MTRR_PHYSBASE_REGISTER mtrrBase;
	IA32_MTRR_PHYSMASK_REGISTER mtrrMask;

	/* Read the capabilities mask. */
	mtrrCapabilities.Flags = __readmsr(IA32_MTRR_CAPABILITIES);

	DEBUG_PRINT("Storing 0x%I64X MTRR register variables.\r\n", mtrrCapabilities.VariableRangeCount);

	for (UINT32 i = 0; i < mtrrCapabilities.VariableRangeCount; i++)
	{
		/* Capture the value MTRR value. */
		mtrrBase.Flags = __readmsr(IA32_MTRR_PHYSBASE0 + i * 2);
		mtrrMask.Flags = __readmsr((IA32_MTRR_PHYSBASE0 + 1) + i * 2);

		/* Check to see if the specific MTRR is enabled. */
		mtrrTable[i].Type = (UINT32)mtrrBase.Type;
		mtrrTable[i].Valid = (UINT32)mtrrMask.Valid;

		if (mtrrTable[i].Valid != FALSE)
		{
			/* Store the minimum physical address. */
			mtrrTable[i].PhysicalAddressMin = mtrrBase.PageFrameNumber * PAGE_SIZE;

			/* Compute the length and store the maximum physical address. */
			unsigned long bit;

			_BitScanForward64(&bit, mtrrMask.PageFrameNumber * PAGE_SIZE);
			mtrrTable[i].PhysicalAddressMax = mtrrTable[i].PhysicalAddressMin + ((1ULL << bit) - 1);
		}

	}
}

/******************** Module Code ********************/