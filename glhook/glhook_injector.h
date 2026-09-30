#pragma once
#include <windows.h>
#include <string>

// Injects a DLL into a running process using CreateRemoteThread + LoadLibraryA.
// Returns true on success.
bool InjectDLL(DWORD processId, const std::string& dllPath);
