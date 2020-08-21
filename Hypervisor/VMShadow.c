#include <ntifs.h>
#include <intrin.h>
#include "VMShadow.h"
#include "MemManage.h"
#include "Intrinsics.h"
#include "Debug.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/


/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/
static NTSTATUS addMonitoredPTE(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS physPTE);
static NTSTATUS hidePage(PEPT_CONFIG eptConfig, PEPROCESS targetProcess, PHYSICAL_ADDRESS targetPA, PVOID executePage);
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

				/* Check to see if target process matches. */
				PEPROCESS currentProcess = PsGetCurrentProcess();

				if ((NULL == foundShadow->targetProcess) || (currentProcess == foundShadow->targetProcess))
				{
					/* TODO: Switch to the modified execute page, we will use MTF tracing to
					*		 know when to put it back to the RW only page. */

					/* If so, we update the PML1E so that the execute only page is visible to the guest. */
					//foundShadow->targetPML1E->Flags = foundShadow->executePML1E.Flags;
				}
				else
				{
					/* TODO: Switch to the original execute page, we will use MTF tracing to
					 *		 know when to put it back to the RW only page. */

					//foundShadow->targetPML1E->Flags = foundShadow->readWritePML1E.Flags;
					//foundShadow->targetPML1E->ExecuteAccess = 1;
				}


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

	status = hidePage(eptConfig, NULL, targetPA, payloadPage);

	if (NT_SUCCESS(status) && (TRUE == hypervisorRunning))
	{
		INVEPT_DESCRIPTOR eptDescriptor;

		eptDescriptor.EptPointer = eptConfig->eptPointer.Flags;
		eptDescriptor.Reserved = 0;
		__invept(1, &eptDescriptor);
	}

	return status;
}

NTSTATUS VMShadow_hideExecInProcess(
	PEPT_CONFIG eptConfig,
	PEPROCESS targetProcess,
	PUINT8 targetVA,
	PUINT8 execVA
)
{
	NTSTATUS status;

	/* Get the physical address of the page table entry that is used for the target VA. */
	PHYSICAL_ADDRESS physTargetPTE;
	status = MemManage_getPTEPhysAddressFromVA(targetProcess, targetVA, &physTargetPTE);
	if (NT_SUCCESS(status))
	{
		/* Add to the list of monitored page table entries. */
		status = addMonitoredPTE(eptConfig, physTargetPTE);
		if (NT_SUCCESS(status))
		{
			/* Calculate the physical address of the target VA,
			 * I know we could calculate this by reading the PTE here and
			 * calculating, however we have a function for this already (at the expense of reading PTE again.. */
			PHYSICAL_ADDRESS physTargetVA;
			status = MemManage_getPhysFromVirtual(targetProcess, targetVA, &physTargetVA);
			if (NT_SUCCESS(status))
			{
				/* Hide the executable page, for that page only. */
				status = hidePage(eptConfig, targetProcess, physTargetVA, execVA);
				if (NT_SUCCESS(status))
				{
					/* As we are attempting to hide exec memory in a process,
					 * it's safe to say the hypervisor & EPT is already running.
					 * Therefore we should invalidate the already existing EPT to flush
					 * in the new config. */
					INVEPT_DESCRIPTOR eptDescriptor;

					eptDescriptor.EptPointer = eptConfig->eptPointer.Flags;
					eptDescriptor.Reserved = 0;
					__invept(1, &eptDescriptor);
				}
			}
		}
	}

	return status;
}

/******************** Module Code ********************/

static NTSTATUS addMonitoredPTE(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS physPTE)
{
	NTSTATUS status;

	if (0ULL != physPTE.QuadPart)
	{
		PEPT_MONITORED_PTE configMonPTE = (PEPT_MONITORED_PTE)ExAllocatePool(NonPagedPoolNx, sizeof(EPT_MONITORED_PTE));
		if (NULL != configMonPTE)
		{
			/* As we have set up PDT to 2MB large pages we need to split this for performance.
			* The lowest we can split it to is the size of a page, 2MB = 512 * 4096 blocks. */
			status = EPT_splitLargePage(eptConfig, physPTE);

			/* Check to see if the split was successful, or has already been split. */
			if ((NT_SUCCESS(status)) || (STATUS_ALREADY_COMPLETE == status))
			{
				/* Store the aligned physical address of where the PTE exists,
				 * also store it's offset, as we may have multiple monitored PTE's
				 * in the same page. */
				configMonPTE->physAlignPTE.QuadPart = (LONGLONG)PAGE_ALIGN(physPTE.QuadPart);
				configMonPTE->pageOffset = ADDRMASK_EPT_PML1_OFFSET(physPTE.QuadPart);

				/* Store a pointer to the PML1E we will be modifying. */
				configMonPTE->targetPML1E = EPT_getPML1EFromAddress(eptConfig, physPTE);
				if (NULL != configMonPTE->targetPML1E)
				{
					/* Set the PML1E so that it is not writable, this will cause a VMEXIT
					 * if an attempt to write to the guest PTE takes place (paging change of phys address). */
					configMonPTE->targetPML1E->WriteAccess = 0;

					/* Add this config to the list of monitored page table entries. */
					InsertHeadList(&eptConfig->monitoredPTEList, &configMonPTE->listEntry);

					status = STATUS_SUCCESS;
				}
				else
				{
					/* Unable to get PML1E so free resources we have allocated. */
					ExFreePool(configMonPTE);
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
		status = STATUS_INVALID_PARAMETER;
	}

	return status;
}

static NTSTATUS hidePage(PEPT_CONFIG eptConfig, PEPROCESS targetProcess, PHYSICAL_ADDRESS targetPA, PVOID executePage)
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

				/* Store the physical address of the targeted page & offset into page. */
				shadowConfig->physicalAlign.QuadPart = (LONGLONG)PAGE_ALIGN(targetPA.QuadPart);
				shadowConfig->pageOffset = ADDRMASK_EPT_PML1_OFFSET(targetPA.QuadPart);

				/* Store the target process. */
				shadowConfig->targetProcess = targetProcess;

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
		if (pageHook->physicalAlign.QuadPart == (LONGLONG)PAGE_ALIGN(guestPA))
		{
			result = pageHook;
			break;
		}
	}

	return result;
}
