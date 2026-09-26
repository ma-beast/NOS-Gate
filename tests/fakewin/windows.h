#ifndef TEST_WINDOWS_H
#define TEST_WINDOWS_H
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef void *HANDLE;
typedef void *LPVOID;
typedef void *HINSTANCE;
typedef char *LPSTR;
typedef struct { unsigned short wYear,wMonth,wDayOfWeek,wDay,wHour,wMinute,wSecond,wMilliseconds; } SYSTEMTIME;
#define WINAPI
#define MAX_PATH 260
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define GENERIC_WRITE 1
#define FILE_SHARE_READ 2
#define FILE_SHARE_WRITE 4
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_NORMAL 0
#define FILE_END 2
#define ERROR_ALREADY_EXISTS 183
#define MB_OK 0
#define MB_ICONERROR 0
#define SW_SHOWNORMAL 1
#define INFINITE 0xFFFFFFFFUL
#define WAIT_OBJECT_0 0
#endif
