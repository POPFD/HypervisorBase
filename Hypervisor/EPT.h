#pragma once
#include <wdm.h>
#include "ia32.h"
#include "MTRR.h"

/******************** Public Defines ********************/

#define EPT_PML4E_COUNT		512
#define EPT_PML3E_COUNT		512
#define EPT_PML2E_COUNT     512
#define EPT_PML1E_COUNT		512

#define SIZE_1GB (1 * 1024 * 1024 * 1024)
#define SIZE_2MB (2 * 1024 * 1024)

/* Calculates the offset into the PDE (PML1) structure. */
#define ADDRMASK_EPT_PML1_OFFSET(_VAR_) ((SIZE_T)_VAR_ & 0xFFFULL)

/* Calculates the index of the PDE (PML1) within the PDT structure. */
#define ADDRMASK_EPT_PML1_INDEX(_VAR_) (((SIZE_T)_VAR_ & 0x1FF000ULL) >> 12)

/* Calculates the index of the PDT (PML2) within the PDPT*/
#define ADDRMASK_EPT_PML2_INDEX(_VAR_) (((SIZE_T)_VAR_ & 0x3FE00000ULL) >> 21)

/* Calculates the index of the PDPT (PML3) within the PML4 */
#define ADDRMASK_EPT_PML3_INDEX(_VAR_) (((SIZE_T)_VAR_ & 0x7FC0000000ULL) >> 30)

/* Calculates the index of PML4. */
#define ADDRMASK_EPT_PML4_INDEX(_VAR_) (((SIZE_T)_VAR_ & 0xFF8000000000ULL) >> 39)

/******************** Public Typedefs ********************/

typedef EPT_PML4 EPT_PML4_POINTER, *PEPT_PML4_POINTER;
typedef EPDPTE EPT_PML3_POINTER, *PEPT_PML3_POINTER;
typedef EPDE_2MB EPT_PML2_2MB, *PEPT_PML2_2MB;
typedef EPDE EPT_PML2_POINTER, *PEPT_PML2_POINTER;
typedef EPTE EPT_PML1_ENTRY, *PEPT_PML1_ENTRY;

typedef struct _PHYSICAL_RANGE
{
	PHYSICAL_ADDRESS start;
	PHYSICAL_ADDRESS end;
} PHYSICAL_RANGE, *PPHYSICAL_RANGE;

/* Structure that will hold the PML1 data for a dynamically split PML2 entry. */
typedef struct _EPT_DYNAMIC_SPLIT
{
	/* The 4096 byte page table entries that correspond to the split 2MB table entry. */
	DECLSPEC_ALIGN(PAGE_SIZE) EPT_PML1_ENTRY PML1[EPT_PML1E_COUNT];

	/* A pointer to the 2MB entry in the page table which this split was created for. */
	PEPT_PML2_2MB pml2Entry;

	/* List entry for the dynamic split, will be used to keep track of all split entries. */
	LIST_ENTRY listEntry;

} EPT_DYNAMIC_SPLIT, *PEPT_DYNAMIC_SPLIT;

typedef struct _EPT_CONFIG
{
	/* Describes 512 contiguous 512GB memory regions each with 512 512GB regions. */
	DECLSPEC_ALIGN(PAGE_SIZE) EPT_PML4_POINTER PML4[EPT_PML4E_COUNT];

	/* Describes exactly 512 contiguous 1GB memory regions within a singular PML4 region. */
	DECLSPEC_ALIGN(PAGE_SIZE) EPT_PML3_POINTER PML3[EPT_PML3E_COUNT];

	/* For each 1GB PML3 entry, create 512 2MB regions. We are using 2MB pages as the smallest paging size in
	* the map so that we do not need to allocate individual 4096 PML1 paging structures. */
	DECLSPEC_ALIGN(PAGE_SIZE) EPT_PML2_2MB PML2[EPT_PML3E_COUNT][EPT_PML2E_COUNT];

	/* List all of the EPT handlers that are used. */
	LIST_ENTRY handlerList;

	/* List of all dynamically split pages (from 2MB to 4KB). This will be used for
	 * when they need to be freed during uninitialisation. 
	 * TODO: Actually implement uninit. */
	LIST_ENTRY dynamicSplitList;

	/* EPT pointer that will be used for the VMCS. */
	EPT_POINTER eptPointer;
} EPT_CONFIG, *PEPT_CONFIG;

/* Callback function for the EPT violation handler. */
typedef BOOLEAN(*fnEPTHandlerCallback)(PEPT_CONFIG eptConfig, PCONTEXT guestContext, PVOID userBuffer);

/* Structure that holds the information of each handler that
* are used for parsing violations. */
typedef struct _EPT_HANDLER
{
	/* Range of physical memory that the handler
	 * is registered to. */
	PHYSICAL_RANGE physRange;

	/* Callback of the handler, that will be called for processing the violation. */
	fnEPTHandlerCallback callback;

	/* Buffer that can be used for user-supplied configs. */
	PVOID userParameter;

	/* Linked list entry, used for traversal. */
	LIST_ENTRY listEntry;
} EPT_HANDLER, *PEPT_HANDLER;

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/

void EPT_initialise(PEPT_CONFIG eptTable, const PMTRR_RANGE mtrrTable);
BOOLEAN EPT_handleViolation(PEPT_CONFIG eptConfig, PCONTEXT guestContext);
NTSTATUS EPT_addViolationHandler(PEPT_CONFIG eptConfig, PHYSICAL_RANGE physicalRange, fnEPTHandlerCallback callback, PVOID userParameter);
NTSTATUS EPT_splitLargePage(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS physicalAddress);
PEPT_PML2_2MB EPT_getPML2EFromAddress(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS physicalAddress);
PEPT_PML1_ENTRY EPT_getPML1EFromAddress(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS physicalAddress);
void EPT_invalidateAndFlush(PEPT_CONFIG eptConfig);