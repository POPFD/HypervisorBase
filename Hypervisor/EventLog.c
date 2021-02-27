#include <ntddk.h>
#include "EventLog.h"
#include "EventLog_Common.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

#define MAX_ELEMENTS 50000

typedef struct _EVENT_QUEUE
{
	int head;
	int tail;
	int count;
	EVENT_DATA elements[MAX_ELEMENTS];
} EVENT_QUEUE, *PEVENT_QUEUE;

/******************** Module Constants ********************/


/******************** Module Variables ********************/

static FAST_MUTEX syncMutex = { 0 };
static EVENT_QUEUE eventQueue = { 0 };

/******************** Module Prototypes ********************/

static BOOLEAN isQueueFull(void);
static BOOLEAN isQueueEmpty(void);

/******************** Public Code ********************/

void EventLog_init(void)
{
	/* Initialize the synchronization mutex. */
	ExInitializeFastMutex(&syncMutex);

	/* Initialize the event queue. */
	eventQueue.head = -1;
	eventQueue.tail = -1;
	eventQueue.count = 0;
}

NTSTATUS EventLog_logEvent(ULONG procIndex, CR0 guestCR0, CR3 guestCR3, 
						   CR4 guestCR4, PCONTEXT guestContext, CHAR const* extraString)
{
	NTSTATUS status;

	/* Acquire the synchronization mutex to add to list. */
	ExAcquireFastMutex(&syncMutex);

	if (FALSE == isQueueFull())
	{

		/* Check to see if queue is empty, if so set head. */
		if (-1 == eventQueue.head)
		{
			eventQueue.head++;
		}

		/* Increment the tail pointer, as we are adding an item. */
		eventQueue.tail++;

		/* Now we modulo by the max element size, to prevent overflow
		 * which will reset it to beginning. */
		eventQueue.tail %= MAX_ELEMENTS;

		/* Now add the item info to the queue. */
		PEVENT_DATA eventData = &eventQueue.elements[eventQueue.tail];

		/* Fill in the event record fields. */
		eventData->procIndex = procIndex;
		eventData->timeStamp = __rdtsc();
		eventData->cr0 = guestCR0;
		eventData->cr3 = guestCR3;
		eventData->cr4 = guestCR4;
		eventData->context = *guestContext;

		/* Fill in extra text strings. */
		if (NULL != extraString)
		{
			memcpy(eventData->extraString, extraString, strlen(extraString));
		}

		/* Increment the event count. */
		eventQueue.count++;

		status = STATUS_SUCCESS;
	}
	else
	{
		/* DEBUG: Queue is full, this shouldn't happen without logger being present. */
		DbgBreakPoint();
		status = STATUS_NO_MEMORY;
	}

	/* Release the mutex. */
	ExReleaseFastMutex(&syncMutex);

	return status;
}

NTSTATUS EventLog_retrieveAndClear(PUINT8 buffer, SIZE_T bufferSize)
{
	/* Retrieves X events from the head of the list, based on buffer size. */
	NTSTATUS status;

	/* Ensure that a buffer size if aligned to event boundary and is big enough for at least one. */
	if (bufferSize >= sizeof(EVENT_DATA))
	{
		/* Acquire the synchronization mutex to use the list. */
		ExAcquireFastMutex(&syncMutex);

		SIZE_T eventCountToReturn = bufferSize / sizeof(EVENT_DATA);

		for (SIZE_T i = 0; (i < eventCountToReturn); i++)
		{
			if (FALSE == isQueueEmpty())
			{
				/* Copy the element data. */
				RtlCopyMemory(buffer, &eventQueue.elements[eventQueue.head], sizeof(EVENT_DATA));

				/* Increment buffer to next location to copy to. */
				buffer += sizeof(EVENT_DATA);

				/* Decrease the item count. */
				eventQueue.count--;

				/* Increase the head index. */
				eventQueue.head++;

				/* Now we modulo by the max element size to prevent
				 * overflow, as this is a circular buffer. */
				eventQueue.head %= MAX_ELEMENTS;

				/* Check the new value of head, if it is above tail
				 * this means we just dequeue'd our last item. */
				if (eventQueue.head > eventQueue.tail)
				{
					eventQueue.head = -1;
					eventQueue.tail = -1;
				}

				status = STATUS_SUCCESS;
			}
			else
			{
				/* DEBUG: This should never be called when the queue is empty. */
				DbgBreakPoint();
				status = STATUS_INTERNAL_ERROR;
			}
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
	/* Acquire the synchronization mutex to use the list. */
	ExAcquireFastMutex(&syncMutex);

	SIZE_T result = eventQueue.count * sizeof(EVENT_DATA);

	/* Release the mutex. */
	ExReleaseFastMutex(&syncMutex);

	return result;
}

/******************** Module Code ********************/

static BOOLEAN isQueueFull(void)
{
	return ((0 == eventQueue.head) && ((MAX_ELEMENTS - 1) == eventQueue.tail)) ||
		(eventQueue.head == (eventQueue.tail + 1));
}

static BOOLEAN isQueueEmpty(void)
{
	return (-1 == eventQueue.tail);
}