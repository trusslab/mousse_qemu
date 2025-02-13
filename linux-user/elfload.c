/* This is the Linux kernel elf-loading code, ported into user space */
/* modifed by Yingtong Liu */
#include <sys/time.h>
#include <sys/param.h>

#include <stdio.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "qemu.h"
#include "disas.h"

#ifdef _ARCH_PPC64
#undef ARCH_DLINFO
#undef ELF_PLATFORM
#undef ELF_HWCAP
#undef ELF_CLASS
#undef ELF_DATA
#undef ELF_ARCH
#endif

#define ELF_OSABI   ELFOSABI_SYSV

/* from personality.h */

/*
 * Flags for bug emulation.
 *
 * These occupy the top three bytes.
 */
enum {
    ADDR_NO_RANDOMIZE = 0x0040000,      /* disable randomization of VA space */
    FDPIC_FUNCPTRS =    0x0080000,      /* userspace function ptrs point to
                                           descriptors (signal handling) */
    MMAP_PAGE_ZERO =    0x0100000,
    ADDR_COMPAT_LAYOUT = 0x0200000,
    READ_IMPLIES_EXEC = 0x0400000,
    ADDR_LIMIT_32BIT =  0x0800000,
    SHORT_INODE =       0x1000000,
    WHOLE_SECONDS =     0x2000000,
    STICKY_TIMEOUTS =   0x4000000,
    ADDR_LIMIT_3GB =    0x8000000,
};

/*
 * Personality types.
 *
 * These go in the low byte.  Avoid using the top bit, it will
 * conflict with error returns.
 */
enum {
    PER_LINUX =         0x0000,
    PER_LINUX_32BIT =   0x0000 | ADDR_LIMIT_32BIT,
    PER_LINUX_FDPIC =   0x0000 | FDPIC_FUNCPTRS,
    PER_SVR4 =          0x0001 | STICKY_TIMEOUTS | MMAP_PAGE_ZERO,
    PER_SVR3 =          0x0002 | STICKY_TIMEOUTS | SHORT_INODE,
    PER_SCOSVR3 =       0x0003 | STICKY_TIMEOUTS | WHOLE_SECONDS | SHORT_INODE,
    PER_OSR5 =          0x0003 | STICKY_TIMEOUTS | WHOLE_SECONDS,
    PER_WYSEV386 =      0x0004 | STICKY_TIMEOUTS | SHORT_INODE,
    PER_ISCR4 =         0x0005 | STICKY_TIMEOUTS,
    PER_BSD =           0x0006,
    PER_SUNOS =         0x0006 | STICKY_TIMEOUTS,
    PER_XENIX =         0x0007 | STICKY_TIMEOUTS | SHORT_INODE,
    PER_LINUX32 =       0x0008,
    PER_LINUX32_3GB =   0x0008 | ADDR_LIMIT_3GB,
    PER_IRIX32 =        0x0009 | STICKY_TIMEOUTS,/* IRIX5 32-bit */
    PER_IRIXN32 =       0x000a | STICKY_TIMEOUTS,/* IRIX6 new 32-bit */
    PER_IRIX64 =        0x000b | STICKY_TIMEOUTS,/* IRIX6 64-bit */
    PER_RISCOS =        0x000c,
    PER_SOLARIS =       0x000d | STICKY_TIMEOUTS,
    PER_UW7 =           0x000e | STICKY_TIMEOUTS | MMAP_PAGE_ZERO,
    PER_OSF4 =          0x000f,                  /* OSF/1 v4 */
    PER_HPUX =          0x0010,
    PER_MASK =          0x00ff,
};

/*
 * Return the base personality without flags.
 */
#define personality(pers)       (pers & PER_MASK)

/* this flag is uneffective under linux too, should be deleted */
#ifndef MAP_DENYWRITE
#define MAP_DENYWRITE 0
#endif

/* should probably go in elf.h */
#ifndef ELIBBAD
#define ELIBBAD 80
#endif

#ifdef TARGET_WORDS_BIGENDIAN
#define ELF_DATA        ELFDATA2MSB
#else
#define ELF_DATA        ELFDATA2LSB
#endif

typedef target_ulong    target_elf_greg_t;
#ifdef USE_UID16
typedef target_ushort   target_uid_t;
typedef target_ushort   target_gid_t;
#else
typedef target_uint     target_uid_t;
typedef target_uint     target_gid_t;
#endif
typedef target_int      target_pid_t;

#ifdef TARGET_I386

#define ELF_PLATFORM get_elf_platform()

static const char *get_elf_platform(void)
{
    static char elf_platform[] = "i386";
    int family = (thread_env->cpuid_version >> 8) & 0xff;
    if (family > 6)
        family = 6;
    if (family >= 3)
        elf_platform[1] = '0' + family;
    return elf_platform;
}

#define ELF_HWCAP get_elf_hwcap()

static uint32_t get_elf_hwcap(void)
{
    return thread_env->cpuid_features;
}

#ifdef TARGET_X86_64
#define ELF_START_MMAP 0x2aaaaab000ULL
#define elf_check_arch(x) ( ((x) == ELF_ARCH) )

#define ELF_CLASS      ELFCLASS64
#define ELF_ARCH       EM_X86_64

static inline void init_thread(struct target_pt_regs *regs, struct image_info *infop)
{
    regs->rax = 0;
    regs->rsp = infop->start_stack;
    regs->rip = infop->entry;
}

#define ELF_NREG    27
typedef target_elf_greg_t  target_elf_gregset_t[ELF_NREG];

/*
 * Note that ELF_NREG should be 29 as there should be place for
 * TRAPNO and ERR "registers" as well but linux doesn't dump
 * those.
 *
 * See linux kernel: arch/x86/include/asm/elf.h
 */
static void elf_core_copy_regs(target_elf_gregset_t *regs, const CPUX86State *env)
{
    (*regs)[0] = env->regs[15];
    (*regs)[1] = env->regs[14];
    (*regs)[2] = env->regs[13];
    (*regs)[3] = env->regs[12];
    (*regs)[4] = env->regs[R_EBP];
    (*regs)[5] = env->regs[R_EBX];
    (*regs)[6] = env->regs[11];
    (*regs)[7] = env->regs[10];
    (*regs)[8] = env->regs[9];
    (*regs)[9] = env->regs[8];
    (*regs)[10] = env->regs[R_EAX];
    (*regs)[11] = env->regs[R_ECX];
    (*regs)[12] = env->regs[R_EDX];
    (*regs)[13] = env->regs[R_ESI];
    (*regs)[14] = env->regs[R_EDI];
    (*regs)[15] = env->regs[R_EAX]; /* XXX */
    (*regs)[16] = env->eip;
    (*regs)[17] = env->segs[R_CS].selector & 0xffff;
    (*regs)[18] = env->eflags;
    (*regs)[19] = env->regs[R_ESP];
    (*regs)[20] = env->segs[R_SS].selector & 0xffff;
    (*regs)[21] = env->segs[R_FS].selector & 0xffff;
    (*regs)[22] = env->segs[R_GS].selector & 0xffff;
    (*regs)[23] = env->segs[R_DS].selector & 0xffff;
    (*regs)[24] = env->segs[R_ES].selector & 0xffff;
    (*regs)[25] = env->segs[R_FS].selector & 0xffff;
    (*regs)[26] = env->segs[R_GS].selector & 0xffff;
}

#else

#define ELF_START_MMAP 0x80000000

/*
 * This is used to ensure we don't load something for the wrong architecture.
 */
#define elf_check_arch(x) ( ((x) == EM_386) || ((x) == EM_486) )

/*
 * These are used to set parameters in the core dumps.
 */
#define ELF_CLASS       ELFCLASS32
#define ELF_ARCH        EM_386

static inline void init_thread(struct target_pt_regs *regs,
                               struct image_info *infop)
{
    regs->esp = infop->start_stack;
    regs->eip = infop->entry;

    /* SVR4/i386 ABI (pages 3-31, 3-32) says that when the program
       starts %edx contains a pointer to a function which might be
       registered using `atexit'.  This provides a mean for the
       dynamic linker to call DT_FINI functions for shared libraries
       that have been loaded before the code runs.

       A value of 0 tells we have no such handler.  */
    regs->edx = 0;
}

#define ELF_NREG    17
typedef target_elf_greg_t  target_elf_gregset_t[ELF_NREG];

/*
 * Note that ELF_NREG should be 19 as there should be place for
 * TRAPNO and ERR "registers" as well but linux doesn't dump
 * those.
 *
 * See linux kernel: arch/x86/include/asm/elf.h
 */
static void elf_core_copy_regs(target_elf_gregset_t *regs, const CPUX86State *env)
{
    (*regs)[0] = env->regs[R_EBX];
    (*regs)[1] = env->regs[R_ECX];
    (*regs)[2] = env->regs[R_EDX];
    (*regs)[3] = env->regs[R_ESI];
    (*regs)[4] = env->regs[R_EDI];
    (*regs)[5] = env->regs[R_EBP];
    (*regs)[6] = env->regs[R_EAX];
    (*regs)[7] = env->segs[R_DS].selector & 0xffff;
    (*regs)[8] = env->segs[R_ES].selector & 0xffff;
    (*regs)[9] = env->segs[R_FS].selector & 0xffff;
    (*regs)[10] = env->segs[R_GS].selector & 0xffff;
    (*regs)[11] = env->regs[R_EAX]; /* XXX */
    (*regs)[12] = env->eip;
    (*regs)[13] = env->segs[R_CS].selector & 0xffff;
    (*regs)[14] = env->eflags;
    (*regs)[15] = env->regs[R_ESP];
    (*regs)[16] = env->segs[R_SS].selector & 0xffff;
}
#endif

#define USE_ELF_CORE_DUMP
#define ELF_EXEC_PAGESIZE       4096

#endif

#ifdef TARGET_ARM

#define ELF_START_MMAP 0x80000000

#define elf_check_arch(x) ( (x) == EM_ARM )

#define ELF_CLASS       ELFCLASS32
#define ELF_ARCH        EM_ARM

static inline void init_thread(struct target_pt_regs *regs,
                               struct image_info *infop)
{
    abi_long stack = infop->start_stack;
    memset(regs, 0, sizeof(*regs));
    regs->ARM_cpsr = 0x10;
    if (infop->entry & 1)
        regs->ARM_cpsr |= CPSR_T;
    regs->ARM_pc = infop->entry & 0xfffffffe;
    regs->ARM_sp = infop->start_stack;
    /* FIXME - what to for failure of get_user()? */
    get_user_ual(regs->ARM_r2, stack + 8); /* envp */
    get_user_ual(regs->ARM_r1, stack + 4); /* envp */
    /* XXX: it seems that r0 is zeroed after ! */
    regs->ARM_r0 = 0;
    /* For uClinux PIC binaries.  */
    /* XXX: Linux does this only on ARM with no MMU (do we care ?) */
    regs->ARM_r10 = infop->start_data;
}

#define ELF_NREG    18
typedef target_elf_greg_t  target_elf_gregset_t[ELF_NREG];

static void elf_core_copy_regs(target_elf_gregset_t *regs, const CPUARMState *env)
{
    (*regs)[0] = tswapl(env->regs[0]);
    (*regs)[1] = tswapl(env->regs[1]);
    (*regs)[2] = tswapl(env->regs[2]);
    (*regs)[3] = tswapl(env->regs[3]);
    (*regs)[4] = tswapl(env->regs[4]);
    (*regs)[5] = tswapl(env->regs[5]);
    (*regs)[6] = tswapl(env->regs[6]);
    (*regs)[7] = tswapl(env->regs[7]);
    (*regs)[8] = tswapl(env->regs[8]);
    (*regs)[9] = tswapl(env->regs[9]);
    (*regs)[10] = tswapl(env->regs[10]);
    (*regs)[11] = tswapl(env->regs[11]);
    (*regs)[12] = tswapl(env->regs[12]);
    (*regs)[13] = tswapl(env->regs[13]);
    (*regs)[14] = tswapl(env->regs[14]);
    (*regs)[15] = tswapl(env->regs[15]);

    (*regs)[16] = tswapl(cpsr_read((CPUARMState *)env));
    (*regs)[17] = tswapl(env->regs[0]); /* XXX */
}

#define USE_ELF_CORE_DUMP
#define ELF_EXEC_PAGESIZE       4096

enum
{
    ARM_HWCAP_ARM_SWP       = 1 << 0,
    ARM_HWCAP_ARM_HALF      = 1 << 1,
    ARM_HWCAP_ARM_THUMB     = 1 << 2,
    ARM_HWCAP_ARM_26BIT     = 1 << 3,
    ARM_HWCAP_ARM_FAST_MULT = 1 << 4,
    ARM_HWCAP_ARM_FPA       = 1 << 5,
    ARM_HWCAP_ARM_VFP       = 1 << 6,
    ARM_HWCAP_ARM_EDSP      = 1 << 7,
    ARM_HWCAP_ARM_JAVA      = 1 << 8,
    ARM_HWCAP_ARM_IWMMXT    = 1 << 9,
    ARM_HWCAP_ARM_THUMBEE   = 1 << 10,
    ARM_HWCAP_ARM_NEON      = 1 << 11,
    ARM_HWCAP_ARM_VFPv3     = 1 << 12,
    ARM_HWCAP_ARM_VFPv3D16  = 1 << 13,
};

