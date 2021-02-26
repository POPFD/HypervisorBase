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

typedef SIZE_T HOST_PHYS_ADDRESS;
typedef SIZE_T GUEST_VIRTUAL_ADDRESS;

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
NTSTATUS MemManage_init(PMM_CONTEXT context, CR3 hostCR3);
void MemManage_uninit(PMM_CONTEXT context);
NTSTATUS MemManage_readVirtualAddress(PMM_CONTEXT context, CR3 tableBase, GUEST_VIRTUAL_ADDRESS guestVA, PVOID buffer, SIZE_T size);
NTSTATUS MemManage_writeVirtualAddress(PMM_CONTEXT context, CR3 tableBase, GUEST_VIRTUAL_ADDRESS guestVA, PVOID buffer, SIZE_T size);
NTSTATUS MemManage_readPhysicalAddress(PMM_CONTEXT context, HOST_PHYS_ADDRESS physicalAddress, VOID* buffer, SIZE_T bytesToCopy);
NTSTATUS MemManage_writePhysicalAddress(PMM_CONTEXT context, HOST_PHYS_ADDRESS physicalAddress, VOID* buffer, SIZE_T bytesToCopy);
CR3 MemManage_getPageTableBase(PEPROCESS process);