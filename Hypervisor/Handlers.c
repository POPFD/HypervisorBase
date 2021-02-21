#include <ntifs.h>
#include <intrin.h>
#include "VMM.h"
#include "Handlers.h"
#include "HandlerShim.h"
#include "Intrinsics.h"
#include "CPUID.h"
#include "VMCALL.h"
#include "VMShadow.h"
#include "Debug.h"

/******************** External API ********************/

/* DEBUG: Just some fields for monitoring VMExits. */
SIZE_T monitoredRangeStart = 0;
SIZE_T monitoredRangeEnd = 0;

/******************** Module Typedefs ********************/


/******************** Module Constants ********************/
#define VM_EXIT_OVERHEAD 500
#define TSC_BELOW_CORRECTION 200

/******************** Module Variables ********************/
static volatile ULONG64 tscOffset = 0;
static volatile ULONG64 lastGuestTSC = 0;

/******************** Module Prototypes ********************/
static void handleExitReason(PVMM_DATA lpData);
static void incrementRIP(void);
static void indicateVMXFail(void);

/******************** Public Code ********************/

DECLSPEC_NORETURN VOID Handlers_hostToGuest(void)
{
	PVMM_DATA lpData;

	/* We are currently executing in the state of the guest,
	* we need to find the original lpData structure, the stack pointer should be within
	* the LP data structure, we use this to find the start of the structure. */
	lpData = (PVMM_DATA)((uintptr_t)_AddressOfReturnAddress() +
		sizeof(CONTEXT) -
		KERNEL_STACK_SIZE);

	/* Record that we are running under the context of the hypervisor as a guest,
	* we do this by setting the align check bit in EFLAGS.
	*
	* This is checked within initialiseVirtualProcessor to continue execution when a guest. */
	lpData->hostContext.EFlags |= EFLAGS_ALIGNMENT_CHECK_FLAG_FLAG;

	_RestoreContext(&lpData->hostContext, NULL);
}


DECLSPEC_NORETURN VOID Handlers_guestToHost(PCONTEXT guestContext)
{
	/* Because we had to use RCX when calling RtlCaptureContext, its value
	* was actually pushed on the stack right before the call.
	* We find the bogus value we set to RCX to and overwrite it with what was originally there. */
	guestContext->Rcx = *(UINT64*)((uintptr_t)guestContext - sizeof(guestContext->Rcx));

	/* Find the LP_DATA structure by using the context variable that was passed in (technically the stack). */
	PVMM_DATA lpData = (VOID*)((uintptr_t)(guestContext + 1) - KERNEL_STACK_SIZE);

	/* Copy the guest context into our LP data structure. */
	RtlCopyMemory(&lpData->guestContext, guestContext, sizeof(CONTEXT));

	//UINT64 exitTSCStart = __rdtsc();

	/* Handle the exit reason. */
	handleExitReason(lpData);

	//UINT64 exitTSCTime = __rdtsc() - exitTSCStart;
	//UINT64 correctionTime = exitTSCTime + VM_EXIT_OVERHEAD;

	/* Increment the offset counter for TSC. */
	//InterlockedAdd64((volatile LONG64*)&tscOffset, correctionTime);

	/* Now to restore back to the guest. */

	/* In the assembly stub code there was a PUSH RCX instruction we used,
	* we need to account for that here and negate the effect of it. This is easily
	* done by adjusting the stack pointer by the size of the register */
	lpData->guestContext.Rsp += sizeof(lpData->guestContext.Rcx);

	/* We now set our desired instruction pointer at the VMXResume handler,
	* We use the restore context function to do this rather than a call to ensure
	* that we are in the EXACT same state as we were prior to this function. */
	lpData->guestContext.Rip = (UINT64)Handlers_VMResume;

	/* Restore the context. */
	_RestoreContext(&lpData->guestContext, NULL);
}

DECLSPEC_NORETURN void Handlers_VMResume(void)
{
	/* Issue a VMXRESUME. The reason that we've defined an entire function for
	* this sole instruction is both so that we can use it as the target of the
	* VMCS when re-entering the VM After a VM-Exit, as well as so that we can
	* decorate it with the DECLSPEC_NORETURN marker, which is not set on the
	* intrinsic (as it can fail in case of an error). */
	__vmx_vmresume();
}

/******************** Module Code ********************/

