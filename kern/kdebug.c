#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/dwarf.h>
#include <inc/elf.h>
#include <inc/x86.h>
#include <inc/error.h>

#include <kern/kdebug.h>
#include <kern/pmap.h>
#include <kern/env.h>
#include <inc/uefi.h>
#include <kern/traceopt.h>

static void
load_kernel_symbol_info(struct ParsedElfSymbolSection *symbols, struct ParsedElfStringSection *strings) {
    symbols->entries = (const struct Elf64_Sym *)uefi_lp->SymbolTableStart;
    symbols->count = (uint64_t)(uefi_lp->SymbolTableEnd - uefi_lp->SymbolTableStart) / sizeof(struct Elf64_Sym);

    strings->data = (const char *)uefi_lp->StringTableStart;
    strings->size = (uint64_t)(uefi_lp->StringTableEnd - uefi_lp->StringTableStart);

    if (trace_elf_kdebug) {
        cprintf("loaded kernel symbol info\n");
        cprintf("  symbols->entries = %p\n", symbols->entries);
        cprintf("  symbols->count   = %lu\n", symbols->count);
        cprintf("  strings->data    = %p\n", strings->data);
        cprintf("  strings->size    = %lu\n", strings->size);
    }
}

static void
load_kernel_dwarf_info(struct Dwarf_Addrs *addrs) {
    addrs->aranges_begin = (uint8_t *)(uefi_lp->DebugArangesStart);
    addrs->aranges_end = (uint8_t *)(uefi_lp->DebugArangesEnd);
    addrs->abbrev_begin = (uint8_t *)(uefi_lp->DebugAbbrevStart);
    addrs->abbrev_end = (uint8_t *)(uefi_lp->DebugAbbrevEnd);
    addrs->info_begin = (uint8_t *)(uefi_lp->DebugInfoStart);
    addrs->info_end = (uint8_t *)(uefi_lp->DebugInfoEnd);
    addrs->line_begin = (uint8_t *)(uefi_lp->DebugLineStart);
    addrs->line_end = (uint8_t *)(uefi_lp->DebugLineEnd);
    addrs->str_begin = (uint8_t *)(uefi_lp->DebugStrStart);
    addrs->str_end = (uint8_t *)(uefi_lp->DebugStrEnd);
    addrs->pubnames_begin = (uint8_t *)(uefi_lp->DebugPubnamesStart);
    addrs->pubnames_end = (uint8_t *)(uefi_lp->DebugPubnamesEnd);
    addrs->pubtypes_begin = (uint8_t *)(uefi_lp->DebugPubtypesStart);
    addrs->pubtypes_end = (uint8_t *)(uefi_lp->DebugPubtypesEnd);
}

static struct Secthdr*
find_section_with_name(const char *name, struct Secthdr *section_headers, uint64_t count, const char *shstr) {
    for (uint64_t i = 0; i < count; ++i) {
        struct Secthdr *cur = &section_headers[i];
        uint32_t shstr_offset = cur->sh_name;

        const char *cur_name = shstr + shstr_offset;

        if (strcmp(name, cur_name) == 0) {
            return cur;
        }
    }

    return NULL;
}

