#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

/******************** Public Typedefs ********************/

typedef enum
{
	VMCALL_ACTION_GET_PROCESS_BASE,
	VMCALL_ACTION_READ_USER_MEMORY,
	VMCALL_ACTION_WRITE_USER_MEMORY,
	VMCALL_ACTION_SHADOW,
	VMCALL_ACTION_COUNT
} VMCALL_ACTION;

typedef struct _VMCALL_COMMAND
{
	VMCALL_ACTION action;
	PVOID buffer;
	SIZE_T bufferSize;
} VMCALL_COMMAND, *PVMCALL_COMMAND;

typedef struct _VM_PARAM_GET_PROCESS_BASE
{
	DWORD32 procID;		/* IN */
	PVOID processBase;	/* OUT */
} VM_PARAM_GET_PROCESS_BASE, *PVM_PARAM_GET_PROCESS_BASE;

typedef struct _VM_PARAM_RW_USER_MEMORY
{
	DWORD32 procID;	/* IN */
	PUINT8 address;	/* IN */
	PUINT8 buffer;	/* IN */
	SIZE_T size;	/* IN */
} VM_PARAM_RW_USER_MEMORY, *PVM_PARAM_RW_USER_MEMORY;

typedef struct _VM_PARAM_SHADOW
{
	UINT64 physTarget;
	PVOID payloadPage;
} VM_PARAM_SHADOW, *PVM_PARAM_SHADOW;

/******************** Public Constants ********************/

#define VMCALL_KEY	((UINT64)0xDEADDEAD)

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/

/* Calling convention of the function for calling the host. */
NTSTATUS VMCALL_actionHost(UINT64 key, PVMCALL_COMMAND command);

#ifdef __cplusplus
}
#endif
