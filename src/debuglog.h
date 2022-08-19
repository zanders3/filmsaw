#pragma once

#ifdef _DEBUG
void DebugLog(const char* s, ...);
#else
#define DebugLog(...)
#endif
