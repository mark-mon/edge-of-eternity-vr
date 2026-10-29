#pragma once
namespace uxr::typedump {
// Writes every class / field / method of the loaded IL2CPP assemblies to `path`. Assemblies whose name starts with
// one of the comma-separated `skipPrefixes` are listed but not expanded.
bool Dump(const wchar_t* path, const char* skipPrefixes);
}
