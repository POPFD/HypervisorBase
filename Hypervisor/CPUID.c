#include "CPUID.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

/* CPUID register indexes */
typedef enum
{
	CPUID_REGISTER_EAX = 0x00,
	CPUID_REGISTER_EBX,
	CPUID_REGISTER_ECX,
	CPUID_REGISTER_EDX,
	CPUID_REGISTER_COUNT /* Used for calculating number of ID registers*/
} CPUID_REGISTER_INDEX;

/******************** Module Constants ********************/

/* CPUID processor info bits */
#define CPUID_VI_BIT_VMX_EXTENSION 0x20
#define CPUID_VI_BIT_HYPERVISOR_PRESENT 0x80000000

/******************** Module Variables ********************/


/******************** Module Prototypes ********************/


/******************** Public Code ********************/

BOOLEAN CPUID_handle(PVMM_DATA lpData, PCONTEXT guestContext)
{
	UNREFERENCED_PARAMETER(lpData);

	/* Call CPUID instruction based on the indexes in the logical processors RAX and RCX registers.*/
	INT32 cpuInfo[CPUID_REGISTER_COUNT];
	__cpuidex(cpuInfo, (INT32)guestContext->Rax, (INT32)guestContext->Rcx);

	/* Override certain conditions. */
	switch (guestContext->Rax)
	{
		case CPUID_SIGNATURE:
		{
			/* Replace the vendor string with ours. */
			//cpuInfo[CPUID_REGISTER_EBX] = 'ekaF';
			//cpuInfo[CPUID_REGISTER_EDX] = 'etnI';
			//cpuInfo[CPUID_REGISTER_ECX] = '!!!l';
			break;
		}

		case CPUID_VERSION_INFORMATION:
		{
			/* Hide the presence of the hypervisor and support for it. */
			//cpuInfo[CPUID_REGISTER_ECX] = (cpuInfo[CPUID_REGISTER_ECX] & ~CPUID_VI_BIT_VMX_EXTENSION);
			cpuInfo[CPUID_REGISTER_ECX] = (cpuInfo[CPUID_REGISTER_ECX] & ~CPUID_VI_BIT_HYPERVISOR_PRESENT);
			break;
		}
	}

	/* Copy the modified CPU info into the guests registers. */
	guestContext->Rax = cpuInfo[CPUID_REGISTER_EAX];
	guestContext->Rbx = cpuInfo[CPUID_REGISTER_EBX];
	guestContext->Rcx = cpuInfo[CPUID_REGISTER_ECX];
	guestContext->Rdx = cpuInfo[CPUID_REGISTER_EDX];

	return TRUE;
}

/******************** Module Code ********************/