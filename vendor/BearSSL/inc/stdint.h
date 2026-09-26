#ifndef NOSWEB_VC6_STDINT_H
#define NOSWEB_VC6_STDINT_H

/* Visual C++ 6 predates stdint.h. Other compilers use their own header. */
#ifndef _MSC_VER
#include_next <stdint.h>
#else
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed long int32_t;
typedef unsigned long uint32_t;
typedef signed __int64 int64_t;
typedef unsigned __int64 uint64_t;
typedef unsigned long uintptr_t;
#endif

#endif
