#include "VMCALL.h"
#include "VMCALL_Common.h"
#include "MemManage.h"
#include "Process.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

typedef NTSTATUS(*fnActionHandler)(PVOID buffer, SIZE_T bufferSize);

/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/
static NTSTATUS actionGetProcessBase(PVOID buffer, SIZE_T bufferSize);
static NTSTATUS actionReadUserMemory(PVOID buffer, SIZE_T bufferSize);
static NTSTATUS actionWriteUserMemory(PVOID buffer, SIZE_T bufferSize);
static NTSTATUS actionShadowKernel(PVOID buffer, SIZE_T bufferSize);

static NTSTATUS getGuestBuffer(PEPROCESS process, PVOID buffer, SIZE_T bufferSize, PVOID* hostBuffer);
/******************** Action Handlers ********************/

static const fnActionHandler ACTION_HANDLERS[VMCALL_ACTION_COUNT] =
{
	[VMCALL_ACTION_GET_PROCESS_BASE] = actionGetProcessBase,
	[VMCALL_ACTION_READ_USER_MEMORY] = actionReadUserMemory,
	[VMCALL_ACTION_WRITE_USER_MEMORY] = actionWriteUserMemory,
	[VMCALL_ACTION_SHADOW_KERNEL] = actionShadowKernel,
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

		PVMCALL_COMMAND guestCommandAddress = (PVMCALL_COMMAND)guestContext->Rdx;

		/* Get the current processes address. */
		PEPROCESS currentProcess = PsGetCurrentProcess();

		/* We need to read the guest's virtual memory into the host's address space. */
		VMCALL_COMMAND hostCommandBuffer;
		NTSTATUS status = MemManage_readVirtualAddress(currentProcess, guestCommandAddress, sizeof(VMCALL_COMMAND), &hostCommandBuffer);
		if (NT_SUCCESS(status))
		{
			/* Attempt to get the parameter buffer, if there is one. */
			PVOID hostParameterBuffer = NULL;
			if (NULL != hostCommandBuffer.buffer)
			{
				status = getGuestBuffer(currentProcess, hostCommandBuffer.buffer, hostCommandBuffer.bufferSize, &hostParameterBuffer);
			}

			/* Call the action handler. */
			if (NT_SUCCESS(status))
			{
				/* Call the specific action handler for the command and put the result NTSTATUS into RAX. */
				if (hostCommandBuffer.action < VMCALL_ACTION_COUNT)
				{
					guestContext->Rax = ACTION_HANDLERS[hostCommandBuffer.action](hostParameterBuffer, hostCommandBuffer.bufferSize);
				}
				else
				{
					guestContext->Rax = (ULONG64)STATUS_INVALID_PARAMETER;
				}

				/* Copy the host parameter buffer back into the guest. */
				if (NULL != hostCommandBuffer.buffer)
				{
					/* Write the data back to guest. */
					status = MemManage_writeVirtualAddress(currentProcess, hostCommandBuffer.buffer, hostCommandBuffer.bufferSize, hostParameterBuffer);

					/* Free the allocated host parameter buffer. */
					if (NULL != hostParameterBuffer)
					{
						ExFreePool(hostParameterBuffer);
					}
				}
			}
		}


		result = TRUE;
	}

	return result;
}

/******************** Module Code ********************/

static NTSTATUS actionGetProcessBase(PVOID buffer, SIZE_T bufferSize)
{
	NTSTATUS status;

	if ((NULL != buffer) && (sizeof(VM_PARAM_GET_PROCESS_BASE) == bufferSize))
	{
		PVM_PARAM_GET_PROCESS_BASE params = (PVM_PARAM_GET_PROCESS_BASE)buffer;

		/* Get the PEPROCESS for the specified process ID. */
		PEPROCESS process;

		status = PsLookupProcessByProcessId((HANDLE)params->procID, &process);
		if (NT_SUCCESS(status))
		{
			PPEB pPEB = PsGetProcessPeb(process);
			if (NULL != pPEB)
			{
				/* Read the processes image base.  */
				status = MemManage_readVirtualAddress(process, 
													  &pPEB->ImageBaseAddress, 
												      sizeof(params->processBase), 
													  &params->processBase);
			}
			else
			{
				status = STATUS_NO_MEMORY;
			}

			ObDereferenceObject(process);
		}
	}
	else
	{
		status = STATUS_INVALID_PARAMETER;
	}

	return status;
}

