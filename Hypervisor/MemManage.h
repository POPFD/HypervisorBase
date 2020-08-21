#pragma once
#include <wdm.h>

/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
NTSTATUS MemManage_changeMemoryProt(PEPROCESS process, PUINT8 baseAddress, SIZE_T size, BOOLEAN writable, BOOLEAN executable);
NTSTATUS MemManage_readVirtualAddress(PEPROCESS targetProcess, void* sourceVA, SIZE_T size, void* targetVA);
NTSTATUS MemManage_writeVirtualAddress(PEPROCESS targetProcess, void* targetVA, SIZE_T size, void* sourceVA);
NTSTATUS MemManage_getPhysFromVirtual(PEPROCESS targetProcess, PVOID virtualAddress, PHYSICAL_ADDRESS* physicalAddress);
NTSTATUS MemManage_getPTEPhysAddressFromVA(PEPROCESS targetProcess, PVOID virtualAddress, PHYSICAL_ADDRESS* physPTE);