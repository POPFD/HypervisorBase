#include <intrin.h>
#include <ntddk.h>
#include "MemManage.h"
#include "Paging.h"
#include "ia32.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/


/******************** Module Constants ********************/
#define OFFSET_DIRECTORY_TABLE_BASE 0x028
#define OFFSET_USER_DIR_TABLE 0x280

/******************** Module Variables ********************/


/******************** Module Prototypes ********************/

static UINT64 getTableBase(PEPROCESS process);
static NTSTATUS readPhysicalAddress(PHYSICAL_ADDRESS sourceAddress, SIZE_T size, void* targetAddress);
static NTSTATUS writePhysicalAddress(PHYSICAL_ADDRESS targetAddress, SIZE_T size, void* sourceAddress);

/******************** Public Code ********************/

NTSTATUS MemManage_changeMemoryProt(PEPROCESS process, PUINT8 baseAddress, SIZE_T size, BOOLEAN writable, BOOLEAN executable)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;

	PUINT8 startVA = PAGE_ALIGN(baseAddress);
	PUINT8 endVA = baseAddress + size;

	for (PUINT8 currentVA = startVA; currentVA < endVA; currentVA += PAGE_SIZE)
	{
		/* Get the physical address of the page table entry. */
		PHYSICAL_ADDRESS physPTE;
		status = MemManage_getPTEPhysAddressFromVA(process, currentVA, &physPTE);
		if (NT_SUCCESS(status))
		{
			/* Read the original page table entry. */
			PTE readPTE;

			status = readPhysicalAddress(physPTE, sizeof(readPTE), &readPTE);
			if (NT_SUCCESS(status))
			{
				/* Modify the page table entry with new flags. */
				readPTE.ReadWrite = writable;
				readPTE.ExecuteDisable = (FALSE == executable);

				/* Write the page table entry. */
				status = writePhysicalAddress(physPTE, sizeof(readPTE), &readPTE);
				if (NT_ERROR(status))
				{
					/* Error detected so break from the loop. */
					break;
				}
			}
		}
	}

	return status;
}

NTSTATUS MemManage_readVirtualAddress(PEPROCESS targetProcess, void* sourceVA, SIZE_T size, void* targetVA)
{
	NTSTATUS status;

	/* Get the physical address of the source. */
	PHYSICAL_ADDRESS targetSource;
	status = MemManage_getPhysFromVirtual(targetProcess, sourceVA, &targetSource);
	if (NT_SUCCESS(status))
	{
		status = readPhysicalAddress(targetSource, size, targetVA);
	}

	return status;
}

NTSTATUS MemManage_writeVirtualAddress(PEPROCESS targetProcess, void* targetVA, SIZE_T size, void* sourceVA)
{
	NTSTATUS status;

	/* Get the physical address of the target. */
	PHYSICAL_ADDRESS targetPhysical;
	status = MemManage_getPhysFromVirtual(targetProcess, targetVA, &targetPhysical);
	if (NT_SUCCESS(status))
	{
		status = writePhysicalAddress(targetPhysical, size, sourceVA);
	}

	return status;
}

NTSTATUS MemManage_getPhysFromVirtual(PEPROCESS targetProcess, PVOID virtualAddress, PHYSICAL_ADDRESS* physicalAddress)
{
	/* Find the physical address of the PTE for the virtual address. */
	PHYSICAL_ADDRESS physPTE;
	NTSTATUS status = MemManage_getPTEPhysAddressFromVA(targetProcess, virtualAddress, &physPTE);
	if (NT_SUCCESS(status))
	{
		/* Read the PTE. */
		PTE readPTE;
		status = readPhysicalAddress(physPTE, sizeof(readPTE), &readPTE);
		if (NT_SUCCESS(status))
		{
			/* Convert from page number to physical address. */
			physicalAddress->QuadPart = (readPTE.PageFrameNumber * PAGE_SIZE) + ADDRMASK_PML1_OFFSET(virtualAddress);
		}
	}

	return status;
}

