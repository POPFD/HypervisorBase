#pragma once
#include <wdm.h>
#include "VMM.h"
#include "EPT.h"

/******************** Public Defines ********************/

/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
BOOLEAN VMShadow_handleEPTViolation(PEPT_CONFIG eptConfig);
BOOLEAN VMShadow_handleMovCR(PVMM_DATA lpData);
BOOLEAN VMShadow_handleMTF(PVMM_DATA lpData);

NTSTATUS VMShadow_hidePageGlobally(
	PEPT_CONFIG eptConfig,
	PHYSICAL_ADDRESS targetPA,
	PUINT8 payloadPage,
	BOOLEAN hypervisorRunning
);

NTSTATUS VMShadow_hideExecInProcess(
	PVMM_DATA lpData,
	PEPROCESS targetProcess,
	PUINT8 targetVA,
	PUINT8 execVA
);