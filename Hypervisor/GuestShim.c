#include "GuestShim.h"
#include "Debug.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

typedef enum
{
	PT_LEVEL_PTE = 1,
	PT_LEVEL_PDE = 2,
	PT_LEVEL_PDPTE = 3,
	PT_LEVEL_PML4E = 4
} PT_LEVEL;

/******************** Module Constants ********************/

#define  SIZE_1GB    0x40000000
#define  SIZE_2MB    0x00200000

/* Calculates the offset into the PT (PML1) table. */
#define ADDRMASK_PML1_OFFSET(_VAR_) ((SIZE_T)_VAR_ & 0xFFFULL)

/* Calculates the offset into the PD (PML2) table. */
#define ADDRMASK_PML2_OFFSET(_VAR_) ((SIZE_T)_VAR_ & 0x1FFFFFULL)

/* Calculates the offset into the PDPT (PML3) table. */
#define ADDRMASK_PML3_OFFSET(_VAR_) ((SIZE_T)_VAR_ & 0x3FFFFFFFULL)

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
static HOST_PHYS_ADDRESS getHostPAFromGuestVA(PMM_CONTEXT mmContext, CR3 guestCR3, PVOID guestVA);
static PT_ENTRY_64 getGuestPTEFromVA(PMM_CONTEXT mmContext, CR3 guestCR3, PVOID guestVA, PT_LEVEL* level);

/******************** Public Code ********************/
HOST_PHYS_ADDRESS GuestShim_GuestUVAToHPA(PMM_CONTEXT mmContext, CR3 userCR3, GUEST_VIRTUAL_ADDRESS guestAddress)
{
	/* Converts a guest user virtual address
	 * to a host physical address. 
	 *
	 * Same as above, guestPA = hostPA. */
	return getHostPAFromGuestVA(mmContext, userCR3, (PVOID)guestAddress);
}

/******************** Module Code ********************/

static HOST_PHYS_ADDRESS getHostPAFromGuestVA(PMM_CONTEXT mmContext, CR3 guestCR3, PVOID guestVA)
{
	HOST_PHYS_ADDRESS result = 0;

	/* Attempt to get the page table entry and level from the guest,
	 * from that we can calculate where in host physical memory it is. 
	 * This is because in host context, all physical memory is identity mapped. */
	PT_LEVEL tableLevel;
	PT_ENTRY_64 guestEntry = getGuestPTEFromVA(mmContext, guestCR3, guestVA, &tableLevel);
	if (0 != guestEntry.Flags)
	{

		/* Check to see which level we retrieved from the guest mapping table. */
		switch (tableLevel)
		{
			case PT_LEVEL_PML4E:
			{
				/* Something went wrong, there should never be a PML4E
				 * and no entries below. */
				DbgBreakPoint();
				break;
			}

			case PT_LEVEL_PDPTE:
			{
				/* If it is present it must mean below is a 1GB large page
				 * as such we calculate where our address would be. */
				PDPTE_1GB_64 guestPDPTE;
				guestPDPTE.Flags = guestEntry.Flags;
				if (TRUE == guestPDPTE.Present)
				{
					/* Calculate the base of where the page begins. */
					result = (guestPDPTE.PageFrameNumber * SIZE_1GB);

					/* Add the offset from the VA. */
					/* TODO: Verify this is how offset calc should work on large pages. */
					result += ADDRMASK_PML3_OFFSET(guestVA);
				}
				break;
			}

			case PT_LEVEL_PDE:
			{
				/* If it is present it must mean below is a 2MB large page
				 * as such we calculate where our address would be. */
				PDE_2MB_64 guestPDE;
				guestPDE.Flags = guestEntry.Flags;
				if (TRUE == guestPDE.Present)
				{
					/* Calculate the base of where the page is. */
					result = (guestPDE.PageFrameNumber * SIZE_2MB);

					/* Add the offset from the VA. */
					result += ADDRMASK_PML2_OFFSET(guestVA);
				}
				break;
			}

			case PT_LEVEL_PTE:
			{
				/* If it is present, this is a 4KB page. */
				PTE_64 guestPTE;
				guestPTE.Flags = guestEntry.Flags;
				if (TRUE == guestPTE.Present)
				{
					/* Calculate the base of where the page is. */
					result = (guestPTE.PageFrameNumber * PAGE_SIZE);

					/* Add the offset from the VA. */
					result += ADDRMASK_PML1_OFFSET(guestVA);
				}
				break;
			}

			default:
			{
				/* Something went wrong. */
				DbgBreakPoint();
				break;
			}
		}

	}

	return result;
}

