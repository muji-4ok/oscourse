/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/monitor.h>
#include <kern/sched.h>
#include <kern/kdebug.h>
#include <kern/macro.h>
#include <kern/pmap.h>
#include <kern/traceopt.h>

/* Currently active environment */
struct Env *curenv = NULL;

#ifdef CONFIG_KSPACE
/* All environments */
struct Env env_array[NENV];
struct Env *envs = env_array;
#else
/* All environments */
struct Env *envs = NULL;
#endif

/* Free environment list
 * (linked by Env->env_link) */
static struct Env *env_free_list;


/* NOTE: Should be at least LOGNENV */
#define ENVGENSHIFT 12

/* Converts an envid to an env pointer.
 * If checkperm is set, the specified environment must be either the
 * current environment or an immediate child of the current environment.
 *
 * RETURNS
 *     0 on success, -E_BAD_ENV on error.
 *   On success, sets *env_store to the environment.
 *   On error, sets *env_store to NULL. */
int
envid2env(envid_t envid, struct Env **env_store, bool need_check_perm) {
    struct Env *env;

    /* If envid is zero, return the current environment. */
    if (!envid) {
        *env_store = curenv;
        return 0;
    }

    /* Look up the Env structure via the index part of the envid,
     * then check the env_id field in that struct Env
     * to ensure that the envid is not stale
     * (i.e., does not refer to a _previous_ environment
     * that used the same slot in the envs[] array). */
    env = &envs[ENVX(envid)];
    if (env->env_status == ENV_FREE || env->env_id != envid) {
        *env_store = NULL;
        return -E_BAD_ENV;
    }

    /* Check that the calling environment has legitimate permission
     * to manipulate the specified environment.
     * If checkperm is set, the specified environment
     * must be either the current environment
     * or an immediate child of the current environment. */
    if (need_check_perm && env != curenv && env->env_parent_id != curenv->env_id) {
        *env_store = NULL;
        return -E_BAD_ENV;
    }

    *env_store = env;
    return 0;
}

/* Mark all environments in 'envs' as free, set their env_ids to 0,
 * and insert them into the env_free_list.
 * Make sure the environments are in the free list in the same order
 * they are in the envs array (i.e., so that the first call to
 * env_alloc() returns envs[0]).
 */
void
env_init(void) {

    /* Set up envs array */

    // LAB 3: Your code here

    assert(NENV > 0);

    env_free_list = &envs[0];

    for (int i = 0; i < NENV; ++i) {
        envs[i].env_id = 0;
        envs[i].env_status = ENV_FREE;
        envs[i].env_link = (i == NENV - 1) ? NULL : &envs[i + 1];
    }
}

/* Allocates and initializes a new environment.
 * On success, the new environment is stored in *newenv_store.
 *
 * Returns
 *     0 on success, < 0 on failure.
 * Errors
 *    -E_NO_FREE_ENV if all NENVS environments are allocated
 *    -E_NO_MEM on memory exhaustion
 */
