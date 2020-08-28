#include <ntifs.h>
#include <intrin.h>
#include "VMShadow.h"
#include "MemManage.h"
#include "Intrinsics.h"
#include "Paging.h"
#include "Debug.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/


/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/
static void handlePotentialPTEWrite(PVMM_DATA lpData);
static BOOLEAN handleInitialPTEWrite(PEPT_CONFIG eptConfig);
static BOOLEAN handleShadowExec(PEPT_CONFIG eptConfig, VMX_EXIT_QUALIFICATION_EPT_VIOLATION qualification);
static NTSTATUS addMonitoredPTE(PVMM_DATA lpData, PHYSICAL_ADDRESS physPTE, PT_LEVEL ptLevel, PEPT_SHADOW_PAGE shadowPage);
static NTSTATUS hidePage(PEPT_CONFIG eptConfig, CR3 targetCR3, PHYSICAL_ADDRESS targetPA, PVOID executePage, PEPT_SHADOW_PAGE* shadowPage);
static void updateShadowPagePA(PEPT_MONITORED_PTE monitoredPte, PT_ENTRY_64* newPTE);
static PEPT_MONITORED_PTE findMonitoredPTE(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS guestPA);
static PEPT_SHADOW_PAGE findShadowPage(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS guestPA);
static void setAllShadowsToReadWrite(PEPT_CONFIG eptConfig);
static void invalidateEPT(PEPT_CONFIG eptConfig);

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
		/* Check to see if the EPT violation was due to a monitored PTE. */
		result = handleShadowExec(eptConfig, violationQualification);

		/* Check to see if the EPT violation was due to a monitored PTE. */
		if (FALSE == result)
		{
			result = handleInitialPTEWrite(eptConfig);
		}

		/* If we didn't handle it, throw an error. */
		if (FALSE == result)
		{
			DbgBreakPoint();
		}
	}

	return result;
}

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
			invalidateEPT(&lpData->eptConfig);

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

