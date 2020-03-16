/*
 * ARM implementation of KVM hooks
 *
 * Copyright Christoffer Dall 2009-2010
 *
 * Copyright (C) 2020, TrussLab@University of California, Irvine.
 *	Authors: Yingtong Liu <yingtong@uci.edu>  
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#if defined(HOST_X86_64) && defined(TARGET_ARM)
#endif

#include <stdio.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/kvm.h>

#include "qemu-common.h"
#include "sysemu.h"
#include "kvm.h"
#include "cpu.h"
#include "hw/arm-misc.h"

const KVMCapabilityInfo kvm_arch_required_capabilities[] = {
    KVM_CAP_LAST_INFO
};

int kvm_arch_init(KVMState *s)
{
    /* For ARM interrupt delivery is always asynchronous,
     * whether we are using an in-kernel VGIC or not.
     */
    kvm_async_interrupts_allowed = true;
    return 0;
}

int kvm_arch_init_vcpu(CPUArchState *env)
{
    struct kvm_vcpu_init init;

    init.target = KVM_ARM_TARGET_CORTEX_A15;
    memset(init.features, 0, sizeof(init.features));
    return kvm_vcpu_ioctl(env, KVM_ARM_VCPU_INIT, &init);
}

typedef struct Reg {
    uint64_t id;
    int offset;
} Reg;

#define COREREG(KERNELNAME, QEMUFIELD)                       \
    {                                                        \
        KVM_REG_ARM | KVM_REG_SIZE_U32 |                     \
        KVM_REG_ARM_CORE | KVM_REG_ARM_CORE_REG(KERNELNAME), \
        offsetof(CPUARMState, QEMUFIELD)                     \
    }

#define CP15REG(CRN, CRM, OPC1, OPC2, QEMUFIELD) \
    {                                            \
        KVM_REG_ARM | KVM_REG_SIZE_U32 |         \
        (15 << KVM_REG_ARM_COPROC_SHIFT) |       \
        ((CRN) << KVM_REG_ARM_32_CRN_SHIFT) |    \
        ((CRM) << KVM_REG_ARM_CRM_SHIFT) |       \
        ((OPC1) << KVM_REG_ARM_OPC1_SHIFT) |     \
        ((OPC2) << KVM_REG_ARM_32_OPC2_SHIFT),   \
        offsetof(CPUARMState, QEMUFIELD)         \
    }

