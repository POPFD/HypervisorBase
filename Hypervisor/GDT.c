#include "GDT.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/


/******************** Module Constants ********************/

#define SELECTOR_TABLE_INDEX    0x04


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/


/******************** Public Code ********************/

VOID GDT_convertGdtEntry(VOID* GdtBase, UINT16 Selector, PVMX_GDTENTRY64 VmxGdtEntry)
{
	PKGDTENTRY64 gdtEntry;

	/* Reject LDT or NULL entries */
	if ((Selector == 0) ||
		(Selector & SELECTOR_TABLE_INDEX) != 0)
	{
		VmxGdtEntry->Limit = VmxGdtEntry->AccessRights = 0;
		VmxGdtEntry->Base = 0;
		VmxGdtEntry->Selector = 0;
		VmxGdtEntry->Bits.Unusable = TRUE;
		return;
	}

	/* Read the GDT entry at the given selector, masking out the RPL bits. */
	gdtEntry = (PKGDTENTRY64)((uintptr_t)GdtBase + (Selector & ~RPL_MASK));

	/* Write the selector directly */
	VmxGdtEntry->Selector = Selector;

	/* Use the LSL intrinsic to read the segment limit */
	VmxGdtEntry->Limit = __segmentlimit(Selector);

	/*
	* Build the full 64-bit effective address, keeping in mind that only when
	* the System bit is unset, should this be done.
	*
	* NOTE: The Windows definition of KGDTENTRY64 is WRONG. The "System" field
	* is incorrectly defined at the position of where the AVL bit should be.
	* The actual location of the SYSTEM bit is encoded as the highest bit in
	* the "Type" field.
	*/
	VmxGdtEntry->Base = ((gdtEntry->Bytes.BaseHigh << 24) |
		(gdtEntry->Bytes.BaseMiddle << 16) |
		(gdtEntry->BaseLow)) & 0xFFFFFFFF;
	VmxGdtEntry->Base |= ((gdtEntry->Bits.Type & 0x10) == 0) ?
		((uintptr_t)gdtEntry->BaseUpper << 32) : 0;

	/* Load the access rights */
	VmxGdtEntry->AccessRights = 0;
	VmxGdtEntry->Bytes.Flags1 = gdtEntry->Bytes.Flags1;
	VmxGdtEntry->Bytes.Flags2 = gdtEntry->Bytes.Flags2;

	/* Finally, handle the VMX-specific bits */
	VmxGdtEntry->Bits.Reserved = 0;
	VmxGdtEntry->Bits.Unusable = !gdtEntry->Bits.Present;
}

/******************** Module Code ********************/