static void handleExitReason(PVMM_DATA lpData)
{
	BOOLEAN moveToNextInstruction = FALSE;

	/* We need to determine what the exit reason was and take appropriate action. */
	size_t exitReason;
	__vmx_vmread(VMCS_EXIT_REASON, &exitReason);
	exitReason &= 0xFFFF;

	///* Check to see if we are actively monitoring a range. */
	//if ((0 != monitoredRangeStart) && (0 != monitoredRangeEnd))
	//{
	//	/* Check to see if guest RIP is within this range. */
	//	/* If so, we log the VMM exit. */
	//	size_t guestRIP;
	//	__vmx_vmread(VMCS_GUEST_RIP, &guestRIP);

	//	if ((guestRIP >= monitoredRangeStart) && (guestRIP <= monitoredRangeEnd))
	//	{
	//		DEBUG_PRINT("VMM exit in monitored range.\n");
	//		DEBUG_PRINT("\tExit Reason: 0x%I64X\n", exitReason);
	//		DEBUG_PRINT("\tGuest RIP: 0x%I64X\n", guestRIP);

	//		static SIZE_T lastExitRIP = 0;

	//		/* Some debug code to prevent multiple breaks on same instruction due to loops */
	//		if (guestRIP != lastExitRIP)
	//		{
	//			//DbgBreakPoint();
	//			lastExitRIP = guestRIP;
	//		}
	//	}
	//}

	switch (exitReason)
	{
		case VMX_EXIT_REASON_EXECUTE_RDTSC:
		case VMX_EXIT_REASON_EXECUTE_RDTSCP:
		{
			/* Read the current TSC. */
			UINT64 hostTSC = __readmsr(IA32_TIME_STAMP_COUNTER);

			UINT64 guestTSC = hostTSC - tscOffset;

			/* Prevent going back in time. */
			if (guestTSC < lastGuestTSC)
			{
				guestTSC = lastGuestTSC + TSC_BELOW_CORRECTION;
			}

			/* Store last sent TSC. */
			lastGuestTSC = guestTSC;

			/* Set the guest registers to the TSC value. */
			lpData->guestContext.Rdx = (UINT32)(guestTSC >> 32);
			lpData->guestContext.Rax = (UINT32)(guestTSC & 0xFFFFFFFF);

			/* Set the auxiliary TSC value if RDTSCP was reason. */
			if (VMX_EXIT_REASON_EXECUTE_RDTSCP == exitReason)
			{
				lpData->guestContext.Rcx = (UINT32)__readmsr(IA32_TSC_AUX);
			}

			moveToNextInstruction = TRUE;
			break;
		}

		case VMX_EXIT_REASON_MONITOR_TRAP_FLAG:
		{
			if (TRUE == MTF_handleTrap(&lpData->mtfConfig))
			{
				/* Do nothing. */
			}
			else
			{
				DbgBreakPoint();
			}

			break;
		}

		case VMX_EXIT_REASON_EPT_VIOLATION:
		{
			if (TRUE == EPT_handleViolation(&lpData->eptConfig, &lpData->guestContext))
			{
				/* If we have handled the violation properly, we don't want to move to the next instruction,
				* We want to try process the instruction again, now that the page has been switched. */
				moveToNextInstruction = FALSE;
			}
			else
			{
				DbgBreakPoint();
			}

			break;
		}

		case VMX_EXIT_REASON_MOV_CR:
		{
			/* If we have handled the MOV to/from CR correctly,
			 * we go to the next instruction. */
			moveToNextInstruction = VMShadow_handleMovCR(lpData);
			break;
		}

		case VMX_EXIT_REASON_EXECUTE_INVD:
		{
			__wbinvd();
			moveToNextInstruction = TRUE;
			break;
		}

		case VMX_EXIT_REASON_EXECUTE_XSETBV:
		{
			_xsetbv((UINT32)lpData->guestContext.Rcx, lpData->guestContext.Rdx << 32 | lpData->guestContext.Rax);
			moveToNextInstruction = TRUE;
			break;
		}

		case VMX_EXIT_REASON_EXECUTE_RDMSR:
		{
			UINT64 msrResult = __readmsr((UINT32)lpData->guestContext.Rcx);

			lpData->guestContext.Rdx = msrResult >> 32;
			lpData->guestContext.Rax = msrResult & 0xFFFFFFFF;
			moveToNextInstruction = TRUE;
			break;
		}

		case VMX_EXIT_REASON_EXECUTE_WRMSR:
		{
			/* Only take 32 bits from each register. */
			UINT32 highBits = (UINT32)lpData->guestContext.Rdx;
			UINT32 lowBits = (UINT32)lpData->guestContext.Rax;

			UINT64 value = ((UINT64)highBits << 32) | lowBits;

			__writemsr((UINT32)lpData->guestContext.Rcx, value);

			moveToNextInstruction = TRUE;
			break;
		}

		case VMX_EXIT_REASON_EXECUTE_CPUID:
		{
			if (TRUE == CPUID_handle(lpData))
			{
				moveToNextInstruction = TRUE;
			}
			break;
		}

		case VMX_EXIT_REASON_EXECUTE_VMCALL:
		{
			if (TRUE == VMCALL_handle(lpData))
			{
				moveToNextInstruction = TRUE;
			}
			else
			{
				indicateVMXFail();
				moveToNextInstruction = FALSE;
			}
			break;
		}

		case VMX_EXIT_REASON_EXECUTE_VMCLEAR:
		case VMX_EXIT_REASON_EXECUTE_VMLAUNCH:
		case VMX_EXIT_REASON_EXECUTE_VMPTRLD:
		case VMX_EXIT_REASON_EXECUTE_VMPTRST:
		case VMX_EXIT_REASON_EXECUTE_VMREAD:
		case VMX_EXIT_REASON_EXECUTE_VMRESUME:
		case VMX_EXIT_REASON_EXECUTE_VMWRITE:
		case VMX_EXIT_REASON_EXECUTE_VMXOFF:
		case VMX_EXIT_REASON_EXECUTE_VMXON:
		case VMX_EXIT_REASON_EXECUTE_INVEPT:
		{
			indicateVMXFail();
			moveToNextInstruction = FALSE;
			break;
		}

		default:
		{
			if (FALSE == KD_DEBUGGER_NOT_PRESENT)
			{
				DbgBreakPoint();
			}

			DEBUG_PRINT("Unhandled VMExit with reason: 0x%I64X\r\n", exitReason);
			break;
		}
	}

	if (TRUE == moveToNextInstruction)
	{
		incrementRIP();
	}
}