static const Reg regs[] = {
    /* R0_usr .. R14_usr */
    COREREG(usr_regs.uregs[0], regs[0]),
    COREREG(usr_regs.uregs[1], regs[1]),
    COREREG(usr_regs.uregs[2], regs[2]),
    COREREG(usr_regs.uregs[3], regs[3]),
    COREREG(usr_regs.uregs[4], regs[4]),
    COREREG(usr_regs.uregs[5], regs[5]),
    COREREG(usr_regs.uregs[6], regs[6]),
    COREREG(usr_regs.uregs[7], regs[7]),
    COREREG(usr_regs.uregs[8], usr_regs[0]),
    COREREG(usr_regs.uregs[9], usr_regs[1]),
    COREREG(usr_regs.uregs[10], usr_regs[2]),
    COREREG(usr_regs.uregs[11], usr_regs[3]),
    COREREG(usr_regs.uregs[12], usr_regs[4]),
    COREREG(usr_regs.uregs[13], banked_r13[0]),
    COREREG(usr_regs.uregs[14], banked_r14[0]),
    /* R13, R14, SPSR for SVC, ABT, UND, IRQ banks */
    COREREG(svc_regs[0], banked_r13[1]),
    COREREG(svc_regs[1], banked_r14[1]),
    COREREG(svc_regs[2], banked_spsr[1]),
    COREREG(abt_regs[0], banked_r13[2]),
    COREREG(abt_regs[1], banked_r14[2]),
    COREREG(abt_regs[2], banked_spsr[2]),
    COREREG(und_regs[0], banked_r13[3]),
    COREREG(und_regs[1], banked_r14[3]),
    COREREG(und_regs[2], banked_spsr[3]),
    COREREG(irq_regs[0], banked_r13[4]),
    COREREG(irq_regs[1], banked_r14[4]),
    COREREG(irq_regs[2], banked_spsr[4]),
    /* R8_fiq .. R14_fiq and SPSR_fiq */
    COREREG(fiq_regs[0], fiq_regs[0]),
    COREREG(fiq_regs[1], fiq_regs[1]),
    COREREG(fiq_regs[2], fiq_regs[2]),
    COREREG(fiq_regs[3], fiq_regs[3]),
    COREREG(fiq_regs[4], fiq_regs[4]),
    COREREG(fiq_regs[5], banked_r13[5]),
    COREREG(fiq_regs[6], banked_r14[5]),
    COREREG(fiq_regs[7], banked_spsr[5]),
    /* R15 */
    COREREG(usr_regs.uregs[15], regs[15]),
    /* A non-comprehensive set of cp15 registers.
     * TODO: drive this from the cp_regs hashtable instead.
     */
    CP15REG(1, 0, 0, 0, cp15.c1_sys), /* SCTLR */
    CP15REG(2, 0, 0, 2, cp15.c2_control), /* TTBCR */
    CP15REG(3, 0, 0, 0, cp15.c3), /* DACR */
};
#ifdef CONFIG_USER_KVM
/* Map CPU modes onto saved register banks.  */
static inline int bank_number(CPUARMState *env, int mode)
{
    switch (mode) {
    case ARM_CPU_MODE_USR:
    case ARM_CPU_MODE_SYS:
        return 0;
    case ARM_CPU_MODE_SVC:
        return 1;
    case ARM_CPU_MODE_ABT:
        return 2;
    case ARM_CPU_MODE_UND:
        return 3;
    case ARM_CPU_MODE_IRQ:
        return 4;
    case ARM_CPU_MODE_FIQ:
        return 5;
    }
    cpu_abort(env, "Bad mode %x\n", mode);
    return -1;
}
#endif
#ifdef CONFIG_USER_KVM
#include <linux-user/qemu.h>
static int kvm_put_opaque(CPUARMState *env)
{
	//This opaque pointer is used for passing guest process TaskState instance.
	void * opaque = env->opaque;
	return kvm_vcpu_ioctl(env, KVM_SET_OPAQUE, opaque);
}
#endif
int kvm_arch_put_registers(CPUArchState *env, int level)
{
    struct kvm_one_reg r;
    int mode, bn;
    int ret, i;
    uint32_t cpsr;
    uint32_t ttbr;

    /* Make sure the banked regs are properly set */
    mode = env->uncached_cpsr & CPSR_M;
    bn = bank_number(env, mode);
    if (mode == ARM_CPU_MODE_FIQ) {
        memcpy(env->fiq_regs, env->regs + 8, 5 * sizeof(uint32_t));
    } else {
        memcpy(env->usr_regs, env->regs + 8, 5 * sizeof(uint32_t));
    }
    env->banked_r13[bn] = env->regs[13];
    env->banked_r14[bn] = env->regs[14];
    env->banked_spsr[bn] = env->spsr;

    /* Now we can safely copy stuff down to the kernel */
    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        r.id = regs[i].id;
        r.addr = (uintptr_t)(env) + regs[i].offset;
        ret = kvm_vcpu_ioctl(env, KVM_SET_ONE_REG, &r);
        if (ret) {
            return ret;
        }
    }

    /* Special cases which aren't a single CPUARMState field */
    cpsr = cpsr_read(env);
    r.id = KVM_REG_ARM | KVM_REG_SIZE_U32 |
        KVM_REG_ARM_CORE | KVM_REG_ARM_CORE_REG(usr_regs.ARM_cpsr);
    r.addr = (uintptr_t)(&cpsr);
    ret = kvm_vcpu_ioctl(env, KVM_SET_ONE_REG, &r);
    if (ret) {
        return ret;
    }

    /* TTBR0: cp15 crm=2 opc1=0 */
    ttbr = env->cp15.c2_base0;
    r.id = KVM_REG_ARM | KVM_REG_SIZE_U64 | (15 << KVM_REG_ARM_COPROC_SHIFT) |
        (2 << KVM_REG_ARM_CRM_SHIFT) | (0 << KVM_REG_ARM_OPC1_SHIFT);
    r.addr = (uintptr_t)(&ttbr);
    ret = kvm_vcpu_ioctl(env, KVM_SET_ONE_REG, &r);
    if (ret) {
        return ret;
    }

    /* TTBR1: cp15 crm=2 opc1=1 */
    ttbr = env->cp15.c2_base1;
    r.id = KVM_REG_ARM | KVM_REG_SIZE_U64 | (15 << KVM_REG_ARM_COPROC_SHIFT) |
        (2 << KVM_REG_ARM_CRM_SHIFT) | (1 << KVM_REG_ARM_OPC1_SHIFT);
    r.addr = (uintptr_t)(&ttbr);
    ret = kvm_vcpu_ioctl(env, KVM_SET_ONE_REG, &r);
#ifdef CONFIG_USER_KVM
    ret = kvm_put_opaque(env);
    if (ret < 0) {
        return ret;
    }
#endif

    return ret;
}

