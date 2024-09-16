#ifndef JOS_INC_ELF_H
#define JOS_INC_ELF_H

#include <inc/uefi.h>
#include "../LoaderPkg/Include/Elf64.h"

static const char* elf_symbol_type_to_name[16] = {
    "notype",
    "object",
    "func",
    "section",
    "file",
    "common",
    "tls",
    "num",
    NULL,
    NULL,
    "loss",
    NULL,
    "hios",
    "loproc",
    NULL,
    "hiproc"
};

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
