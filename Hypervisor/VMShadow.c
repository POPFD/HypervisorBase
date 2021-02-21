#include <ntifs.h>
#include <intrin.h>
#include "VMShadow.h"
#include "MemManage.h"
#include "Intrinsics.h"
#include "Debug.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

/* Structure that will hold the shadow configuration for hiding executable pages. */
typedef struct _SHADOW_PAGE
{
	DECLSPEC_ALIGN(PAGE_SIZE) UINT8 executePage[PAGE_SIZE];

	/* Target process that will be hooked, NULL if global. */
	CR3 targetCR3;

	/* Pointer to the PML1 entry that will be modified between RW and E. */
	PEPT_PML1_ENTRY targetPML1E;

	/* Will store the flags of the specific PML1E's that will be
	 * used for targetting shadowing. */
	EPT_PML1_ENTRY originalPML1E;
	EPT_PML1_ENTRY activeExecTargetPML1E;
	EPT_PML1_ENTRY activeExecNotTargetPML1E;
	EPT_PML1_ENTRY activeRWPML1E;

} SHADOW_PAGE, *PSHADOW_PAGE;

/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/
static BOOLEAN handleShadowExec(PEPT_CONFIG eptConfig, PCONTEXT guestContext, PVOID userBuffer);
static NTSTATUS hidePage(PEPT_CONFIG eptConfig, CR3 targetCR3, PHYSICAL_ADDRESS targetPA, PVOID executePage);
static void setAllShadowsToReadWrite(PEPT_CONFIG eptConfig);

/******************** Public Code ********************/

BOOLEAN VMShadow_handleMovCR(PVMM_DATA lpData)
{
	/* Cast the exit qualification to its proper type. */
	VMX_EXIT_QUALIFICATION_MOV_CR exitQualification;
	__vmx_vmread(VMCS_EXIT_QUALIFICATION, &exitQualification.Flags);

	/* Check if caused by a MOV CR3, REG */
	if (VMX_EXIT_QUALIFICATION_REGISTER_CR3 == exitQualification.ControlRegister)
	{
		if (VMX_EXIT_QUALIFICATION_ACCESS_MOV_TO_CR == exitQualification.AccessType)
		{
			/* TODO: Make it so we only switch shadows to execute if new CR3 is
			 *		 in one of the target pages. Will be quicker. */

			/* MOV CR3, XXX has taken place, this indicates a new page table has been loaded.
			 * We should iterate through all of the shadow pages and ensure RW pages are all
			 * set instead of execute. That way if an execute happens on one, the target
			 * will flip to the right execute entry later depending if it is a targetted process or not. */
			setAllShadowsToReadWrite(&lpData->eptConfig);
			EPT_invalidateAndFlush(&lpData->eptConfig);

			/* Set the guest CR3 register, to the value of the general purpose register. */
			ULONG64* registerList = &lpData->guestContext.Rax;

			ULONG64 registerValue;
			if (VMX_EXIT_QUALIFICATION_GENREG_RSP == exitQualification.GeneralPurposeRegister)
			{
				__vmx_vmread(VMCS_GUEST_RSP, &registerValue);
			}
			else
			{
				registerValue = registerList[exitQualification.GeneralPurposeRegister];
			}
			registerValue &= ~(1ULL << 63);

			__vmx_vmwrite(VMCS_GUEST_CR3, registerValue);

			/* Flush the TLB for the current logical processor. */
			INVVPID_DESCRIPTOR descriptor = { 0 };
			descriptor.Vpid = 1;
			__invvpid(InvvpidSingleContextRetainingGlobals, &descriptor);
		}
	}

	return TRUE;
}

NTSTATUS VMShadow_hidePageGlobally(
	PEPT_CONFIG eptConfig,
	PHYSICAL_ADDRESS targetPA,
	PUINT8 payloadPage,
	BOOLEAN hypervisorRunning
)
{
	NTSTATUS status = STATUS_INVALID_PARAMETER;

	CR3 nullCR3 = { .Flags = 0 };
	status = hidePage(eptConfig, nullCR3, targetPA, payloadPage);

	if (NT_SUCCESS(status) && (TRUE == hypervisorRunning))
	{
		/* We have modified EPT layout, therefore flush and reload. */
		EPT_invalidateAndFlush(eptConfig);
	}

	return status;
}

