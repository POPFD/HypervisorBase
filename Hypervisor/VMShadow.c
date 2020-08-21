#include <ntifs.h>
#include <intrin.h>
#include "VMShadow.h"
#include "VMCall_Common.h"
#include "Intrinsics.h"
#include "Debug.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/


/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/
static ULONG_PTR routineHideCodeOnEachCore(ULONG_PTR Argument);
static NTSTATUS hidePage(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS targetPA, PVOID executePage);
static PEPT_SHADOW_PAGE findShadow(PEPT_CONFIG eptConfig, UINT64 guestPA);

/******************** Public Code ********************/

BOOLEAN VMShadow_handleEPTViolation(PEPT_CONFIG eptConfig)
{
	/* Set to false as we haven't successfully handled the violation yet. */
	BOOLEAN result = FALSE;

	/* Cast the exit qualification to it's proper type, as an EPT violation. */
	VMX_EXIT_QUALIFICATION_EPT_VIOLATION violationQualification;
	__vmx_vmread(VMCS_EXIT_QUALIFICATION, &violationQualification.Flags);

	/* We should only deal with shadow pages caused by translation. */
	if (TRUE == violationQualification.CausedByTranslation)
	{
		SIZE_T guestPA;
		__vmx_vmread(VMCS_GUEST_PHYSICAL_ADDRESS, &guestPA);
		DEBUG_PRINT("Shadow page: 0x%I64X\t", guestPA);

		/* Find the shadow page that caused the violation. */
		PEPT_SHADOW_PAGE foundShadow = findShadow(eptConfig, guestPA);
		if (NULL != foundShadow)
		{
			/* Check to see if the violation was from trying to execute a non-executable page. */
			if ((FALSE == violationQualification.EptExecutable) && (TRUE == violationQualification.ExecuteAccess))
			{
				DEBUG_PRINT("Attempted execute, switching to executable page.\r\n");

				/* If so, we update the PML1E so that the execute only page is visible to the guest. */
				foundShadow->targetPML1E->Flags = foundShadow->executePML1E.Flags;

				result = TRUE;
			}
			else if ((TRUE == violationQualification.EptExecutable) &&
				(violationQualification.ReadAccess || violationQualification.WriteAccess))
			{
				DEBUG_PRINT("Attempted read or write, switched to read/write page.\r\n");

				/* If so, we update the PML1E so that the read/write page is visible to the guest. */
				foundShadow->targetPML1E->Flags = foundShadow->readWritePML1E.Flags;

				result = TRUE;
			}
		}
	}

	return result;
}

NTSTATUS VMShadow_hidePageAsRoot(
	PEPT_CONFIG eptConfig,
	PHYSICAL_ADDRESS targetPA,
	PUINT8 payloadPage,
	BOOLEAN hypervisorRunning
)
{
	NTSTATUS status = STATUS_INVALID_PARAMETER;

	status = hidePage(eptConfig, targetPA, payloadPage);

	if (NT_SUCCESS(status) && (TRUE == hypervisorRunning))
	{
		INVEPT_DESCRIPTOR eptDescriptor;

		eptDescriptor.EptPointer = eptConfig->eptPointer.Flags;
		eptDescriptor.Reserved = 0;
		__invept(1, &eptDescriptor);
	}

	return status;
}

NTSTATUS VMShadow_hideCodeAsGuest(
	PUINT8 targetStart,
	PUINT8 payloadStart,
	ULONG payloadSize
)
{
	NTSTATUS status = STATUS_INVALID_PARAMETER;

	/* Calculate where in the first page of the target the payload will be placed. */
	ULONG offsetIntoTarget = ADDRMASK_EPT_PML1_OFFSET((UINT64)targetStart);

	/* Calculate number of pages to copy, this value is made so beginning and end pages
	* contain the original bytes still. */
	ULONG pagesToShadow = ((offsetIntoTarget + payloadSize) / PAGE_SIZE) + 1;
	ULONG totalBytesToShadow = pagesToShadow * PAGE_SIZE;

	PUINT8 alignedTarget = PAGE_ALIGN(targetStart);

	/* Allocate a buffer that will be our shadow payload. */
	PUINT8 executePages = (PUINT8)ExAllocatePool(NonPagedPoolNx, totalBytesToShadow);
	if (NULL != executePages)
	{
		/* Copy the original bytes into the execute page. */
		RtlCopyMemory(executePages, alignedTarget, totalBytesToShadow);

		/* Copy the executable payload over the correct bytes. */
		RtlCopyMemory(executePages + offsetIntoTarget, payloadStart, payloadSize);

		/* Now iterate through each of the pages and shadow them. */
		ULONG bytesShadowed = 0;
		while (bytesShadowed < totalBytesToShadow)
		{
			/* We need to break from the guest back up to the hypervisor,
			* This is done by sending a IPI notification to each core, which
			* will run a function to enter the VMExit handler and add the hook.
			* The physical address has to be sent rather than virtual as the
			* other processors will have a different page table when in VMX root. */
			VM_PARAM_SHADOW apHidePage;
			apHidePage.physTarget = MmGetPhysicalAddress(alignedTarget + bytesShadowed).QuadPart;
			apHidePage.payloadPage = executePages + bytesShadowed;

			status = (NTSTATUS)KeIpiGenericCall(routineHideCodeOnEachCore, (ULONG_PTR)&apHidePage);

			if (NT_SUCCESS(status))
			{
				bytesShadowed += PAGE_SIZE;
			}
			else
			{
				/* Something went wrong while hiding the page. */
				break;
			}
		}

		ExFreePool(executePages);
	}
	else
	{
		status = STATUS_NO_MEMORY;
	}

	return status;
}