#define TARGET_HAS_VALIDATE_GUEST_SPACE
/* Return 1 if the proposed guest space is suitable for the guest.
 * Return 0 if the proposed guest space isn't suitable, but another
 * address space should be tried.
 * Return -1 if there is no way the proposed guest space can be
 * valid regardless of the base.
 * The guest code may leave a page mapped and populate it if the
 * address is suitable.
 */
static int validate_guest_space(unsigned long guest_base,
                                unsigned long guest_size)
{
    unsigned long real_start, test_page_addr;

    /* We need to check that we can force a fault on access to the
     * commpage at 0xffff0fxx
     */
    test_page_addr = guest_base + (0xffff0f00 & qemu_host_page_mask);

    /* If the commpage lies within the already allocated guest space,
     * then there is no way we can allocate it.
     */
    if (test_page_addr >= guest_base
        && test_page_addr <= (guest_base + guest_size)) {
        return -1;
    }

    /* Note it needs to be writeable to let us initialise it */
    real_start = (unsigned long)
                 mmap((void *)test_page_addr, qemu_host_page_size,
                     PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    /* If we can't map it then try another address */
    if (real_start == -1ul) {
        return 0;
    }

    if (real_start != test_page_addr) {
        /* OS didn't put the page where we asked - unmap and reject */
        munmap((void *)real_start, qemu_host_page_size);
        return 0;
    }

    /* Leave the page mapped
     * Populate it (mmap should have left it all 0'd)
     */

    /* Kernel helper versions */
    __put_user(5, (uint32_t *)g2h(0xffff0ffcul));

    /* Now it's populated make it RO */
    if (mprotect((void *)test_page_addr, qemu_host_page_size, PROT_READ)) {
        perror("Protecting guest commpage");
        exit(-1);
    }

    return 1; /* All good */
}


#define ELF_HWCAP get_elf_hwcap()

static uint32_t get_elf_hwcap(void)
{
    CPUARMState *e = thread_env;
    uint32_t hwcaps = 0;

    hwcaps |= ARM_HWCAP_ARM_SWP;
    hwcaps |= ARM_HWCAP_ARM_HALF;
    hwcaps |= ARM_HWCAP_ARM_THUMB;
    hwcaps |= ARM_HWCAP_ARM_FAST_MULT;
    hwcaps |= ARM_HWCAP_ARM_FPA;

    /* probe for the extra features */
#define GET_FEATURE(feat, hwcap) \
    do {if (arm_feature(e, feat)) { hwcaps |= hwcap; } } while (0)
    GET_FEATURE(ARM_FEATURE_VFP, ARM_HWCAP_ARM_VFP);
    GET_FEATURE(ARM_FEATURE_IWMMXT, ARM_HWCAP_ARM_IWMMXT);
    GET_FEATURE(ARM_FEATURE_THUMB2EE, ARM_HWCAP_ARM_THUMBEE);
    GET_FEATURE(ARM_FEATURE_NEON, ARM_HWCAP_ARM_NEON);
    GET_FEATURE(ARM_FEATURE_VFP3, ARM_HWCAP_ARM_VFPv3);
    GET_FEATURE(ARM_FEATURE_VFP_FP16, ARM_HWCAP_ARM_VFPv3D16);
#undef GET_FEATURE

    return hwcaps;
}

#endif

#ifdef TARGET_UNICORE32

#define ELF_START_MMAP          0x80000000

#define elf_check_arch(x)       ((x) == EM_UNICORE32)

#define ELF_CLASS               ELFCLASS32
#define ELF_DATA                ELFDATA2LSB
#define ELF_ARCH                EM_UNICORE32

static inline void init_thread(struct target_pt_regs *regs,
        struct image_info *infop)
{
    abi_long stack = infop->start_stack;
    memset(regs, 0, sizeof(*regs));
    regs->UC32_REG_asr = 0x10;
    regs->UC32_REG_pc = infop->entry & 0xfffffffe;
    regs->UC32_REG_sp = infop->start_stack;
    /* FIXME - what to for failure of get_user()? */
    get_user_ual(regs->UC32_REG_02, stack + 8); /* envp */
    get_user_ual(regs->UC32_REG_01, stack + 4); /* envp */
    /* XXX: it seems that r0 is zeroed after ! */
    regs->UC32_REG_00 = 0;
}

#define ELF_NREG    34
typedef target_elf_greg_t  target_elf_gregset_t[ELF_NREG];

static void elf_core_copy_regs(target_elf_gregset_t *regs, const CPUUniCore32State *env)
{
    (*regs)[0] = env->regs[0];
    (*regs)[1] = env->regs[1];
    (*regs)[2] = env->regs[2];
    (*regs)[3] = env->regs[3];
    (*regs)[4] = env->regs[4];
    (*regs)[5] = env->regs[5];
    (*regs)[6] = env->regs[6];
    (*regs)[7] = env->regs[7];
    (*regs)[8] = env->regs[8];
    (*regs)[9] = env->regs[9];
    (*regs)[10] = env->regs[10];
    (*regs)[11] = env->regs[11];
    (*regs)[12] = env->regs[12];
    (*regs)[13] = env->regs[13];
    (*regs)[14] = env->regs[14];
    (*regs)[15] = env->regs[15];
    (*regs)[16] = env->regs[16];
    (*regs)[17] = env->regs[17];
    (*regs)[18] = env->regs[18];
    (*regs)[19] = env->regs[19];
    (*regs)[20] = env->regs[20];
    (*regs)[21] = env->regs[21];
    (*regs)[22] = env->regs[22];
    (*regs)[23] = env->regs[23];
    (*regs)[24] = env->regs[24];
    (*regs)[25] = env->regs[25];
    (*regs)[26] = env->regs[26];
    (*regs)[27] = env->regs[27];
    (*regs)[28] = env->regs[28];
    (*regs)[29] = env->regs[29];
    (*regs)[30] = env->regs[30];
    (*regs)[31] = env->regs[31];

    (*regs)[32] = cpu_asr_read((CPUUniCore32State *)env);
    (*regs)[33] = env->regs[0]; /* XXX */
}

#define USE_ELF_CORE_DUMP
#define ELF_EXEC_PAGESIZE               4096

#define ELF_HWCAP                       (UC32_HWCAP_CMOV | UC32_HWCAP_UCF64)

#endif

#ifdef TARGET_SPARC
#ifdef TARGET_SPARC64

#define ELF_START_MMAP 0x80000000
#define ELF_HWCAP  (HWCAP_SPARC_FLUSH | HWCAP_SPARC_STBAR | HWCAP_SPARC_SWAP \
                    | HWCAP_SPARC_MULDIV | HWCAP_SPARC_V9)
#ifndef TARGET_ABI32
#define elf_check_arch(x) ( (x) == EM_SPARCV9 || (x) == EM_SPARC32PLUS )
#else
#define elf_check_arch(x) ( (x) == EM_SPARC32PLUS || (x) == EM_SPARC )
#endif

#define ELF_CLASS   ELFCLASS64
#define ELF_ARCH    EM_SPARCV9

#define STACK_BIAS              2047

static inline void init_thread(struct target_pt_regs *regs,
                               struct image_info *infop)
{
#ifndef TARGET_ABI32
    regs->tstate = 0;
#endif
    regs->pc = infop->entry;
    regs->npc = regs->pc + 4;
    regs->y = 0;
#ifdef TARGET_ABI32
    regs->u_regs[14] = infop->start_stack - 16 * 4;
#else
    if (personality(infop->personality) == PER_LINUX32)
        regs->u_regs[14] = infop->start_stack - 16 * 4;
    else
        regs->u_regs[14] = infop->start_stack - 16 * 8 - STACK_BIAS;
#endif
}

#else
#define ELF_START_MMAP 0x80000000
#define ELF_HWCAP  (HWCAP_SPARC_FLUSH | HWCAP_SPARC_STBAR | HWCAP_SPARC_SWAP \
                    | HWCAP_SPARC_MULDIV)
#define elf_check_arch(x) ( (x) == EM_SPARC )

#define ELF_CLASS   ELFCLASS32
#define ELF_ARCH    EM_SPARC

static inline void init_thread(struct target_pt_regs *regs,
                               struct image_info *infop)
{
    regs->psr = 0;
    regs->pc = infop->entry;
    regs->npc = regs->pc + 4;
    regs->y = 0;
    regs->u_regs[14] = infop->start_stack - 16 * 4;
}

#endif
#endif

#ifdef TARGET_PPC

#define ELF_START_MMAP 0x80000000

#if defined(TARGET_PPC64) && !defined(TARGET_ABI32)

#define elf_check_arch(x) ( (x) == EM_PPC64 )

#define ELF_CLASS       ELFCLASS64

#else

#define elf_check_arch(x) ( (x) == EM_PPC )

#define ELF_CLASS       ELFCLASS32

#endif

#define ELF_ARCH        EM_PPC

/* Feature masks for the Aux Vector Hardware Capabilities (AT_HWCAP).
   See arch/powerpc/include/asm/cputable.h.  */
enum {
    QEMU_PPC_FEATURE_32 = 0x80000000,
    QEMU_PPC_FEATURE_64 = 0x40000000,
    QEMU_PPC_FEATURE_601_INSTR = 0x20000000,
    QEMU_PPC_FEATURE_HAS_ALTIVEC = 0x10000000,
    QEMU_PPC_FEATURE_HAS_FPU = 0x08000000,
    QEMU_PPC_FEATURE_HAS_MMU = 0x04000000,
    QEMU_PPC_FEATURE_HAS_4xxMAC = 0x02000000,
    QEMU_PPC_FEATURE_UNIFIED_CACHE = 0x01000000,
    QEMU_PPC_FEATURE_HAS_SPE = 0x00800000,
    QEMU_PPC_FEATURE_HAS_EFP_SINGLE = 0x00400000,
    QEMU_PPC_FEATURE_HAS_EFP_DOUBLE = 0x00200000,
    QEMU_PPC_FEATURE_NO_TB = 0x00100000,
    QEMU_PPC_FEATURE_POWER4 = 0x00080000,
    QEMU_PPC_FEATURE_POWER5 = 0x00040000,
    QEMU_PPC_FEATURE_POWER5_PLUS = 0x00020000,
    QEMU_PPC_FEATURE_CELL = 0x00010000,
    QEMU_PPC_FEATURE_BOOKE = 0x00008000,
    QEMU_PPC_FEATURE_SMT = 0x00004000,
    QEMU_PPC_FEATURE_ICACHE_SNOOP = 0x00002000,
    QEMU_PPC_FEATURE_ARCH_2_05 = 0x00001000,
    QEMU_PPC_FEATURE_PA6T = 0x00000800,
    QEMU_PPC_FEATURE_HAS_DFP = 0x00000400,
    QEMU_PPC_FEATURE_POWER6_EXT = 0x00000200,
    QEMU_PPC_FEATURE_ARCH_2_06 = 0x00000100,
    QEMU_PPC_FEATURE_HAS_VSX = 0x00000080,
    QEMU_PPC_FEATURE_PSERIES_PERFMON_COMPAT = 0x00000040,

    QEMU_PPC_FEATURE_TRUE_LE = 0x00000002,
    QEMU_PPC_FEATURE_PPC_LE = 0x00000001,
};

#define ELF_HWCAP get_elf_hwcap()

static uint32_t get_elf_hwcap(void)
{
    CPUPPCState *e = thread_env;
    uint32_t features = 0;

    /* We don't have to be terribly complete here; the high points are
       Altivec/FP/SPE support.  Anything else is just a bonus.  */
#define GET_FEATURE(flag, feature)                                      \
    do {if (e->insns_flags & flag) features |= feature; } while(0)
    GET_FEATURE(PPC_64B, QEMU_PPC_FEATURE_64);
    GET_FEATURE(PPC_FLOAT, QEMU_PPC_FEATURE_HAS_FPU);
    GET_FEATURE(PPC_ALTIVEC, QEMU_PPC_FEATURE_HAS_ALTIVEC);
    GET_FEATURE(PPC_SPE, QEMU_PPC_FEATURE_HAS_SPE);
    GET_FEATURE(PPC_SPE_SINGLE, QEMU_PPC_FEATURE_HAS_EFP_SINGLE);
    GET_FEATURE(PPC_SPE_DOUBLE, QEMU_PPC_FEATURE_HAS_EFP_DOUBLE);
    GET_FEATURE(PPC_BOOKE, QEMU_PPC_FEATURE_BOOKE);
    GET_FEATURE(PPC_405_MAC, QEMU_PPC_FEATURE_HAS_4xxMAC);
#undef GET_FEATURE

    return features;
}

/*
 * The requirements here are:
 * - keep the final alignment of sp (sp & 0xf)
 * - make sure the 32-bit value at the first 16 byte aligned position of
 *   AUXV is greater than 16 for glibc compatibility.
 *   AT_IGNOREPPC is used for that.
 * - for compatibility with glibc ARCH_DLINFO must always be defined on PPC,
 *   even if DLINFO_ARCH_ITEMS goes to zero or is undefined.
 */
#define DLINFO_ARCH_ITEMS       5
#define ARCH_DLINFO                                     \
    do {                                                \
        NEW_AUX_ENT(AT_DCACHEBSIZE, 0x20);              \
        NEW_AUX_ENT(AT_ICACHEBSIZE, 0x20);              \
        NEW_AUX_ENT(AT_UCACHEBSIZE, 0);                 \
        /*                                              \
         * Now handle glibc compatibility.              \
         */                                             \
        NEW_AUX_ENT(AT_IGNOREPPC, AT_IGNOREPPC);        \
        NEW_AUX_ENT(AT_IGNOREPPC, AT_IGNOREPPC);        \
    } while (0)