NTSTATUS VMShadow_hideExecInProcess(
	PVMM_DATA lpData,
	PEPROCESS targetProcess,
	PUINT8 targetVA,
	PUINT8 execVA
)
{
	NTSTATUS status;

	/* Get the process/page table we want to shadow the memory from. */
	CR3 tableBase = MemManage_getPageTableBase(targetProcess);
	if (0 != tableBase.Flags)
	{
		/* Calculate the physical address of the target VA,
		* I know we could calculate this by reading the PTE here and
		* calculating, however we have a function for this already (at the expense of reading PTE again.. */
		PHYSICAL_ADDRESS physTargetVA;
		status = MemManage_getPAForGuest(&lpData->mmContext, tableBase, targetVA, &physTargetVA);
		if (NT_SUCCESS(status))
		{
			/* Hide the executable page, for that page only. */
			status = hidePage(&lpData->eptConfig, tableBase, physTargetVA, execVA);
			if (NT_SUCCESS(status))
			{			
				/* As we are attempting to hide exec memory in a process,
				 * it's safe to say the hypervisor & EPT is already running.
			     * Therefore we should invalidate the already existing EPT to flush
				 * in the new config. */
				EPT_invalidateAndFlush(&lpData->eptConfig);
			}
		}
	}
	else
	{
		/* Unable to get the table base. */
		status = STATUS_INVALID_MEMBER;
	}

	return status;
}

/******************** Module Code ********************/

static BOOLEAN handleShadowExec(PEPT_CONFIG eptConfig, PCONTEXT guestContext, PVOID userBuffer)
{
	UNREFERENCED_PARAMETER(eptConfig);
	UNREFERENCED_PARAMETER(guestContext);
	BOOLEAN result = FALSE;

	/* Cast the exit qualification to it's proper type, as an EPT violation. */
	VMX_EXIT_QUALIFICATION_EPT_VIOLATION violationQual;
	__vmx_vmread(VMCS_EXIT_QUALIFICATION, &violationQual.Flags);

	/* We should only deal with shadow pages caused by translation. */
	if (TRUE == violationQual.CausedByTranslation)
	{
		/* The user supplied parameter when the handler was registered supplies
		 * the shadow page config, so we re-cast it to the desired type. */
		PSHADOW_PAGE shadowPage = (PSHADOW_PAGE)userBuffer;
		if (NULL != shadowPage)
		{
			/* Check to see if the violation was from trying to execute a non-executable page. */
			if ((FALSE == violationQual.EptExecutable) && (TRUE == violationQual.ExecuteAccess))
			{
				/* Check to see if target process matches. */
				CR3 guestCR3;
				__vmx_vmread(VMCS_GUEST_CR3, &guestCR3.Flags);

				if ((0 == shadowPage->targetCR3.Flags) || (guestCR3.AddressOfPageDirectory == shadowPage->targetCR3.AddressOfPageDirectory))
				{
					/* Switch to the target execute page, this is if there it is a global shadow (no target CR3)
					* or the CR3 matches the target. */
					shadowPage->targetPML1E->Flags = shadowPage->activeExecTargetPML1E.Flags;
				}
				else
				{
					/* Switch to the original execute page */
					shadowPage->targetPML1E->Flags = shadowPage->activeExecNotTargetPML1E.Flags;
				}

				result = TRUE;
			}
			else if ((TRUE == violationQual.EptExecutable) &&
					 (violationQual.ReadAccess || violationQual.WriteAccess))
			{
				/* If so, we update the PML1E so that the read/write page is visible to the guest. */
				shadowPage->targetPML1E->Flags = shadowPage->activeRWPML1E.Flags;
				result = TRUE;
			}
		}
	}

	return result;
}

