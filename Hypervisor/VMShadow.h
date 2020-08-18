#pragma once
#include <wdm.h>
#include "EPT.h"

/******************** Public Defines ********************/

/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
BOOLEAN VMShadow_handleEPTViolation(PEPT_CONFIG eptConfig);
NTSTATUS VMShadow_hidePageAsRoot(
	PEPT_CONFIG eptConfig,
	PHYSICAL_ADDRESS targetPA,
	PUINT8 payloadPage,
	BOOLEAN hypervisorRunning
);
NTSTATUS VMShadow_hideCodeAsGuest(
	PUINT8 targetStart,
	PUINT8 payloadStart,
	ULONG payloadSize
);