static inline void init_thread(struct target_pt_regs *_regs, struct image_info *infop)
{
    _regs->gpr[1] = infop->start_stack;
#if defined(TARGET_PPC64) && !defined(TARGET_ABI32)
    _regs->gpr[2] = ldq_raw(infop->entry + 8) + infop->load_bias;
    infop->entry = ldq_raw(infop->entry) + infop->load_bias;
#endif
    _regs->nip = infop->entry;
}

/* See linux kernel: arch/powerpc/include/asm/elf.h.  */
#define ELF_NREG 48
typedef target_elf_greg_t target_elf_gregset_t[ELF_NREG];

static void elf_core_copy_regs(target_elf_gregset_t *regs, const CPUPPCState *env)
{
    int i;
    target_ulong ccr = 0;

    for (i = 0; i < ARRAY_SIZE(env->gpr); i++) {
        (*regs)[i] = tswapl(env->gpr[i]);
    }

    (*regs)[32] = tswapl(env->nip);
    (*regs)[33] = tswapl(env->msr);
    (*regs)[35] = tswapl(env->ctr);
    (*regs)[36] = tswapl(env->lr);
    (*regs)[37] = tswapl(env->xer);

    for (i = 0; i < ARRAY_SIZE(env->crf); i++) {
        ccr |= env->crf[i] << (32 - ((i + 1) * 4));
    }
    (*regs)[38] = tswapl(ccr);
}

#define USE_ELF_CORE_DUMP
#define ELF_EXEC_PAGESIZE       4096

#endif

#ifdef TARGET_MIPS

#define ELF_START_MMAP 0x80000000

#define elf_check_arch(x) ( (x) == EM_MIPS )

#ifdef TARGET_MIPS64
#define ELF_CLASS   ELFCLASS64
#else
#define ELF_CLASS   ELFCLASS32
#endif
#define ELF_ARCH    EM_MIPS

static inline void init_thread(struct target_pt_regs *regs,
                               struct image_info *infop)
{
    regs->cp0_status = 2 << CP0St_KSU;
    regs->cp0_epc = infop->entry;
    regs->regs[29] = infop->start_stack;
}

/* See linux kernel: arch/mips/include/asm/elf.h.  */
#define ELF_NREG 45
typedef target_elf_greg_t target_elf_gregset_t[ELF_NREG];

/* See linux kernel: arch/mips/include/asm/reg.h.  */
enum {
#ifdef TARGET_MIPS64
    TARGET_EF_R0 = 0,
#else
    TARGET_EF_R0 = 6,
#endif
    TARGET_EF_R26 = TARGET_EF_R0 + 26,
    TARGET_EF_R27 = TARGET_EF_R0 + 27,
    TARGET_EF_LO = TARGET_EF_R0 + 32,
    TARGET_EF_HI = TARGET_EF_R0 + 33,
    TARGET_EF_CP0_EPC = TARGET_EF_R0 + 34,
    TARGET_EF_CP0_BADVADDR = TARGET_EF_R0 + 35,
    TARGET_EF_CP0_STATUS = TARGET_EF_R0 + 36,
    TARGET_EF_CP0_CAUSE = TARGET_EF_R0 + 37
};

/* See linux kernel: arch/mips/kernel/process.c:elf_dump_regs.  */
static void elf_core_copy_regs(target_elf_gregset_t *regs, const CPUMIPSState *env)
{
    int i;

    for (i = 0; i < TARGET_EF_R0; i++) {
        (*regs)[i] = 0;
    }
    (*regs)[TARGET_EF_R0] = 0;

    for (i = 1; i < ARRAY_SIZE(env->active_tc.gpr); i++) {
        (*regs)[TARGET_EF_R0 + i] = tswapl(env->active_tc.gpr[i]);
    }

    (*regs)[TARGET_EF_R26] = 0;
    (*regs)[TARGET_EF_R27] = 0;
    (*regs)[TARGET_EF_LO] = tswapl(env->active_tc.LO[0]);
    (*regs)[TARGET_EF_HI] = tswapl(env->active_tc.HI[0]);
    (*regs)[TARGET_EF_CP0_EPC] = tswapl(env->active_tc.PC);
    (*regs)[TARGET_EF_CP0_BADVADDR] = tswapl(env->CP0_BadVAddr);
    (*regs)[TARGET_EF_CP0_STATUS] = tswapl(env->CP0_Status);
    (*regs)[TARGET_EF_CP0_CAUSE] = tswapl(env->CP0_Cause);
}

#define USE_ELF_CORE_DUMP
#define ELF_EXEC_PAGESIZE        4096

#endif /* TARGET_MIPS */

#ifdef TARGET_MICROBLAZE

#define ELF_START_MMAP 0x80000000

#define elf_check_arch(x) ( (x) == EM_MICROBLAZE || (x) == EM_MICROBLAZE_OLD)

#define ELF_CLASS   ELFCLASS32
#define ELF_ARCH    EM_MICROBLAZE

static inline void init_thread(struct target_pt_regs *regs,
                               struct image_info *infop)
{
    regs->pc = infop->entry;
    regs->r1 = infop->start_stack;

}

#define ELF_EXEC_PAGESIZE        4096

#define USE_ELF_CORE_DUMP
#define ELF_NREG 38
typedef target_elf_greg_t target_elf_gregset_t[ELF_NREG];

/* See linux kernel: arch/mips/kernel/process.c:elf_dump_regs.  */
static void elf_core_copy_regs(target_elf_gregset_t *regs, const CPUMBState *env)
{
    int i, pos = 0;

    for (i = 0; i < 32; i++) {
        (*regs)[pos++] = tswapl(env->regs[i]);
    }

    for (i = 0; i < 6; i++) {
        (*regs)[pos++] = tswapl(env->sregs[i]);
    }
}

#endif /* TARGET_MICROBLAZE */

#ifdef TARGET_OPENRISC

#define ELF_START_MMAP 0x08000000

#define elf_check_arch(x) ((x) == EM_OPENRISC)

#define ELF_ARCH EM_OPENRISC
#define ELF_CLASS ELFCLASS32
#define ELF_DATA  ELFDATA2MSB

static inline void init_thread(struct target_pt_regs *regs,
                               struct image_info *infop)
{
    regs->pc = infop->entry;
    regs->gpr[1] = infop->start_stack;
}

#define USE_ELF_CORE_DUMP
#define ELF_EXEC_PAGESIZE 8192

/* See linux kernel arch/openrisc/include/asm/elf.h.  */
#define ELF_NREG 34 /* gprs and pc, sr */
typedef target_elf_greg_t target_elf_gregset_t[ELF_NREG];

static void elf_core_copy_regs(target_elf_gregset_t *regs,
                               const CPUOpenRISCState *env)
{
    int i;

    for (i = 0; i < 32; i++) {
        (*regs)[i] = tswapl(env->gpr[i]);
    }

    (*regs)[32] = tswapl(env->pc);
    (*regs)[33] = tswapl(env->sr);
}
#define ELF_HWCAP 0
#define ELF_PLATFORM NULL

#endif /* TARGET_OPENRISC */

#ifdef TARGET_SH4

#define ELF_START_MMAP 0x80000000

#define elf_check_arch(x) ( (x) == EM_SH )

#define ELF_CLASS ELFCLASS32
#define ELF_ARCH  EM_SH

static inline void init_thread(struct target_pt_regs *regs,
                               struct image_info *infop)
{
    /* Check other registers XXXXX */
    regs->pc = infop->entry;
    regs->regs[15] = infop->start_stack;
}

/* See linux kernel: arch/sh/include/asm/elf.h.  */
#define ELF_NREG 23
typedef target_elf_greg_t target_elf_gregset_t[ELF_NREG];

/* See linux kernel: arch/sh/include/asm/ptrace.h.  */
enum {
    TARGET_REG_PC = 16,
    TARGET_REG_PR = 17,
    TARGET_REG_SR = 18,
    TARGET_REG_GBR = 19,
    TARGET_REG_MACH = 20,
    TARGET_REG_MACL = 21,
    TARGET_REG_SYSCALL = 22
};

static inline void elf_core_copy_regs(target_elf_gregset_t *regs,
                                      const CPUSH4State *env)
{
    int i;

    for (i = 0; i < 16; i++) {
        (*regs[i]) = tswapl(env->gregs[i]);
    }

    (*regs)[TARGET_REG_PC] = tswapl(env->pc);
    (*regs)[TARGET_REG_PR] = tswapl(env->pr);
    (*regs)[TARGET_REG_SR] = tswapl(env->sr);
    (*regs)[TARGET_REG_GBR] = tswapl(env->gbr);
    (*regs)[TARGET_REG_MACH] = tswapl(env->mach);
    (*regs)[TARGET_REG_MACL] = tswapl(env->macl);
    (*regs)[TARGET_REG_SYSCALL] = 0; /* FIXME */
}

#define USE_ELF_CORE_DUMP
#define ELF_EXEC_PAGESIZE        4096

#endif

#ifdef TARGET_CRIS

#define ELF_START_MMAP 0x80000000

#define elf_check_arch(x) ( (x) == EM_CRIS )

#define ELF_CLASS ELFCLASS32
#define ELF_ARCH  EM_CRIS

static inline void init_thread(struct target_pt_regs *regs,
                               struct image_info *infop)
{
    regs->erp = infop->entry;
}

#define ELF_EXEC_PAGESIZE        8192

#endif

#ifdef TARGET_M68K

#define ELF_START_MMAP 0x80000000

#define elf_check_arch(x) ( (x) == EM_68K )

#define ELF_CLASS       ELFCLASS32
#define ELF_ARCH        EM_68K

/* ??? Does this need to do anything?
   #define ELF_PLAT_INIT(_r) */

static inline void init_thread(struct target_pt_regs *regs,
                               struct image_info *infop)
{
    regs->usp = infop->start_stack;
    regs->sr = 0;
    regs->pc = infop->entry;
}

/* See linux kernel: arch/m68k/include/asm/elf.h.  */
#define ELF_NREG 20
typedef target_elf_greg_t target_elf_gregset_t[ELF_NREG];

static void elf_core_copy_regs(target_elf_gregset_t *regs, const CPUM68KState *env)
{
    (*regs)[0] = tswapl(env->dregs[1]);
    (*regs)[1] = tswapl(env->dregs[2]);
    (*regs)[2] = tswapl(env->dregs[3]);
    (*regs)[3] = tswapl(env->dregs[4]);
    (*regs)[4] = tswapl(env->dregs[5]);
    (*regs)[5] = tswapl(env->dregs[6]);
    (*regs)[6] = tswapl(env->dregs[7]);
    (*regs)[7] = tswapl(env->aregs[0]);
    (*regs)[8] = tswapl(env->aregs[1]);
    (*regs)[9] = tswapl(env->aregs[2]);
    (*regs)[10] = tswapl(env->aregs[3]);
    (*regs)[11] = tswapl(env->aregs[4]);
    (*regs)[12] = tswapl(env->aregs[5]);
    (*regs)[13] = tswapl(env->aregs[6]);
    (*regs)[14] = tswapl(env->dregs[0]);
    (*regs)[15] = tswapl(env->aregs[7]);
    (*regs)[16] = tswapl(env->dregs[0]); /* FIXME: orig_d0 */
    (*regs)[17] = tswapl(env->sr);
    (*regs)[18] = tswapl(env->pc);
    (*regs)[19] = 0;  /* FIXME: regs->format | regs->vector */
}

#define USE_ELF_CORE_DUMP
#define ELF_EXEC_PAGESIZE       8192

#endif

#ifdef TARGET_ALPHA

#define ELF_START_MMAP (0x30000000000ULL)

#define elf_check_arch(x) ( (x) == ELF_ARCH )

#define ELF_CLASS      ELFCLASS64
#define ELF_ARCH       EM_ALPHA

static inline void init_thread(struct target_pt_regs *regs,
                               struct image_info *infop)
{
    regs->pc = infop->entry;
    regs->ps = 8;
    regs->usp = infop->start_stack;
}

#define ELF_EXEC_PAGESIZE        8192

#endif /* TARGET_ALPHA */

#ifdef TARGET_S390X

#define ELF_START_MMAP (0x20000000000ULL)

#define elf_check_arch(x) ( (x) == ELF_ARCH )

#define ELF_CLASS	ELFCLASS64
#define ELF_DATA	ELFDATA2MSB
#define ELF_ARCH	EM_S390

static inline void init_thread(struct target_pt_regs *regs, struct image_info *infop)
{
    regs->psw.addr = infop->entry;
    regs->psw.mask = PSW_MASK_64 | PSW_MASK_32;
    regs->gprs[15] = infop->start_stack;
}

#endif /* TARGET_S390X */

#ifndef ELF_PLATFORM
#define ELF_PLATFORM (NULL)
#endif

#ifndef ELF_HWCAP
#define ELF_HWCAP 0
#endif

#ifdef TARGET_ABI32
#undef ELF_CLASS
#define ELF_CLASS ELFCLASS32
#undef bswaptls
#define bswaptls(ptr) bswap32s(ptr)
#endif

#include "elf.h"

