#include <inc/elf.h>

const char *elf_prog_header_type_to_name(uint32_t type) {
    switch (type) {
    case PT_NULL:
        return "null";
    case PT_LOAD:
        return "load";
    case PT_DYNAMIC:
        return "dynamic";
    case PT_INTERP:
        return "interp";
    case PT_NOTE:
        return "note";
    case PT_SHLIB:
        return "shlib";
    case PT_PHDR:
        return "phdr";
    case PT_TLS:
        return "tls";
    case PT_NUM:
        return "num";
    case PT_LOOS:
        return "loos";
    case PT_GNU_EH_FRAME:
        return "gnu.eh.frame";
    case PT_GNU_STACK:
        return "gnu.stack";
    case PT_GNU_RELRO:
        return "gnu.relro";
    case PT_GNU_PROPERTY:
        return "gnu.property";
    case PT_SUNWBSS:
        return "sunwbss";
    case PT_SUNWSTACK:
        return "sunwstack";
    case PT_HIOS:
        return "hios";
    case PT_LOPROC:
        return "loproc";
    case PT_HIPROC:
        return "hiproc";
    default:
        return "<unknown>";
    }
}

const char* elf_symbol_type_to_name(uint8_t type) {
    static const char* values[16] = {
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

    return values[type];
}

const char* elf_symbol_bind_to_name(uint8_t bind) {
    static const char* values[16] = {
        "local",
        "global",
        "weak",
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        "loss",
        NULL,
        "hios",
        "loproc",
        NULL,
        "hiproc"
    };

    return values[bind];
}

const char* elf_symbol_visibility_to_name(uint8_t visibility) {
    static const char* values[4] = {
        "default",
        "internal",
        "hidden",
        "protected"
    };

    return values[visibility];
}
