#include <intrin.h>
#include <ntddk.h>
#include "MemManage.h"
#include "ia32.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

/* Each level of a paging structure. */
typedef enum
{
	PT_LEVEL_PTE = 1,
	PT_LEVEL_PDE = 2,
	PT_LEVEL_PDPTE = 3,
	PT_LEVEL_PML4E = 4
} PT_LEVEL;


/******************** Module Constants ********************/
#define OFFSET_DIRECTORY_TABLE_BASE 0x028
#define OFFSET_USER_DIR_TABLE 0x280

#define SIZE_2MB (2 * 1024 * 1024)

/* Counts of how many entries are in a table. */
#define PAGING_TABLE_ENTRY_COUNT 512
#define PAGING_PML4E_COUNT		512
#define PAGING_PML3E_COUNT		512
#define PAGING_PML2E_COUNT		512
#define PAGING_PML1E_COUNT		512

/* Calculates the offset into the PDE (PML1) structure. */
#define ADDRMASK_PML1_OFFSET(_VAR_) ((SIZE_T)_VAR_ & 0xFFFULL)

/* Calculates the index of the PDE (PML1) within the PDT structure. */
#define ADDRMASK_PML1_INDEX(_VAR_) (((SIZE_T)_VAR_ & 0x1FF000ULL) >> 12)

/* Calculates the index of the PDT (PML2) within the PDPT*/
#define ADDRMASK_PML2_INDEX(_VAR_) (((SIZE_T)_VAR_ & 0x3FE00000ULL) >> 21)

/* Calculates the index of the PDPT (PML3) within the PML4 */
#define ADDRMASK_PML3_INDEX(_VAR_) (((SIZE_T)_VAR_ & 0x7FC0000000ULL) >> 30)

/* Calculates the index of PML4. */
#define ADDRMASK_PML4_INDEX(_VAR_) (((SIZE_T)_VAR_ & 0xFF8000000000ULL) >> 39)

/******************** Module Variables ********************/


/******************** Module Prototypes ********************/
static NTSTATUS split2MbPage(PDE_2MB_64* pdeLarge);
static PT_ENTRY_64* getPTEFromVA(CR3 tableBase, PVOID virtualAddress, PT_LEVEL* level);
static UINT64 physicalFromVirtual(VOID* virtualAddress);
static VOID* virtualFromPhysical(UINT64 physicalAddress);

/******************** Public Code ********************/

NTSTATUS MemManage_init(PMM_CONTEXT context, CR3 hostCR3)
{
	NTSTATUS status = STATUS_SUCCESS;

	/* Reserve a single page, this will be used for mapping in the guest 
	 * page data into. */
	context->reservedPage = MmAllocateMappingAddress(PAGE_SIZE, 0);
	if (NULL != context->reservedPage)
	{
		/* Attempt to get the page table entry of the reserved page,
		 * we need to ensure this is not a 2MB large page, if so we must split it. */
		PT_LEVEL tableLevel;
		PT_ENTRY_64* reservedPagePTE = getPTEFromVA(hostCR3, context->reservedPage, &tableLevel);
		if (PT_LEVEL_PDE == tableLevel)
		{
			/* A split must take place. */
			status = split2MbPage((PDE_2MB_64*)reservedPagePTE);

			/* Get the new PTE. */
			reservedPagePTE = getPTEFromVA(hostCR3, context->reservedPage, &tableLevel);
		}

		/* Ensure we are still in success state, splitting could have failed. */
		if (NT_SUCCESS(status))
		{

		}
	}
	else
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
	}

	return status;
}

UINT64 MemManage_getPageTableBase(PEPROCESS process)
{
	/* As KVA shadowing is used for CR3 as a mitigation for spectre/meltdown
	* we cannot use the CR3 as it is a shadowed table instead. Directly
	* access to DirectoryTableBase of the process that caused
	* the VMExit.
	*
	* UserDirectoryTableBase at a fixed offset, EPROCESS->Pcb.UserDirectoryTableBase
	* This was found using the following windbg commands:
	* dt nt!_EPROCESS
	* dt nt!_KPROCESS
	*
	* This is subject to change between Windows versions. */

	/* If running as admin, UserDirectoryTableBase will be set as 1 and KVA shadowing will not be present,
	* therefor we should use the directory table base (kernel). Again due to KVA shadowing... */
	UINT64 tableBase;

	tableBase = *((UINT64*)((UINT64)process + OFFSET_USER_DIR_TABLE));
	tableBase &= ~0xFFF;
	if (0 == tableBase)
	{
		tableBase = *((UINT64*)((UINT64)process + OFFSET_DIRECTORY_TABLE_BASE));
		tableBase &= ~0xFFF;
	}

	return tableBase;
}