struct exec
{
    unsigned int a_info;   /* Use macros N_MAGIC, etc for access */
    unsigned int a_text;   /* length of text, in bytes */
    unsigned int a_data;   /* length of data, in bytes */
    unsigned int a_bss;    /* length of uninitialized data area, in bytes */
    unsigned int a_syms;   /* length of symbol table data in file, in bytes */
    unsigned int a_entry;  /* start address */
    unsigned int a_trsize; /* length of relocation info for text, in bytes */
    unsigned int a_drsize; /* length of relocation info for data, in bytes */
};


#define N_MAGIC(exec) ((exec).a_info & 0xffff)
#define OMAGIC 0407
#define NMAGIC 0410
#define ZMAGIC 0413
#define QMAGIC 0314

/* Necessary parameters */
#define TARGET_ELF_EXEC_PAGESIZE TARGET_PAGE_SIZE
#define TARGET_ELF_PAGESTART(_v) ((_v) & ~(unsigned long)(TARGET_ELF_EXEC_PAGESIZE-1))
#define TARGET_ELF_PAGEOFFSET(_v) ((_v) & (TARGET_ELF_EXEC_PAGESIZE-1))

#define DLINFO_ITEMS 13

static inline void memcpy_fromfs(void * to, const void * from, unsigned long n)
{
    memcpy(to, from, n);
}

#ifdef BSWAP_NEEDED
static void bswap_ehdr(struct elfhdr *ehdr)
{
    bswap16s(&ehdr->e_type);            /* Object file type */
    bswap16s(&ehdr->e_machine);         /* Architecture */
    bswap32s(&ehdr->e_version);         /* Object file version */
    bswaptls(&ehdr->e_entry);           /* Entry point virtual address */
    bswaptls(&ehdr->e_phoff);           /* Program header table file offset */
    bswaptls(&ehdr->e_shoff);           /* Section header table file offset */
    bswap32s(&ehdr->e_flags);           /* Processor-specific flags */
    bswap16s(&ehdr->e_ehsize);          /* ELF header size in bytes */
    bswap16s(&ehdr->e_phentsize);       /* Program header table entry size */
    bswap16s(&ehdr->e_phnum);           /* Program header table entry count */
    bswap16s(&ehdr->e_shentsize);       /* Section header table entry size */
    bswap16s(&ehdr->e_shnum);           /* Section header table entry count */
    bswap16s(&ehdr->e_shstrndx);        /* Section header string table index */
}

static void bswap_phdr(struct elf_phdr *phdr, int phnum)
{
    int i;
    for (i = 0; i < phnum; ++i, ++phdr) {
        bswap32s(&phdr->p_type);        /* Segment type */
        bswap32s(&phdr->p_flags);       /* Segment flags */
        bswaptls(&phdr->p_offset);      /* Segment file offset */
        bswaptls(&phdr->p_vaddr);       /* Segment virtual address */
        bswaptls(&phdr->p_paddr);       /* Segment physical address */
        bswaptls(&phdr->p_filesz);      /* Segment size in file */
        bswaptls(&phdr->p_memsz);       /* Segment size in memory */
        bswaptls(&phdr->p_align);       /* Segment alignment */
    }
}

static void bswap_shdr(struct elf_shdr *shdr, int shnum)
{
    int i;
    for (i = 0; i < shnum; ++i, ++shdr) {
        bswap32s(&shdr->sh_name);
        bswap32s(&shdr->sh_type);
        bswaptls(&shdr->sh_flags);
        bswaptls(&shdr->sh_addr);
        bswaptls(&shdr->sh_offset);
        bswaptls(&shdr->sh_size);
        bswap32s(&shdr->sh_link);
        bswap32s(&shdr->sh_info);
        bswaptls(&shdr->sh_addralign);
        bswaptls(&shdr->sh_entsize);
    }
}

static void bswap_sym(struct elf_sym *sym)
{
    bswap32s(&sym->st_name);
    bswaptls(&sym->st_value);
    bswaptls(&sym->st_size);
    bswap16s(&sym->st_shndx);
}
#else
static inline void bswap_ehdr(struct elfhdr *ehdr) { }
static inline void bswap_phdr(struct elf_phdr *phdr, int phnum) { }
static inline void bswap_shdr(struct elf_shdr *shdr, int shnum) { }
static inline void bswap_sym(struct elf_sym *sym) { }
#endif

#ifdef USE_ELF_CORE_DUMP
static int elf_core_dump(int, const CPUArchState *);
#endif /* USE_ELF_CORE_DUMP */
static void load_symbols(struct elfhdr *hdr, int fd, abi_ulong load_bias);

/* Verify the portions of EHDR within E_IDENT for the target.
   This can be performed before bswapping the entire header.  */
static bool elf_check_ident(struct elfhdr *ehdr)
{
    return (ehdr->e_ident[EI_MAG0] == ELFMAG0
            && ehdr->e_ident[EI_MAG1] == ELFMAG1
            && ehdr->e_ident[EI_MAG2] == ELFMAG2
            && ehdr->e_ident[EI_MAG3] == ELFMAG3
            && ehdr->e_ident[EI_CLASS] == ELF_CLASS
            && ehdr->e_ident[EI_DATA] == ELF_DATA
            && ehdr->e_ident[EI_VERSION] == EV_CURRENT);
}

/* Verify the portions of EHDR outside of E_IDENT for the target.
   This has to wait until after bswapping the header.  */
static bool elf_check_ehdr(struct elfhdr *ehdr)
{
    return (elf_check_arch(ehdr->e_machine)
            && ehdr->e_ehsize == sizeof(struct elfhdr)
            && ehdr->e_phentsize == sizeof(struct elf_phdr)
            && ehdr->e_shentsize == sizeof(struct elf_shdr)
            && (ehdr->e_type == ET_EXEC || ehdr->e_type == ET_DYN));
}

/*
 * 'copy_elf_strings()' copies argument/envelope strings from user
 * memory to free pages in kernel mem. These are in a format ready
 * to be put directly into the top of new user memory.
 *
 */
static abi_ulong copy_elf_strings(int argc,char ** argv, void **page,
                                  abi_ulong p)
{
    char *tmp, *tmp1, *pag = NULL;
    int len, offset = 0;

    if (!p) {
        return 0;       /* bullet-proofing */
    }
    while (argc-- > 0) {
        tmp = argv[argc];
        if (!tmp) {
            fprintf(stderr, "VFS: argc is wrong");
            exit(-1);
        }
        tmp1 = tmp;
        while (*tmp++);
        len = tmp - tmp1;
        if (p < len) {  /* this shouldn't happen - 128kB */
            return 0;
        }
        while (len) {
            --p; --tmp; --len;
            if (--offset < 0) {
                offset = p % TARGET_PAGE_SIZE;
                pag = (char *)page[p/TARGET_PAGE_SIZE];
                if (!pag) {
                    pag = g_try_malloc0(TARGET_PAGE_SIZE);
                    page[p/TARGET_PAGE_SIZE] = pag;
                    if (!pag)
                        return 0;
                }
            }
            if (len == 0 || offset == 0) {
                *(pag + offset) = *tmp;
            }
            else {
                int bytes_to_copy = (len > offset) ? offset : len;
                tmp -= bytes_to_copy;
                p -= bytes_to_copy;
                offset -= bytes_to_copy;
                len -= bytes_to_copy;
                memcpy_fromfs(pag + offset, tmp, bytes_to_copy + 1);
            }
        }
    }
    return p;
}

static abi_ulong setup_arg_pages(abi_ulong p, struct linux_binprm *bprm,
                                 struct image_info *info)
{
    abi_ulong stack_base, size, error, guard;
    int i;

    /* Create enough stack to hold everything.  If we don't use
       it for args, we'll use it for something else.  */
    size = guest_stack_size;
    if (size < MAX_ARG_PAGES*TARGET_PAGE_SIZE) {
        size = MAX_ARG_PAGES*TARGET_PAGE_SIZE;
    }
    guard = TARGET_PAGE_SIZE;
    if (guard < qemu_real_host_page_size) {
        guard = qemu_real_host_page_size;
    }

    error = target_mmap(0, size + guard, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (error == -1) {
        perror("mmap stack");
        exit(-1);
    }

    /* We reserve one extra page at the top of the stack as guard.  */
    target_mprotect(error, guard, PROT_NONE);

    info->stack_limit = error + guard;
    stack_base = info->stack_limit + size - MAX_ARG_PAGES*TARGET_PAGE_SIZE;
    p += stack_base;

    for (i = 0 ; i < MAX_ARG_PAGES ; i++) {
        if (bprm->page[i]) {
            info->rss++;
            /* FIXME - check return value of memcpy_to_target() for failure */
            memcpy_to_target(stack_base, bprm->page[i], TARGET_PAGE_SIZE);
            g_free(bprm->page[i]);
        }
        stack_base += TARGET_PAGE_SIZE;
    }
    return p;
}

/* Map and zero the bss.  We need to explicitly zero any fractional pages
   after the data section (i.e. bss).  */
static void zero_bss(abi_ulong elf_bss, abi_ulong last_bss, int prot)
{
    uintptr_t host_start, host_map_start, host_end;

    last_bss = TARGET_PAGE_ALIGN(last_bss);

    /* ??? There is confusion between qemu_real_host_page_size and
       qemu_host_page_size here and elsewhere in target_mmap, which
       may lead to the end of the data section mapping from the file
       not being mapped.  At least there was an explicit test and
       comment for that here, suggesting that "the file size must
       be known".  The comment probably pre-dates the introduction
       of the fstat system call in target_mmap which does in fact
       find out the size.  What isn't clear is if the workaround
       here is still actually needed.  For now, continue with it,
       but merge it with the "normal" mmap that would allocate the bss.  */

    host_start = (uintptr_t) g2h(elf_bss);
    host_end = (uintptr_t) g2h(last_bss);
    host_map_start = (host_start + qemu_real_host_page_size - 1);
    host_map_start &= -qemu_real_host_page_size;

    if (host_map_start < host_end) {
        void *p = mmap((void *)host_map_start, host_end - host_map_start,
                       prot, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            perror("cannot mmap brk");
            exit(-1);
        }
#ifdef CONFIG_USER_KVM
//For user mode, we should update physical memory in s2e after real mmap() is called succssfully.
	ram_memory_change(h2g(p), host_end - host_map_start, prot);
#endif
        /* Since we didn't use target_mmap, make sure to record
           the validity of the pages with qemu.  */
        page_set_flags(elf_bss & TARGET_PAGE_MASK, last_bss, prot|PAGE_VALID);
    }

    if (host_start < host_map_start) {
        memset((void *)host_start, 0, host_map_start - host_start);
    }
}

#ifdef CONFIG_USE_FDPIC
static abi_ulong loader_build_fdpic_loadmap(struct image_info *info, abi_ulong sp)
{
    uint16_t n;
    struct elf32_fdpic_loadseg *loadsegs = info->loadsegs;

    /* elf32_fdpic_loadseg */
    n = info->nsegs;
    while (n--) {
        sp -= 12;
        put_user_u32(loadsegs[n].addr, sp+0);
        put_user_u32(loadsegs[n].p_vaddr, sp+4);
        put_user_u32(loadsegs[n].p_memsz, sp+8);
    }

    /* elf32_fdpic_loadmap */
    sp -= 4;
    put_user_u16(0, sp+0); /* version */
    put_user_u16(info->nsegs, sp+2); /* nsegs */

    info->personality = PER_LINUX_FDPIC;
    info->loadmap_addr = sp;

    return sp;
}
#endif
unsigned long qemu_getauxval(unsigned long key);
static abi_ulong create_elf_tables(abi_ulong p, int argc, int envc,
                                   struct elfhdr *exec,
                                   struct image_info *info,
                                   struct image_info *interp_info)
{
    abi_ulong sp;
    abi_ulong sp_auxv;
    int size;
    int i;
    abi_ulong u_rand_bytes;
    uint8_t k_rand_bytes[16];
    abi_ulong u_platform;
    const char *k_platform;
    const int n = sizeof(elf_addr_t);

    sp = p;

#ifdef CONFIG_USE_FDPIC
    /* Needs to be before we load the env/argc/... */
    if (elf_is_fdpic(exec)) {
        /* Need 4 byte alignment for these structs */
        sp &= ~3;
        sp = loader_build_fdpic_loadmap(info, sp);
        info->other_info = interp_info;
        if (interp_info) {
            interp_info->other_info = info;
            sp = loader_build_fdpic_loadmap(interp_info, sp);
        }
    }
#endif

    u_platform = 0;
    k_platform = ELF_PLATFORM;
    if (k_platform) {
        size_t len = strlen(k_platform) + 1;
        sp -= (len + n - 1) & ~(n - 1);
        u_platform = sp;
        /* FIXME - check return value of memcpy_to_target() for failure */
        memcpy_to_target(sp, k_platform, len);
    }

    /*
     * Generate 16 random bytes for userspace PRNG seeding (not
     * cryptically secure but it's not the aim of QEMU).
     */
    srand((unsigned int) time(NULL));
    for (i = 0; i < 16; i++) {
        k_rand_bytes[i] = rand();
    }
    sp -= 16;
    u_rand_bytes = sp;
    /* FIXME - check return value of memcpy_to_target() for failure */
    memcpy_to_target(sp, k_rand_bytes, 16);

    /*
     * Force 16 byte _final_ alignment here for generality.
     */
    sp = sp &~ (abi_ulong)15;
    size = (DLINFO_ITEMS + 1) * 2;
    if (k_platform)
        size += 2;
#ifdef DLINFO_ARCH_ITEMS
    size += DLINFO_ARCH_ITEMS * 2;
#endif
    size += envc + argc + 2;
    size += 1;  /* argc itself */
    size *= n;
    if (size & 15)
        sp -= 16 - (size & 15);

