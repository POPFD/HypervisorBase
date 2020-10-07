#include <ntifs.h>
#include "EPT.h"
#include "Debug.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/


/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/
static UINT32 adjustEffectiveMemoryType(const PMTRR_RANGE mtrrTable, UINT64 pageAddress, UINT32 desiredType);

/******************** Public Code ********************/

void EPT_initialise(PEPT_CONFIG eptConfig, const PMTRR_RANGE mtrrTable)
{
	DEBUG_PRINT("Initialising the EPT for the virtual machine.\r\n");

	/* Initialise the linked list used for holding violation handlers. */
	InitializeListHead(&eptConfig->handlerList);

	/* Initialise the linked list used for holding split pages. */
	InitializeListHead(&eptConfig->dynamicSplitList);

	/* Create the EPT pointer for the structure. */
	eptConfig->eptPointer.PageWalkLength = 3;
	eptConfig->eptPointer.MemoryType = MEMORY_TYPE_WRITE_BACK;
	eptConfig->eptPointer.PageFrameNumber = MmGetPhysicalAddress(&eptConfig->PML4).QuadPart / PAGE_SIZE;

	/* Fill out the PML4 which covers the first 512GB of RAM. */
	eptConfig->PML4[0].ReadAccess = 1;
	eptConfig->PML4[0].WriteAccess = 1;
	eptConfig->PML4[0].ExecuteAccess = 1;

	/* Set the page frame number to the page index of the PDPT. */
	eptConfig->PML4[0].PageFrameNumber = MmGetPhysicalAddress(&eptConfig->PML3).QuadPart / PAGE_SIZE;

	/* Create a RWX PDPTE. */
	EPT_PML3_POINTER tempPML3P = { 0 };
	tempPML3P.ReadAccess = 1;
	tempPML3P.WriteAccess = 1;
	tempPML3P.ExecuteAccess = 1;

	/* Store the temporarily created PDPTE to each of the entries in the page directory pointer table. */
	__stosq((UINT64*)eptConfig->PML3, tempPML3P.Flags, EPT_PML3E_COUNT);

	/* Set the page frame numbers for each of the created PDPTEs so that they
	* link to their root respective page data entry. */
	for (UINT32 i = 0; i < EPT_PML3E_COUNT; i++)
	{
		eptConfig->PML3[i].PageFrameNumber = MmGetPhysicalAddress(&eptConfig->PML2[i][0]).QuadPart / PAGE_SIZE;
	}

	/* Create a large PDE. */
	EPT_PML2_ENTRY tempLargePML2E = { 0 };
	tempLargePML2E.ReadAccess = 1;
	tempLargePML2E.WriteAccess = 1;
	tempLargePML2E.ExecuteAccess = 1;
	tempLargePML2E.LargePage = 1;

	/* Store the temporarily create LARGE_PDE to each of the entries in the page directory table. */
	__stosq((UINT64*)eptConfig->PML2, tempLargePML2E.Flags, EPT_PML3E_COUNT * EPT_PML2E_COUNT);

	/* Loop every 1GB of RAM (described by the PDPTE). */
	for (UINT32 i = 0; i < EPT_PML3E_COUNT; i++)
	{
		/* Construct the EPT identity map for every 2MB of RAM. */
		for (UINT32 j = 0; j < EPT_PML2E_COUNT; j++)
		{
			eptConfig->PML2[i][j].PageFrameNumber = ((UINT64)i * EPT_PML3E_COUNT) + j;

			/* Calculate the page physical address, by using the page number and multiplying it
			* by the size of a LARGE_PAGE. */
			UINT64 largePageAddress = eptConfig->PML2[i][j].PageFrameNumber * SIZE_2MB;

			/* Adjust the type for each page entry based on the MTRR table.
			* We want to use writeback, unless the page address falls within a MTRR entry. */
			UINT32 adjustedType = adjustEffectiveMemoryType(mtrrTable, largePageAddress, MEMORY_TYPE_WRITE_BACK);

			eptConfig->PML2[i][j].MemoryType = adjustedType;
		}
	}
}

