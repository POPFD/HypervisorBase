#include <ntifs.h>
#include "VMM.h"
#include "VMHook.h"
#include "Intrinsics.h"
#include "MSR.h"
#include "GDT.h"
#include "MemManage.h"
#include "HandlerShim.h"
#include "Debug.h"
#include "ia32.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/


/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/
static NTSTATUS launchVMMOnProcessor(PVMM_DATA lpData);
static NTSTATUS enterRootMode(PVMM_DATA lpData);
static void setupVMCS(PVMM_DATA lpData);
static NTSTATUS launchVMX(void);
static void captureControlRegisters(PCONTROL_REGISTERS registers);

/******************** Public Code ********************/

NTSTATUS VMM_init(PVMM_DATA lpData)
{
	NTSTATUS status;

	/* Capture the control registers for the processor. */
	captureControlRegisters(&lpData->controlRegisters);
	DEBUG_PRINT("VMM %d Control Registers:\r\n"
				"\tCR0: %I64X\r\n"
				"\tCR3: %I64X\r\n"
				"\tCR4: %I64X\r\n",
				lpData->processorIndex, lpData->controlRegisters.Cr0, 
				lpData->controlRegisters.Cr3, lpData->controlRegisters.Cr4);

	/* Ensure the align check flag isn't set when here, as we are going to use this
	 * to verify we enter VM guest successfully. */
	__writeeflags(__readeflags() & ~EFLAGS_ALIGNMENT_CHECK_FLAG_FLAG);

	/* Capture the entire register state for the processor.
	* This is needed as once the VM is launched it will begin execution at
	* the defined instruction. This will be the on launch function, within here
	* we need to restore everything back to what it originally was (as we are hijacking and containerising the host). */
	DEBUG_PRINT("Capturing host context.\r\n");
	RtlCaptureContext(&lpData->hostContext);

	/* This is where the hypervisor will re-enter once launched,
	* We will use the AC flag within EFLAGS to indicate whether we are hypervised.
	* The first time it gets to this point in code we haven't ran the launch function thus the flag won't be set.
	* We check to ensure that this flag has been set to ensure we don't try launch it twice on the processor. */
	if ((__readeflags() & EFLAGS_ALIGNMENT_CHECK_FLAG_FLAG) == 0)
	{
		status = launchVMMOnProcessor(lpData);
	}
	else
	{
		/* If we are here, we are hypervised, so return success. */
		status = STATUS_SUCCESS;
	}

	return status;
}

/******************** Module Code ********************/

static NTSTATUS launchVMMOnProcessor(PVMM_DATA lpData)
{
	NTSTATUS status;

	/* Read all of the MSRs that are related to VMX. */
	MSR_readXMSR(lpData->msrData, sizeof(lpData->msrData) / sizeof(lpData->msrData[0]), IA32_VMX_BASIC);

	/* Store all of the MTRR-related MSRs. */
	MTRR_readAll(lpData->mtrrTable);

	/* Initialise the memory manager module. */
	status = MemManage_init(&lpData->mmContext, lpData->hostCR3);
	if (NT_SUCCESS(status))
	{
		/* Initialise the MTF structure. */
		MTF_initialise(&lpData->mtfConfig);

		/* Initialise EPT structure. */
		EPT_initialise(&lpData->eptConfig, (const PMTRR_RANGE)&lpData->mtrrTable);

		/* Initialise all of the pending hooks. */
		VMHook_init(&lpData->eptConfig);

		/* Attempt to enter VMX root. */
		status = enterRootMode(lpData);

		if (NT_SUCCESS(status))
		{
			/* Initialise VMCS for both the guest and host. */
			setupVMCS(lpData);

			/* Launch hypervisor using VMX. */
			status = launchVMX();
		}
	}

	return status;
}