    /* This is correct because Linux defines
     * elf_addr_t as Elf32_Off / Elf64_Off
     */
#define NEW_AUX_ENT(id, val) do {               \
        sp -= n; put_user_ual(val, sp);         \
        sp -= n; put_user_ual(id, sp);          \
    } while(0)

    sp_auxv = sp;
    NEW_AUX_ENT (AT_NULL, 0);

    /* There must be exactly DLINFO_ITEMS entries here.  */
    NEW_AUX_ENT(AT_PHDR, (abi_ulong)(info->load_addr + exec->e_phoff));
    NEW_AUX_ENT(AT_PHENT, (abi_ulong)(sizeof (struct elf_phdr)));
    NEW_AUX_ENT(AT_PHNUM, (abi_ulong)(exec->e_phnum));
    NEW_AUX_ENT(AT_PAGESZ, (abi_ulong)(TARGET_PAGE_SIZE));
    NEW_AUX_ENT(AT_BASE, (abi_ulong)(interp_info ? interp_info->load_addr : 0));
    NEW_AUX_ENT(AT_FLAGS, (abi_ulong)0);
    NEW_AUX_ENT(AT_ENTRY, info->entry);
    NEW_AUX_ENT(AT_UID, (abi_ulong) getuid());
    NEW_AUX_ENT(AT_EUID, (abi_ulong) geteuid());
    NEW_AUX_ENT(AT_GID, (abi_ulong) getgid());
    NEW_AUX_ENT(AT_EGID, (abi_ulong) getegid());
    NEW_AUX_ENT(AT_HWCAP, (abi_ulong) ELF_HWCAP);
    NEW_AUX_ENT(AT_CLKTCK, (abi_ulong) sysconf(_SC_CLK_TCK));
    NEW_AUX_ENT(AT_RANDOM, (abi_ulong) u_rand_bytes);
    //NEW_AUX_ENT(AT_SECURE, (abi_ulong)0);
	abi_ulong test = (abi_ulong)qemu_getauxval(AT_SECURE);
    NEW_AUX_ENT(AT_SECURE, test);

    if (k_platform)
        NEW_AUX_ENT(AT_PLATFORM, u_platform);
#ifdef ARCH_DLINFO
    /*
     * ARCH_DLINFO must come last so platform specific code can enforce
     * special alignment requirements on the AUXV if necessary (eg. PPC).
     */
    ARCH_DLINFO;
#endif
#undef NEW_AUX_ENT

    info->saved_auxv = sp;
    info->auxv_len = sp_auxv - sp;

    sp = loader_build_argptr(envc, argc, sp, p, 0);
    return sp;
}

#ifndef TARGET_HAS_VALIDATE_GUEST_SPACE
/* If the guest doesn't have a validation function just agree */
static int validate_guest_space(unsigned long guest_base,
                                unsigned long guest_size)
{
    return 1;
}
#endif

unsigned long init_guest_space(unsigned long host_start,
                               unsigned long host_size,
                               unsigned long guest_start,
                               bool fixed)
{
    unsigned long current_start, real_start;
    int flags;

    assert(host_start || host_size);

    /* If just a starting address is given, then just verify that
     * address.  */
    if (host_start && !host_size) {
        if (validate_guest_space(host_start, host_size) == 1) {
            return host_start;
        } else {
            return (unsigned long)-1;
        }
    }

    /* Setup the initial flags and start address.  */
    current_start = host_start & qemu_host_page_mask;
    flags = MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE;
    if (fixed) {
        flags |= MAP_FIXED;
    }

    /* Otherwise, a non-zero size region of memory needs to be mapped
     * and validated.  */
    while (1) {
        unsigned long real_size = host_size;

        /* Do not use mmap_find_vma here because that is limited to the
         * guest address space.  We are going to make the
         * guest address space fit whatever we're given.
         */
        real_start = (unsigned long)
            mmap((void *)current_start, host_size, PROT_NONE, flags, -1, 0);
        if (real_start == (unsigned long)-1) {
            return (unsigned long)-1;
        }

        /* Ensure the address is properly aligned.  */
        if (real_start & ~qemu_host_page_mask) {
            munmap((void *)real_start, host_size);
            real_size = host_size + qemu_host_page_size;
            real_start = (unsigned long)
                mmap((void *)real_start, real_size, PROT_NONE, flags, -1, 0);
            if (real_start == (unsigned long)-1) {
                return (unsigned long)-1;
            }
            real_start = HOST_PAGE_ALIGN(real_start);
        }

        /* Check to see if the address is valid.  */
        if (!host_start || real_start == current_start) {
            int valid = validate_guest_space(real_start - guest_start,
                                             real_size);
            if (valid == 1) {
                break;
            } else if (valid == -1) {
                return (unsigned long)-1;
            }
            /* valid == 0, so try again. */
        }

        /* That address didn't work.  Unmap and try a different one.
         * The address the host picked because is typically right at
         * the top of the host address space and leaves the guest with
         * no usable address space.  Resort to a linear search.  We
         * already compensated for mmap_min_addr, so this should not
         * happen often.  Probably means we got unlucky and host
         * address space randomization put a shared library somewhere
         * inconvenient.
         */
        munmap((void *)real_start, host_size);
        current_start += qemu_host_page_size;
        if (host_start == current_start) {
            /* Theoretically possible if host doesn't have any suitably
             * aligned areas.  Normally the first mmap will fail.
             */
            return (unsigned long)-1;
        }
    }

    qemu_log("Reserved 0x%lx bytes of guest address space\n", host_size);

    return real_start;
}

static void probe_guest_base(const char *image_name,
                             abi_ulong loaddr, abi_ulong hiaddr)
{
    /* Probe for a suitable guest base address, if the user has not set
     * it explicitly, and set guest_base appropriately.
     * In case of error we will print a suitable message and exit.
     */
#if defined(CONFIG_USE_GUEST_BASE)
    const char *errmsg;
    if (!have_guest_base && !reserved_va) {
        unsigned long host_start, real_start, host_size;

        /* Round addresses to page boundaries.  */
        loaddr &= qemu_host_page_mask;
        hiaddr = HOST_PAGE_ALIGN(hiaddr);

        if (loaddr < mmap_min_addr) {
            host_start = HOST_PAGE_ALIGN(mmap_min_addr);
        } else {
            host_start = loaddr;
            if (host_start != loaddr) {
                errmsg = "Address overflow loading ELF binary";
                goto exit_errmsg;
            }
        }
        host_size = hiaddr - loaddr;

        /* Setup the initial guest memory space with ranges gleaned from
         * the ELF image that is being loaded.
         */
        real_start = init_guest_space(host_start, host_size, loaddr, false);
        if (real_start == (unsigned long)-1) {
            errmsg = "Unable to find space for application";
            goto exit_errmsg;
        }
        guest_base = real_start - loaddr;

        qemu_log("Relocating guest address space from 0x"
                 TARGET_ABI_FMT_lx " to 0x%lx\n",
                 loaddr, real_start);
    }
    return;

exit_errmsg:
    fprintf(stderr, "%s: %s\n", image_name, errmsg);
    exit(-1);
#endif
}


/* Load an ELF image into the address space.

   IMAGE_NAME is the filename of the image, to use in error messages.
   IMAGE_FD is the open file descriptor for the image.

   BPRM_BUF is a copy of the beginning of the file; this of course
   contains the elf file header at offset 0.  It is assumed that this
   buffer is sufficiently aligned to present no problems to the host
   in accessing data at aligned offsets within the buffer.

   On return: INFO values will be filled in, as necessary or available.  */

static void load_elf_image(const char *image_name, int image_fd,
                           struct image_info *info, char **pinterp_name,
                           char bprm_buf[BPRM_BUF_SIZE])
{
    struct elfhdr *ehdr = (struct elfhdr *)bprm_buf;
    struct elf_phdr *phdr;
    abi_ulong load_addr, load_bias, loaddr, hiaddr, error;
    int i, retval;
    const char *errmsg;

    /* First of all, some simple consistency checks */
    errmsg = "Invalid ELF image for this architecture";
    if (!elf_check_ident(ehdr)) {
        goto exit_errmsg;
    }
    bswap_ehdr(ehdr);
    if (!elf_check_ehdr(ehdr)) {
        goto exit_errmsg;
    }

    i = ehdr->e_phnum * sizeof(struct elf_phdr);
    if (ehdr->e_phoff + i <= BPRM_BUF_SIZE) {
        phdr = (struct elf_phdr *)(bprm_buf + ehdr->e_phoff);
    } else {
        phdr = (struct elf_phdr *) alloca(i);
        retval = pread(image_fd, phdr, i, ehdr->e_phoff);
        if (retval != i) {
            goto exit_read;
        }
    }
    bswap_phdr(phdr, ehdr->e_phnum);

#ifdef CONFIG_USE_FDPIC
    info->nsegs = 0;
    info->pt_dynamic_addr = 0;
#endif

    /* Find the maximum size of the image and allocate an appropriate
       amount of memory to handle that.  */
    loaddr = -1, hiaddr = 0;
    for (i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type == PT_LOAD) {
            abi_ulong a = phdr[i].p_vaddr;
            if (a < loaddr) {
                loaddr = a;
            }
            a += phdr[i].p_memsz;
            if (a > hiaddr) {
                hiaddr = a;
            }
#ifdef CONFIG_USE_FDPIC
            ++info->nsegs;
#endif
        }
    }

    load_addr = loaddr;
    if (ehdr->e_type == ET_DYN) {
        /* The image indicates that it can be loaded anywhere.  Find a
           location that can hold the memory space required.  If the
           image is pre-linked, LOADDR will be non-zero.  Since we do
           not supply MAP_FIXED here we'll use that address if and
           only if it remains available.  */
        load_addr = target_mmap(loaddr, hiaddr - loaddr, PROT_NONE,
                                MAP_PRIVATE | MAP_ANON | MAP_NORESERVE,
                                -1, 0);
        if (load_addr == -1) {
            goto exit_perror;
        }
    } else if (pinterp_name != NULL) {
        /* This is the main executable.  Make sure that the low
           address does not conflict with MMAP_MIN_ADDR or the
           QEMU application itself.  */
        probe_guest_base(image_name, loaddr, hiaddr);
    }
    load_bias = load_addr - loaddr;

#ifdef CONFIG_USE_FDPIC
    {
        struct elf32_fdpic_loadseg *loadsegs = info->loadsegs =
            g_malloc(sizeof(*loadsegs) * info->nsegs);

        for (i = 0; i < ehdr->e_phnum; ++i) {
            switch (phdr[i].p_type) {
            case PT_DYNAMIC:
                info->pt_dynamic_addr = phdr[i].p_vaddr + load_bias;
                break;
            case PT_LOAD:
                loadsegs->addr = phdr[i].p_vaddr + load_bias;
                loadsegs->p_vaddr = phdr[i].p_vaddr;
                loadsegs->p_memsz = phdr[i].p_memsz;
                ++loadsegs;
                break;
            }
        }
    }
#endif

    info->load_bias = load_bias;
    info->load_addr = load_addr;
    info->entry = ehdr->e_entry + load_bias;
    info->start_code = -1;
    info->end_code = 0;
    info->start_data = -1;
    info->end_data = 0;
    info->brk = 0;
    info->elf_flags = ehdr->e_flags;

    for (i = 0; i < ehdr->e_phnum; i++) {
        struct elf_phdr *eppnt = phdr + i;
        if (eppnt->p_type == PT_LOAD) {
            abi_ulong vaddr, vaddr_po, vaddr_ps, vaddr_ef, vaddr_em;
            int elf_prot = 0;

            if (eppnt->p_flags & PF_R) elf_prot =  PROT_READ;
            if (eppnt->p_flags & PF_W) elf_prot |= PROT_WRITE;
            if (eppnt->p_flags & PF_X) elf_prot |= PROT_EXEC;

            vaddr = load_bias + eppnt->p_vaddr;
            vaddr_po = TARGET_ELF_PAGEOFFSET(vaddr);
            vaddr_ps = TARGET_ELF_PAGESTART(vaddr);

            error = target_mmap(vaddr_ps, eppnt->p_filesz + vaddr_po,
                                elf_prot, MAP_PRIVATE | MAP_FIXED,
                                image_fd, eppnt->p_offset - vaddr_po);
            if (error == -1) {
                goto exit_perror;
            }

            vaddr_ef = vaddr + eppnt->p_filesz;
            vaddr_em = vaddr + eppnt->p_memsz;

            /* If the load segment requests extra zeros (e.g. bss), map it.  */
            if (vaddr_ef < vaddr_em) {
                zero_bss(vaddr_ef, vaddr_em, elf_prot);
            }

            /* Find the full program boundaries.  */
            if (elf_prot & PROT_EXEC) {
                if (vaddr < info->start_code) {
                    info->start_code = vaddr;
                }
                if (vaddr_ef > info->end_code) {
                    info->end_code = vaddr_ef;
                }
            }
            if (elf_prot & PROT_WRITE) {
                if (vaddr < info->start_data) {
                    info->start_data = vaddr;
                }
                if (vaddr_ef > info->end_data) {
                    info->end_data = vaddr_ef;
                }
                if (vaddr_em > info->brk) {
                    info->brk = vaddr_em;
                }
            }
        } else if (eppnt->p_type == PT_INTERP && pinterp_name) {
            char *interp_name;

            if (*pinterp_name) {
                errmsg = "Multiple PT_INTERP entries";
                goto exit_errmsg;
            }
            interp_name = malloc(eppnt->p_filesz);
            if (!interp_name) {
                goto exit_perror;
            }

            if (eppnt->p_offset + eppnt->p_filesz <= BPRM_BUF_SIZE) {
                memcpy(interp_name, bprm_buf + eppnt->p_offset,
                       eppnt->p_filesz);
            } else {
                retval = pread(image_fd, interp_name, eppnt->p_filesz,
                               eppnt->p_offset);
                if (retval != eppnt->p_filesz) {
                    goto exit_perror;
                }
            }
            if (interp_name[eppnt->p_filesz - 1] != 0) {
                errmsg = "Invalid PT_INTERP entry";
                goto exit_errmsg;
            }
            *pinterp_name = interp_name;
        }
    }

    if (info->end_data == 0) {
        info->start_data = info->end_code;
        info->end_data = info->end_code;
        info->brk = info->end_code;
    }

    if (qemu_log_enabled()) {
        load_symbols(ehdr, image_fd, load_bias);
    }

    close(image_fd);
    return;

 exit_read:
    if (retval >= 0) {
        errmsg = "Incomplete read of file header";
        goto exit_errmsg;
    }
 exit_perror:
    errmsg = strerror(errno);
 exit_errmsg:
    fprintf(stderr, "%s: %s\n", image_name, errmsg);
    exit(-1);
}