BOOLEAN EPT_handleViolation(PEPT_CONFIG eptConfig)
{
	/* Result indicates handled successfully. */
	BOOLEAN result = FALSE;

	/* Get the physical address of the page that caused the violation. */
	PHYSICAL_ADDRESS guestPA;
	__vmx_vmread(VMCS_GUEST_PHYSICAL_ADDRESS, (SIZE_T*)&guestPA.QuadPart);

	/* Search the list of EPT handlers and determine which one to call. */
	for (PLIST_ENTRY currentEntry = eptConfig->handlerList.Flink;
		currentEntry != &eptConfig->handlerList;
		currentEntry = currentEntry->Flink)
	{
		/* Use the CONTAINING_RECORD macro to get the actual record 
		 * the linked list is holding. */
		PEPT_HANDLER eptHandler = CONTAINING_RECORD(currentEntry, EPT_HANDLER, listEntry);

		/* Check to see if the physical address associated with the handler matches,
		 * if so we call that handler. */
		if (PAGE_ALIGN(guestPA.QuadPart) == PAGE_ALIGN(eptHandler->physicalAlign.QuadPart))
		{
			result = eptHandler->callback(eptConfig, eptHandler->userParameter);
			break;
		}
	}

	return result;
}

NTSTATUS EPT_addViolationHandler(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS guestPA, fnEPTHandlerCallback callback, PVOID userParameter)
{
	NTSTATUS status;

	if (NULL != callback)
	{
		/* Allocate a new handler structure that will be used for traversal later. */
		PEPT_HANDLER newHandler = (PEPT_HANDLER)ExAllocatePool(NonPagedPoolNx, sizeof(PEPT_HANDLER));
		if (NULL != newHandler)
		{
			newHandler->physicalAlign.QuadPart = (LONGLONG)PAGE_ALIGN(guestPA.QuadPart);
			newHandler->callback = callback;
			newHandler->userParameter = userParameter;

			/* Add this structure to the linked list of already existing handlers. */
			InsertHeadList(&eptConfig->handlerList, &newHandler->listEntry);
			status = STATUS_SUCCESS;
		}
		else
		{
			status = STATUS_NO_MEMORY;
		}
	}
	else
	{
		status = STATUS_INVALID_PARAMETER;
	}

	return status;
}

NTSTATUS EPT_splitLargePage(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS physicalAddress)
{
	NTSTATUS status;

	/* Find the PML2E that relates to the physical address. */
	PEPT_PML2_ENTRY targetPML2E = EPT_getPML2EFromAddress(eptConfig, physicalAddress);

	if (NULL != targetPML2E)
	{
		/* Check to see if the PDE is marked as a large page, if it isn't
		* then we don't have to split it as it is already done. */
		if (FALSE != targetPML2E->LargePage)
		{
			PEPT_DYNAMIC_SPLIT newSplit = (PEPT_DYNAMIC_SPLIT)ExAllocatePool(NonPagedPoolNx, sizeof(EPT_DYNAMIC_SPLIT));

			if (NULL != newSplit)
			{
				newSplit->pml2Entry = targetPML2E;

				/* Make a template for RWX. */
				EPT_PML1_ENTRY tempPML1 = { 0 };
				tempPML1.ReadAccess = 1;
				tempPML1.WriteAccess = 1;
				tempPML1.ExecuteAccess = 1;
				tempPML1.MemoryType = targetPML2E->MemoryType;
				tempPML1.IgnorePat = targetPML2E->IgnorePat;
				tempPML1.SuppressVe = targetPML2E->SuppressVe;

				/* Copy the template into all of the PML1 entries. */
				__stosq((PULONG64)&newSplit->PML1[0], tempPML1.Flags, EPT_PML1E_COUNT);

				/* Calculate the physical address of the PML2 entry. */
				UINT64 addressPML2E = targetPML2E->PageFrameNumber * SIZE_2MB;

				/* Calculate the page frame number of the first page in the table. */
				UINT64 basePageNumber = addressPML2E / PAGE_SIZE;

				/* Set page frame numbres for each of the entries within the PML1 table. */
				for (UINT32 i = 0; i < EPT_PML1E_COUNT; i++)
				{
					/* Convert the 2MB page frame number to the 4096 page entry number, plus the offset into the frame. */
					newSplit->PML1[i].PageFrameNumber = basePageNumber + i;
				}

				/* Create a new PML2 pointer that will replace the 2MB entry with a pointer to the newly
				* created PML1 table. */
				EPT_PML2_POINTER tempPML2 = { 0 };
				tempPML2.ReadAccess = 1;
				tempPML2.WriteAccess = 1;
				tempPML2.ExecuteAccess = 1;
				tempPML2.PageFrameNumber = MmGetPhysicalAddress(&newSplit->PML1[0]).QuadPart / PAGE_SIZE;

				/* Replace the old entry with the new split pointer. */
				targetPML2E->Flags = tempPML2.Flags;

				/* Add the split entry to the list of split pages, so we can de-allocate them later. */
				InsertHeadList(&eptConfig->dynamicSplitList, &newSplit->listEntry);

				status = STATUS_SUCCESS;
			}
			else
			{
				status = STATUS_NO_MEMORY;
			}
		}
		else
		{
			/* Page is already split, do nothing. */
			status = STATUS_ALREADY_COMPLETE;
		}
	}
	else
	{
		status = STATUS_INVALID_ADDRESS;
	}

	return status;
}

