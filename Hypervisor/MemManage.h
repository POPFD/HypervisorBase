#pragma once
#include <wdm.h>
#include "ia32.h"

/******************** Public Typedefs ********************/
typedef struct _MM_CONTEXT
{
	/* The reserved page that will be used for storing data in. */
	PVOID reservedPage;

	/* PTE that belongs to the reserved page. */
	PTE_64* reservedPagePte;
} MM_CONTEXT, *PMM_CONTEXT;

/* Each level of a paging structure. */
typedef enum
{
	PT_LEVEL_PTE = 1,
	PT_LEVEL_PDE = 2,
	PT_LEVEL_PDPTE = 3,
	PT_LEVEL_PML4E = 4
} PT_LEVEL;

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
NTSTATUS MemManage_init(PMM_CONTEXT context, CR3 hostCR3);
NTSTATUS MemManage_readVirtualAddress(PMM_CONTEXT context, CR3 tableBase, PVOID guestVA, PVOID buffer, SIZE_T size);
NTSTATUS MemManage_writeVirtualAddress(PMM_CONTEXT context, CR3 tableBase, PVOID guestVA, CONST PVOID buffer, SIZE_T size);
NTSTATUS MemManage_getPAForGuest(PMM_CONTEXT context, CR3 guestTableBase, PVOID guestVA, PHYSICAL_ADDRESS* physAddr);
PT_ENTRY_64* MemManage_getPTEFromVA(CR3 tableBase, PVOID virtualAddress, PT_LEVEL* level);
CR3 MemManage_getPageTableBase(PEPROCESS process);