#include "VMCALL.h"
#include "VMCALL_Common.h"
#include "MemManage.h"
#include "VMShadow.h"
#include "EventLog.h"
#include "EventLog_Common.h"
#include "Process.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

typedef NTSTATUS(*fnActionHandler)(PVMM_DATA lpData, CR3 guestCR3, GUEST_VIRTUAL_ADDRESS buffer, SIZE_T bufferSize);

/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/
static NTSTATUS actionCheckPresence(PVMM_DATA lpData, CR3 guestCR3, GUEST_VIRTUAL_ADDRESS buffer, SIZE_T bufferSize);
static NTSTATUS actionRunAsRoot(PVMM_DATA lpData, CR3 guestCR3, GUEST_VIRTUAL_ADDRESS buffer, SIZE_T bufferSize);
static NTSTATUS actionShadowInProcess(PVMM_DATA lpData, CR3 guestCR3, GUEST_VIRTUAL_ADDRESS buffer, SIZE_T bufferSize);
static NTSTATUS actionGatherEvents(PVMM_DATA lpData, CR3 guestCR3, GUEST_VIRTUAL_ADDRESS buffer, SIZE_T bufferSize);

/******************** Action Handlers ********************/

static const fnActionHandler ACTION_HANDLERS[VMCALL_ACTION_COUNT] =
{
	[VMCALL_ACTION_CHECK_PRESENCE] = actionCheckPresence,
	[VMCALL_ACTION_RUN_AS_ROOT] = actionRunAsRoot,
	[VMCALL_ACTION_SHADOW_IN_PROCESS] = actionShadowInProcess,
	[VMCALL_ACTION_GATHER_EVENTS] = actionGatherEvents,
};

/******************** Public Code ********************/

BOOLEAN VMCALL_handle(PVMM_DATA lpData)
{
	UNREFERENCED_PARAMETER(lpData);

	BOOLEAN result = FALSE;

	/* This is used when the guest wants to send something to the host.
	 * A secret key is used to protect malicious (or unknown) actors from
	 * trying to do a hyper call. 
	 *
	 *	RCX = Secret Key
	 *	RDX = VMCALL Command Buffer
	 */
	if (VMCALL_KEY == lpData->guestContext.Rcx)
	{
		/* Attempt to read the guest command buffer. */
		CR3 guestCR3;
		__vmx_vmread(VMCS_GUEST_CR3, &guestCR3.Flags);

		/* Treat RDX of the guest as the pointer for the command. */
		VMCALL_COMMAND readCommand = { 0 };
		NTSTATUS status = MemManage_readVirtualAddress(&lpData->mmContext, guestCR3, lpData->guestContext.Rdx,
													   &readCommand, sizeof(readCommand));

		if (NT_SUCCESS(status))
		{
			/* Call the specific action handler for the command and put the result NTSTATUS into RAX. */
			if (readCommand.action < VMCALL_ACTION_COUNT)
			{
				lpData->guestContext.Rax = ACTION_HANDLERS[readCommand.action](lpData, guestCR3, 
					(GUEST_VIRTUAL_ADDRESS)readCommand.buffer, 
					readCommand.bufferSize);
			}
			else
			{
				lpData->guestContext.Rax = (ULONG64)STATUS_INVALID_PARAMETER;
			}

			result = TRUE;
		}
	}

	return result;
}

/******************** Module Code ********************/

static NTSTATUS actionCheckPresence(PVMM_DATA lpData, CR3 guestCR3, GUEST_VIRTUAL_ADDRESS buffer, SIZE_T bufferSize)
{
	UNREFERENCED_PARAMETER(lpData);
	UNREFERENCED_PARAMETER(guestCR3);
	UNREFERENCED_PARAMETER(buffer);
	UNREFERENCED_PARAMETER(bufferSize);

	return STATUS_SUCCESS;
}

static NTSTATUS actionRunAsRoot(PVMM_DATA lpData, CR3 guestCR3, GUEST_VIRTUAL_ADDRESS buffer, SIZE_T bufferSize)
{
	NTSTATUS status;

	if ((0 != buffer) && (sizeof(VM_PARAM_RUN_AS_ROOT) == bufferSize))
	{
		VM_PARAM_RUN_AS_ROOT params = { 0 };

		status = MemManage_readVirtualAddress(&lpData->mmContext, guestCR3, buffer, &params, sizeof(params));
		if (NT_SUCCESS(status))
		{
			/* Call the specified function (we will currently be in VMX ROOT). */
			if (NULL != params.callback)
			{
				status = params.callback(lpData, params.parameter);
			}
			else
			{
				status = STATUS_INVALID_PARAMETER;
			}
		}
	}
	else
	{
		status = STATUS_INVALID_PARAMETER;
	}

	return status;
}

