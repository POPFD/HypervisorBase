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

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
NTSTATUS MemManage_init(PMM_CONTEXT context, CR3 hostCR3);
CR3 MemManage_getPageTableBase(PEPROCESS process);