BOOLEAN VMShadow_handleMTF(PVMM_DATA lpData)
{
	/* MTF tracing is only enabled when we are trying to monitor writes
	 * to PTE's due to paging in Windows. This will be used so that we
	 * can update the guest physical address to host PA for shadowing. */
	handlePotentialPTEWrite(lpData);

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
	PEPT_SHADOW_PAGE shadowPage;
	status = hidePage(eptConfig, nullCR3, targetPA, payloadPage, &shadowPage);

	if (NT_SUCCESS(status) && (TRUE == hypervisorRunning))
	{
		/* We have modified EPT layout, therefore flush and reload. */
		invalidateEPT(eptConfig);
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
			PEPT_SHADOW_PAGE shadowPage;
			status = hidePage(&lpData->eptConfig, tableBase, physTargetVA, execVA, &shadowPage);
			if (NT_SUCCESS(status))
			{
				/* We have hidden a usermode address so we need to also monitor the PTE
				 * in the process that relates to it, this way we can update the shadow page
				 * guest/host translation if it gets paged back out/in. */

				/* Get the physical address of the page table entry that is used for the target VA. */
				PT_LEVEL pteLevel;
				PHYSICAL_ADDRESS physTargetPTE = MemManage_getPTEForGuest(&lpData->mmContext, tableBase, targetVA, &pteLevel);
				if (0 != physTargetPTE.QuadPart)
				{
					/* Add to the list of monitored page table entries. */
					status = addMonitoredPTE(lpData, physTargetPTE, pteLevel, shadowPage);
				}

				/* As we are attempting to hide exec memory in a process,
				 * it's safe to say the hypervisor & EPT is already running.
			     * Therefore we should invalidate the already existing EPT to flush
				 * in the new config. */
				invalidateEPT(&lpData->eptConfig);
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

static void handlePotentialPTEWrite(PVMM_DATA lpData)
{
	/* As we have no way of detecting if the write has taken place
	 * to the target PTE, we just iterate through all monitored and check
	 * if any of the PFN's in the PTE have been modified, if so we update
	 * the guest -> host mapping. */

	 /* Keep going through the whole linked list, until the flink points back to the root. */
	for (PLIST_ENTRY currentEntry = lpData->eptConfig.monitoredPTEList.Flink;
		currentEntry != &lpData->eptConfig.monitoredPTEList;
		currentEntry = currentEntry->Flink)
	{
		/* The CONTAINING_RECORD macro can uses the address of the linked list and then subtracts where the
		* list entry is stored in the structure from the address to give us the address of the parent. */
		PEPT_MONITORED_PTE currentConfig = CONTAINING_RECORD(currentEntry, EPT_MONITORED_PTE, listEntry);

		/* Attempt to read the PTE for this config. */
		PT_ENTRY_64 readPTE;
		NTSTATUS status = MemManage_readPhysicalAddress(&lpData->mmContext, currentConfig->physTargetPTE, &readPTE, sizeof(readPTE));
		if (NT_SUCCESS(status))
		{
			/* Check to see if the PFN of the PTE matches the last known value of the PTE,
			* if it doesn't that means paging has taken place. */
			if (readPTE.Flags != currentConfig->lastPTEValue.Flags)
			{
				if (FALSE == KD_DEBUGGER_NOT_PRESENT)
				{
					DbgBreakPoint();
				}

				/* We need to update the VM Shadow related to this. */
				updateShadowPagePA(currentConfig, &readPTE);

				/* Re-enable the hook. */
				currentConfig->shadowPage->targetPML1E->Flags = currentConfig->shadowPage->activeRWPML1E.Flags;
			}
		}



		/* Set the PTE so that it cannot be written again, so we can trap on next changes. */
		currentConfig->targetPML1E->WriteAccess = 0;
	}

	/* Disable MTF tracing. */
	SIZE_T procCtls;
	__vmx_vmread(VMCS_CTRL_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, &procCtls);

	procCtls &= ~IA32_VMX_PROCBASED_CTLS_MONITOR_TRAP_FLAG_FLAG;
	__vmx_vmwrite(VMCS_CTRL_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, procCtls);

	/* Flush EPT as we have updated a mapping/write access. */
	invalidateEPT(&lpData->eptConfig);
}

static BOOLEAN handleInitialPTEWrite(PEPT_CONFIG eptConfig)
{
	/* The initial write will be when the virtual memory get's paged out,
	 * setting the PTE to Not Present. So we enable MTF tracing, until the
	 * page frame number of the PTE has been updated. */
	BOOLEAN result = FALSE;

	/* Get the guest physical address that caused the violation. */
	PHYSICAL_ADDRESS guestPA;
	__vmx_vmread(VMCS_GUEST_PHYSICAL_ADDRESS, (SIZE_T*)&guestPA.QuadPart);

	/* Find the monitored PTE entry. */
	PEPT_MONITORED_PTE foundMonitored = findMonitoredPTE(eptConfig, guestPA);
	if (NULL != foundMonitored)
	{
		//if (FALSE == KD_DEBUGGER_NOT_PRESENT)
		//{
		//	DbgBreakPoint();
		//}

		/* Enable MTF tracing so that we can trace to the instruction after it has been written. */
		SIZE_T procCtls;
		__vmx_vmread(VMCS_CTRL_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, &procCtls);

		procCtls |= IA32_VMX_PROCBASED_CTLS_MONITOR_TRAP_FLAG_FLAG;
		__vmx_vmwrite(VMCS_CTRL_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, procCtls);

		/* Enable the write bit for the PTE, so that the guest can write to it. */
		foundMonitored->targetPML1E->WriteAccess = 1;

		/* Disable the hook while we are tracing. */
		foundMonitored->shadowPage->targetPML1E->Flags = foundMonitored->shadowPage->originalPML1E.Flags;

		/* Flush EPT as we have updated a mapping. */
		invalidateEPT(eptConfig);

		result = TRUE;
	}

	return result;
}

static BOOLEAN handleShadowExec(PEPT_CONFIG eptConfig, VMX_EXIT_QUALIFICATION_EPT_VIOLATION qualification)
{
	BOOLEAN result = FALSE;

	/* Get the guest physical address that caused the violation. */
	PHYSICAL_ADDRESS guestPA;
	__vmx_vmread(VMCS_GUEST_PHYSICAL_ADDRESS, (SIZE_T*)&guestPA.QuadPart);

	/* Find the shadow page that caused the violation. */
	PEPT_SHADOW_PAGE foundShadow = findShadowPage(eptConfig, guestPA);
	if (NULL != foundShadow)
	{
		/* Check to see if the violation was from trying to execute a non-executable page. */
		if ((FALSE == qualification.EptExecutable) && (TRUE == qualification.ExecuteAccess))
		{
			/* Check to see if target process matches. */
			CR3 guestCR3;
			__vmx_vmread(VMCS_GUEST_CR3, &guestCR3.Flags);

			if ((0 == foundShadow->targetCR3.Flags) || (guestCR3.AddressOfPageDirectory == foundShadow->targetCR3.AddressOfPageDirectory))
			{
				/* Switch to the target execute page, this is if there it is a global shadow (no target CR3)
				* or the CR3 matches the target. */
				foundShadow->targetPML1E->Flags = foundShadow->activeExecTargetPML1E.Flags;
			}
			else
			{
				/* Switch to the original execute page, we will use MTF tracing to
				* know when to put it back to the RW only page. */
				foundShadow->targetPML1E->Flags = foundShadow->activeExecNotTargetPML1E.Flags;
			}

			result = TRUE;
		}
		else if ((TRUE == qualification.EptExecutable) &&
			(qualification.ReadAccess || qualification.WriteAccess))
		{
			/* If so, we update the PML1E so that the read/write page is visible to the guest. */
			foundShadow->targetPML1E->Flags = foundShadow->activeRWPML1E.Flags;

			result = TRUE;
		}
	}

	return result;
}

static NTSTATUS addMonitoredPTE(PVMM_DATA lpData, PHYSICAL_ADDRESS physPTE, PT_LEVEL ptLevel, PEPT_SHADOW_PAGE shadowPage)
{
	NTSTATUS status;

	PEPT_MONITORED_PTE configMonPTE = (PEPT_MONITORED_PTE)ExAllocatePool(NonPagedPoolNx, sizeof(EPT_MONITORED_PTE));
	if (NULL != configMonPTE)
	{
		/* As we have set up PDT to 2MB large pages we need to split this for performance.
		* The lowest we can split it to is the size of a page, 2MB = 512 * 4096 blocks. */
		status = EPT_splitLargePage(&lpData->eptConfig, physPTE);

		/* Check to see if the split was successful, or has already been split. */
		if ((NT_SUCCESS(status)) || (STATUS_ALREADY_COMPLETE == status))
		{
			/* Store the aligned physical address that relates to the target, this
				* will be used so that we can update the shadow entry's translation
				* at a later point. */
			configMonPTE->shadowPage = shadowPage;

			/* Store a pointer to the PML1E we will be modifying. */
			configMonPTE->targetPML1E = EPT_getPML1EFromAddress(&lpData->eptConfig, physPTE);
			if (NULL != configMonPTE->targetPML1E)
			{
				/* Read the current page table entry information. */
				status = MemManage_readPhysicalAddress(&lpData->mmContext, physPTE, &configMonPTE->lastPTEValue, sizeof(PT_ENTRY_64));
				if (NT_SUCCESS(status))
				{
					/* Store the information on the PTE we have read. */
					configMonPTE->physTargetPTE = physPTE;
					configMonPTE->lastEntryLevel = ptLevel;

					/* Set the PML1E so that it is not writable, this will cause a VMEXIT
					* if an attempt to write to the guest PTE takes place (paging change of phys address). */
					configMonPTE->targetPML1E->WriteAccess = 0;

					/* Add this config to the list of monitored page table entries. */
					InsertHeadList(&lpData->eptConfig.monitoredPTEList, &configMonPTE->listEntry);

					status = STATUS_SUCCESS;
				}
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

	return status;
}

static NTSTATUS hidePage(PEPT_CONFIG eptConfig, CR3 targetCR3, PHYSICAL_ADDRESS targetPA, PVOID executePage, PEPT_SHADOW_PAGE* shadowPage)
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

					/* Add this shadow hook to the EPT shadow list. */
					InsertHeadList(&eptConfig->pageShadowList, &shadowConfig->listEntry);

					*shadowPage = shadowConfig;
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

static void updateShadowPagePA(PEPT_MONITORED_PTE monitoredPte, PT_ENTRY_64* newPTE)
{
	/* Calculate the new physical address as where our shadow should be. */
	PHYSICAL_ADDRESS physNewTarget;
	switch (monitoredPte->lastEntryLevel)
	{
		case PT_LEVEL_PDPTE:
		{
			physNewTarget.QuadPart = newPTE->PageFrameNumber * SIZE_1GB;
			break;
		}

		case PT_LEVEL_PDE:
		{
			physNewTarget.QuadPart = newPTE->PageFrameNumber * SIZE_2MB;
			break;
		}

		case PT_LEVEL_PTE:
		{
			physNewTarget.QuadPart = newPTE->PageFrameNumber * PAGE_SIZE;
			break;
		}

		case PT_LEVEL_PML4E:
		default:
		{
			/* Not possible to be this. */
			physNewTarget.QuadPart = 0;
			DbgBreakPoint();
			break;
		}
	}

	SIZE_T newPageNumber = physNewTarget.QuadPart / PAGE_SIZE;

	/* Update the PFN's and PA in the entry,
	 * We DO NOT modify the executeTarget entry as this should point
	 * to the physical address of our executeOnly target buffer. */
	monitoredPte->shadowPage->physicalAlign = physNewTarget;
	monitoredPte->shadowPage->originalPML1E.PageFrameNumber = newPageNumber;
	monitoredPte->shadowPage->activeExecNotTargetPML1E.PageFrameNumber = newPageNumber;
	monitoredPte->shadowPage->activeRWPML1E.PageFrameNumber = newPageNumber;

	/* Store the page frame number so we can monitor for paging at a later point. */
	monitoredPte->lastPTEValue.Flags = newPTE->Flags;
}

static PEPT_MONITORED_PTE findMonitoredPTE(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS guestPA)
{
	PEPT_MONITORED_PTE result = NULL;

	/* Linked lists are initialised as a circular buffer, therefor the last one points back to the beginning,
	* We can use this to determine when we have gone through all items, or if no items are present. */

	/* Keep going through the whole linked list, until the flink points back to the root. */
	for (PLIST_ENTRY currentEntry = eptConfig->monitoredPTEList.Flink;
		currentEntry != &eptConfig->monitoredPTEList;
		currentEntry = currentEntry->Flink)
	{
		/* The CONTAINING_RECORD macro can uses the address of the linked list and then subtracts where the
		* list entry is stored in the structure from the address to give us the address of the parent. */
		PEPT_MONITORED_PTE monitoredPTE = CONTAINING_RECORD(currentEntry, EPT_MONITORED_PTE, listEntry);

		/* If the page hook's base address matches where the current guest state is we can assume that
		* the violation was caused by this hook. Therefor we switch pages appropriately. */
		if (PAGE_ALIGN(monitoredPTE->physTargetPTE.QuadPart) == PAGE_ALIGN(guestPA.QuadPart))
		{
			result = monitoredPTE;
			break;
		}
	}

	return result;
}

static PEPT_SHADOW_PAGE findShadowPage(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS guestPA)
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
		if (PAGE_ALIGN(pageHook->physicalAlign.QuadPart) == PAGE_ALIGN(guestPA.QuadPart))
		{
			result = pageHook;
			break;
		}
	}

	return result;
}

static void setAllShadowsToReadWrite(PEPT_CONFIG eptConfig)
{
	/* Keep going through the whole linked list, until the flink points back to the root. */
	for (PLIST_ENTRY currentEntry = eptConfig->pageShadowList.Flink;
		currentEntry != &eptConfig->pageShadowList;
		currentEntry = currentEntry->Flink)
	{
		/* The CONTAINING_RECORD macro can uses the address of the linked list and then subtracts where the
		* list entry is stored in the structure from the address to give us the address of the parent. */
		PEPT_SHADOW_PAGE pageHook = CONTAINING_RECORD(currentEntry, EPT_SHADOW_PAGE, listEntry);

		pageHook->targetPML1E->Flags = pageHook->activeRWPML1E.Flags;
	}
}

static void invalidateEPT(PEPT_CONFIG eptConfig)
{
	INVEPT_DESCRIPTOR eptDescriptor;

	eptDescriptor.EptPointer = eptConfig->eptPointer.Flags;
	eptDescriptor.Reserved = 0;
	__invept(InveptSingleContext, &eptDescriptor);
}