static void load_elf_interp(const char *filename, struct image_info *info,
                            char bprm_buf[BPRM_BUF_SIZE])
{
    int fd, retval;

    fd = open(path(filename), O_RDONLY);
    if (fd < 0) {
        goto exit_perror;
    }

    retval = read(fd, bprm_buf, BPRM_BUF_SIZE);
    if (retval < 0) {
        goto exit_perror;
    }
    if (retval < BPRM_BUF_SIZE) {
        memset(bprm_buf + retval, 0, BPRM_BUF_SIZE - retval);
    }

    load_elf_image(filename, fd, info, NULL, bprm_buf);
    return;

 exit_perror:
    fprintf(stderr, "%s: %s\n", filename, strerror(errno));
    exit(-1);
}

static int symfind(const void *s0, const void *s1)
{
    target_ulong addr = *(target_ulong *)s0;
    struct elf_sym *sym = (struct elf_sym *)s1;
    int result = 0;
    if (addr < sym->st_value) {
        result = -1;
    } else if (addr >= sym->st_value + sym->st_size) {
        result = 1;
    }
    return result;
}

static const char *lookup_symbolxx(struct syminfo *s, target_ulong orig_addr)
{
#if ELF_CLASS == ELFCLASS32
    struct elf_sym *syms = s->disas_symtab.elf32;
#else
    struct elf_sym *syms = s->disas_symtab.elf64;
#endif

    // binary search
    struct elf_sym *sym;

    sym = bsearch(&orig_addr, syms, s->disas_num_syms, sizeof(*syms), symfind);
    if (sym != NULL) {
        return s->disas_strtab + sym->st_name;
    }

    return "";
}

/* FIXME: This should use elf_ops.h  */
static int symcmp(const void *s0, const void *s1)
{
    struct elf_sym *sym0 = (struct elf_sym *)s0;
    struct elf_sym *sym1 = (struct elf_sym *)s1;
    return (sym0->st_value < sym1->st_value)
        ? -1
        : ((sym0->st_value > sym1->st_value) ? 1 : 0);
}

/* Best attempt to load symbols from this ELF object. */
static void load_symbols(struct elfhdr *hdr, int fd, abi_ulong load_bias)
{
    int i, shnum, nsyms, sym_idx = 0, str_idx = 0;
    struct elf_shdr *shdr;
    char *strings = NULL;
    struct syminfo *s = NULL;
    struct elf_sym *new_syms, *syms = NULL;

    shnum = hdr->e_shnum;
    i = shnum * sizeof(struct elf_shdr);
    shdr = (struct elf_shdr *)alloca(i);
    if (pread(fd, shdr, i, hdr->e_shoff) != i) {
        return;
    }

    bswap_shdr(shdr, shnum);
    for (i = 0; i < shnum; ++i) {
        if (shdr[i].sh_type == SHT_SYMTAB) {
            sym_idx = i;
            str_idx = shdr[i].sh_link;
            goto found;
        }
    }

    /* There will be no symbol table if the file was stripped.  */
    return;

 found:
    /* Now know where the strtab and symtab are.  Snarf them.  */
    s = malloc(sizeof(*s));
    if (!s) {
        goto give_up;
    }

    i = shdr[str_idx].sh_size;
    s->disas_strtab = strings = malloc(i);
    if (!strings || pread(fd, strings, i, shdr[str_idx].sh_offset) != i) {
        goto give_up;
    }

    i = shdr[sym_idx].sh_size;
    syms = malloc(i);
    if (!syms || pread(fd, syms, i, shdr[sym_idx].sh_offset) != i) {
        goto give_up;
    }

    nsyms = i / sizeof(struct elf_sym);
    for (i = 0; i < nsyms; ) {
        bswap_sym(syms + i);
        /* Throw away entries which we do not need.  */
        if (syms[i].st_shndx == SHN_UNDEF
            || syms[i].st_shndx >= SHN_LORESERVE
            || ELF_ST_TYPE(syms[i].st_info) != STT_FUNC) {
            if (i < --nsyms) {
                syms[i] = syms[nsyms];
            }
        } else {
#if defined(TARGET_ARM) || defined (TARGET_MIPS)
            /* The bottom address bit marks a Thumb or MIPS16 symbol.  */
            syms[i].st_value &= ~(target_ulong)1;
#endif
            syms[i].st_value += load_bias;
            i++;
        }
    }

    /* No "useful" symbol.  */
    if (nsyms == 0) {
        goto give_up;
    }

    /* Attempt to free the storage associated with the local symbols
       that we threw away.  Whether or not this has any effect on the
       memory allocation depends on the malloc implementation and how
       many symbols we managed to discard.  */
    new_syms = realloc(syms, nsyms * sizeof(*syms));
    if (new_syms == NULL) {
        goto give_up;
    }
    syms = new_syms;

    qsort(syms, nsyms, sizeof(*syms), symcmp);

    s->disas_num_syms = nsyms;
#if ELF_CLASS == ELFCLASS32
    s->disas_symtab.elf32 = syms;
#else
    s->disas_symtab.elf64 = syms;
#endif
    s->lookup_symbol = lookup_symbolxx;
    s->next = syminfos;
    syminfos = s;

    return;

give_up:
    free(s);
    free(strings);
    free(syms);
}

int load_elf_binary(struct linux_binprm * bprm, struct target_pt_regs * regs,
                    struct image_info * info)
{
    struct image_info *interp_info = NULL;
    struct elfhdr elf_ex;
    char *elf_interpreter = NULL;

    info->start_mmap = (abi_ulong)ELF_START_MMAP;
    info->mmap = 0;
    info->rss = 0;

    load_elf_image(bprm->filename, bprm->fd, info,
                   &elf_interpreter, bprm->buf);

    /* ??? We need a copy of the elf header for passing to create_elf_tables.
       If we do nothing, we'll have overwritten this when we re-use bprm->buf
       when we load the interpreter.  */
    elf_ex = *(struct elfhdr *)bprm->buf;

    bprm->p = copy_elf_strings(1, &bprm->filename, bprm->page, bprm->p);
    bprm->p = copy_elf_strings(bprm->envc,bprm->envp,bprm->page,bprm->p);
    bprm->p = copy_elf_strings(bprm->argc,bprm->argv,bprm->page,bprm->p);
    if (!bprm->p) {
        fprintf(stderr, "%s: %s\n", bprm->filename, strerror(E2BIG));
        exit(-1);
    }

    /* Do this so that we can load the interpreter, if need be.  We will
       change some of these later */
    bprm->p = setup_arg_pages(bprm->p, bprm, info);

    if (elf_interpreter) {
        interp_info = malloc(sizeof(struct image_info));
        load_elf_interp(elf_interpreter, interp_info, bprm->buf);
#ifdef CONFIG_USER_KVM
        info->interp_info = interp_info;
#endif

        /* If the program interpreter is one of these two, then assume
           an iBCS2 image.  Otherwise assume a native linux image.  */

        if (strcmp(elf_interpreter, "/usr/lib/libc.so.1") == 0
            || strcmp(elf_interpreter, "/usr/lib/ld.so.1") == 0) {
            info->personality = PER_SVR4;

            /* Why this, you ask???  Well SVr4 maps page 0 as read-only,
               and some applications "depend" upon this behavior.  Since
               we do not have the power to recompile these, we emulate
               the SVr4 behavior.  Sigh.  */
            target_mmap(0, qemu_host_page_size, PROT_READ | PROT_EXEC,
                        MAP_FIXED | MAP_PRIVATE, -1, 0);
        }
    }

    bprm->p = create_elf_tables(bprm->p, bprm->argc, bprm->envc, &elf_ex,
                                info, (elf_interpreter ? interp_info : NULL));
    info->start_stack = bprm->p;

    /* If we have an interpreter, set that as the program's entry point.
       Copy the load_bias as well, to help PPC64 interpret the entry
       point as a function descriptor.  Do this after creating elf tables
       so that we copy the original program entry point into the AUXV.  */
    if (elf_interpreter) {
        info->load_bias = interp_info->load_bias;
        info->entry = interp_info->entry;
        free(elf_interpreter);
    }

#ifdef USE_ELF_CORE_DUMP
    bprm->core_dump = &elf_core_dump;
#endif

    return 0;
}

#ifdef USE_ELF_CORE_DUMP
/*
 * Definitions to generate Intel SVR4-like core files.
 * These mostly have the same names as the SVR4 types with "target_elf_"
 * tacked on the front to prevent clashes with linux definitions,
 * and the typedef forms have been avoided.  This is mostly like
 * the SVR4 structure, but more Linuxy, with things that Linux does
 * not support and which gdb doesn't really use excluded.
 *
 * Fields we don't dump (their contents is zero) in linux-user qemu
 * are marked with XXX.
 *
 * Core dump code is copied from linux kernel (fs/binfmt_elf.c).
 *
 * Porting ELF coredump for target is (quite) simple process.  First you
 * define USE_ELF_CORE_DUMP in target ELF code (where init_thread() for
 * the target resides):
 *
 * #define USE_ELF_CORE_DUMP
 *
 * Next you define type of register set used for dumping.  ELF specification
 * says that it needs to be array of elf_greg_t that has size of ELF_NREG.
 *
 * typedef <target_regtype> target_elf_greg_t;
 * #define ELF_NREG <number of registers>
 * typedef taret_elf_greg_t target_elf_gregset_t[ELF_NREG];
 *
 * Last step is to implement target specific function that copies registers
 * from given cpu into just specified register set.  Prototype is:
 *
 * static void elf_core_copy_regs(taret_elf_gregset_t *regs,
 *                                const CPUArchState *env);
 *
 * Parameters:
 *     regs - copy register values into here (allocated and zeroed by caller)
 *     env - copy registers from here
 *
 * Example for ARM target is provided in this file.
 */

/* An ELF note in memory */
struct memelfnote {
    const char *name;
    size_t     namesz;
    size_t     namesz_rounded;
    int        type;
    size_t     datasz;
    size_t     datasz_rounded;
    void       *data;
    size_t     notesz;
};

struct target_elf_siginfo {
    target_int  si_signo; /* signal number */
    target_int  si_code;  /* extra code */
    target_int  si_errno; /* errno */
};

struct target_elf_prstatus {
    struct target_elf_siginfo pr_info;      /* Info associated with signal */
    target_short       pr_cursig;    /* Current signal */
    target_ulong       pr_sigpend;   /* XXX */
    target_ulong       pr_sighold;   /* XXX */
    target_pid_t       pr_pid;
    target_pid_t       pr_ppid;
    target_pid_t       pr_pgrp;
    target_pid_t       pr_sid;
    struct target_timeval pr_utime;  /* XXX User time */
    struct target_timeval pr_stime;  /* XXX System time */
    struct target_timeval pr_cutime; /* XXX Cumulative user time */
    struct target_timeval pr_cstime; /* XXX Cumulative system time */
    target_elf_gregset_t      pr_reg;       /* GP registers */
    target_int         pr_fpvalid;   /* XXX */
};

#define ELF_PRARGSZ     (80) /* Number of chars for args */

struct target_elf_prpsinfo {
    char         pr_state;       /* numeric process state */
    char         pr_sname;       /* char for pr_state */
    char         pr_zomb;        /* zombie */
    char         pr_nice;        /* nice val */
    target_ulong pr_flag;        /* flags */
    target_uid_t pr_uid;
    target_gid_t pr_gid;
    target_pid_t pr_pid, pr_ppid, pr_pgrp, pr_sid;
    /* Lots missing */
    char    pr_fname[16];           /* filename of executable */
    char    pr_psargs[ELF_PRARGSZ]; /* initial part of arg list */
};

/* Here is the structure in which status of each thread is captured. */
struct elf_thread_status {
    QTAILQ_ENTRY(elf_thread_status)  ets_link;
    struct target_elf_prstatus prstatus;   /* NT_PRSTATUS */
#if 0
    elf_fpregset_t fpu;             /* NT_PRFPREG */
    struct task_struct *thread;
    elf_fpxregset_t xfpu;           /* ELF_CORE_XFPREG_TYPE */
#endif
    struct memelfnote notes[1];
    int num_notes;
};

