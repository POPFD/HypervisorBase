#include <ntddk.h>
#include "EventLog.h"
#include "EventLog_Common.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

/* A single event record, this will be part of the list. */
typedef struct _EVENT_LIST_ENTRY
{
	LIST_ENTRY listEntry;
	EVENT_DATA data;
} EVENT_LIST_ENTRY, *PEVENT_LIST_ENTRY;

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
}

NTSTATUS EventLog_logEvent(ULONG procIndex, CR0 guestCR0, CR3 guestCR3, 
						   CR4 guestCR4, PCONTEXT guestContext, CHAR const* extraString)
{
	NTSTATUS status;

	/* Allocate a new event record. */
	PEVENT_LIST_ENTRY eventListEntry = (PEVENT_LIST_ENTRY)ExAllocatePool(NonPagedPool, sizeof(EVENT_LIST_ENTRY));
	if (NULL != eventListEntry)
	{
		/* Zero the allocated memory. */
		RtlZeroMemory(eventListEntry, sizeof(EVENT_LIST_ENTRY));

		/* Acquire the synchronization mutex to add to list. */
		ExAcquireFastMutex(&syncMutex);

		/* Fill in the event record fields. */
		eventListEntry->data.procIndex = procIndex;
		eventListEntry->data.timeStamp = __rdtsc();
		eventListEntry->data.cr0 = guestCR0;
		eventListEntry->data.cr3 = guestCR3;
		eventListEntry->data.cr4 = guestCR4;
		eventListEntry->data.context = *guestContext;

		/* Fill in extra text strings. */
		if (NULL != extraString)
		{
			memcpy(eventListEntry->data.extraString, extraString, strlen(extraString));
		}

		/* Add the item to the list. */
		InsertTailList(&eventList, &eventListEntry->listEntry);

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
	if (bufferSize >= sizeof(EVENT_LIST_ENTRY))
	{
		/* Acquire the synchronization mutex to use the list. */
		ExAcquireFastMutex(&syncMutex);

		SIZE_T eventCountToReturn = bufferSize / sizeof(EVENT_LIST_ENTRY);

		for (SIZE_T i = 0; (i < eventCountToReturn); i++)
		{
			/* Always take the head from the list. */
			PEVENT_LIST_ENTRY eventListEntry = CONTAINING_RECORD(eventList.Flink, EVENT_LIST_ENTRY, listEntry);

			RtlCopyMemory(buffer, &eventListEntry->data, sizeof(EVENT_DATA));

			/* Increment buffer to next location to copy to. */
			buffer += sizeof(EVENT_DATA);

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
	return eventCount * sizeof(EVENT_DATA);
}

/******************** Module Code ********************/