int
env_alloc(struct Env **newenv_store, envid_t parent_id, enum EnvType type) {

    struct Env *env;
    if (!(env = env_free_list))
        return -E_NO_FREE_ENV;

    /* Generate an env_id for this environment */
    int32_t generation = (env->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
    /* Don't create a negative env_id */
    if (generation <= 0) generation = 1 << ENVGENSHIFT;
    env->env_id = generation | (env - envs);

    /* Set the basic status variables */
    env->env_parent_id = parent_id;
#ifdef CONFIG_KSPACE
    env->env_type = ENV_TYPE_KERNEL;
#else
    env->env_type = type;
#endif
    env->env_status = ENV_RUNNABLE;
    env->env_runs = 0;

    /* Clear out all the saved register state,
     * to prevent the register values
     * of a prior environment inhabiting this Env structure
     * from "leaking" into our new environment */
    memset(&env->env_tf, 0, sizeof(env->env_tf));

    /* Set up appropriate initial values for the segment registers.
     * GD_UD is the user data (KD - kernel data) segment selector in the GDT, and
     * GD_UT is the user text (KT - kernel text) segment selector (see inc/memlayout.h).
     * The low 2 bits of each segment register contains the
     * Requestor Privilege Level (RPL); 3 means user mode, 0 - kernel mode.  When
     * we switch privilege levels, the hardware does various
     * checks involving the RPL and the Descriptor Privilege Level
     * (DPL) stored in the descriptors themselves */

#ifdef CONFIG_KSPACE
    env->env_tf.tf_ds = GD_KD;
    env->env_tf.tf_es = GD_KD;
    env->env_tf.tf_ss = GD_KD;
    env->env_tf.tf_cs = GD_KT;

    // LAB 3: Your code here:

    static uintptr_t stack_top = 0x2000000;
    uint32_t index = env->env_id % NENV;
    env->env_tf.tf_rsp = stack_top - PAGE_SIZE * 2 * index;

    if_cprintf(trace_elf, "[%08x] setting rsp = %p, index = %u, nenv = %d\n", env->env_id, (void*)env->env_tf.tf_rsp, index, NENV);
#else
    env->env_tf.tf_ds = GD_UD | 3;
    env->env_tf.tf_es = GD_UD | 3;
    env->env_tf.tf_ss = GD_UD | 3;
    env->env_tf.tf_cs = GD_UT | 3;
    env->env_tf.tf_rsp = USER_STACK_TOP;
#endif

    /* For now init trapframe with IF set */
    env->env_tf.tf_rflags = FL_IF;

    /* Commit the allocation */
    env_free_list = env->env_link;
    *newenv_store = env;

    if (trace_envs) cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, env->env_id);
    return 0;
}

/* Pass the original ELF image to binary/size and bind all the symbols within
 * its loaded address space specified by image_start/image_end.
 * Make sure you understand why you need to check that each binding
 * must be performed within the image_start/image_end range.
 */
static int
bind_functions(
    struct Env *env, struct Elf *elf, uintptr_t image_start, uintptr_t image_end, struct ParsedElfSections *parsed_sections
) {
    // LAB 3: Your code here:

    /* NOTE: find_function from kdebug.c should be used */

    if_cprintf(trace_elf, "parsing symbol table\n");

    for (uint64_t i = 0; i < parsed_sections->symbols.count; ++i) {
        const struct Elf64_Sym *symbol = &parsed_sections->symbols.entries[i];

        const char *name = parsed_sections->symbol_strings.data + symbol->st_name;
        uint8_t type = ELF_ST_TYPE(symbol->st_info);
        uint8_t bind = ELF_ST_BIND(symbol->st_info);

        if (type == STT_OBJECT && (bind == STB_GLOBAL || bind == STB_WEAK)) {
            uintptr_t addr = find_function(name);

            if (trace_elf) {
                cprintf("reading symbol at index = %lu\n", i);
                cprintf("  name            = %s\n", name);
                cprintf("  type            = %s\n", elf_symbol_type_to_name(type));
                cprintf("  bind            = %s\n", elf_symbol_bind_to_name(bind));
                cprintf("  visibility      = %s\n", elf_symbol_visibility_to_name(symbol->st_other));
                cprintf("  shndx           = %hu\n", symbol->st_shndx);
                cprintf("  section index   = %hu\n", symbol->st_shndx);
                cprintf("  value           = 0x%08lX\n", symbol->st_value);
                cprintf("  size            = 0x%08lX\n", symbol->st_size);
                cprintf("  addr from dwarf = %p\n", (void*)addr);
            }

            // Don't override symbols of non-pointer size.
            if (symbol->st_size != sizeof(uintptr_t)) {
                if_cprintf(trace_elf, "  --- not overriding because size != %lu\n", sizeof(uintptr_t));
                continue;
            }

            if (!(image_start <= symbol->st_value && symbol->st_value < image_end)) {
                if_cprintf(trace_elf, "  --- not overriding because symbol points to outside of image data\n");
                continue;
            }

            uintptr_t *symbol_value_ptr = (uintptr_t *)symbol->st_value;

            if (addr == 0) {
                if_cprintf(trace_elf, "  --- not overriding because could not find fitting symbol\n");
                continue;
            }

            if (*symbol_value_ptr) {
                if_cprintf(trace_elf, "  --- not overriding because symbol already has non-zero value: 0x%08lX\n", *symbol_value_ptr);
                continue;
            }

            *symbol_value_ptr = addr;
            if_cprintf(trace_elf, "  +++ overriding. new value: 0x%08lX\n", *symbol_value_ptr);
        }
    }

    return 0;
}

