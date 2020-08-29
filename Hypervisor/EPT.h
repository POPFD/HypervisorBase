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
typedef EPDE_2MB EPT_PML2_ENTRY, *PEPT_PML2_ENTRY;
typedef EPDE EPT_PML2_POINTER, *PEPT_PML2_POINTER;
typedef EPTE EPT_PML1_ENTRY, *PEPT_PML1_ENTRY;

/* Structure that will hold the shadow configuration for hiding executable pages. */
typedef struct _EPT_SHADOW_PAGE
{
	DECLSPEC_ALIGN(PAGE_SIZE) UINT8 executePage[PAGE_SIZE];

	/* Base address of the page that is being shadowed. */
	PHYSICAL_ADDRESS physicalAlign;

	/* Target process that will be hooked, NULL if global. */
	CR3 targetCR3;

	/* Pointer to the PML1 entry that will be modified between RW and E. */
	PEPT_PML1_ENTRY targetPML1E;

	/* Will store the flags of the specific PML1E's that will be
	 * used for targetting shadowing. */
	EPT_PML1_ENTRY originalPML1E;
	EPT_PML1_ENTRY activeExecTargetPML1E;
	EPT_PML1_ENTRY activeExecNotTargetPML1E;
	EPT_PML1_ENTRY activeRWPML1E;

	/* List entry for the page hook, this will be used to keep track of
	* all shadow pages. */
	LIST_ENTRY listEntry;
} EPT_SHADOW_PAGE, *PEPT_SHADOW_PAGE;

/* Structure that will hold configuration of all monitored page table entries,
* this is to account for paging changes on the EPT shadow hooked pages. */
typedef struct _EPT_MONITORED_PTE
{
	/* Hold the physical address of the target PTE. */
	PHYSICAL_ADDRESS physPTE;

	/* Holds the level the PT belonds at. */
	SIZE_T pagingLevel;

	/* Pointer to the EPT PML1 entry that will have the write bit toggled. */
	PEPT_PML1_ENTRY targetPML1E;

	/* Pointer to the shadow page this relates to. */
	PEPT_SHADOW_PAGE shadowPage;

	/* List entry for the dynamic split, will be used to keep track of all split entries. */
	LIST_ENTRY listEntry;
} EPT_MONITORED_PTE, *PEPT_MONITORED_PTE;

/* Structure that will hold the PML1 data for a dynamically split PML2 entry. */
typedef struct _EPT_DYNAMIC_SPLIT
{
	/* The 4096 byte page table entries that correspond to the split 2MB table entry. */
	DECLSPEC_ALIGN(PAGE_SIZE) EPT_PML1_ENTRY PML1[EPT_PML1E_COUNT];

	/* A pointer to the 2MB entry in the page table which this split was created for. */
	PEPT_PML2_ENTRY pml2Entry;

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
	DECLSPEC_ALIGN(PAGE_SIZE) EPT_PML2_ENTRY PML2[EPT_PML3E_COUNT][EPT_PML2E_COUNT];

	/* List of all dynamically split pages (from 2MB to 4KB). This will be used for
	* when they need to be freed during uninitialisation. */
	LIST_ENTRY dynamicSplitList;

	/* List of all the monitored PTE's in the system. */
	LIST_ENTRY monitoredPTEList;

	/* List of all the PAGE shadows in the system. */
	LIST_ENTRY pageShadowList;

	/* EPT pointer that will be used for the VMCS. */
	EPT_POINTER eptPointer;
} EPT_CONFIG, *PEPT_CONFIG;

/******************** Public Constants ********************/

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/

void EPT_initialise(PEPT_CONFIG eptTable, const PMTRR_RANGE mtrrTable);
NTSTATUS EPT_splitLargePage(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS physicalAddress);
PEPT_PML2_ENTRY EPT_getPML2EFromAddress(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS physicalAddress);
PEPT_PML1_ENTRY EPT_getPML1EFromAddress(PEPT_CONFIG eptConfig, PHYSICAL_ADDRESS physicalAddress);