int kvm_arch_get_registers(CPUArchState *env)
{
    struct kvm_one_reg r;
    int mode, bn;
    int ret, i;
    uint32_t cpsr;
    uint64_t ttbr;

    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        r.id = regs[i].id;
        r.addr = (uintptr_t)(env) + regs[i].offset;
        ret = kvm_vcpu_ioctl(env, KVM_GET_ONE_REG, &r);
        if (ret) {
            return ret;
        }
    }

    /* Special cases which aren't a single CPUARMState field */
    r.id = KVM_REG_ARM | KVM_REG_SIZE_U32 |
        KVM_REG_ARM_CORE | KVM_REG_ARM_CORE_REG(usr_regs.ARM_cpsr);
    r.addr = (uintptr_t)(&cpsr);
    ret = kvm_vcpu_ioctl(env, KVM_GET_ONE_REG, &r);
    if (ret) {
        return ret;
    }
    cpsr_write(env, cpsr, 0xffffffff);

    /* TTBR0: cp15 crm=2 opc1=0 */
    r.id = KVM_REG_ARM | KVM_REG_SIZE_U64 | (15 << KVM_REG_ARM_COPROC_SHIFT) |
        (2 << KVM_REG_ARM_CRM_SHIFT) | (0 << KVM_REG_ARM_OPC1_SHIFT);
    r.addr = (uintptr_t)(&ttbr);
    ret = kvm_vcpu_ioctl(env, KVM_GET_ONE_REG, &r);
    if (ret) {
        return ret;
    }
    env->cp15.c2_base0 = ttbr;

    /* TTBR1: cp15 crm=2 opc1=1 */
    r.id = KVM_REG_ARM | KVM_REG_SIZE_U64 | (15 << KVM_REG_ARM_COPROC_SHIFT) |
        (2 << KVM_REG_ARM_CRM_SHIFT) | (1 << KVM_REG_ARM_OPC1_SHIFT);
    r.addr = (uintptr_t)(&ttbr);
    ret = kvm_vcpu_ioctl(env, KVM_GET_ONE_REG, &r);
    if (ret) {
        return ret;
    }
    env->cp15.c2_base1 = ttbr;

    /* Make sure the current mode regs are properly set */
    mode = env->uncached_cpsr & CPSR_M;
    bn = bank_number(env, mode);
    if (mode == ARM_CPU_MODE_FIQ) {
        memcpy(env->regs + 8, env->fiq_regs, 5 * sizeof(uint32_t));
    } else {
        memcpy(env->regs + 8, env->usr_regs, 5 * sizeof(uint32_t));
    }
    env->regs[13] = env->banked_r13[bn];
    env->regs[14] = env->banked_r14[bn];
    env->spsr = env->banked_spsr[bn];

    /* The main GET_ONE_REG loop above set c2_control, but we need to
     * update some extra cached precomputed values too.
     * When this is driven from the cp_regs hashtable then this ugliness
     * can disappear because we'll use the access function which sets
     * these values automatically.
     */
    env->cp15.c2_mask = ~(0xffffffffu >> env->cp15.c2_control);
    env->cp15.c2_base_mask = ~(0x3fffu >> env->cp15.c2_control);

    return 0;
}

void kvm_arch_pre_run(CPUArchState *env, struct kvm_run *run)
{
}

void kvm_arch_post_run(CPUArchState *env, struct kvm_run *run)
{
}

int kvm_arch_handle_exit(CPUArchState *env, struct kvm_run *run)
{
    return 0;
}

void kvm_arch_reset_vcpu(CPUArchState *env)
{
}

bool kvm_arch_stop_on_emulation_error(CPUArchState *env)
{
    return true;
}

int kvm_arch_process_async_events(CPUArchState *env)
{
    return 0;
}

int kvm_arch_on_sigbus_vcpu(CPUArchState *env, int code, void *addr)
{
    return 1;
}

int kvm_arch_on_sigbus(int code, void *addr)
{
    return 1;
}

void kvm_arch_update_guest_debug(CPUArchState *env, struct kvm_guest_debug *dbg)
{
}

int kvm_arch_insert_sw_breakpoint(CPUArchState *current_env,
                                  struct kvm_sw_breakpoint *bp)
{
    return -EINVAL;
}

int kvm_arch_insert_hw_breakpoint(target_ulong addr,
                                  target_ulong len, int type)
{
    return -EINVAL;
}

int kvm_arch_remove_hw_breakpoint(target_ulong addr,
                                  target_ulong len, int type)
{
    return -EINVAL;
}

int kvm_arch_remove_sw_breakpoint(CPUArchState *current_env,
                                  struct kvm_sw_breakpoint *bp)
{
    return -EINVAL;
}

void kvm_arch_remove_all_hw_breakpoints(void)
{
}

void kvm_arch_init_irq_routing(KVMState *s)
{
}