static PT_ENTRY_64 getGuestPTEFromVA(PMM_CONTEXT mmContext, CR3 guestCR3, PVOID guestVA, PT_LEVEL* level)
{
	PT_ENTRY_64 result = { 0 };

	/* Calculated the indexes for each of the tables in the paging structure. */
	SIZE_T indexPML4 = ADDRMASK_PML4_INDEX(guestVA);
	SIZE_T indexPML3 = ADDRMASK_PML3_INDEX(guestVA);
	SIZE_T indexPML2 = ADDRMASK_PML2_INDEX(guestVA);
	SIZE_T indexPML1 = ADDRMASK_PML1_INDEX(guestVA);

	/* Read the PML4 address of our paging table.
	 * As we are identity mapped, VA -> PA as we are identity mapped. */
	PML4E_64* pml4 = (PML4E_64*)(guestCR3.AddressOfPageDirectory * PAGE_SIZE);
	HOST_PHYS_ADDRESS physPML4E = (HOST_PHYS_ADDRESS)&pml4[indexPML4];

	PML4E_64 readPML4E;
	NTSTATUS status = MemManage_readPhysicalAddress(mmContext, physPML4E, &readPML4E, sizeof(readPML4E));

	/* Set the initial level we were able to traverse down to as PML4,
	 * We then check if it's present. If so we can traverse the next table. */
	if (NT_SUCCESS(status) && (readPML4E.Present))
	{
		result.Flags = readPML4E.Flags;
		*level = PT_LEVEL_PML4E;

		/* Read PML3 */
		PDPTE_64* pdpt = (PDPTE_64*)(readPML4E.PageFrameNumber * PAGE_SIZE);
		HOST_PHYS_ADDRESS physPDPTE = (HOST_PHYS_ADDRESS)&pdpt[indexPML3];

		PDPTE_64 readPDPTE;
		status = MemManage_readPhysicalAddress(mmContext, physPDPTE, &readPDPTE, sizeof(readPDPTE));

		/* Only attempt to get lower level if present. */
		if (NT_SUCCESS(status) && (TRUE == readPDPTE.Present))
		{
			result.Flags = readPDPTE.Flags;
			*level = PT_LEVEL_PDPTE;

			/* If not a large page that means we can traverse lower. */
			if (FALSE == readPDPTE.LargePage)
			{
				/* Read PML2 */
				PDE_64* pd = (PDE_64*)(readPDPTE.PageFrameNumber * PAGE_SIZE);
				HOST_PHYS_ADDRESS physPDE = (HOST_PHYS_ADDRESS)&pd[indexPML2];

				PDE_64 readPDE;
				status = MemManage_readPhysicalAddress(mmContext, physPDE, &readPDE, sizeof(readPDE));

				/* Only attempt to go lower if present. */
				if (NT_SUCCESS(status) && (TRUE == readPDE.Present))
				{
					result.Flags = readPDE.Flags;
					*level = PT_LEVEL_PDE;

					/* If not a large page, that means we can traverse lower. */
					if (FALSE == readPDE.LargePage)
					{
						/* Read PML1 */
						PTE_64* pt = (PTE_64*)(readPDE.PageFrameNumber * PAGE_SIZE);
						HOST_PHYS_ADDRESS physPTE = (HOST_PHYS_ADDRESS)&pt[indexPML1];

						PTE_64 readPTE;
						status = MemManage_readPhysicalAddress(mmContext, physPTE, &readPTE, sizeof(readPTE));

						if (NT_SUCCESS(status))
						{
							result.Flags = readPDE.Flags;
							*level = PT_LEVEL_PTE;
						}
					}
				}
				else
				{
					result.Flags = 0;
				}
			}
		}
		else
		{
			result.Flags = 0;
		}
	}
	else
	{
		result.Flags = 0;
	}

	return result;
}