PEPT_PML2_ENTRY EPT_getPML2EFromAddress(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS physicalAddress)
{
	PEPT_PML2_ENTRY result;

	UINT64 unsignedAddr = physicalAddress.QuadPart;

	UINT64 indexPML4 = ADDRMASK_EPT_PML4_INDEX(unsignedAddr);

	if (indexPML4 == 0)
	{
		UINT64 indexPML3 = ADDRMASK_EPT_PML3_INDEX(unsignedAddr);
		UINT64 indexPML2 = ADDRMASK_EPT_PML2_INDEX(unsignedAddr);

		result = &(eptConfig->PML2[indexPML3][indexPML2]);
	}
	else
	{
		/* Cannot support an entry above 512GB. */
		result = NULL;
	}

	return result;
}

PEPT_PML1_ENTRY EPT_getPML1EFromAddress(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS physicalAddress)
{
	PEPT_PML1_ENTRY result = NULL;

	UINT64 unsignedAddr = physicalAddress.QuadPart;

	UINT64 indexPML4 = ADDRMASK_EPT_PML4_INDEX(unsignedAddr);

	/* Ensure the entry is within the first 512GB. */
	if (indexPML4 == 0)
	{
		UINT64 indexPML3 = ADDRMASK_EPT_PML3_INDEX(unsignedAddr);
		UINT64 indexPML2 = ADDRMASK_EPT_PML2_INDEX(unsignedAddr);

		PEPT_PML2_ENTRY entryPML2 = &eptConfig->PML2[indexPML3][indexPML2];

		/* Ensure that PML2 entry is not a large page, if it is this means we are at the lowest level already
		* so it is impossible to get PML1E as it doesn't exist. */
		if (FALSE == entryPML2->LargePage)
		{
			/* We have determined it is not the lowest level, so cast the entry to a pointer to the level 1 table. */
			PEPT_PML2_POINTER pointerPML2 = (PEPT_PML2_POINTER)entryPML2;

			/* Convert the physical address to a virtual address. */
			PHYSICAL_ADDRESS physicalPML1;
			physicalPML1.QuadPart = pointerPML2->PageFrameNumber * PAGE_SIZE;

			PEPT_PML1_ENTRY entryPML1 = (PEPT_PML1_ENTRY)MmGetVirtualForPhysical(physicalPML1);

			if (NULL != entryPML1)
			{
				UINT64 indexPML1 = ADDRMASK_EPT_PML1_INDEX(unsignedAddr);

				result = &entryPML1[indexPML1];
			}
		}
	}

	return result;
}

/******************** Module Code ********************/

static UINT32 adjustEffectiveMemoryType(const PMTRR_RANGE mtrrTable, UINT64 pageAddress, UINT32 desiredType)
{
	for (UINT32 i = 0; i < IA32_MTRR_VARIABLE_COUNT; i++)
	{
		/* Check to see if the MTRR is active. */
		if (mtrrTable[i].Valid != FALSE)
		{
			/* Check if the page address falls within the boundary of the MTRR.
			* If a single MTRR touches it, we need to override the desired PDE's type. */
			if (((pageAddress + (SIZE_2MB - 1) >= mtrrTable[i].PhysicalAddressMin) &&
				(pageAddress <= mtrrTable[i].PhysicalAddressMax)))
			{
				desiredType = mtrrTable[i].Type;
			}
		}
	}

	return desiredType;
}
