/**
 * @file dberror.h
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief Shared return codes, error macros, and page-size constants.
 */
#ifndef DBERROR_H
#define DBERROR_H

#include "stdio.h"

#define CHECKEX(__code__) if (__code__ != RC_OK) { return __code__; }
#define CHECKCODE(__code__, __return__) if (__code__ != RC_OK) { return __return__; }
#define CHECK_OR_NULL(__ptr__) if (__ptr__ != RC_OK) { return NULL; }

/* module wide constants */
#define PAGE_SIZE 4096

/* return code definitions */
typedef int RC;

#define RC_OK 0
#define RC_FILE_NOT_FOUND 1
#define RC_FILE_HANDLE_NOT_INIT 2
#define RC_WRITE_FAILED 3
#define RC_READ_NON_EXISTING_PAGE 4

#define RC_CREATE_FAILED -5
#define RC_OPEN_FAILED -6
#define RC_READ_FAILED -7
#define RC_ALLOCATION_FAILED -8

#define RC_BUFF_SHUT_FAILED -9
#define RC_PIN_FAILED -10
#define RC_FLUSH_FAILED -11
#define RC_DIRTY_FAILED -12
#define RC_UNPIN_FAILED -13

#define RC_RM_INIT_FAILED -14
#define RC_RM_NO_SPACE_PAGE -15
#define RC_RM_SCAN_FAILED -16

#define RC_RM_COMPARE_VALUE_OF_DIFFERENT_DATATYPE 200
#define RC_RM_EXPR_RESULT_IS_NOT_BOOLEAN 201
#define RC_RM_BOOLEAN_EXPR_ARG_IS_NOT_BOOLEAN 202
#define RC_RM_NO_MORE_TUPLES 203
#define RC_RM_NO_PRINT_FOR_DATATYPE 204
#define RC_RM_UNKOWN_DATATYPE 205

#define RC_IM_KEY_NOT_FOUND 300
#define RC_IM_INCOMPATIBLE_DATA 305
#define RC_IM_UNSUPPORTED_TYPE 306
#define RC_IM_ROOT_EMPTY 307
#define RC_IM_KEY_ALREADY_EXISTS 301
#define RC_IM_N_TO_LAGE 302
#define RC_IM_NO_MORE_ENTRIES 303

#define RC_NULL_POINTER 500
#define RC_BM_INVALID_STRATEGY 501
#define RC_BM_PAGE_NOT_BUFFERED 503
#define RC_BM_BUFFER_IN_USE 504
#define RC_RM_TABLE_EXISTS 505
#define RC_RM_INVALID_SCHEMA_DATA 506
#define RC_RM_MANAGER_CLOSED 507

#define RC_NO_REMOVABLE_PAGE 105
#define RC_PAGELIST_NOT_INITIALIZED 106
#define RC_PAGE_NOT_FOUND 107
#define RC_INVALID_NUMPAGES 108
#define RC_PAGE_FOUND 109
#define RC_FLUSH_POOL_ERROR 110
#define RC_RS_NOT_IMPLEMENTED 111

/* holder for error messages */
extern char *RC_message;

/* print a message to standard out describing the error */
extern void printError (RC error);
extern char *errorMessage (RC error);

#define THROW(rc,message) \
  do {			  \
    RC_message=message;	  \
    return rc;		  \
  } while (0)		  \

// check the return code and exit if it is an error
#define CHECK(code)							\
  do {									\
    int rc_internal = (code);						\
    if (rc_internal != RC_OK)						\
      {									\
	char *message = errorMessage(rc_internal);			\
	printf("[%s-L%i-%s] ERROR: Operation returned error: %s\n",__FILE__, __LINE__, __TIME__, message); \
	free(message);							\
	exit(1);							\
      }									\
  } while(0);


#endif
