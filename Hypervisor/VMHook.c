#include <ntifs.h>
#include <intrin.h>
#include "VMHook.h"
#include "VMShadow.h"
#include "BEA\BeaEngine.h"
#include "Debug.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

typedef struct _PENDING_HOOK
{
	PVOID target;
	PVOID hook;
	PVOID* original;
} PENDING_HOOK, *PPENDING_HOOK;

/******************** Module Constants ********************/
#define MAX_HOOKS 50

/******************** Module Variables ********************/

static SIZE_T pendingHookCount = 0;
static PENDING_HOOK pendingHooks[MAX_HOOKS] = { 0 };

/******************** Module Prototypes ********************/
static NTSTATUS hookAFunction(PEPT_CONFIG eptConfig, PVOID targetFunction, PVOID hookFunction, PVOID* origFunction);
static NTSTATUS createTrampoline(PUINT8 hookPage, PVOID targetFunction, PVOID hookFunction, PVOID* origFunction);
static void generateAbsoluteJump(PUINT8 targetBuffer, SIZE_T targetAddress);

/******************** Public Code ********************/

NTSTATUS VMHook_init(PEPT_CONFIG eptConfig)
{
	/* This is called when the hypervisor IS initialised. Hooks can be pending before.
	* This is called once per logical-processor. */

	/* Assume successful until failure. */
	NTSTATUS status = STATUS_SUCCESS;

	/* Iterate through all the pending hooks and initialise them. */
	for (SIZE_T i = 0; i < pendingHookCount; i++)
	{
		PPENDING_HOOK current = &pendingHooks[i];

		status = hookAFunction(eptConfig, current->target, current->hook, current->original);

		if (FALSE == NT_SUCCESS(status))
		{
			/* Don't continue with hooking, just fail gracefully. */
			break;
		}
	}

	return status;
}

void VMHook_queueHook(PVOID targetFunction, PVOID hookFunction, PVOID* origFunction)
{
	/* As some hooks are may be called before the hypervisor is initialised, we provide the
	* ability to create a queue of hooks, this means we can do the hooks without needing to be
	* running at the hypervisor's DPC IRQL.
	*
	* All pending hooks are hooked at hypervisor initialisation. */

	/* NOTE: At the moment this only works with virtual addresses in the kernel as they are mapped
	* to every logical processor. We will need to use IoAllocateMdl if we want to hook usermode
	* addresses in the future. */

	PPENDING_HOOK newHook = &pendingHooks[pendingHookCount];

	newHook->target = targetFunction;
	newHook->hook = hookFunction;
	newHook->original = origFunction;

	pendingHookCount++;
}

/******************** Module Code ********************/

static NTSTATUS hookAFunction(PEPT_CONFIG eptConfig, PVOID targetFunction, PVOID hookFunction, PVOID* origFunction)
{
	NTSTATUS status;

	/* Create the page that will be used as the shadow. */
	PVOID alignedTarget = PAGE_ALIGN(targetFunction);

	/* Create a buffer that will contain the hook
	* First copy the original bytes of the page into it. */
	PUINT8 hookPage = (PUINT8)ExAllocatePool(NonPagedPoolNx, PAGE_SIZE);

	if (NULL != hookPage)
	{
		/* Copy the original bytes into the hook page. */
		RtlCopyMemory(hookPage, alignedTarget, PAGE_SIZE);

		/* Create the trampoline in the hook page. */
		status = createTrampoline(hookPage, targetFunction, hookFunction, origFunction);

		if (NT_SUCCESS(status))
		{
			PHYSICAL_ADDRESS targetPA = MmGetPhysicalAddress(alignedTarget);
			status = VMShadow_hidePageAsRoot(eptConfig, targetPA, hookPage, FALSE);
		}

		/* Free the memory as no longer needed as VMShadow copies the hooked page. */
		ExFreePool(hookPage);
	}
	else
	{
		status = STATUS_NO_MEMORY;
	}


	return status;
}

