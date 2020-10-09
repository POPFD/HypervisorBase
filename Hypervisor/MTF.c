#include <ntifs.h>
#include <intrin.h>
#include "MTF.h"
#include "Debug.h"
#include "ia32.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

typedef struct _MTF_HANDLER
{
	/* Start/End addresses of the range monitored. */
	PUINT8 rangeStart;
	PUINT8 rangeEnd;

	/* Callback of the handler, that will be called for processing the trap. */
	fnMTFHandlerCallback callback;

	/* Buffer that can be used for user-supplied configs. */
	PVOID userParameter;

	/* Linked list entry, used for traversal. */
	LIST_ENTRY listEntry;
} MTF_HANDLER, *PMTF_HANDLER;

/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/


/******************** Public Code ********************/

void MTF_initialise(PMTF_CONFIG mtfConfig)
{
	/* By default MTF tracing will be disabled, however we still
	 * need to initialise it when the VMM starts up. 
	 * 
	 * This is so we can keep track of any MTF handlers are ran at runtime
	 * before MTF tracing is enabled. */
	InitializeListHead(&mtfConfig->handlerList);
}

BOOLEAN MTF_handleTrap(PMTF_CONFIG mtfConfig)
{
	/* Result indicates handled successfully. */
	BOOLEAN result = FALSE;

	/* Get the value of the guest RIP. */
	UINT64 guestRIP;
	__vmx_vmread(VMCS_GUEST_RIP, &guestRIP);

	/* Search the list of MTF handler and determine which one to call. */
	for (PLIST_ENTRY currentEntry = mtfConfig->handlerList.Flink;
		currentEntry != &mtfConfig->handlerList;
		currentEntry = currentEntry->Flink)
	{
		/* Get the handler structure. */
		PMTF_HANDLER mtfHandler = CONTAINING_RECORD(currentEntry, MTF_HANDLER, listEntry);

		/* Check to see if the guest RIP is within these bounds. */
		if ((guestRIP >= (SIZE_T)mtfHandler->rangeStart) && (guestRIP <= (SIZE_T)mtfHandler->rangeEnd))
		{
			result = mtfHandler->callback(mtfConfig, mtfHandler->userParameter);
			break;
		}
	}

	return result;
}

NTSTATUS MTF_addHandler(PMTF_CONFIG mtfConfig, PUINT8 rangeStart, PUINT8 rangeEnd, fnMTFHandlerCallback callback, PVOID userParameter)
{
	NTSTATUS status;

	if (NULL != callback)
	{
		PMTF_HANDLER newHandler = (PMTF_HANDLER)ExAllocatePool(NonPagedPoolNx, sizeof(MTF_HANDLER));
		if (NULL != newHandler)
		{
			newHandler->rangeStart = rangeStart;
			newHandler->rangeEnd = rangeEnd;
			newHandler->callback = callback;
			newHandler->userParameter = userParameter;

			/* Add this structure to the linked list of already existing handlers. */
			InsertHeadList(&mtfConfig->handlerList, &newHandler->listEntry);
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

void MTF_setTracingEnabled(BOOLEAN enabled)
{
	IA32_VMX_PROCBASED_CTLS_REGISTER procCtls;
	__vmx_vmread(VMCS_CTRL_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, &procCtls.Flags);

	procCtls.MonitorTrapFlag = enabled;
	__vmx_vmwrite(VMCS_CTRL_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, procCtls.Flags);
}

/******************** Module Code ********************/