static int
verify_elf(const struct Elf *elf) {
    if (elf->e_magic != ELF_MAGIC) {
        warn(
            "elf header magic is invalid. got magic = %04X, expected = %04X",
            elf->e_magic, ELF_MAGIC
        );
        return -E_INVALID_EXE;
    }

    if (elf->e_ehsize != sizeof(struct Elf)) {
        warn(
            "elf header size mismatch. got size = %hu, expected = %lu",
            elf->e_ehsize, sizeof(struct Elf)
        );
        return -E_INVALID_EXE;
    }

    if (elf->e_machine != EM_X86_64 && elf->e_machine != EM_AMD64) {
        warn(
            "elf machine type is not x86/amd64, which is unsupported. machine type = %02X",
            elf->e_machine
        );
        return -E_INVALID_EXE;
    }

    if (elf->e_shentsize != sizeof(struct Secthdr)) {
        warn(
            "elf section header size mismatch. got size = %hu, expected = %lu",
            elf->e_shentsize, sizeof(struct Secthdr)
        );
        return -E_INVALID_EXE;
    }

    return 0;
}

static int
parse_elf_sections(const struct Elf *elf, const uint8_t *binary, struct ParsedElfSections *output) {
    uint64_t sect_headers_file_offset = elf->e_shoff;

    int sect_headers_count = elf->e_shnum;

    const struct Secthdr *sections = (struct Secthdr*)(binary + sect_headers_file_offset);

    if (elf->e_shstrndx >= ET_LOPROC) {
        warn("e_shstrndx >= 0xff00 unsupported. e_shstrndx = %hu", elf->e_shstrndx);
        return -E_INVALID_EXE;
    }

    const struct Secthdr *section_name_table = &sections[elf->e_shstrndx];

    output->section_strings.data = (char *)(binary + section_name_table->sh_offset);
    output->section_strings.size = section_name_table->sh_size;

    if_cprintf(
        trace_elf,
        "loaded elf section string table. index = %d, addr = %p, size = %lu\n",
        elf->e_shstrndx, output->section_strings.data, output->section_strings.size
    );

    if_cprintf(
        trace_elf,
        "reading elf section headers. sect_headers_count = %d, sect_headers_file_offset = %lu\n",
        sect_headers_count, sect_headers_file_offset
    );

    for (int i = 0; i < sect_headers_count; ++i) {
        const struct Secthdr *section = &sections[i];
        const char *name = output->section_strings.data + section->sh_name;

        if_cprintf(trace_elf, "section header. index = %d, name = %s\n", i, name);

        if (strcmp(name, ".symtab") == 0) {
            if_cprintf(trace_elf, "  found symbol table\n");

            if (section->sh_size % sizeof(struct Elf64_Sym) != 0) {
                warn(
                    "symbol table size does not divide by Elf64_Sym struct size. table size = %lu, struct size = %lu",
                    section->sh_size, sizeof(struct Elf64_Sym)
                );
                return -E_INVALID_EXE;
            }

            output->symbols.entries = (const struct Elf64_Sym*)(binary + section->sh_offset);
            output->symbols.count = section->sh_size / sizeof(struct Elf64_Sym);

            if_cprintf(trace_elf, "  number of entries = %lu\n", output->symbols.count);
        } else if (strcmp(name, ".strtab") == 0) {
            if_cprintf(trace_elf, "  found symbol string table\n");

            output->symbol_strings.data = (char *)(binary + section->sh_offset);
            output->symbol_strings.size = section->sh_size;
        }
    }

    return 0;
}