static NTSTATUS actionShadowInProcess(PVMM_DATA lpData, CR3 guestCR3, GUEST_VIRTUAL_ADDRESS buffer, SIZE_T bufferSize)
{
	NTSTATUS status;

	if ((0 != buffer) && (sizeof(VM_PARAM_SHADOW_PROC) == bufferSize))
	{
		VM_PARAM_SHADOW_PROC params = { 0 };

		status = MemManage_readVirtualAddress(&lpData->mmContext, guestCR3, buffer, &params, sizeof(params));
		if (NT_SUCCESS(status))
		{

			/* Get the PEPROCESS of the target process. */
			PEPROCESS targetProcess;
			if (0 != params.procID)
			{
				status = PsLookupProcessByProcessId((HANDLE)params.procID, &targetProcess);
				if (NT_SUCCESS(status))
				{
					/* Tell the VMShadow module to hide the executable page at the specified
						* address, for the target process only. */
					status = VMShadow_hideExecInProcess(lpData,
						targetProcess,
						params.userTargetVA,
						params.kernelExecPageVA);

					ObDereferenceObject(targetProcess);
				}
			}
			else
			{
				status = STATUS_INVALID_PARAMETER;
			}
		}
	}
	else
	{
		status = STATUS_INVALID_PARAMETER;
	}

	return status;
}

static NTSTATUS actionGatherEvents(PVMM_DATA lpData, CR3 guestCR3, GUEST_VIRTUAL_ADDRESS buffer, SIZE_T bufferSize)
{
	UNREFERENCED_PARAMETER(lpData);
	UNREFERENCED_PARAMETER(guestCR3);
	UNREFERENCED_PARAMETER(buffer);
	UNREFERENCED_PARAMETER(bufferSize);

	NTSTATUS status = STATUS_UNSUCCESSFUL;

	//if ((0 != buffer) && (sizeof(VM_PARAM_GATHER_EVENTS) == bufferSize))
	//{
	//	VM_PARAM_GATHER_EVENTS params = { 0 };

	//	status = MemManage_readVirtualAddress(&lpData->mmContext, guestCR3, buffer, &params, sizeof(params));
	//	if (NT_SUCCESS(status))
	//	{
	//		/* Check to see if expected size is zero.
	//		 * if so, we return the current size of the event log. */
	//		if ((NULL != params.buffer) && (0 != params.bufferSize))
	//		{
	//			/* Here is a static buffer that is reserved for holding events,
	//			 * we use this to store events. */
	//			static UINT8 staticEventBuffer[EVENT_BUFFER_SIZE];

	//			/* Ensure we can fetch only enough for our static buffer size. */
	//			if (params.bufferSize > EVENT_BUFFER_SIZE)
	//			{
	//				params.bufferSize = EVENT_BUFFER_SIZE;
	//			}

	//			/* Read the events into our allocated buffer. */
	//			status = EventLog_retrieveAndClear(staticEventBuffer, params.bufferSize, &params.eventCount);

	//			if (0 == params.eventCount)
	//			{
	//				/* Unable to retrieve, due to no events being present. */
	//				status = STATUS_UNSUCCESSFUL;
	//			}

	//			if (NT_SUCCESS(status))
	//			{
	//				/* Now write the buffer to the guest VA specified. */
	//				status = MemManage_writeVirtualAddress(&lpData->mmContext,
	//					guestCR3,
	//					(GUEST_VIRTUAL_ADDRESS)params.buffer,
	//					staticEventBuffer,
	//					params.bufferSize);
	//			}
	//		}
	//		else
	//		{
	//			status = STATUS_INVALID_PARAMETER;
	//		}

	//		/* Write the parameters back to the guest, as they may have been modified. */
	//		if (NT_SUCCESS(status))
	//		{
	//			status = MemManage_writeVirtualAddress(&lpData->mmContext, guestCR3, buffer, &params, sizeof(params));
	//		}
	//	}
	//}
	//else
	//{
	//	status = STATUS_INVALID_PARAMETER;
	//}

	return status;
}