#pragma once

/* Result Type */
typedef uint32_t kinowasm_result_t;
typedef enum {
	RES_SUCCESS = 0,
	RES_ERROR = 1
} kinowasm_resultcode_t;

/* Excption */
#define _is_error(err) ((err) > RES_SUCCESS)
#define _try kinowasm_result_t _result = RES_SUCCESS;
#define _throw(err) do { \
	_result = err; \
	goto _catch; \
} while(0)
#define _throwif(err, condition) do { \
	if(condition) { \
		_throw(err); \
	} \
} while(0)
#define _throwiferr(instr) do { \
	kinowasm_result_t _errcode = instr; \
	if(_errcode != RES_SUCCESS) \
		_throw(_errcode); \
} while(0)

typedef enum {
	ERR_NULLPOINTER = 10,
	ERR_OUTOFMEMORY = 11,
	ERR_UNEXPECTED_END = 12,
	ERR_INTEGER_CONVERT_TOO_LONG = 13,
	ERR_INTEGER_TOO_LARGE = 14,
	ERR_INVALID_ITEMSIZE = 15,
	ERR_NOT_SUPPORTED = 16
} kinowasm_system_errorcode_t;
