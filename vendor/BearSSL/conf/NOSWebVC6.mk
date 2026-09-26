# BearSSL static-library build for NOS-WEB and Visual C++ 6.
# Absolute tool paths avoid overflowing the small Windows 9x environment.

BUILD = build
E = .exe
O = .obj
LP =
L = s.lib
DP =
D = .dll
RM = del /Q
MKDIR = mkdir

CC = "C:\Program Files\Microsoft Visual Studio\VC98\Bin\CL.EXE"
CFLAGS = -nologo -W2 -O1 -Dinline=__inline -D_WIN32_WINNT=0x0400 -Iinc -I"C:\Program Files\Microsoft Visual Studio\VC98\Include"
CCOUT = -c -Fo

AR = "C:\Program Files\Microsoft Visual Studio\VC98\Bin\LIB.EXE"
ARFLAGS = -nologo
AROUT = -out:

LDDLL = $(CC)
LDDLLFLAGS = -nologo -LD -MT
LDDLLOUT = -Fe
LD = $(CC)
LDFLAGS = -nologo
LDOUT = -Fe

MKT0COMP = mk$PmkT0.cmd
RUNT0COMP = T0Comp.exe
DLL =
TOOLS =
TESTS =