static NTSTATUS actionReadUserMemory(PVOID buffer, SIZE_T bufferSize)
{
	NTSTATUS status;

	if ((NULL != buffer) && (sizeof(VM_PARAM_RW_USER_MEMORY) == bufferSize))
	{
		PVM_PARAM_RW_USER_MEMORY params = (PVM_PARAM_RW_USER_MEMORY)buffer;

		/* Get the PEPROCESS of the target process. */
		PEPROCESS sourceProcess;
		status = PsLookupProcessByProcessId((HANDLE)params->procID, &sourceProcess);
		if (NT_SUCCESS(status))
		{
			/* Allocate a kernel buffer that will hold the read data. */
			PUINT8 kernelBuffer = ExAllocatePool(NonPagedPoolNx, params->size);
			if (NULL != kernelBuffer)
			{
				/* Read into the kernel buffer. */
				status = MemManage_readVirtualAddress(sourceProcess, params->address, params->size, kernelBuffer);
				if (NT_SUCCESS(status))
				{
					/* Write the kernel buffer to our target process (us). */
					PEPROCESS targetProcess = PsGetCurrentProcess();

					status = MemManage_writeVirtualAddress(targetProcess, params->buffer, params->size, kernelBuffer);
				}

				/* Free the temporary kernel buffer. */
				ExFreePool(kernelBuffer);
			}
			else
			{
				status = STATUS_NO_MEMORY;
			}

			ObDereferenceObject(sourceProcess);
		}
	}
	else
	{
		status = STATUS_INVALID_PARAMETER;
	}

	return status;
}

static NTSTATUS actionWriteUserMemory(PVOID buffer, SIZE_T bufferSize)
{
	NTSTATUS status;

	if ((NULL != buffer) && (sizeof(VM_PARAM_RW_USER_MEMORY) == bufferSize))
	{
		PVM_PARAM_RW_USER_MEMORY params = (PVM_PARAM_RW_USER_MEMORY)buffer;

		/* Get the PEPROCESS of the target process. */
		PEPROCESS targetProcess;
		status = PsLookupProcessByProcessId((HANDLE)params->procID, &targetProcess);
		if (NT_SUCCESS(status))
		{
			/* Allocate a kernel buffer that will hold the read data. */
			PUINT8 kernelBuffer = ExAllocatePool(NonPagedPoolNx, params->size);
			if (NULL != kernelBuffer)
			{
				PEPROCESS sourceProcess = PsGetCurrentProcess();

				/* Read into the kernel buffer. */
				status = MemManage_readVirtualAddress(sourceProcess, params->buffer, params->size, kernelBuffer);
				if (NT_SUCCESS(status))
				{
					/* Write the kernel buffer to the target process. */

					status = MemManage_writeVirtualAddress(targetProcess, params->address, params->size, kernelBuffer);
				}

				/* Free the temporary kernel buffer. */
				ExFreePool(kernelBuffer);
			}
			else
			{
				status = STATUS_NO_MEMORY;
			}

			ObDereferenceObject(targetProcess);
		}
	}
	else
	{
		status = STATUS_INVALID_PARAMETER;
	}

	return status;
}

static NTSTATUS actionShadowKernel(PVOID buffer, SIZE_T bufferSize)
{
	UNREFERENCED_PARAMETER(buffer);
	UNREFERENCED_PARAMETER(bufferSize);
	return STATUS_UNSUCCESSFUL;
}

static NTSTATUS getGuestBuffer(PEPROCESS process, PVOID buffer, SIZE_T bufferSize, PVOID* hostBuffer)
{
	NTSTATUS status;

	*hostBuffer = ExAllocatePool(NonPagedPool, bufferSize);

	if (NULL != *hostBuffer)
	{
		/* Read the guest memory of the parameter into the host. */
		status = MemManage_readVirtualAddress(process, buffer, bufferSize, *hostBuffer);
	}
	else
	{
		status = STATUS_NO_MEMORY;
	}

	return status;
}