#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

/******************** Public Typedefs ********************/

typedef enum
{
	VMCALL_ACTION_SHADOW_IN_PROCESS,
	VMCALL_ACTION_COUNT
} VMCALL_ACTION;

typedef struct _VMCALL_COMMAND
{
	VMCALL_ACTION action;
	PVOID buffer;
	SIZE_T bufferSize;
} VMCALL_COMMAND, *PVMCALL_COMMAND;

typedef struct _VM_PARAM_SHADOW_PROC
{
	DWORD32 procID;				/* IN */
	PUINT8 userTargetVA;		/* IN */
	PUINT8 kernelExecPageVA;	/* IN */
} VM_PARAM_SHADOW_PROC, *PVM_PARAM_SHADOW_PROC;

/******************** Public Constants ********************/

#define VMCALL_KEY	((UINT64)0xDEADDEAD)

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/

/* Calling convention of the function for calling the host. */
NTSTATUS VMCALL_actionHost(UINT64 key, PVMCALL_COMMAND command);

#ifdef __cplusplus
}
#endif