/* Set up the initial program binary, stack, and processor flags
 * for a user process.
 * This function is ONLY called during kernel initialization,
 * before running the first environment.
 *
 * This function loads all loadable segments from the ELF binary image
 * into the environment's user memory, starting at the appropriate
 * virtual addresses indicated in the ELF program header.
 * At the same time it clears to zero any portions of these segments
 * that are marked in the program header as being mapped
 * but not actually present in the ELF file - i.e., the program's bss section.
 *
 * All this is very similar to what our boot loader does, except the boot
 * loader also needs to read the code from disk.  Take a look at
 * LoaderPkg/Loader/Bootloader.c to get ideas.
 *
 * Finally, this function maps one page for the program's initial stack.
 *
 * load_icode returns -E_INVALID_EXE if it encounters problems.
 *  - How might load_icode fail?  What might be wrong with the given input?
 *
 * Hints:
 *   Load each program segment into memory
 *   at the address specified in the ELF section header.
 *   You should only load segments with ph->p_type == ELF_PROG_LOAD.
 *   Each segment's address can be found in ph->p_va
 *   and its size in memory can be found in ph->p_memsz.
 *   The ph->p_filesz bytes from the ELF binary, starting at
 *   'binary + ph->p_offset', should be copied to address
 *   ph->p_va.  Any remaining memory bytes should be cleared to zero.
 *   (The ELF header should have ph->p_filesz <= ph->p_memsz.)
 *
 *   All page protection bits should be user read/write for now.
 *   ELF segments are not necessarily page-aligned, but you can
 *   assume for this function that no two segments will touch
 *   the same page.
 *
 *   You must also do something with the program's entry point,
 *   to make sure that the environment starts executing there.
 *   What?  (See env_run() and env_pop_tf() below.) */