static void incrementRIP(void)
{
	/* Move the instruction pointer to the next instruction after the one that
	* caused the exit. */
	size_t guestRIP;
	__vmx_vmread(VMCS_GUEST_RIP, &guestRIP);

	size_t instructionLength;
	__vmx_vmread(VMCS_VMEXIT_INSTRUCTION_LENGTH, &instructionLength);
	guestRIP += instructionLength;

	__vmx_vmwrite(VMCS_GUEST_RIP, guestRIP);

	RFLAGS guestRFLAGS;
	__vmx_vmread(VMCS_GUEST_RFLAGS, &guestRFLAGS.Flags);

	/* Check to see if trap flag set. */
	if (TRUE == guestRFLAGS.TrapFlag)
	{
		/* Check to see if bit BTF is clear in DEBUGCTL, if so
		 * single step on instructions. */
		IA32_DEBUGCTL_REGISTER debugCtrl = { 0 };
		__vmx_vmread(VMCS_GUEST_DEBUGCTL, &debugCtrl.Flags);

		if (FALSE == debugCtrl.Btf)
		{
			/* Clear the trap flag, and write to guest. */
			guestRFLAGS.TrapFlag = FALSE;
			__vmx_vmwrite(VMCS_GUEST_RFLAGS, guestRFLAGS.Flags);

			/* Clear the blocking interruptibility state fields (apart from NMI)
			 * So bits [2:0]. */
			UINT64 guestIS = 0;
			__vmx_vmread(VMCS_GUEST_INTERRUPTIBILITY_STATE, &guestIS);

			guestIS &= ~7;
			__vmx_vmwrite(VMCS_GUEST_INTERRUPTIBILITY_STATE, guestIS);

			/* Inject the interrupt for TF. */
			VMENTRY_INTERRUPT_INFORMATION interruptInfo = { 0 };
			interruptInfo.Vector = Debug;
			interruptInfo.InterruptionType = HardwareException;
			interruptInfo.DeliverErrorCode = FALSE;
			interruptInfo.Valid = TRUE;
			__vmx_vmwrite(VMCS_CTRL_VMENTRY_INTERRUPTION_INFORMATION_FIELD, interruptInfo.Flags);
		}
	}
}

static void indicateVMXFail(void)
{
	VMENTRY_INTERRUPT_INFORMATION interruptInfo;

	/* Zero the vector*/
	interruptInfo.Flags = 0; /* Zero the field first. */
	interruptInfo.Vector = InvalidOpcode;
	interruptInfo.InterruptionType = HardwareException;
	interruptInfo.DeliverErrorCode = 0;
	interruptInfo.Valid = 1;

	/* Set the exception. */
	__vmx_vmwrite(VMCS_CTRL_VMENTRY_INTERRUPTION_INFORMATION_FIELD, interruptInfo.Flags);

	/* Indicate the instruction shouldn't move to next. */
	__vmx_vmwrite(VMCS_CTRL_VMENTRY_INSTRUCTION_LENGTH, 0);

	/* Set the CF flag, this is how VMX instructions indicate a failure. */
	UINT64 guestFlags;
	__vmx_vmread(VMCS_GUEST_RFLAGS, &guestFlags);
	guestFlags |= EFLAGS_CARRY_FLAG_FLAG;

	/* Set the EFLAGS in the VMCS with the updated field. */
	//__vmx_vmwrite(VMCS_GUEST_RFLAGS, guestFlags);
}
