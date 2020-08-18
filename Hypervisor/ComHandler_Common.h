#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

/******************** Public Typedefs ********************/

typedef enum
{
	CH_CHECK_PRESENT,
	CH_GET_MODULE_BASE,
	CH_ALLOCATE_MEMORY,
	CH_CHANGE_MEMORY,
	CH_READ_MEMORY,
	CH_WRITE_MEMORY,
	CH_SHADOW_PAGES,
	CH_ACTION_COUNT
} CH_ACTION;

typedef struct _CH_COMMAND
{
	CH_ACTION action;
	PVOID buffer;
	SIZE_T bufferSize;
} CH_COMMAND, *PCH_COMMAND;

typedef struct _CH_PARAM_GET_MODULE_BASE
{
	DWORD32 procID;
	WCHAR moduleName[50];
	PVOID moduleBase;
} CH_PARAM_GET_MODULE_BASE, *PCH_PARAM_GET_MODULE_BASE;

typedef struct _CH_PARAM_ALLOCATE_MEMORY
{
	DWORD32 procID;		/* IN */
	SIZE_T size;		/* IN */
	ULONG type;			/* IN */
	ULONG protect;		/* IN */
	PVOID baseAddress;	/* OUT */
} CH_PARAM_ALLOCATE_MEMORY, *PCH_PARAM_ALLOCATE_MEMORY;

typedef struct _CH_PARAM_CHANGE_MEMORY
{
	DWORD32 procID;		/* IN */
	PVOID baseAddress;	/* IN */
	SIZE_T size;		/* IN */
	BOOLEAN writable;	/* IN */
	BOOLEAN executable;	/* IN */
} CH_PARAM_CHANGE_MEMORY, *PCH_PARAM_CHANGE_MEMORY;

typedef struct _CH_PARAM_READ_WRITE
{
	DWORD32 procID;
	PVOID targetBuffer;
	PVOID sourceBuffer;
	SIZE_T size;
} CH_PARAM_READ_WRITE, *PCH_PARAM_READ_WRITE;

typedef struct _CH_PARAM_SHADOW_PAGES
{
	PUINT8 targetAddress;
	PUINT8 executePayload;
	ULONG size;
} CH_PARAM_SHADOW_PAGES, *PCH_PARAM_SHADOW_PAGES;

/******************** Public Constants ********************/

#define CH_SECRET ((NTSTATUS)0xDEADBEEF)

/******************** Public Variables ********************/


/******************** Public Prototypes ********************/

#ifdef __cplusplus
}
#endif