static int
load_icode(struct Env *env, uint8_t *binary, size_t size) {
    // LAB 3: Your code here

    if_cprintf(trace_elf, "loading elf for env with id = %d\n", env->env_id);

    struct Elf *elf = (struct Elf*)binary;

    int result = verify_elf(elf);
    
    if (result < 0) {
        warn("verifying elf failed: %i", result);
        return result;
    }

    struct ParsedElfSections parsed_sections;
    parsed_sections.section_strings.data = NULL;
    parsed_sections.section_strings.size = 0;
    parsed_sections.symbol_strings.data = NULL;
    parsed_sections.symbol_strings.size = 0;
    parsed_sections.symbols.entries = NULL;
    parsed_sections.symbols.count = 0;

    result = parse_elf_sections(elf, binary, &parsed_sections);

    if (result < 0) {
        warn("parsing elf sections failed: %i", result);
        return result;
    }

    uint64_t prog_headers_file_offset = elf->e_phoff;
    int prog_headers_count = elf->e_phnum;

    struct Proghdr *prog_headers = (struct Proghdr *)(binary + prog_headers_file_offset);

    if_cprintf(
        trace_elf,
        "reading elf program headers. prog_headers_count = %d, prog_headers_file_offset = %lu\n",
        prog_headers_count, prog_headers_file_offset
    );

    // TODO(e-kutovoi): Assuming image is laid out continuously
    uintptr_t image_start = 0;
    uintptr_t image_end = 0;

    for (int i = 0; i < prog_headers_count; ++i) {
        struct Proghdr *prog_header = &prog_headers[i];

        if (trace_elf) {
            cprintf("program header. index = %d\n", i);
            cprintf("  type      = %s\n", elf_prog_header_type_to_name(prog_header->p_type));
            cprintf("  type(int) = 0x%04X\n", prog_header->p_type);
            cprintf("  p_offset  = 0x%08lX\n", prog_header->p_offset);
            cprintf("  p_va      = 0x%08lX\n", prog_header->p_va);
            cprintf("  p_pa      = 0x%08lX\n", prog_header->p_pa);
            cprintf("  p_filesz  = 0x%08lX\n", prog_header->p_filesz);
            cprintf("  p_memsz   = 0x%08lX\n", prog_header->p_memsz);
            cprintf("  p_flags   = 0x%08X\n", prog_header->p_flags);
            cprintf("  p_align   = 0x%08lX\n", prog_header->p_align);
        }

        if (prog_header->p_filesz > prog_header->p_memsz) {
            warn("program header has file size > memory size, which is invalid");
            return -E_INVALID_EXE;
        }

        if (prog_header->p_type == PT_LOAD) {
            if (image_start == 0 || prog_header->p_va < image_start) {
                image_start = prog_header->p_va;
            }

            if (image_end == 0 || image_end < prog_header->p_va + prog_header->p_memsz) {
                image_end = prog_header->p_va + prog_header->p_memsz;
            }

            uint8_t *copy_src = binary + prog_header->p_offset;
            uint8_t *copy_dst = (uint8_t *)(prog_header->p_va);
            uint64_t copy_size = prog_header->p_filesz;

            uint8_t *zero_dst = copy_dst + copy_size;
            uint64_t zero_size = prog_header->p_memsz - prog_header->p_filesz;

            if_cprintf(
                trace_elf,
                "  +++ loading program header into memory\n"
                "    copy_src = %p, copy_dst = %p, copy_size = %lx\n"
                "    zero_dst = %p, zero_size = %lx\n",
                copy_src, copy_dst, copy_size,
                zero_dst, zero_size
            );

            memcpy(copy_dst, copy_src, copy_size);
            memset(zero_dst, 0, zero_size);
        }
    }

    if_cprintf(trace_elf, "determined image loaded region: [0x%08lX : 0x%08lX]\n", image_start, image_end);

    // Either both set (normal case), or both unset (edge case - no loadable segments, not impossible?)
    assert((image_start == 0) == (image_start == 0));

    if_cprintf(trace_elf, "setting entry point to env = %lx\n", elf->e_entry);
    if_cprintf(trace_elf, "setting flags to env = %x\n", elf->e_flags);

    env->env_tf.tf_rip = elf->e_entry;
    // Set in env_alloc
    // env->env_tf.tf_rflags = elf->e_flags;

    if_cprintf(trace_elf, "binding functions for env\n");

    bind_functions(env, elf, image_start, image_end, &parsed_sections);

    return 0;
}

/* Allocates a new env with env_alloc, loads the named elf
 * binary into it with load_icode, and sets its env_type.
 * This function is ONLY called during kernel initialization,
 * before running the first user-mode environment.
 * The new env's parent ID is set to 0.
 */
void
env_create(uint8_t *binary, size_t size, enum EnvType type) {
    // LAB 3: Your code here

    struct Env *env = NULL;

    int result = env_alloc(&env, 0, type);

    if (result < 0) {
        panic("env_alloc failed during env_create with error: %i", result);
    }

    assert(env != NULL);

    result = load_icode(env, binary, size);

    if (result < 0) {
        panic("load_icode failed during env_create with error: %i", result);
    }
}


/* Frees env and all memory it uses */
void
env_free(struct Env *env) {

    /* Note the environment's demise. */
    if (trace_envs) cprintf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, env->env_id);

    /* Return the environment to the free list */
    env->env_status = ENV_FREE;
    env->env_link = env_free_list;
    env_free_list = env;
}

/* Frees environment env
 *
 * If env was the current one, then runs a new environment
 * (and does not return to the caller)
 */
