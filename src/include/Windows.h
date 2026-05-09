// Case-shim for Linux MinGW cross-compile: ReShade SDK includes <Windows.h>
// (capital W) but MinGW-w64 ships windows.h (lowercase) and ext4 is
// case-sensitive. This shim resolves the include without touching the SDK
// or system headers. Compiled into nothing on Windows hosts where the real
// Windows.h is found first.
#pragma once
#include <windows.h>