void
load_user_dwarf_info(struct Dwarf_Addrs *addrs) {
    assert(curenv);

    uint8_t *binary = curenv->binary;
    assert(binary);

    struct {
        const uint8_t **end;
        const uint8_t **start;
        const char *name;
    } sections[] = {
            {&addrs->aranges_end, &addrs->aranges_begin, ".debug_aranges"},
            {&addrs->abbrev_end, &addrs->abbrev_begin, ".debug_abbrev"},
            {&addrs->info_end, &addrs->info_begin, ".debug_info"},
            {&addrs->line_end, &addrs->line_begin, ".debug_line"},
            {&addrs->str_end, &addrs->str_begin, ".debug_str"},
            {&addrs->pubnames_end, &addrs->pubnames_begin, ".debug_pubnames"},
            {&addrs->pubtypes_end, &addrs->pubtypes_begin, ".debug_pubtypes"},
    };
    uint64_t sections_count = sizeof(sections) / sizeof(sections[0]);

    memset(addrs, 0, sizeof(*addrs));

    /* Load debug sections from curenv->binary elf image */
    // LAB 8: Your code here
    struct Elf *elf_header = (struct Elf*) binary;
    
    struct Secthdr *section_headers = (struct Secthdr*)(binary + elf_header->e_shoff);
    uint64_t section_headers_count = elf_header->e_shnum;

    assert(elf_header->e_shstrndx < section_headers_count);
    struct Secthdr *shstr_header = &section_headers[elf_header->e_shstrndx];

    uint64_t shstr_offset = shstr_header->sh_offset;
    const char* shstr = (const char*)(binary + shstr_offset);

    for (uint64_t i = 0; i < sections_count; ++i) {
        const char *name = sections[i].name;
        struct Secthdr *header = find_section_with_name(name, section_headers, section_headers_count, shstr);

        if (!header) {
            panic("Could not find section with name %s", name);
        }

        *sections[i].start = binary + header->sh_offset;
        *sections[i].end = *sections[i].start + header->sh_size;
    }
}

#define UNKNOWN       "<unknown>"
#define CALL_INSN_LEN 5

/* debuginfo_rip(addr, info)
 * Fill in the 'info' structure with information about the specified
 * instruction address, 'addr'.  Returns 0 if information was found, and
 * negative if not.  But even if it returns negative it has stored some
 * information into '*info'
 */
int
debuginfo_rip(uintptr_t addr, struct Ripdebuginfo *info) {
    if (!addr) return 0;

    /* Initialize *info */
    strcpy(info->rip_file, UNKNOWN);
    strcpy(info->rip_fn_name, UNKNOWN);
    info->rip_fn_namelen = sizeof UNKNOWN - 1;
    info->rip_line = 0;
    info->rip_fn_addr = addr;
    info->rip_fn_narg = 0;

    /* Temporarily load kernel cr3 and return back once done.
     * Make sure that you fully understand why it is necessary. */

    // Because we'll be reading env->binary, which is available from kernel space only

    // LAB 8: Your code here:
    struct AddressSpace *prev_as = switch_address_space(&kspace);

    /* Load dwarf section pointers from either
     * currently running program binary or use
     * kernel debug info provided by bootloader
     * depending on whether addr is pointing to userspace
     * or kernel space */

    // LAB 8: Your code here:

    struct Dwarf_Addrs addrs;

    if (addr >= MAX_USER_ADDRESS) {
        load_kernel_dwarf_info(&addrs);
    } else {
        load_user_dwarf_info(&addrs);
    }

    switch_address_space(prev_as);

    Dwarf_Off offset = 0, line_offset = 0;
    int res = info_by_address(&addrs, addr, &offset);
    if (res < 0) goto error;

    char *tmp_buf = NULL;
    res = file_name_by_info(&addrs, offset, &tmp_buf, &line_offset);
    if (res < 0) goto error;
    strncpy(info->rip_file, tmp_buf, sizeof(info->rip_file));

    /* Find line number corresponding to given address.
     * Hint: note that we need the address of `call` instruction, but rip holds
     * address of the next instruction, so we should substract 5 from it.
     * Hint: use line_for_address from kern/dwarf_lines.c */

    // LAB 2: Your res here:
    const int CALL_INSTRUCTION_OFFSET = 5;

    if (addr < CALL_INSTRUCTION_OFFSET) {
        return 0;
    }

    uintptr_t call_addr = addr - CALL_INSTRUCTION_OFFSET;

    int line_num = 0;
    res = line_for_address(&addrs, call_addr, line_offset, &line_num);

    if (res < 0) {
        return res;
    }

    info->rip_line = line_num;

    /* Find function name corresponding to given address.
     * Hint: note that we need the address of `call` instruction, but rip holds
     * address of the next instruction, so we should substract 5 from it.
     * Hint: use function_by_info from kern/dwarf.c
     * Hint: info->rip_fn_name can be not NULL-terminated,
     * string returned by function_by_info will always be */

    // LAB 2: Your res here:
    char *rip_fn_name = NULL;
    uintptr_t rip_fn_addr = 0;
    res = function_by_info(&addrs, call_addr, offset, &rip_fn_name, &rip_fn_addr);

    if (res < 0) {
        return res;
    }

    info->rip_fn_namelen = strlen(rip_fn_name);
    strncpy(info->rip_fn_name, rip_fn_name, info->rip_fn_namelen);
    info->rip_fn_addr = rip_fn_addr;

error:
    return res;
}

