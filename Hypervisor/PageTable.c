#include <ntifs.h>
#include "PageTable.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/


/******************** Module Constants ********************/
#define PML4E_COUNT 512

/******************** Module Variables ********************/

static DECLSPEC_ALIGN(PAGE_SIZE) PML4E_64 vmPML4[PML4E_COUNT] = { 0 };

/******************** Module Prototypes ********************/


/******************** Public Code ********************/

NTSTATUS PageTable_init(CR3 originalCR3, CR3* newCR3)
{
	/* We emulate providing our own page table,
	 * to do this, we copy the original hosts PML4E
	 * and provide our own CR3, this should be enough. */
	NTSTATUS status;

	/* Get the virtual address of the original hosts PML4. */
	PHYSICAL_ADDRESS physOrigPML4;
	physOrigPML4.QuadPart = originalCR3.AddressOfPageDirectory * PAGE_SIZE;

	PML4E_64* originalPML4 = (PML4E_64*)MmGetVirtualForPhysical(physOrigPML4);
	if (NULL != originalPML4)
	{
		/* Copy the PML4E entries from the host/original CR3. */
		for (SIZE_T i = 0; i < PML4E_COUNT; i++)
		{
			vmPML4[i] = originalPML4[i];
		}

		/* Calculate the physical address of our PML4 table. */
		PHYSICAL_ADDRESS physNewPML4;
		physNewPML4 = MmGetPhysicalAddress(vmPML4);

		/* Create the new CR3 that will indicate to our PML4 base. */
		newCR3->Flags = 0;
		newCR3->AddressOfPageDirectory = (physNewPML4.QuadPart) / PAGE_SIZE;

		status = STATUS_SUCCESS;
	}
	else
	{
		status = STATUS_INVALID_ADDRESS;
	}

	return status;
}


/******************** Module Code ********************/