#pragma once
#include <wdm.h>
#include "EPT.h"

/******************** Public Defines ********************/

/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
BOOLEAN VMShadow_handleEPTViolation(PEPT_CONFIG eptConfig);
BOOLEAN VMShadow_handleMovCR(PEPT_CONFIG eptConfig);

NTSTATUS VMShadow_hideExecInProcess(
	PEPT_CONFIG eptConfig,
	PEPROCESS targetProcess, 
	PUINT8 targetVA, 
	PUINT8 execVA
);

NTSTATUS VMShadow_hidePageAsRoot(
	PEPT_CONFIG eptConfig,
	PHYSICAL_ADDRESS targetPA,
	PUINT8 payloadPage,
	BOOLEAN hypervisorRunning
);