static uintptr_t
symbol_table_address_by_fname(const char *const target_name, uintptr_t *addr) {
    struct ParsedElfSymbolSection symbols;
    struct ParsedElfStringSection strings;
    load_kernel_symbol_info(&symbols, &strings);

    if_cprintf(trace_elf_kdebug, "parsing kernel symbol table\n");

    for (uint64_t i = 0; i < symbols.count; ++i) {
        const struct Elf64_Sym *symbol = &symbols.entries[i];

        const char *name = strings.data + symbol->st_name;
        uint8_t type = ELF_ST_TYPE(symbol->st_info);
        uint8_t bind = ELF_ST_BIND(symbol->st_info);

        if ((type == STT_OBJECT || type == STT_FUNC) && strcmp(name, target_name) == 0) {
            if (trace_elf_kdebug) {
                cprintf("reading kernel symbol at index = %lu\n", i);
                cprintf("  name            = %s\n", name);
                cprintf("  type            = %s\n", elf_symbol_type_to_name(type));
                cprintf("  bind            = %s\n", elf_symbol_bind_to_name(bind));
                cprintf("  visibility      = %s\n", elf_symbol_visibility_to_name(symbol->st_other));
                cprintf("  shndx           = %hu\n", symbol->st_shndx);
                cprintf("  section index   = %hu\n", symbol->st_shndx);
                cprintf("  value           = 0x%08lX\n", symbol->st_value);
                cprintf("  size            = 0x%08lX\n", symbol->st_size);
            }

            *addr = (uintptr_t)symbol->st_value;

            return 0;
        }
    }

    return -E_NO_ENT;
}

uintptr_t
find_function(const char *const fname) {
    /* There are two functions for function name lookup.
     * address_by_fname, which looks for function name in section .debug_pubnames
     * and naive_address_by_fname which performs full traversal of DIE tree.
     * It may also be useful to look to kernel symbol table for symbols defined
     * in assembly. */

    // LAB 3: Your code here:

    struct Dwarf_Addrs addrs;
    load_kernel_dwarf_info(&addrs);

    uintptr_t addr;

    int symbol_table_result = 0;
    int dwarf_pubnames_result = 0;
    int dwarf_naive_result = 0;

    symbol_table_result = symbol_table_address_by_fname(fname, &addr);

    if (symbol_table_result < 0) {
        dwarf_pubnames_result = address_by_fname(&addrs, fname, &addr);
    }

    if (symbol_table_result < 0 && dwarf_pubnames_result < 0) {
        dwarf_naive_result = naive_address_by_fname(&addrs, fname, &addr);
    }

    if (symbol_table_result < 0 && dwarf_pubnames_result < 0 && dwarf_naive_result < 0) {
        if (trace_elf_kdebug) {
            warn(
                "failed to find function with name '%s' in debug symbols"
                ". symbol table error = %i, pubnames error = %i, naive error = %i",
                fname, symbol_table_result, dwarf_pubnames_result, dwarf_naive_result
            );
        }

        return 0;
    }

    return addr;
}
