#pragma once
#include "ia32.h"

/******************** Public Typedefs ********************/
#define EVENT_COUNT_MAX 100000
#define EVENT_BUFFER_SIZE (EVENT_COUNT_MAX * sizeof(EVENT_DATA))

#define MAX_EXTRA_CHARS 50

typedef struct _EVENT_DATA
{
	ULONG procIndex;
	UINT64 timeStamp;
	CR0 cr0;
	CR3 cr3;
	CR4 cr4;
	CONTEXT context;
	CHAR extraString[MAX_EXTRA_CHARS];
} EVENT_DATA, *PEVENT_DATA;

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
