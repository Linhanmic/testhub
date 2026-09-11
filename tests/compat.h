#pragma once

#ifdef _WIN32
#include <process.h>
inline int testhubGetPid() { return _getpid(); }
#else
#include <unistd.h>
inline int testhubGetPid() { return ::getpid(); }
#endif
