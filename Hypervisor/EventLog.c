#include <ntddk.h>
#include "EventLog.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

/* A single event record, this will be part of the list. */
typedef struct _EVENT_RECORD
{
	LIST_ENTRY listEntry;

	ULONG procIndex;
	UINT64 timeStamp;
	CR0 cr0;
	CR3 cr3;
	CR4 cr4;
	CONTEXT context;
	CHAR extraString[MAX_EXTRA_CHARS];
} EVENT_RECORD, *PEVENT_RECORD;

/******************** Module Constants ********************/


/******************** Module Variables ********************/

static FAST_MUTEX syncMutex = { 0 };
static LIST_ENTRY eventList = { 0 };
static SIZE_T eventCount = 0;

/******************** Module Prototypes ********************/


/******************** Public Code ********************/

void EventLog_init(void)
{
	/* Initialize the synchronization mutex. */
	ExInitializeFastMutex(&syncMutex);

	/* Initialize the linked list for events. */
	InitializeListHead(&eventList);

	eventCount = 0;

	/* DEBUG: Add some fake events. */
	CR0 testCR0 = { 0 };
	CR3 testCR3 = { 0 };
	CR4 testCR4 = { 0 };
	CONTEXT testConext = { 0 };
	EventLog_logEvent(1, testCR0, testCR3, testCR4, &testConext, "debug1");
	EventLog_logEvent(2, testCR0, testCR3, testCR4, &testConext, "debug2");
	EventLog_logEvent(3, testCR0, testCR3, testCR4, &testConext, "debug3");
	EventLog_logEvent(4, testCR0, testCR3, testCR4, &testConext, "debug4");
}

NTSTATUS EventLog_logEvent(ULONG procIndex, CR0 guestCR0, CR3 guestCR3, 
						   CR4 guestCR4, PCONTEXT guestContext, CHAR const* extraString)
{
	NTSTATUS status;

	/* Allocate a new event record. */
	PEVENT_RECORD eventRecord = (PEVENT_RECORD)ExAllocatePool(NonPagedPool, sizeof(EVENT_RECORD));
	if (NULL != eventRecord)
	{
		/* Zero the allocated memory. */
		RtlZeroMemory(eventRecord, sizeof(EVENT_RECORD));

		/* Acquire the synchronization mutex to add to list. */
		ExAcquireFastMutex(&syncMutex);

		/* Fill in the event record fields. */
		eventRecord->procIndex = procIndex;
		eventRecord->timeStamp = __rdtsc();
		eventRecord->cr0 = guestCR0;
		eventRecord->cr3 = guestCR3;
		eventRecord->cr4 = guestCR4;
		eventRecord->context = *guestContext;

		/* Fill in extra text strings. */
		if (NULL != extraString)
		{
			memcpy(eventRecord->extraString, extraString, strlen(extraString));
		}

		/* Add the item to the list. */
		InsertTailList(&eventList, &eventRecord->listEntry);

		/* Increment the event count. */
		eventCount++;

		/* Release the mutex. */
		ExReleaseFastMutex(&syncMutex);

		status = STATUS_SUCCESS;
	}
	else
	{
		status = STATUS_NO_MEMORY;
	}

	return status;
}

NTSTATUS EventLog_retrieveAndClear(PUINT8 buffer, SIZE_T bufferSize)
{
	/* Retrieves X events from the head of the list, based on buffer size. */
	NTSTATUS status;

	/* Ensure that a buffer size if aligned to event boundary and is big enough for at least one. */
	if (bufferSize >= sizeof(EVENT_RECORD))
	{
		/* Acquire the synchronization mutex to use the list. */
		ExAcquireFastMutex(&syncMutex);

		SIZE_T eventCountToReturn = bufferSize / sizeof(EVENT_RECORD);

		for (SIZE_T i = 0; (i < eventCountToReturn); i++)
		{
			/* Always take the head from the list. */
			PEVENT_RECORD eventRecord = CONTAINING_RECORD(eventList.Flink, EVENT_RECORD, listEntry);

			RtlCopyMemory(buffer, eventRecord, sizeof(EVENT_RECORD));

			/* Increment buffer to next location to copy to. */
			buffer += sizeof(EVENT_RECORD);

			/* Remove the head from the list as we have just parsed it. */
			(void)RemoveHeadList(&eventList);

			eventCount--;
		}

		/* Release the mutex. */
		ExReleaseFastMutex(&syncMutex);

		status = STATUS_SUCCESS;
	}
	else
	{
		status = STATUS_INVALID_PARAMETER;
	}

	return status;
}

SIZE_T EventLog_getBufferSize(void)
{
	return eventCount * sizeof(EVENT_RECORD);
}

/******************** Module Code ********************/