struct elf_note_info {
    struct memelfnote   *notes;
    struct target_elf_prstatus *prstatus;  /* NT_PRSTATUS */
    struct target_elf_prpsinfo *psinfo;    /* NT_PRPSINFO */

    QTAILQ_HEAD(thread_list_head, elf_thread_status) thread_list;
#if 0
    /*
     * Current version of ELF coredump doesn't support
     * dumping fp regs etc.
     */
    elf_fpregset_t *fpu;
    elf_fpxregset_t *xfpu;
    int thread_status_size;
#endif
    int notes_size;
    int numnote;
};

struct vm_area_struct {
    abi_ulong   vma_start;  /* start vaddr of memory region */
    abi_ulong   vma_end;    /* end vaddr of memory region */
    abi_ulong   vma_flags;  /* protection etc. flags for the region */
    QTAILQ_ENTRY(vm_area_struct) vma_link;
};

struct mm_struct {
    QTAILQ_HEAD(, vm_area_struct) mm_mmap;
    int mm_count;           /* number of mappings */
};

static struct mm_struct *vma_init(void);
static void vma_delete(struct mm_struct *);
static int vma_add_mapping(struct mm_struct *, abi_ulong,
                           abi_ulong, abi_ulong);
static int vma_get_mapping_count(const struct mm_struct *);
static struct vm_area_struct *vma_first(const struct mm_struct *);
static struct vm_area_struct *vma_next(struct vm_area_struct *);
static abi_ulong vma_dump_size(const struct vm_area_struct *);
static int vma_walker(void *priv, abi_ulong start, abi_ulong end,
                      unsigned long flags);

static void fill_elf_header(struct elfhdr *, int, uint16_t, uint32_t);
static void fill_note(struct memelfnote *, const char *, int,
                      unsigned int, void *);
static void fill_prstatus(struct target_elf_prstatus *, const TaskState *, int);
static int fill_psinfo(struct target_elf_prpsinfo *, const TaskState *);
static void fill_auxv_note(struct memelfnote *, const TaskState *);
static void fill_elf_note_phdr(struct elf_phdr *, int, off_t);
static size_t note_size(const struct memelfnote *);
static void free_note_info(struct elf_note_info *);
static int fill_note_info(struct elf_note_info *, long, const CPUArchState *);
static void fill_thread_info(struct elf_note_info *, const CPUArchState *);
static int core_dump_filename(const TaskState *, char *, size_t);

static int dump_write(int, const void *, size_t);
static int write_note(struct memelfnote *, int);
static int write_note_info(struct elf_note_info *, int);

#ifdef BSWAP_NEEDED
static void bswap_prstatus(struct target_elf_prstatus *prstatus)
{
    prstatus->pr_info.si_signo = tswapl(prstatus->pr_info.si_signo);
    prstatus->pr_info.si_code = tswapl(prstatus->pr_info.si_code);
    prstatus->pr_info.si_errno = tswapl(prstatus->pr_info.si_errno);
    prstatus->pr_cursig = tswap16(prstatus->pr_cursig);
    prstatus->pr_sigpend = tswapl(prstatus->pr_sigpend);
    prstatus->pr_sighold = tswapl(prstatus->pr_sighold);
    prstatus->pr_pid = tswap32(prstatus->pr_pid);
    prstatus->pr_ppid = tswap32(prstatus->pr_ppid);
    prstatus->pr_pgrp = tswap32(prstatus->pr_pgrp);
    prstatus->pr_sid = tswap32(prstatus->pr_sid);
    /* cpu times are not filled, so we skip them */
    /* regs should be in correct format already */
    prstatus->pr_fpvalid = tswap32(prstatus->pr_fpvalid);
}

static void bswap_psinfo(struct target_elf_prpsinfo *psinfo)
{
    psinfo->pr_flag = tswapl(psinfo->pr_flag);
    psinfo->pr_uid = tswap16(psinfo->pr_uid);
    psinfo->pr_gid = tswap16(psinfo->pr_gid);
    psinfo->pr_pid = tswap32(psinfo->pr_pid);
    psinfo->pr_ppid = tswap32(psinfo->pr_ppid);
    psinfo->pr_pgrp = tswap32(psinfo->pr_pgrp);
    psinfo->pr_sid = tswap32(psinfo->pr_sid);
}

static void bswap_note(struct elf_note *en)
{
    bswap32s(&en->n_namesz);
    bswap32s(&en->n_descsz);
    bswap32s(&en->n_type);
}
#else
static inline void bswap_prstatus(struct target_elf_prstatus *p) { }
static inline void bswap_psinfo(struct target_elf_prpsinfo *p) {}
static inline void bswap_note(struct elf_note *en) { }
#endif /* BSWAP_NEEDED */

/*
 * Minimal support for linux memory regions.  These are needed
 * when we are finding out what memory exactly belongs to
 * emulated process.  No locks needed here, as long as
 * thread that received the signal is stopped.
 */

static struct mm_struct *vma_init(void)
{
    struct mm_struct *mm;

    if ((mm = g_malloc(sizeof (*mm))) == NULL)
        return (NULL);

    mm->mm_count = 0;
    QTAILQ_INIT(&mm->mm_mmap);

    return (mm);
}

static void vma_delete(struct mm_struct *mm)
{
    struct vm_area_struct *vma;

    while ((vma = vma_first(mm)) != NULL) {
        QTAILQ_REMOVE(&mm->mm_mmap, vma, vma_link);
        g_free(vma);
    }
    g_free(mm);
}

static int vma_add_mapping(struct mm_struct *mm, abi_ulong start,
                           abi_ulong end, abi_ulong flags)
{
    struct vm_area_struct *vma;

    if ((vma = g_malloc0(sizeof (*vma))) == NULL)
        return (-1);

    vma->vma_start = start;
    vma->vma_end = end;
    vma->vma_flags = flags;

    QTAILQ_INSERT_TAIL(&mm->mm_mmap, vma, vma_link);
    mm->mm_count++;

    return (0);
}

static struct vm_area_struct *vma_first(const struct mm_struct *mm)
{
    return (QTAILQ_FIRST(&mm->mm_mmap));
}

static struct vm_area_struct *vma_next(struct vm_area_struct *vma)
{
    return (QTAILQ_NEXT(vma, vma_link));
}

static int vma_get_mapping_count(const struct mm_struct *mm)
{
    return (mm->mm_count);
}

/*
 * Calculate file (dump) size of given memory region.
 */
static abi_ulong vma_dump_size(const struct vm_area_struct *vma)
{
    /* if we cannot even read the first page, skip it */
    if (!access_ok(VERIFY_READ, vma->vma_start, TARGET_PAGE_SIZE))
        return (0);

    /*
     * Usually we don't dump executable pages as they contain
     * non-writable code that debugger can read directly from
     * target library etc.  However, thread stacks are marked
     * also executable so we read in first page of given region
     * and check whether it contains elf header.  If there is
     * no elf header, we dump it.
     */
    if (vma->vma_flags & PROT_EXEC) {
        char page[TARGET_PAGE_SIZE];

        copy_from_user(page, vma->vma_start, sizeof (page));
        if ((page[EI_MAG0] == ELFMAG0) &&
            (page[EI_MAG1] == ELFMAG1) &&
            (page[EI_MAG2] == ELFMAG2) &&
            (page[EI_MAG3] == ELFMAG3)) {
            /*
             * Mappings are possibly from ELF binary.  Don't dump
             * them.
             */
            return (0);
        }
    }

    return (vma->vma_end - vma->vma_start);
}

static int vma_walker(void *priv, abi_ulong start, abi_ulong end,
                      unsigned long flags)
{
    struct mm_struct *mm = (struct mm_struct *)priv;

    vma_add_mapping(mm, start, end, flags);
    return (0);
}

static void fill_note(struct memelfnote *note, const char *name, int type,
                      unsigned int sz, void *data)
{
    unsigned int namesz;

    namesz = strlen(name) + 1;
    note->name = name;
    note->namesz = namesz;
    note->namesz_rounded = roundup(namesz, sizeof (int32_t));
    note->type = type;
    note->datasz = sz;
    note->datasz_rounded = roundup(sz, sizeof (int32_t));

    note->data = data;

    /*
     * We calculate rounded up note size here as specified by
     * ELF document.
     */
    note->notesz = sizeof (struct elf_note) +
        note->namesz_rounded + note->datasz_rounded;
}

static void fill_elf_header(struct elfhdr *elf, int segs, uint16_t machine,
                            uint32_t flags)
{
    (void) memset(elf, 0, sizeof(*elf));

    (void) memcpy(elf->e_ident, ELFMAG, SELFMAG);
    elf->e_ident[EI_CLASS] = ELF_CLASS;
    elf->e_ident[EI_DATA] = ELF_DATA;
    elf->e_ident[EI_VERSION] = EV_CURRENT;
    elf->e_ident[EI_OSABI] = ELF_OSABI;

    elf->e_type = ET_CORE;
    elf->e_machine = machine;
    elf->e_version = EV_CURRENT;
    elf->e_phoff = sizeof(struct elfhdr);
    elf->e_flags = flags;
    elf->e_ehsize = sizeof(struct elfhdr);
    elf->e_phentsize = sizeof(struct elf_phdr);
    elf->e_phnum = segs;

    bswap_ehdr(elf);
}

static void fill_elf_note_phdr(struct elf_phdr *phdr, int sz, off_t offset)
{
    phdr->p_type = PT_NOTE;
    phdr->p_offset = offset;
    phdr->p_vaddr = 0;
    phdr->p_paddr = 0;
    phdr->p_filesz = sz;
    phdr->p_memsz = 0;
    phdr->p_flags = 0;
    phdr->p_align = 0;

    bswap_phdr(phdr, 1);
}

static size_t note_size(const struct memelfnote *note)
{
    return (note->notesz);
}

static void fill_prstatus(struct target_elf_prstatus *prstatus,
                          const TaskState *ts, int signr)
{
    (void) memset(prstatus, 0, sizeof (*prstatus));
    prstatus->pr_info.si_signo = prstatus->pr_cursig = signr;
    prstatus->pr_pid = ts->ts_tid;
    prstatus->pr_ppid = getppid();
    prstatus->pr_pgrp = getpgrp();
    prstatus->pr_sid = getsid(0);

    bswap_prstatus(prstatus);
}

static int fill_psinfo(struct target_elf_prpsinfo *psinfo, const TaskState *ts)
{
    char *filename, *base_filename;
    unsigned int i, len;

    (void) memset(psinfo, 0, sizeof (*psinfo));

    len = ts->info->arg_end - ts->info->arg_start;
    if (len >= ELF_PRARGSZ)
        len = ELF_PRARGSZ - 1;
    if (copy_from_user(&psinfo->pr_psargs, ts->info->arg_start, len))
        return -EFAULT;
    for (i = 0; i < len; i++)
        if (psinfo->pr_psargs[i] == 0)
            psinfo->pr_psargs[i] = ' ';
    psinfo->pr_psargs[len] = 0;

    psinfo->pr_pid = getpid();
    psinfo->pr_ppid = getppid();
    psinfo->pr_pgrp = getpgrp();
    psinfo->pr_sid = getsid(0);
    psinfo->pr_uid = getuid();
    psinfo->pr_gid = getgid();

    filename = strdup(ts->bprm->filename);
    base_filename = strdup((const char *)basename(filename));
    (void) strncpy(psinfo->pr_fname, base_filename,
                   sizeof(psinfo->pr_fname));
    free(base_filename);
    free(filename);

    bswap_psinfo(psinfo);
    return (0);
}

static void fill_auxv_note(struct memelfnote *note, const TaskState *ts)
{
    elf_addr_t auxv = (elf_addr_t)ts->info->saved_auxv;
    elf_addr_t orig_auxv = auxv;
    void *ptr;
    int len = ts->info->auxv_len;

    /*
     * Auxiliary vector is stored in target process stack.  It contains
     * {type, value} pairs that we need to dump into note.  This is not
     * strictly necessary but we do it here for sake of completeness.
     */

    /* read in whole auxv vector and copy it to memelfnote */
    ptr = lock_user(VERIFY_READ, orig_auxv, len, 0);
    if (ptr != NULL) {
        fill_note(note, "CORE", NT_AUXV, len, ptr);
        unlock_user(ptr, auxv, len);
    }
}

/*
 * Constructs name of coredump file.  We have following convention
 * for the name:
 *     qemu_<basename-of-target-binary>_<date>-<time>_<pid>.core
 *
 * Returns 0 in case of success, -1 otherwise (errno is set).
 */
static int core_dump_filename(const TaskState *ts, char *buf,
                              size_t bufsize)
{
    char timestamp[64];
    char *filename = NULL;
    char *base_filename = NULL;
    struct timeval tv;
    struct tm tm;

    assert(bufsize >= PATH_MAX);

    if (gettimeofday(&tv, NULL) < 0) {
        (void) fprintf(stderr, "unable to get current timestamp: %s",
                       strerror(errno));
        return (-1);
    }

    filename = strdup(ts->bprm->filename);
    base_filename = strdup((const char *)basename(filename));
    (void) strftime(timestamp, sizeof (timestamp), "%Y%m%d-%H%M%S",
                    localtime_r(&tv.tv_sec, &tm));
    (void) snprintf(buf, bufsize, "qemu_%s_%s_%d.core",
                    base_filename, timestamp, (int)getpid());
    free(base_filename);
    free(filename);

    return (0);
}

