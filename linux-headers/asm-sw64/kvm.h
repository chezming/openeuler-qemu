#ifndef __LINUX_KVM_SW64_H
#define __LINUX_KVM_SW64_H

#include <linux/types.h>

#define __KVM_HAVE_GUEST_DEBUG

/*
 * for KVM_GET_REGS and KVM_SET_REGS
 */
struct kvm_regs {
	union {
		struct {
			unsigned long r[27];
			unsigned long fpcr;

			unsigned long fp[124];
			/* These are saved by hmcode: */
			unsigned long ps;
			unsigned long pc;
			unsigned long gp;
			unsigned long r16;
			unsigned long r17;
			unsigned long r18;
		} c3_regs;
		struct {
			unsigned long r[31];
			unsigned long fpcr;

			unsigned long fp[124];
			unsigned long ps;
			unsigned long pc;
		}c4_regs;
	};
};

/*
 * for KVM_GET_FPU and KVM_SET_FPU
 */
struct kvm_fpu {
};

/*
 * KVM SW_64 specific structures and definitions
 */
struct kvm_debug_exit_arch {
	unsigned long epc;
};

/* for KVM_SET_GUEST_DEBUG */
struct kvm_guest_debug_arch {
};

/* definition of registers in kvm_run */
struct kvm_sync_regs {
};

/* dummy definition */
struct kvm_sregs {
};

#define KVM_SW64_VCPU_INIT    _IO(KVMIO, 0xba)
#define KVM_SW64_USE_SLAVE    _IO(KVMIO, 0xbb)
#define KVM_SW64_GET_VCB      _IO(KVMIO, 0xbc)
#define KVM_SW64_SET_VCB      _IO(KVMIO, 0xbd)

#endif /* __LINUX_KVM_SW64_H */
