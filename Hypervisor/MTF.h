#pragma once
#include <wdm.h>

/******************** Public Typedefs ********************/

typedef struct _MTF_CONFIG
{
	/* List of all MTF handlers that are used. */
	LIST_ENTRY handlerList;

} MTF_CONFIG, *PMTF_CONFIG;

/* Callback function for the MTF trap handler. */
typedef BOOLEAN(*fnMTFHandlerCallback)(PMTF_CONFIG, PVOID userBuffer);

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
void MTF_initialise(PMTF_CONFIG mtfConfig);
BOOLEAN MTF_handleTrap(PMTF_CONFIG mtfConfig);
NTSTATUS MTF_addHandler(PMTF_CONFIG mtfConfig, PUINT8 rangeStart, PUINT8 rangeEnd, fnMTFHandlerCallback callback, PVOID userParameter);
void MTF_setTracingEnabled(BOOLEAN enabled);