/******************** Module Code ********************/

static ULONG_PTR routineHideCodeOnEachCore(ULONG_PTR Argument)
{
	/* This logical processor now needs to VMCALL to
	* tell the hypervisor to shadow the following page.
	* The parameters for the VMCall are stored in the argument
	* of the IPI notification so we need to send them. */
	PVM_PARAM_SHADOW paramShadow = (PVM_PARAM_SHADOW)Argument;

	VMCALL_COMMAND command;
	command.action = VMCALL_ACTION_SHADOW_KERNEL;
	command.buffer = paramShadow;
	command.bufferSize = sizeof(VM_PARAM_SHADOW);

	return (ULONG_PTR)VMCALL_actionHost(VMCALL_KEY, &command);
}

static NTSTATUS hidePage(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS targetPA, PVOID executePage)
{
	NTSTATUS status;

	if (0ULL != targetPA.QuadPart)
	{
		PEPT_SHADOW_PAGE shadowConfig = (PEPT_SHADOW_PAGE)ExAllocatePool(NonPagedPoolNx, sizeof(EPT_SHADOW_PAGE));

		if (NULL != shadowConfig)
		{
			/* As we have set up PDT to 2MB large pages we need to split this for performance.
			* The lowest we can split it to is the size of a page, 2MB = 512 * 4096 blocks. */
			status = EPT_splitLargePage(eptConfig, targetPA);

			/* If the page split was successful or was already split, continue. */
			if (NT_SUCCESS(status) || (STATUS_ALREADY_COMPLETE == status))
			{
				/* Zero the newly allocated page config. */
				RtlZeroMemory(shadowConfig, sizeof(EPT_SHADOW_PAGE));

				/* Store the physical address of the targeted page. */
				shadowConfig->physicalBaseAddress.QuadPart = (LONGLONG)PAGE_ALIGN(targetPA.QuadPart);

				/* Store a pointer to the PML1E we will be modifying. */
				shadowConfig->targetPML1E = EPT_getPML1EFromAddress(eptConfig, targetPA);

				if (NULL != shadowConfig->targetPML1E)
				{
					/* Create the executable PML1E. */
					shadowConfig->executePML1E.Flags = 0;
					shadowConfig->executePML1E.ReadAccess = 0;
					shadowConfig->executePML1E.WriteAccess = 0;
					shadowConfig->executePML1E.ExecuteAccess = 1;
					shadowConfig->executePML1E.PageFrameNumber = MmGetPhysicalAddress(&shadowConfig->executePage).QuadPart / PAGE_SIZE;

					/* Create the readwrite PML1E */
					shadowConfig->readWritePML1E.Flags = shadowConfig->targetPML1E->Flags;
					shadowConfig->readWritePML1E.ReadAccess = 1;
					shadowConfig->readWritePML1E.WriteAccess = 1;
					shadowConfig->readWritePML1E.ExecuteAccess = 0;

					/* Set the actual PML1E to the value of the readWrite. */
					shadowConfig->targetPML1E->Flags = shadowConfig->readWritePML1E.Flags;

					/* Copy the fake bytes */
					RtlCopyMemory(&shadowConfig->executePage[0], executePage, PAGE_SIZE);

					/* Add this shadow hook to the EPT shadow list. */
					InsertHeadList(&eptConfig->pageShadowList, &shadowConfig->listEntry);

					status = STATUS_SUCCESS;
				}
				else
				{
					/* Unable to find the PML1E for the target page. */
					ExFreePool(shadowConfig);
					status = STATUS_NO_SUCH_MEMBER;
				}
			}
		}
		else
		{
			status = STATUS_NO_MEMORY;
		}
	}
	else
	{
		status = STATUS_INVALID_ADDRESS;
	}

	return status;
}

static PEPT_SHADOW_PAGE findShadow(PEPT_CONFIG eptConfig, UINT64 guestPA)
{
	PEPT_SHADOW_PAGE result = NULL;

	/* Linked lists are initialised as a circular buffer, therefor the last one points back to the beginning,
	* We can use this to determine when we have gone through all items, or if no items are present. */

	/* Keep going through the whole linked list, until the flink points back to the root. */
	for (PLIST_ENTRY currentEntry = eptConfig->pageShadowList.Flink;
		currentEntry != &eptConfig->pageShadowList;
		currentEntry = currentEntry->Flink)
	{
		/* The CONTAINING_RECORD macro can uses the address of the linked list and then subtracts where the
		* list entry is stored in the structure from the address to give us the address of the parent. */
		PEPT_SHADOW_PAGE pageHook = CONTAINING_RECORD(currentEntry, EPT_SHADOW_PAGE, listEntry);

		/* If the page hook's base address matches where the current guest state is we can assume that
		* the violation was caused by this hook. Therefor we switch pages appropriately. */
		if (pageHook->physicalBaseAddress.QuadPart == (LONGLONG)PAGE_ALIGN(guestPA))
		{
			result = pageHook;
			break;
		}
	}

	return result;
}
