#include <ntifs.h>
#include <intrin.h>
#include "Hypervisor.h"
#include "PageTable.h"
#include "VMM.h"
#include "Debug.h"
#include "ia32.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/


/******************** Module Constants ********************/
#define MAX_LOGICAL_PROCESSORS 64

/******************** Module Variables ********************/

/* Holds the runtime data for each logical processor. */
static VMM_DATA vmmData[MAX_LOGICAL_PROCESSORS] = { 0 };

/******************** Module Prototypes ********************/
static ULONG_PTR logicalProcessorInit(ULONG_PTR argument);
static NTSTATUS isHVSupported(void);

/******************** Public Code ********************/

NTSTATUS Hypervisor_init(void)
{
	/* Initialises the virtualisation of the host environment so that
	 * instrospection of it (as a guest) can take place. */
	NTSTATUS status;

	//if (FALSE == KD_DEBUGGER_NOT_PRESENT)
	//{
	//	DbgBreakPoint();
	//}

	status = isHVSupported();
	if (NT_SUCCESS(status))
	{
		/* Holds the CR3/PML4 entry that our HOST (when we are VMX root) will use. */
		CR3 originalCR3;
		CR3 vmCR3;
		originalCR3.Flags = __readcr3();

		status = PageTable_init(originalCR3, &vmCR3);
		if (NT_SUCCESS(status))
		{

			/* We need to notify each logical processor to start the hypervisor.
			 * This is done using using a IPI.
			 *
			 * TODO: IPI result only returns callee processors status
			 *		 We are discarding other X logical processors results, need to fix this. */
			status = (NTSTATUS)KeIpiGenericCall(logicalProcessorInit, (ULONG_PTR)vmCR3.Flags);
		}
	}

	return status;
}

/******************** Module Code ********************/

static ULONG_PTR logicalProcessorInit(ULONG_PTR argument)
{
	/* Re-cast the argument back to the correct type so we
	 * can use it. */
	CR3 hostCR3;
	hostCR3.Flags = argument;

	NTSTATUS status = STATUS_UNSUCCESSFUL;

	/* Determine which logical processor we are running on, so we can
	 * get a pointer to the config/data space we will be using. */
	ULONG procIndex = KeGetCurrentProcessorIndex();
	PVMM_DATA lpData = &vmmData[procIndex];

	/* We need to ensure that the logical processor this is executing on uses the correct
	 * PML4/CR3 when we are VM ROOT / HOST so we store it in the config. */
	lpData->processorIndex = procIndex;
	lpData->hostCR3 = hostCR3;

	/* Initialise the VMM here. */
	status = VMM_init(lpData);

	/* Explicitly cast to desired format for IPI broadcast. */
	return (ULONG_PTR)status;
}

static NTSTATUS isHVSupported(void)
{
	NTSTATUS status;

	DEBUG_PRINT("Checking to see if a hypervisor is supported.\r\n");

	/* Check to see if VMX extensions are supported on the processor. */
	CPUID_EAX_01 versionInfo;

	__cpuid((int*)&versionInfo, CPUID_VERSION_INFORMATION);

	if (TRUE == versionInfo.CpuidFeatureInformationEcx.VirtualMachineExtensions)
	{
		/* Check to see if the feature control MSR is locked in.
		* If it is not locked in, this means BIOS/UEFI has messed up and we cannot
		* use the MSR properly. */
		IA32_FEATURE_CONTROL_REGISTER featureControl;
		featureControl.Flags = __readmsr(IA32_FEATURE_CONTROL);

		if (TRUE == featureControl.LockBit)
		{
			/* Check to see if VMX is enabled in normal operation. */
			if (TRUE == featureControl.EnableVmxOutsideSmx)
			{
				status = STATUS_SUCCESS;
			}
			else
			{
				DEBUG_PRINT("VMX is not enabled outside of SMX.\r\n");
				status = STATUS_INVALID_PARAMETER;
			}
		}
		else
		{
			DEBUG_PRINT("Feature control bit os not locked.\r\n");
			status = STATUS_INVALID_PARAMETER;
		}
	}
	else
	{
		DEBUG_PRINT("VMX Extensions are not supported.\r\n");
		status = STATUS_NOT_SUPPORTED;
	}

	return status;
}