static NTSTATUS hidePage(PEPT_CONFIG eptConfig, CR3 targetCR3, PHYSICAL_ADDRESS targetPA, PVOID executePage)
{
	NTSTATUS status;

	if (0ULL != targetPA.QuadPart)
	{
		PSHADOW_PAGE shadowConfig = (PSHADOW_PAGE)ExAllocatePool(NonPagedPoolNx, sizeof(SHADOW_PAGE));

		if (NULL != shadowConfig)
		{
			/* As we have set up PDT to 2MB large pages we need to split this for performance.
			* The lowest we can split it to is the size of a page, 2MB = 512 * 4096 blocks. */
			status = EPT_splitLargePage(eptConfig, targetPA);

			/* If the page split was successful or was already split, continue. */
			if (NT_SUCCESS(status) || (STATUS_ALREADY_COMPLETE == status))
			{
				/* Zero the newly allocated page config. */
				RtlZeroMemory(shadowConfig, sizeof(SHADOW_PAGE));

				/* Calculate the start and end of the physical address page we are hooking. */
				PHYSICAL_ADDRESS physStart;
				PHYSICAL_ADDRESS physEnd;

				physStart.QuadPart = (LONGLONG)PAGE_ALIGN(targetPA.QuadPart);
				physEnd.QuadPart = physStart.QuadPart + PAGE_SIZE;

				/* Store the target process. */
				shadowConfig->targetCR3 = targetCR3;

				/* Store a pointer to the PML1E we will be modifying. */
				shadowConfig->targetPML1E = EPT_getPML1EFromAddress(eptConfig, targetPA);

				if (NULL != shadowConfig->targetPML1E)
				{
					/* Store a copy of the original */
					shadowConfig->originalPML1E.Flags = shadowConfig->targetPML1E->Flags;

					/* Create the executable PML1E when it IS the target process. */
					shadowConfig->activeExecTargetPML1E.Flags = shadowConfig->targetPML1E->Flags;
					shadowConfig->activeExecTargetPML1E.ReadAccess = 0;
					shadowConfig->activeExecTargetPML1E.WriteAccess = 0;
					shadowConfig->activeExecTargetPML1E.ExecuteAccess = 1;
					shadowConfig->activeExecTargetPML1E.PageFrameNumber = MmGetPhysicalAddress(&shadowConfig->executePage).QuadPart / PAGE_SIZE;

					/* Create the executable PML1E when the it is NOT the target process.
					 * Here we want to keep original flags and guest physical address, but just disable read/write. */
					shadowConfig->activeExecNotTargetPML1E.Flags = shadowConfig->targetPML1E->Flags;
					shadowConfig->activeExecNotTargetPML1E.ReadAccess = 0;
					shadowConfig->activeExecNotTargetPML1E.WriteAccess = 0;
					shadowConfig->activeExecNotTargetPML1E.ExecuteAccess = 1;

					/* Create the readwrite PML1E when ANY read write to the page takes place.
					 * Here we want to keep original flags, however disable execute access. */
					shadowConfig->activeRWPML1E.Flags = shadowConfig->targetPML1E->Flags;
					shadowConfig->activeRWPML1E.ReadAccess = 1;
					shadowConfig->activeRWPML1E.WriteAccess = 1;
					shadowConfig->activeRWPML1E.ExecuteAccess = 0;

					/* Set the actual PML1E to the value of the readWrite. */
					shadowConfig->targetPML1E->Flags = shadowConfig->activeRWPML1E.Flags;

					/* Copy the fake bytes */
					RtlCopyMemory(&shadowConfig->executePage[0], executePage, PAGE_SIZE);

					/* Calculate the range, that this handler will be for. */
					

					PHYSICAL_RANGE handlerRange;
					handlerRange.start = physStart;
					handlerRange.end = physEnd;

					/* Add this shadow hook to the EPT shadow list. */
					status = EPT_addViolationHandler(eptConfig, handlerRange, handleShadowExec, (PVOID)shadowConfig);
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

static void setAllShadowsToReadWrite(PEPT_CONFIG eptConfig)
{
	/* Go through the whole linked list of the EPT handlers for each of the
	 * user addresses. If the handler matches the one we use for shadow exec, set it to read/write. */
	for (PLIST_ENTRY currentEntry = eptConfig->handlerList.Flink;
		currentEntry != &eptConfig->handlerList;
		currentEntry = currentEntry->Flink)
	{
		PEPT_HANDLER eptHandler = CONTAINING_RECORD(currentEntry, EPT_HANDLER, listEntry);

		/* TODO: Not a great solution, maybe think of something more elegant,
		 *		 VMShadow is currently coupled to EPT internal configs (which is shouldn't really have access to). */
		if (eptHandler->callback == handleShadowExec)
		{
			PSHADOW_PAGE shadowPage = (PSHADOW_PAGE)eptHandler->userParameter;

			/* Only set hooks that are currently enabled. */
			if (shadowPage->targetPML1E->Flags != shadowPage->originalPML1E.Flags)
			{
				shadowPage->targetPML1E->Flags = shadowPage->activeRWPML1E.Flags;
			}
		}
	}
}
