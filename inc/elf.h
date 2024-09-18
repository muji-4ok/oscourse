#ifndef JOS_INC_ELF_H
#define JOS_INC_ELF_H

#include <inc/uefi.h>
#include "../LoaderPkg/Include/Elf64.h"

const char* elf_symbol_type_to_name(uint8_t type);
const char* elf_symbol_bind_to_name(uint8_t type);
const char* elf_symbol_visibility_to_name(uint8_t type);

const char *elf_prog_header_type_to_name(uint32_t type);

struct ParsedElfStringSection {
    const char *data;
    uint64_t size;
};

struct ParsedElfSymbolSection {
    const struct Elf64_Sym *entries;
    uint64_t count;
};

struct ParsedElfSections {
    struct ParsedElfStringSection section_strings;
    struct ParsedElfStringSection symbol_strings;
    struct ParsedElfSymbolSection symbols;
};



#endif /* !JOS_INC_ELF_H */
