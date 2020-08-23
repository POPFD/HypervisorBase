#include "VMCALL.h"
#include "VMCALL_Common.h"
#include "MemManage.h"
#include "VMShadow.h"
#include "Process.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

typedef NTSTATUS(*fnActionHandler)(PVMM_DATA lpData, PVOID buffer, SIZE_T bufferSize);

/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/
static NTSTATUS actionShadowInProcess(PVMM_DATA lpData, PVOID buffer, SIZE_T bufferSize);

/******************** Action Handlers ********************/

static const fnActionHandler ACTION_HANDLERS[VMCALL_ACTION_COUNT] =
{
	[VMCALL_ACTION_SHADOW_IN_PROCESS] = actionShadowInProcess,
};

/******************** Public Code ********************/

BOOLEAN VMCALL_handle(PVMM_DATA lpData, PCONTEXT guestContext)
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
	if (VMCALL_KEY == guestContext->Rcx)
	{
		if (FALSE == KD_DEBUGGER_NOT_PRESENT)
		{
			DbgBreakPoint();
		}

		PVMCALL_COMMAND guestCommand = (PVMCALL_COMMAND)guestContext->Rdx;

		/* Call the specific action handler for the command and put the result NTSTATUS into RAX. */
		if (guestCommand->action < VMCALL_ACTION_COUNT)
		{
			guestContext->Rax = ACTION_HANDLERS[guestCommand->action](lpData, guestCommand->buffer, guestCommand->bufferSize);
		}
		else
		{
			guestContext->Rax = (ULONG64)STATUS_INVALID_PARAMETER;
		}

		result = TRUE;
	}

	return result;
}

/******************** Module Code ********************/

static NTSTATUS actionShadowInProcess(PVMM_DATA lpData, PVOID buffer, SIZE_T bufferSize)
{
	NTSTATUS status;

	if ((NULL != buffer) && (sizeof(VM_PARAM_SHADOW_PROC) == bufferSize))
	{
		PVM_PARAM_SHADOW_PROC params = (PVM_PARAM_SHADOW_PROC)buffer;

		/* Get the PEPROCESS of the target process. */
		PEPROCESS targetProcess;
		status = PsLookupProcessByProcessId((HANDLE)params->procID, &targetProcess);
		if (NT_SUCCESS(status))
		{
			/* Tell the VMShadow module to hide the executable page at the specified
				* address, for the target process only. */
			status = VMShadow_hideExecInProcess(&lpData->eptConfig,
												targetProcess,
												params->userTargetVA, 
												params->kernelExecPageVA);

			ObDereferenceObject(targetProcess);
		}
	}
	else
	{
		status = STATUS_INVALID_PARAMETER;
	}

	return status;
}