NTSTATUS MemManage_getPTEPhysAddressFromVA(PEPROCESS targetProcess, PVOID virtualAddress, PHYSICAL_ADDRESS* physPTE)
{
	NTSTATUS status;

	/* Get the base address of the paging table/PML4. */
	UINT64 tableBase = getTableBase(targetProcess);

	/* Gather the indexes for the page tables from the VA. */
	UINT64 indexPML4 = ADDRMASK_PML4_INDEX(virtualAddress);
	UINT64 indexPML3 = ADDRMASK_PML3_INDEX(virtualAddress);
	UINT64 indexPML2 = ADDRMASK_PML2_INDEX(virtualAddress);
	UINT64 indexPML1 = ADDRMASK_PML1_INDEX(virtualAddress);

	/* Read PML4 from the guest. */
	PHYSICAL_ADDRESS physPML4;
	physPML4.QuadPart = (LONGLONG)&((PPML4E)tableBase)[indexPML4];
	PML4E readPML4;

	status = readPhysicalAddress(physPML4, sizeof(readPML4), &readPML4);
	if (NT_SUCCESS(status))
	{
		if (TRUE == readPML4.Present)
		{
			/* Read PML3 from the guest. */
			PHYSICAL_ADDRESS physPML3;
			physPML3.QuadPart = (LONGLONG)&((PPDPTE)(readPML4.PageFrameNumber * PAGE_SIZE))[indexPML3];
			PDPTE readPML3;

			status = readPhysicalAddress(physPML3, sizeof(readPML3), &readPML3);
			if (NT_SUCCESS(status))
			{
				if (TRUE == readPML3.Present)
				{
					/* Read PML2 from the guest. */
					PHYSICAL_ADDRESS physPML2;
					physPML2.QuadPart = (LONGLONG)&((PPDE)(readPML3.PageFrameNumber * PAGE_SIZE))[indexPML2];
					PDE readPML2;

					status = readPhysicalAddress(physPML2, sizeof(readPML2), &readPML2);
					if (NT_SUCCESS(status))
					{
						if (TRUE == readPML2.Present)
						{
							/* Read PML1E from the guest. */
							physPTE->QuadPart = (LONGLONG)&((PPTE)(readPML2.PageFrameNumber * PAGE_SIZE))[indexPML1];
						}
						else
						{
							status = STATUS_INVALID_ADDRESS;
						}
					}
				}
				else
				{
					status = STATUS_INVALID_ADDRESS;
				}
			}
		}
		else
		{
			status = STATUS_INVALID_ADDRESS;
		}
	}

	return status;
}

/******************** Module Code ********************/

static UINT64 getTableBase(PEPROCESS process)
{
	/* As KVA shadowing is used for CR3 as a mitigation for spectre/meltdown
	* we cannot use the CR3 as it is a shadowed table instead. Directly
	* access to DirectoryTableBase of the process that caused
	* the VMExit.
	*
	* UserDirectoryTableBase at a fixed offset, EPROCESS->Pcb.UserDirectoryTableBase
	* This was found using the following windbg commands:
	* dt nt!_EPROCESS
	* dt nt!_KPROCESS
	*
	* This is subject to change between Windows versions. */

	/* If running as admin, UserDirectoryTableBase will be set as 1 and KVA shadowing will not be present,
	* therefor we should use the directory table base (kernel). Again due to KVA shadowing... */
	UINT64 tableBase;

	tableBase = *((UINT64*)((UINT64)process + OFFSET_USER_DIR_TABLE));
	tableBase &= ~0xFFF;
	if (0 == tableBase)
	{
		tableBase = *((UINT64*)((UINT64)process + OFFSET_DIRECTORY_TABLE_BASE));
		tableBase &= ~0xFFF;
	}

	return tableBase;
}

static NTSTATUS readPhysicalAddress(PHYSICAL_ADDRESS sourceAddress, SIZE_T size, void* targetAddress)
{
	MM_COPY_ADDRESS copyAddress;
	copyAddress.PhysicalAddress = sourceAddress;

	SIZE_T bytesTransferred;
	return MmCopyMemory(targetAddress, copyAddress, size, MM_COPY_MEMORY_PHYSICAL, &bytesTransferred);
}

static NTSTATUS writePhysicalAddress(PHYSICAL_ADDRESS targetAddress, SIZE_T size, void* sourceAddress)
{
	NTSTATUS status;

	/* Attempt to map the physical memory into this address space. */
	PVOID mappedTarget = MmMapIoSpace(targetAddress, size, MmNonCached);
	if (NULL != mappedTarget)
	{
		/* Copy to the mapped area. */
		RtlCopyMemory(mappedTarget, sourceAddress, size);

		/* Unmap the mapped physical address. */
		MmUnmapIoSpace(mappedTarget, size);

		status = STATUS_SUCCESS;
	}
	else
	{
		status = STATUS_NO_MEMORY;
	}

	return status;
}