static NTSTATUS enterRootMode(PVMM_DATA lpData)
{
	/* Instead of using magic numbers, calulate the indexes of the ID's we need. */
	static const UINT32 INDEX_VMX_BASIC = IA32_VMX_BASIC - IA32_VMX_BASIC;
	static const UINT32 INDEX_VMX_EPT_VPID = IA32_VMX_EPT_VPID_CAP - IA32_VMX_BASIC;
	static const UINT32 INDEX_VMX_CR0_FIXED0 = IA32_VMX_CR0_FIXED0 - IA32_VMX_BASIC;
	static const UINT32 INDEX_VMX_CR0_FIXED1 = IA32_VMX_CR0_FIXED1 - IA32_VMX_BASIC;
	static const UINT32 INDEX_VMX_CR4_FIXED0 = IA32_VMX_CR4_FIXED0 - IA32_VMX_BASIC;
	static const UINT32 INDEX_VMX_CR4_FIXED1 = IA32_VMX_CR4_FIXED1 - IA32_VMX_BASIC;

	NTSTATUS status = STATUS_INTERNAL_ERROR;

	/* Ensure the VMCS can fit into a single page. */
	if (IA32_VMX_BASIC_VMCS_SIZE_IN_BYTES(lpData->msrData[INDEX_VMX_BASIC].QuadPart) > PAGE_SIZE)
	{
		DEBUG_PRINT("VMCS does not fit inside a single page.\r\n");
		return status;
	}

	/* Ensure the VMCS is supported in writeback memory. */
	if (IA32_VMX_BASIC_MEMORY_TYPE(lpData->msrData[INDEX_VMX_BASIC].QuadPart) != MEMORY_TYPE_WRITE_BACK)
	{
		DEBUG_PRINT("VMCS is not supported in writeback memory.\r\n");
		return status;
	}

	/* Ensure that true MSRs can be used for capabilities. */
	if (IA32_VMX_BASIC_VMX_CONTROLS(lpData->msrData[INDEX_VMX_BASIC].QuadPart) == 0)
	{
		DEBUG_PRINT("MSRs cannot be used for capabilities.\r\n");
		return status;
	}

	/* Ensure that EPT is available with the features needed. */
	if ((lpData->msrData[INDEX_VMX_EPT_VPID].QuadPart & IA32_VMX_EPT_VPID_CAP_PAGE_WALK_LENGTH_4_FLAG) &&
		(lpData->msrData[INDEX_VMX_EPT_VPID].QuadPart & IA32_VMX_EPT_VPID_CAP_MEMORY_TYPE_WRITE_BACK_FLAG) &&
		(lpData->msrData[INDEX_VMX_EPT_VPID].QuadPart & IA32_VMX_EPT_VPID_CAP_PDE_2MB_PAGES_FLAG))
	{
		lpData->eptControls = IA32_VMX_PROCBASED_CTLS2_ENABLE_EPT_FLAG | IA32_VMX_PROCBASED_CTLS2_ENABLE_VPID_FLAG;
	}

	/* Set the revision ID for the VMXON and VMCS regions to what was received in the MSR. */
	lpData->vmxOn.RevisionId = lpData->msrData[INDEX_VMX_BASIC].LowPart;
	lpData->vmcs.RevisionId = lpData->msrData[INDEX_VMX_BASIC].LowPart;

	/* Update CR0 with the fixed 0 and fixed 1 requirments. */
	lpData->controlRegisters.Cr0 &= lpData->msrData[INDEX_VMX_CR0_FIXED1].LowPart;
	lpData->controlRegisters.Cr0 |= lpData->msrData[INDEX_VMX_CR0_FIXED0].LowPart;

	/* Do the same with CR1. */
	lpData->controlRegisters.Cr4 &= lpData->msrData[INDEX_VMX_CR4_FIXED1].LowPart;
	lpData->controlRegisters.Cr4 |= lpData->msrData[INDEX_VMX_CR4_FIXED0].LowPart;

	/* Update the hosts CR0 and CR4 with the new requirements. */
	__writecr0(lpData->controlRegisters.Cr0);
	__writecr4(lpData->controlRegisters.Cr4);

	UINT64 vmxOnPhysicalAddress = MmGetPhysicalAddress(&lpData->vmxOn).QuadPart;

	/* Enable VMX root mode. */
	if (__vmx_on(&vmxOnPhysicalAddress))
	{
		DEBUG_PRINT("Unable to enter VMX root mode.\r\n");
		return status;
	}

	UINT64 vmcsPhysicalAddress = MmGetPhysicalAddress(&lpData->vmcs).QuadPart;

	/* Clear the state of the VMCS, setting it to inactive. */
	if (__vmx_vmclear(&vmcsPhysicalAddress))
	{
		DEBUG_PRINT("Unable to clear the VMCS.\r\n");
		__vmx_off();
		return status;
	}

	/* Load the VMCS, setting its state to active. */
	if (__vmx_vmptrld(&vmcsPhysicalAddress))
	{
		DEBUG_PRINT("Unable to load the VMCS.\r\n");
		__vmx_off();
		return status;
	}

	/* If we have reached this point, we have successfully entered root mode. */
	return STATUS_SUCCESS;
}

