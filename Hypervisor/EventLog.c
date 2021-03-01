#include <ntddk.h>
#include "EventLog.h"
#include "EventLog_Common.h"
#include "Intrinsics.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/


/******************** Module Constants ********************/


/******************** Module Variables ********************/


/******************** Module Prototypes ********************/
static NTSTATUS saveEventToFile(LPWSTR fileName, PEVENT_DATA eventData);

/******************** Public Code ********************/

void EventLog_init(void)
{

}

DECLSPEC_NORETURN void EventLog_logAsGuestThenRestore(PCONTEXT context, ULONG procIndex, CHAR const* extraString)
{
	/* Back up the context to a stack based variable as we need to free
	 * the one allocated by a pool */
	EVENT_DATA eventData;

	eventData.procIndex = procIndex;
	eventData.timeStamp = __rdtsc();
	eventData.cr0.Flags = __readcr0();
	eventData.cr3.Flags = __readcr3();
	eventData.cr4.Flags = __readcr4();
	eventData.context = *context;

	/* Fill in extra text strings. */
	if (NULL != extraString)
	{
		memcpy(eventData.extraString, extraString, strlen(extraString));
	}

	/* Attempt to write it the event to a file. */
	(void)saveEventToFile(L"\\??\\C:\\EventLog.bin", &eventData);

	/* Restore the context. */
	_RestoreFromLog(context, NULL);
}

/******************** Module Code ********************/

static NTSTATUS saveEventToFile(LPWSTR fileName, PEVENT_DATA eventData)
{
	/* Create the file. */
	HANDLE hFile;
	OBJECT_ATTRIBUTES objectAttributes;
	IO_STATUS_BLOCK ioStatusBlock;

	UNICODE_STRING usFilePath;
	RtlInitUnicodeString(&usFilePath, fileName);

	InitializeObjectAttributes(&objectAttributes, &usFilePath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

	KIRQL origIRQL = KeGetCurrentIrql();

	KeLowerIrql(PASSIVE_LEVEL);

	NTSTATUS status = ZwCreateFile(&hFile, GENERIC_WRITE, &objectAttributes, &ioStatusBlock, NULL,
		FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);

	if (NT_SUCCESS(status))
	{
		/* Write the current event data to the file. */
		LARGE_INTEGER byteOffset;
		byteOffset.HighPart = -1;
		byteOffset.LowPart = FILE_WRITE_TO_END_OF_FILE;

		status = ZwWriteFile(hFile, NULL, NULL, NULL, &ioStatusBlock,
			eventData, (ULONG)sizeof(EVENT_DATA), &byteOffset, NULL);

		ZwClose(hFile);
	}

	KeRaiseIrql(origIRQL, &origIRQL);

	return status;
}
