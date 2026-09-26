#ifndef TEST_WINSOCK_H
#define TEST_WINSOCK_H
typedef int SOCKET;
typedef struct { int unused; } WSADATA;
struct hostent { char *h_name; char **h_aliases; short h_addrtype;
                 short h_length; char **h_addr_list; };
struct in_addr { unsigned long s_addr; };
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct sockaddr_in { short sin_family; unsigned short sin_port; struct in_addr sin_addr; char sin_zero[8]; };
#define AF_INET 2
#define SOCK_STREAM 1
#define IPPROTO_TCP 6
#define INADDR_LOOPBACK 0x7F000001UL
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define MAKEWORD(a,b) ((unsigned short)(((unsigned char)(a)) | ((unsigned short)((unsigned char)(b))) << 8))
#define h_addr h_addr_list[0]
int WSAStartup(unsigned short version, WSADATA *data);
int WSACleanup(void);
struct hostent *gethostbyname(const char *name);
SOCKET socket(int family, int type, int protocol);
int connect(SOCKET s, const struct sockaddr *name, int namelen);
int closesocket(SOCKET s);
int recv(SOCKET s, char *buffer, int length, int flags);
int send(SOCKET s, const char *buffer, int length, int flags);
unsigned short htons(unsigned short value);
#endif