static NTSTATUS createTrampoline(PUINT8 hookPage, PVOID targetFunction, PVOID hookFunction, PVOID* origFunction)
{
	static const Int32 GP_CONTROL_TRANSFER = (GENERAL_PURPOSE_INSTRUCTION | CONTROL_TRANSFER);
	static const SIZE_T BYTES_FOR_ABSOLUTE_JUMP = 16;

	NTSTATUS status = STATUS_SUCCESS;

	/* Calculate the function's offset into the page. */
	SIZE_T offsetIntoPage = ADDRMASK_EPT_PML1_OFFSET((UINT64)targetFunction);

	/* Allocate some memory for the trampoline. */
	PUINT8 trampoline = ExAllocatePool(NonPagedPoolExecute, PAGE_SIZE);

	if (NULL != trampoline)
	{
		/* Zero the trampoline buffer. */
		RtlZeroMemory(trampoline, PAGE_SIZE);

		/* Determine the number of instructions necessary to overwrite to fit the hook using a disassembler. */
		SIZE_T sizeOfTrampoline = 0;
		SIZE_T sizeOfDisassembled = 0;

		/* Get the size of instructions we will overwrite. */
		DISASM disInfo = { 0 };
		disInfo.EIP = (UIntPtr)targetFunction;

		while (sizeOfTrampoline < BYTES_FOR_ABSOLUTE_JUMP)
		{
			SIZE_T instrLength = Disasm(&disInfo);

			/* Check to see if the disassembled instruction uses relative control transfers
			* CALL, JXX etc. If it does we need to patch the address. */
			if (disInfo.Error != UNKNOWN_OPCODE)
			{
				if (((disInfo.Instruction.Category & GP_CONTROL_TRANSFER) == GP_CONTROL_TRANSFER) &&
					(CallType == disInfo.Instruction.BranchType))
				{
					/* At the moment we will only fix CALL's, we trash the R11 register doing so too.
					* hopefully as it's rare R11 is used in first few bytes this is acceptable. */

					/* MOVABS R11, 0x0000000000000000
					* CALL R11 */
					UINT8 shimAbsCall[12] = { 0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xD0 };
					*((UINT64*)&shimAbsCall[2]) = disInfo.Instruction.AddrValue;

					/* Copy the shim. */
					RtlCopyMemory(trampoline + sizeOfTrampoline,
						shimAbsCall,
						sizeof(shimAbsCall));

					/* Adjust the size of hooked instruction + EIP. */
					sizeOfTrampoline += sizeof(shimAbsCall);
					sizeOfDisassembled += instrLength;		/* Not the same size as the shim. */
					disInfo.EIP += instrLength;
				}
				else
				{
					/* We don't need to patch, just copy. */
					RtlCopyMemory(trampoline + sizeOfTrampoline,
						((PUINT8)targetFunction) + sizeOfTrampoline,
						instrLength);

					sizeOfTrampoline += instrLength;
					sizeOfDisassembled += instrLength;
					disInfo.EIP += instrLength;
				}
			}
			else
			{
				/* No tidy way of returning here really. */
				status = STATUS_UNSUCCESSFUL;
				break;
			}
		}

		/* Ensure the hook isn't over two pages. */
		if ((offsetIntoPage + sizeOfTrampoline) < (PAGE_SIZE - 1))
		{
			/* Add the absolute jump to the trampoline to return back to actual code. */
			generateAbsoluteJump(&trampoline[sizeOfTrampoline], (SIZE_T)targetFunction + sizeOfDisassembled);

			/* Add the absolute jump to detour the target to the hook code. */
			generateAbsoluteJump(&hookPage[offsetIntoPage], (SIZE_T)hookFunction);

			/* Store the hook function, so it can be used. */
			*origFunction = trampoline;

			status = STATUS_SUCCESS;
		}
		else
		{
			status = STATUS_NOT_CAPABLE;
		}
	}
	else
	{
		status = STATUS_NO_MEMORY;
	}

	if (FALSE == NT_SUCCESS(status))
	{
		ExFreePool(trampoline);
	}

	return status;
}

static void generateAbsoluteJump(PUINT8 targetBuffer, SIZE_T targetAddress)
{
	/* PUSH RAX */
	targetBuffer[0] = 0x50;

	/* MOV RAX, targetAddress */
	targetBuffer[1] = 0x48;
	targetBuffer[2] = 0xB8;

	*((PSIZE_T)&targetBuffer[3]) = targetAddress;

	/* XCHG [RSP], RAX */
	targetBuffer[11] = 0x48;
	targetBuffer[12] = 0x87;
	targetBuffer[13] = 0x04;
	targetBuffer[14] = 0x24;

	/* RET */
	targetBuffer[15] = 0xC3;
}