/******************** Module Code ********************/

static NTSTATUS split2MbPage(PDE_2MB_64* pdeLarge)
{
	NTSTATUS status;

	/* Allocate a new page table, this will be used for splitting the 2MB
	 * entry into 512 4kb pages, 512 8byte entries = one page. */
	PTE_64* pt = MmAllocateMappingAddress(PAGE_SIZE, 0);
	if (NULL != pt)
	{
		/* Close the large page bit and then propagate the current permissions to
		 * all the entries in our newly allocated PT. */
		pdeLarge->LargePage = FALSE;
		__stosq((UINT64*)pt, pdeLarge->Flags, PAGING_TABLE_ENTRY_COUNT);

		/* Calculate the physical address of where the PDPTE will be. */
		UINT64 base = pdeLarge->PageFrameNumber * SIZE_2MB;

		/* Update the page frame mapping for each PTE. */
		for (SIZE_T i = 0; i < PAGING_TABLE_ENTRY_COUNT; i++)
		{
			UINT64 physAddrToMap = base + (i * PAGE_SIZE);
			pt[i].PageFrameNumber = physAddrToMap / PAGE_SIZE;
		}

		/* Now update the convert the largePDE to a standard entry. */
		PDE_64* pde = (PDE_64*)pdeLarge;
		pde->Reserved1 = 0;
		pde->Reserved2 = 0;
		pde->PageFrameNumber = physicalFromVirtual(pt) / PAGE_SIZE;
		status = STATUS_SUCCESS;
	}
	else
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
	}
	
	return status;
}

static PT_ENTRY_64* getPTEFromVA(CR3 tableBase, PVOID virtualAddress, PT_LEVEL* level)
{
	PT_ENTRY_64* result = NULL;

	/* Gather the indexes for the page tables from the VA. */
	UINT64 indexPML4 = ADDRMASK_PML4_INDEX(virtualAddress);
	UINT64 indexPML3 = ADDRMASK_PML3_INDEX(virtualAddress);
	UINT64 indexPML2 = ADDRMASK_PML2_INDEX(virtualAddress);
	UINT64 indexPML1 = ADDRMASK_PML1_INDEX(virtualAddress);

	/* Read the PML4 from the target. */
	PML4E_64* pml4 = virtualFromPhysical(tableBase.AddressOfPageDirectory * PAGE_SIZE);
	PML4E_64* pml4e = &pml4[indexPML4];
	if (TRUE == pml4e->Present)
	{
		/* Indicate we have atleast got the entry. */
		result = (PT_ENTRY_64*)pml4e;
		*level = PT_LEVEL_PML4E;

		/* Read PML3 from the guest. */
		PDPTE_64* pdpt = virtualFromPhysical(pml4e->PageFrameNumber * PAGE_SIZE);
		PDPTE_64* pdpte = &pdpt[indexPML3];
		if (TRUE == pdpte->Present)
		{
			result = (PT_ENTRY_64*)pdpte;
			*level = PT_LEVEL_PDPTE;

			/* Read PML2 from the guest. */
			PDE_64* pd = virtualFromPhysical(pdpte->PageFrameNumber * PAGE_SIZE);
			PDE_64* pde = &pd[indexPML2];
			if ((FALSE == pdpte->LargePage) && (TRUE == pde->Present))
			{
				result = (PT_ENTRY_64*)pde;
				*level = PT_LEVEL_PDE;

				/* Read PML1 from the guest. */
				PTE_64* pt = virtualFromPhysical(pde->PageFrameNumber * PAGE_SIZE);
				PTE_64* pte = &pt[indexPML1];
				if ((FALSE == pde->LargePage) && (TRUE == pte->Present))
				{
					result = (PT_ENTRY_64*)pte;
					*level = PT_LEVEL_PTE;
				}
			}
		}
	}

	return result;
}

static UINT64 physicalFromVirtual(VOID* virtualAddress)
{
	return MmGetPhysicalAddress(virtualAddress).QuadPart;
}

static VOID* virtualFromPhysical(UINT64 physicalAddress)
{
	PHYSICAL_ADDRESS pa;
	pa.QuadPart = physicalAddress;

	return MmGetVirtualForPhysical(pa);
}
