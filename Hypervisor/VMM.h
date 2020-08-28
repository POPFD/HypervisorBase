#pragma once
#include "ia32.h"
#include "MTRR.h"
#include "EPT.h"
#include "MemManage.h"

/******************** Public Typedefs ********************/

typedef struct _KDESCRIPTOR
{
	UINT16 Pad[3];
	UINT16 Limit;
	void* Base;
} KDESCRIPTOR, *PKDESCRIPTOR;

typedef struct _CONTROL_REGISTERS
{
	UINT64 Cr0;
	UINT64 Cr3;
	UINT64 Cr4;
	UINT64 MsrGsBase;
	UINT16 Tr;
	UINT16 Ldtr;
	UINT64 DebugControl;
	UINT64 KernelDr7;
	KDESCRIPTOR Idtr;
	KDESCRIPTOR Gdtr;
} CONTROL_REGISTERS, *PCONTROL_REGISTERS;

/* Structure for holding information for a logical processor. */
typedef struct _VMM_DATA
{
	/* Hypervisor stack must be at the beginning, this is as when we are hypervised,
	 * we use the stack pointer to find the location of the LP_DATA structure. */
	DECLSPEC_ALIGN(PAGE_SIZE) UINT8 hypervisorStack[KERNEL_STACK_SIZE];

	DECLSPEC_ALIGN(PAGE_SIZE) EPT_CONFIG eptConfig;
	DECLSPEC_ALIGN(PAGE_SIZE) UINT8 msrBitmap[PAGE_SIZE];
	DECLSPEC_ALIGN(PAGE_SIZE) VMCS vmxOn;
	DECLSPEC_ALIGN(PAGE_SIZE) VMCS vmcs;

	MM_CONTEXT mmContext;
	ULONG processorIndex;
	CR3 hostCR3;
	CONTROL_REGISTERS controlRegisters;
	CONTEXT hostContext;
	CONTEXT guestContext;
	LARGE_INTEGER msrData[17];
	MTRR_RANGE mtrrTable[IA32_MTRR_VARIABLE_COUNT];
	UINT32 eptControls;
} VMM_DATA, *PVMM_DATA;

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/

NTSTATUS VMM_init(PVMM_DATA lpData);
