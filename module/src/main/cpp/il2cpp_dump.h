//
// Created by Perfare on 2020/7/4.
//

#ifndef ZYGISK_IL2CPPDUMPER_IL2CPP_DUMP_H
#define ZYGISK_IL2CPPDUMPER_IL2CPP_DUMP_H

// Resolves the il2cpp API and waits for the runtime to be ready.
// Returns false if the runtime cannot be used.
bool il2cpp_api_init(void *handle);

// True when every API function used by il2cpp_dump() was found.
bool il2cpp_api_complete();

// Load base of libil2cpp.so, 0 if unknown.
unsigned long long il2cpp_get_base();

// Writes dump.cs in <outDir>/files/. Returns false on error.
bool il2cpp_dump(const char *outDir);

#endif //ZYGISK_IL2CPPDUMPER_IL2CPP_DUMP_H