static void setupVMCS(PVMM_DATA lpData)
{
	PCONTROL_REGISTERS controlRegisters = &lpData->controlRegisters;
	PCONTEXT context = &lpData->hostContext;

	DEBUG_PRINT("Initialising the VMCS for the logical processor.\r\n");

	/* Set the link pointer to the required value for the 4KB VMCS. */
	__vmx_vmwrite(VMCS_GUEST_VMCS_LINK_POINTER, ~0ULL);

	/* Enable EPT if it is supported. */
	if (0 != lpData->eptControls)
	{
		/* Load the EPT root pointer. */
		__vmx_vmwrite(VMCS_CTRL_EPT_POINTER, lpData->eptConfig.eptPointer.Flags);

		/* Set the VPID to one. */
		__vmx_vmwrite(VMCS_CTRL_VIRTUAL_PROCESSOR_IDENTIFIER, 1);
	}

	/* Load the MSR bitmap. Unlike other bitmaps, not having a MSR bitmap will trap all of the MSRs,
	* So we allocate an empty MSR bitmap. */
	UINT64 msrBitmapPhysicalAddress = MmGetPhysicalAddress(&lpData->msrBitmap).QuadPart;
	__vmx_vmwrite(VMCS_CTRL_MSR_BITMAP_ADDRESS, msrBitmapPhysicalAddress);

	/*
	* Enable support for RDTSCP and XSAVES/XRESTORES in the guest. Windows 10
	* makes use of both of these instructions if the CPU supports it. By using
	* MSR_adjustMSR, these options will be ignored if this processor does
	* not actually support the instructions to begin with.
	*
	* Also enable EPT support, for additional performance and ability to trap
	* memory access efficiently.
	*/
	UINT32 adjustedMSR = MSR_adjustMSR(lpData->msrData[11],
		IA32_VMX_PROCBASED_CTLS2_ENABLE_RDTSCP_FLAG |
		IA32_VMX_PROCBASED_CTLS2_ENABLE_INVPCID_FLAG |
		IA32_VMX_PROCBASED_CTLS2_ENABLE_XSAVES_FLAG |
		lpData->eptControls);

	__vmx_vmwrite(VMCS_CTRL_SECONDARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, adjustedMSR);

	/*
	* Enable no pin-based options ourselves, but there may be some required by
	* the processor. Use ShvUtilAdjustMsr to add those in.
	*/
	adjustedMSR = MSR_adjustMSR(lpData->msrData[13], 0);
	__vmx_vmwrite(VMCS_CTRL_PIN_BASED_VM_EXECUTION_CONTROLS, adjustedMSR);

	/*
	* In order for our choice of supporting RDTSCP and XSAVE/RESTORES above to
	* actually mean something, we have to request secondary controls. We also
	* want to activate the MSR bitmap in order to keep them from being caught.
	*/
	adjustedMSR = MSR_adjustMSR(lpData->msrData[14],
		IA32_VMX_PROCBASED_CTLS_USE_MSR_BITMAPS_FLAG |
		IA32_VMX_PROCBASED_CTLS_ACTIVATE_SECONDARY_CONTROLS_FLAG | 
		IA32_VMX_PROCBASED_CTLS_CR3_LOAD_EXITING_FLAG);

	__vmx_vmwrite(VMCS_CTRL_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, adjustedMSR);

	/* Make sure to enter us in x64 mode at all times.*/
	adjustedMSR = MSR_adjustMSR(lpData->msrData[15], IA32_VMX_EXIT_CTLS_HOST_ADDRESS_SPACE_SIZE_FLAG);
	__vmx_vmwrite(VMCS_CTRL_VMEXIT_CONTROLS, adjustedMSR);

	/* As we exit back into the guest, make sure to exist in x64 mode as well. */
	adjustedMSR = MSR_adjustMSR(lpData->msrData[16], IA32_VMX_ENTRY_CTLS_IA32E_MODE_GUEST_FLAG);
	__vmx_vmwrite(VMCS_CTRL_VMENTRY_CONTROLS, adjustedMSR);

	/* Load the CS Segment (Ring 0 Code) */
	VMX_GDTENTRY64 vmxGdtEntry;

	GDT_convertGdtEntry(controlRegisters->Gdtr.Base, context->SegCs, &vmxGdtEntry);
	__vmx_vmwrite(VMCS_GUEST_CS_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(VMCS_GUEST_CS_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(VMCS_GUEST_CS_ACCESS_RIGHTS, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(VMCS_GUEST_CS_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(VMCS_HOST_CS_SELECTOR, context->SegCs & ~RPL_MASK);

	/* Load the SS Segment (Ring 0 Data) */
	GDT_convertGdtEntry(controlRegisters->Gdtr.Base, context->SegSs, &vmxGdtEntry);
	__vmx_vmwrite(VMCS_GUEST_SS_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(VMCS_GUEST_SS_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(VMCS_GUEST_SS_ACCESS_RIGHTS, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(VMCS_GUEST_SS_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(VMCS_HOST_SS_SELECTOR, context->SegSs & ~RPL_MASK);

	/* Load the DS Segment (Ring 3 Data) */
	GDT_convertGdtEntry(controlRegisters->Gdtr.Base, context->SegDs, &vmxGdtEntry);
	__vmx_vmwrite(VMCS_GUEST_DS_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(VMCS_GUEST_DS_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(VMCS_GUEST_DS_ACCESS_RIGHTS, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(VMCS_GUEST_DS_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(VMCS_HOST_DS_SELECTOR, context->SegDs & ~RPL_MASK);

	/* Load the ES Segment (Ring 3 Data) */
	GDT_convertGdtEntry(controlRegisters->Gdtr.Base, context->SegEs, &vmxGdtEntry);
	__vmx_vmwrite(VMCS_GUEST_ES_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(VMCS_GUEST_ES_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(VMCS_GUEST_ES_ACCESS_RIGHTS, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(VMCS_GUEST_ES_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(VMCS_HOST_ES_SELECTOR, context->SegEs & ~RPL_MASK);

	/* Load the FS Segment (Ring 3 Compatibility-Mode TEB) */
	GDT_convertGdtEntry(controlRegisters->Gdtr.Base, context->SegFs, &vmxGdtEntry);
	__vmx_vmwrite(VMCS_GUEST_FS_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(VMCS_GUEST_FS_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(VMCS_GUEST_FS_ACCESS_RIGHTS, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(VMCS_GUEST_FS_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(VMCS_HOST_FS_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(VMCS_HOST_FS_SELECTOR, context->SegFs & ~RPL_MASK);

	/* Load the GS Segment (Ring 3 Data if in Compatibility-Mode, MSR-based in Long Mode) */
	GDT_convertGdtEntry(controlRegisters->Gdtr.Base, context->SegGs, &vmxGdtEntry);
	__vmx_vmwrite(VMCS_GUEST_GS_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(VMCS_GUEST_GS_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(VMCS_GUEST_GS_ACCESS_RIGHTS, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(VMCS_GUEST_GS_BASE, controlRegisters->MsrGsBase);
	__vmx_vmwrite(VMCS_HOST_GS_BASE, controlRegisters->MsrGsBase);
	__vmx_vmwrite(VMCS_HOST_GS_SELECTOR, context->SegGs & ~RPL_MASK);

	/* Load the Task Register (Ring 0 TSS) */
	GDT_convertGdtEntry(controlRegisters->Gdtr.Base, controlRegisters->Tr, &vmxGdtEntry);
	__vmx_vmwrite(VMCS_GUEST_TR_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(VMCS_GUEST_TR_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(VMCS_GUEST_TR_ACCESS_RIGHTS, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(VMCS_GUEST_TR_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(VMCS_HOST_TR_BASE, vmxGdtEntry.Base);
	__vmx_vmwrite(VMCS_HOST_TR_SELECTOR, controlRegisters->Tr & ~RPL_MASK);

	/* Load the Local Descriptor Table (Ring 0 LDT on Redstone) */
	GDT_convertGdtEntry(controlRegisters->Gdtr.Base, controlRegisters->Ldtr, &vmxGdtEntry);
	__vmx_vmwrite(VMCS_GUEST_LDTR_SELECTOR, vmxGdtEntry.Selector);
	__vmx_vmwrite(VMCS_GUEST_LDTR_LIMIT, vmxGdtEntry.Limit);
	__vmx_vmwrite(VMCS_GUEST_LDTR_ACCESS_RIGHTS, vmxGdtEntry.AccessRights);
	__vmx_vmwrite(VMCS_GUEST_LDTR_BASE, vmxGdtEntry.Base);

	/* Now load the GDT itself */
	__vmx_vmwrite(VMCS_GUEST_GDTR_BASE, (uintptr_t)controlRegisters->Gdtr.Base);
	__vmx_vmwrite(VMCS_GUEST_GDTR_LIMIT, controlRegisters->Gdtr.Limit);
	__vmx_vmwrite(VMCS_HOST_GDTR_BASE, (uintptr_t)controlRegisters->Gdtr.Base);

	/* And then the IDT */
	__vmx_vmwrite(VMCS_GUEST_IDTR_BASE, (uintptr_t)controlRegisters->Idtr.Base);
	__vmx_vmwrite(VMCS_GUEST_IDTR_LIMIT, controlRegisters->Idtr.Limit);
	__vmx_vmwrite(VMCS_HOST_IDTR_BASE, (uintptr_t)controlRegisters->Idtr.Base);

	/* Load CR0 */
	__vmx_vmwrite(VMCS_CTRL_CR0_READ_SHADOW, controlRegisters->Cr0);
	__vmx_vmwrite(VMCS_HOST_CR0, controlRegisters->Cr0);
	__vmx_vmwrite(VMCS_GUEST_CR0, controlRegisters->Cr0);

	/*
	* Load CR3 -- do not use the current process' address space for the host,
	* because we may be executing in an arbitrary user-mode process right now
	* as part of the DPC interrupt we execute in.
	*/
	__vmx_vmwrite(VMCS_HOST_CR3, lpData->hostCR3.Flags);
	__vmx_vmwrite(VMCS_GUEST_CR3, controlRegisters->Cr3);
	__vmx_vmwrite(VMCS_CTRL_CR3_TARGET_COUNT, 0);

	/* Load CR4 */
	__vmx_vmwrite(VMCS_HOST_CR4, controlRegisters->Cr4);
	__vmx_vmwrite(VMCS_GUEST_CR4, controlRegisters->Cr4);

	/* Set the guest/host mask and a shadow, this is used to hide
	* the fact that VMXE bit in CR4 is set.
	*
	* Setting a bit to 1 ensures that the bit is host owned,
	* meaning that the value will be read from the shadow register.
	*
	* Setting a bit to 0 ensures that the bit is guest owned,
	* meaning that the actual value will be read.
	*/
	static const UINT64 POSITION_VMXE_BIT = (1 << 13);
	static const UINT64 DISABLE_VMXE_BIT = ~((UINT64)(1 << 13));

	__vmx_vmwrite(VMCS_CTRL_CR4_GUEST_HOST_MASK, POSITION_VMXE_BIT);
	__vmx_vmwrite(VMCS_CTRL_CR4_READ_SHADOW, controlRegisters->Cr4 & DISABLE_VMXE_BIT);

	/* Load debug MSR and register (DR7) */
	__vmx_vmwrite(VMCS_GUEST_DEBUGCTL, controlRegisters->DebugControl);
	__vmx_vmwrite(VMCS_GUEST_DR7, controlRegisters->KernelDr7);

	/*
	* Finally, load the guest stack, instruction pointer, and rflags, which
	* corresponds exactly to the location where RtlCaptureContext will return
	* to inside of initialiseVirtualProcessor.
	*/

	/* DEBUG: Setup some marking of the stack. */
	for (UINT32 i = 0; i < sizeof(lpData->hypervisorStack); i++)
	{
		lpData->hypervisorStack[i] = 0xAA;
	}

	__vmx_vmwrite(VMCS_GUEST_RSP, (uintptr_t)lpData->hypervisorStack + KERNEL_STACK_SIZE - sizeof(CONTEXT));
	__vmx_vmwrite(VMCS_GUEST_RIP, (uintptr_t)HandlerShim_hostToGuest);
	__vmx_vmwrite(VMCS_GUEST_RFLAGS, context->EFlags);

	/*
	* Load the hypervisor entrypoint and stack. We give ourselves a standard
	* size kernel stack (24KB) and bias for the context structure that the
	* hypervisor entrypoint will push on the stack, avoiding the need for RSP
	* modifying instructions in the entrypoint. Note that the CONTEXT pointer
	* and thus the stack itself, must be 16-byte aligned for ABI compatibility
	* with AMD64 -- specifically, XMM operations will fail otherwise, such as
	* the ones that RtlCaptureContext will perform.
	*/
	C_ASSERT((KERNEL_STACK_SIZE - sizeof(CONTEXT)) % 16 == 0);
	__vmx_vmwrite(VMCS_HOST_RSP, (uintptr_t)lpData->hypervisorStack + KERNEL_STACK_SIZE - sizeof(CONTEXT));
	__vmx_vmwrite(VMCS_HOST_RIP, (uintptr_t)HandlerShim_guestToHost);
}

static NTSTATUS launchVMX(void)
{
	/* Launch the VMX, Good luck!!! */
	__vmx_vmlaunch();

	/* If we got here, the VMCS failed in some way, or the launch did not proceed as planned.
	* This should never get here in a normal situation. */

	/* Breakpoint for debugging. */
#ifdef _DEBUG 
	if (FALSE == KD_DEBUGGER_NOT_PRESENT)
	{
		DbgBreakPoint();
	}
#endif

	NTSTATUS result = FALSE;
	__vmx_vmread(VMCS_VM_INSTRUCTION_ERROR, &result);

	/* Exit VMX root mode. */
	__vmx_off();

	return result;
}

static void captureControlRegisters(PCONTROL_REGISTERS registers)
{
	registers->Cr0 = __readcr0();
	registers->Cr3 = __readcr3();
	registers->Cr4 = __readcr4();
	registers->DebugControl = __readmsr(IA32_DEBUGCTL);
	registers->MsrGsBase = __readmsr(IA32_GS_BASE);
	registers->KernelDr7 = __readdr(7);

	_sgdt(&registers->Gdtr.Limit);
	__sidt(&registers->Idtr.Limit);
	_str(&registers->Tr);
	_sldt(&registers->Ldtr);
}