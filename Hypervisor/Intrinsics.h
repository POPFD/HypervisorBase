#pragma once
#include <wdm.h>
#include "ia32.h"

/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/

VOID _str(_In_ UINT16* Tr);
VOID _sldt(_In_ UINT16* Ldtr);
VOID __invept(UINT64 Type, INVEPT_DESCRIPTOR* Descriptor);
VOID __invvpid(UINT64 Type, INVVPID_DESCRIPTOR* Descriptor);

DECLSPEC_NORETURN
VOID
__cdecl
_RestoreContext(
	_In_ PCONTEXT ContextRecord,
	_In_opt_ struct _EXCEPTION_RECORD * ExceptionRecord
);
