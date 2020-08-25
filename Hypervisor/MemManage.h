#pragma once
#include <wdm.h>
#include "ia32.h"

/******************** Public Typedefs ********************/
typedef struct _MM_CONTEXT
{
	PVOID reservedPage;
} MM_CONTEXT, *PMM_CONTEXT;

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
NTSTATUS MemManage_init(PMM_CONTEXT context, CR3 hostCR3);