void
env_destroy(struct Env *env) {
    /* If env is currently running on other CPUs, we change its state to
     * ENV_DYING. A zombie environment will be freed the next time
     * it traps to the kernel. */

    // LAB 3: Your code here

    env_free(env);

    if (curenv == env) {
        sched_yield();
    }
}

#ifdef CONFIG_KSPACE
void
csys_exit(void) {
    if (!curenv) panic("curenv = NULL");
    env_destroy(curenv);
}

void
csys_yield(struct Trapframe *tf) {
    memcpy(&curenv->env_tf, tf, sizeof(struct Trapframe));
    sched_yield();
}
#endif

/* Restores the register values in the Trapframe with the 'ret' instruction.
 * This exits the kernel and starts executing some environment's code.
 *
 * This function does not return.
 */

_Noreturn void
env_pop_tf(struct Trapframe *tf) {
    asm volatile(
            "movq %0, %%rsp\n"
            "movq 0(%%rsp), %%r15\n"
            "movq 8(%%rsp), %%r14\n"
            "movq 16(%%rsp), %%r13\n"
            "movq 24(%%rsp), %%r12\n"
            "movq 32(%%rsp), %%r11\n"
            "movq 40(%%rsp), %%r10\n"
            "movq 48(%%rsp), %%r9\n"
            "movq 56(%%rsp), %%r8\n"
            "movq 64(%%rsp), %%rsi\n"
            "movq 72(%%rsp), %%rdi\n"
            "movq 80(%%rsp), %%rbp\n"
            "movq 88(%%rsp), %%rdx\n"
            "movq 96(%%rsp), %%rcx\n"
            "movq 104(%%rsp), %%rbx\n"
            "movq 112(%%rsp), %%rax\n"
            "movw 120(%%rsp), %%es\n"
            "movw 128(%%rsp), %%ds\n"
            "addq $152,%%rsp\n" /* skip tf_trapno and tf_errcode */
            "iretq" ::"g"(tf)
            : "memory");

    /* Mostly to placate the compiler */
    panic("Reached unrecheble\n");
}

/* Context switch from curenv to env.
 * This function does not return.
 *
 * Step 1: If this is a context switch (a new environment is running):
 *       1. Set the current environment (if any) back to
 *          ENV_RUNNABLE if it is ENV_RUNNING (think about
 *          what other states it can be in),
 *       2. Set 'curenv' to the new environment,
 *       3. Set its status to ENV_RUNNING,
 *       4. Update its 'env_runs' counter,
 * Step 2: Use env_pop_tf() to restore the environment's
 *       registers and starting execution of process.

 * Hints:
 *    If this is the first call to env_run, curenv is NULL.
 *
 *    This function loads the new environment's state from
 *    env->env_tf.  Go back through the code you wrote above
 *    and make sure you have set the relevant parts of
 *    env->env_tf to sensible values.
 */
_Noreturn void
env_run(struct Env *env) {
    assert(env);

    if (trace_envs_more) {
        const char *state[] = {"FREE", "DYING", "RUNNABLE", "RUNNING", "NOT_RUNNABLE"};
        if (curenv) cprintf("[%08X] env stopped: %s\n", curenv->env_id, state[curenv->env_status]);
        cprintf("[%08X] env started: %s\n", env->env_id, state[env->env_status]);
    }

    // LAB 3: Your code here

    if (curenv != NULL) {
        if (curenv->env_status == ENV_RUNNING) {
            curenv->env_status = ENV_RUNNABLE;
        } else if (curenv->env_status == ENV_FREE) {
            // pass
        } else {
            panic("unreachable - unexpected curenv status in env_run");
        }
    }

    curenv = env;

    curenv->env_status = ENV_RUNNING;
    curenv->env_runs += 1;

    env_pop_tf(&curenv->env_tf);

    panic("env_run unreachable?");
}
