#pragma once


/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

#ifdef _DEBUG 
	#define DEBUG_PRINT(format, ...) DbgPrint(format, ##__VA_ARGS__)
#else
	#define DEBUG_PRINT(format, ...)
#endif

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
