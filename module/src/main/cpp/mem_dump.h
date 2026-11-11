//
// Zygisk-Il2CppDumper v2 - raw memory dump (decrypted metadata + libil2cpp).
//

#ifndef ZYGISK_IL2CPPDUMPER_MEM_DUMP_H
#define ZYGISK_IL2CPPDUMPER_MEM_DUMP_H

#include <stdint.h>

// Writes <data_dir>/files/il2cpp-dump/libil2cpp-0x<base>.so and every
// decrypted global-metadata found in memory. Returns true if anything was
// written.
bool memory_dump(const char *data_dir, uintptr_t il2cpp_base);

#endif // ZYGISK_IL2CPPDUMPER_MEM_DUMP_H