static int dump_write(int fd, const void *ptr, size_t size)
{
    const char *bufp = (const char *)ptr;
    ssize_t bytes_written, bytes_left;
    struct rlimit dumpsize;
    off_t pos;

    bytes_written = 0;
    getrlimit(RLIMIT_CORE, &dumpsize);
    if ((pos = lseek(fd, 0, SEEK_CUR))==-1) {
        if (errno == ESPIPE) { /* not a seekable stream */
            bytes_left = size;
        } else {
            return pos;
        }
    } else {
        if (dumpsize.rlim_cur <= pos) {
            return -1;
        } else if (dumpsize.rlim_cur == RLIM_INFINITY) {
            bytes_left = size;
        } else {
            size_t limit_left=dumpsize.rlim_cur - pos;
            bytes_left = limit_left >= size ? size : limit_left ;
        }
    }

    /*
     * In normal conditions, single write(2) should do but
     * in case of socket etc. this mechanism is more portable.
     */
    do {
        bytes_written = write(fd, bufp, bytes_left);
        if (bytes_written < 0) {
            if (errno == EINTR)
                continue;
            return (-1);
        } else if (bytes_written == 0) { /* eof */
            return (-1);
        }
        bufp += bytes_written;
        bytes_left -= bytes_written;
    } while (bytes_left > 0);

    return (0);
}

static int write_note(struct memelfnote *men, int fd)
{
    struct elf_note en;

    en.n_namesz = men->namesz;
    en.n_type = men->type;
    en.n_descsz = men->datasz;

    bswap_note(&en);

    if (dump_write(fd, &en, sizeof(en)) != 0)
        return (-1);
    if (dump_write(fd, men->name, men->namesz_rounded) != 0)
        return (-1);
    if (dump_write(fd, men->data, men->datasz_rounded) != 0)
        return (-1);

    return (0);
}

static void fill_thread_info(struct elf_note_info *info, const CPUArchState *env)
{
    TaskState *ts = (TaskState *)env->opaque;
    struct elf_thread_status *ets;

    ets = g_malloc0(sizeof (*ets));
    ets->num_notes = 1; /* only prstatus is dumped */
    fill_prstatus(&ets->prstatus, ts, 0);
    elf_core_copy_regs(&ets->prstatus.pr_reg, env);
    fill_note(&ets->notes[0], "CORE", NT_PRSTATUS, sizeof (ets->prstatus),
              &ets->prstatus);

    QTAILQ_INSERT_TAIL(&info->thread_list, ets, ets_link);

    info->notes_size += note_size(&ets->notes[0]);
}

static int fill_note_info(struct elf_note_info *info,
                          long signr, const CPUArchState *env)
{
#define NUMNOTES 3
    CPUArchState *cpu = NULL;
    TaskState *ts = (TaskState *)env->opaque;
    int i;

    (void) memset(info, 0, sizeof (*info));

    QTAILQ_INIT(&info->thread_list);

    info->notes = g_malloc0(NUMNOTES * sizeof (struct memelfnote));
    if (info->notes == NULL)
        return (-ENOMEM);
    info->prstatus = g_malloc0(sizeof (*info->prstatus));
    if (info->prstatus == NULL)
        return (-ENOMEM);
    info->psinfo = g_malloc0(sizeof (*info->psinfo));
    if (info->prstatus == NULL)
        return (-ENOMEM);

    /*
     * First fill in status (and registers) of current thread
     * including process info & aux vector.
     */
    fill_prstatus(info->prstatus, ts, signr);
    elf_core_copy_regs(&info->prstatus->pr_reg, env);
    fill_note(&info->notes[0], "CORE", NT_PRSTATUS,
              sizeof (*info->prstatus), info->prstatus);
    fill_psinfo(info->psinfo, ts);
    fill_note(&info->notes[1], "CORE", NT_PRPSINFO,
              sizeof (*info->psinfo), info->psinfo);
    fill_auxv_note(&info->notes[2], ts);
    info->numnote = 3;

    info->notes_size = 0;
    for (i = 0; i < info->numnote; i++)
        info->notes_size += note_size(&info->notes[i]);

    /* read and fill status of all threads */
    cpu_list_lock();
    for (cpu = first_cpu; cpu != NULL; cpu = cpu->next_cpu) {
        if (cpu == thread_env)
            continue;
        fill_thread_info(info, cpu);
    }
    cpu_list_unlock();

    return (0);
}

static void free_note_info(struct elf_note_info *info)
{
    struct elf_thread_status *ets;

    while (!QTAILQ_EMPTY(&info->thread_list)) {
        ets = QTAILQ_FIRST(&info->thread_list);
        QTAILQ_REMOVE(&info->thread_list, ets, ets_link);
        g_free(ets);
    }

    g_free(info->prstatus);
    g_free(info->psinfo);
    g_free(info->notes);
}

static int write_note_info(struct elf_note_info *info, int fd)
{
    struct elf_thread_status *ets;
    int i, error = 0;

    /* write prstatus, psinfo and auxv for current thread */
    for (i = 0; i < info->numnote; i++)
        if ((error = write_note(&info->notes[i], fd)) != 0)
            return (error);

    /* write prstatus for each thread */
    for (ets = info->thread_list.tqh_first; ets != NULL;
         ets = ets->ets_link.tqe_next) {
        if ((error = write_note(&ets->notes[0], fd)) != 0)
            return (error);
    }

    return (0);
}

/*
 * Write out ELF coredump.
 *
 * See documentation of ELF object file format in:
 * http://www.caldera.com/developers/devspecs/gabi41.pdf
 *
 * Coredump format in linux is following:
 *
 * 0   +----------------------+         \
 *     | ELF header           | ET_CORE  |
 *     +----------------------+          |
 *     | ELF program headers  |          |--- headers
 *     | - NOTE section       |          |
 *     | - PT_LOAD sections   |          |
 *     +----------------------+         /
 *     | NOTEs:               |
 *     | - NT_PRSTATUS        |
 *     | - NT_PRSINFO         |
 *     | - NT_AUXV            |
 *     +----------------------+ <-- aligned to target page
 *     | Process memory dump  |
 *     :                      :
 *     .                      .
 *     :                      :
 *     |                      |
 *     +----------------------+
 *
 * NT_PRSTATUS -> struct elf_prstatus (per thread)
 * NT_PRSINFO  -> struct elf_prpsinfo
 * NT_AUXV is array of { type, value } pairs (see fill_auxv_note()).
 *
 * Format follows System V format as close as possible.  Current
 * version limitations are as follows:
 *     - no floating point registers are dumped
 *
 * Function returns 0 in case of success, negative errno otherwise.
 *
 * TODO: make this work also during runtime: it should be
 * possible to force coredump from running process and then
 * continue processing.  For example qemu could set up SIGUSR2
 * handler (provided that target process haven't registered
 * handler for that) that does the dump when signal is received.
 */
static int elf_core_dump(int signr, const CPUArchState *env)
{
    const TaskState *ts = (const TaskState *)env->opaque;
    struct vm_area_struct *vma = NULL;
    char corefile[PATH_MAX];
    struct elf_note_info info;
    struct elfhdr elf;
    struct elf_phdr phdr;
    struct rlimit dumpsize;
    struct mm_struct *mm = NULL;
    off_t offset = 0, data_offset = 0;
    int segs = 0;
    int fd = -1;

    errno = 0;
    getrlimit(RLIMIT_CORE, &dumpsize);
    if (dumpsize.rlim_cur == 0)
        return 0;

    if (core_dump_filename(ts, corefile, sizeof (corefile)) < 0)
        return (-errno);

    if ((fd = open(corefile, O_WRONLY | O_CREAT,
                   S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH)) < 0)
        return (-errno);

    /*
     * Walk through target process memory mappings and
     * set up structure containing this information.  After
     * this point vma_xxx functions can be used.
     */
    if ((mm = vma_init()) == NULL)
        goto out;

    walk_memory_regions(mm, vma_walker);
    segs = vma_get_mapping_count(mm);

    /*
     * Construct valid coredump ELF header.  We also
     * add one more segment for notes.
     */
    fill_elf_header(&elf, segs + 1, ELF_MACHINE, 0);
    if (dump_write(fd, &elf, sizeof (elf)) != 0)
        goto out;

    /* fill in in-memory version of notes */
    if (fill_note_info(&info, signr, env) < 0)
        goto out;

    offset += sizeof (elf);                             /* elf header */
    offset += (segs + 1) * sizeof (struct elf_phdr);    /* program headers */

    /* write out notes program header */
    fill_elf_note_phdr(&phdr, info.notes_size, offset);

    offset += info.notes_size;
    if (dump_write(fd, &phdr, sizeof (phdr)) != 0)
        goto out;

    /*
     * ELF specification wants data to start at page boundary so
     * we align it here.
     */
    data_offset = offset = roundup(offset, ELF_EXEC_PAGESIZE);

    /*
     * Write program headers for memory regions mapped in
     * the target process.
     */
    for (vma = vma_first(mm); vma != NULL; vma = vma_next(vma)) {
        (void) memset(&phdr, 0, sizeof (phdr));

        phdr.p_type = PT_LOAD;
        phdr.p_offset = offset;
        phdr.p_vaddr = vma->vma_start;
        phdr.p_paddr = 0;
        phdr.p_filesz = vma_dump_size(vma);
        offset += phdr.p_filesz;
        phdr.p_memsz = vma->vma_end - vma->vma_start;
        phdr.p_flags = vma->vma_flags & PROT_READ ? PF_R : 0;
        if (vma->vma_flags & PROT_WRITE)
            phdr.p_flags |= PF_W;
        if (vma->vma_flags & PROT_EXEC)
            phdr.p_flags |= PF_X;
        phdr.p_align = ELF_EXEC_PAGESIZE;

        bswap_phdr(&phdr, 1);
        dump_write(fd, &phdr, sizeof (phdr));
    }

    /*
     * Next we write notes just after program headers.  No
     * alignment needed here.
     */
    if (write_note_info(&info, fd) < 0)
        goto out;

    /* align data to page boundary */
    if (lseek(fd, data_offset, SEEK_SET) != data_offset)
        goto out;

    /*
     * Finally we can dump process memory into corefile as well.
     */
    for (vma = vma_first(mm); vma != NULL; vma = vma_next(vma)) {
        abi_ulong addr;
        abi_ulong end;

        end = vma->vma_start + vma_dump_size(vma);

        for (addr = vma->vma_start; addr < end;
             addr += TARGET_PAGE_SIZE) {
            char page[TARGET_PAGE_SIZE];
            int error;

            /*
             *  Read in page from target process memory and
             *  write it to coredump file.
             */
            error = copy_from_user(page, addr, sizeof (page));
            if (error != 0) {
                (void) fprintf(stderr, "unable to dump " TARGET_ABI_FMT_lx "\n",
                               addr);
                errno = -error;
                goto out;
            }
            if (dump_write(fd, page, TARGET_PAGE_SIZE) < 0)
                goto out;
        }
    }

 out:
    free_note_info(&info);
    if (mm != NULL)
        vma_delete(mm);
    (void) close(fd);

    if (errno != 0)
        return (-errno);
    return (0);
}
#endif /* USE_ELF_CORE_DUMP */

void do_init_thread(struct target_pt_regs *regs, struct image_info *infop)
{
    init_thread(regs, infop);
}

/* copied from qemu_master/util/getauxcal.c */
#ifdef CONFIG_GETAUXVAL
/* Don't inline this in qemu/osdep.h, because pulling in <sys/auxv.h> for
   the system declaration of getauxval pulls in the system <elf.h>, which
   conflicts with qemu's version.  */

#include <sys/auxv.h>

unsigned long qemu_getauxval(unsigned long key)
{
    return getauxval(key);
}
#elif defined(__linux__)
#include "elf.h"

/* Our elf.h doesn't contain Elf32_auxv_t and Elf64_auxv_t, which is ok because
   that just makes it easier to define it properly for the host here.  */
typedef struct {
    unsigned long a_type;
    unsigned long a_val;
} ElfW_auxv_t;

static const ElfW_auxv_t *auxv;

static const ElfW_auxv_t *qemu_init_auxval(void)
{
    ElfW_auxv_t *a;
    ssize_t size = 512, r, ofs;
    int fd;

    /* Allocate some initial storage.  Make sure the first entry is set
       to end-of-list, so that we've got a valid list in case of error.  */
    auxv = a = g_malloc(size);
    a[0].a_type = 0;
    a[0].a_val = 0;

    fd = open("/proc/self/auxv", O_RDONLY);
    if (fd < 0) {
        return a;
    }

    /* Read the first SIZE bytes.  Hopefully, this covers everything.  */
    r = read(fd, a, size);

    if (r == size) {
        /* Continue to expand until we do get a partial read.  */
        do {
            ofs = size;
            size *= 2;
            auxv = a = g_realloc(a, size);
            r = read(fd, (char *)a + ofs, ofs);
        } while (r == ofs);
    }

    close(fd);
    return a;
}

unsigned long qemu_getauxval(unsigned long type)
{
    const ElfW_auxv_t *a = auxv;

    if (unlikely(a == NULL)) {
        a = qemu_init_auxval();
    }

    for (; a->a_type != 0; a++) {
        if (a->a_type == type) {
            return a->a_val;
        }
    }

    return 0;
}

#else

unsigned long qemu_getauxval(unsigned long type)
{
    return 0;
}

#endif

