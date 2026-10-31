#define _GNU_SOURCE
#include "../common/security_sandbox.h"
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <stddef.h>

static int write_map(const char *path, const char *value) {
    int fd=open(path,O_WRONLY|O_CLOEXEC);if(fd<0)return 0;size_t n=strlen(value),off=0;while(off<n){ssize_t w=write(fd,value+off,n-off);if(w<0&&errno==EINTR)continue;if(w<=0){close(fd);return 0;}off+=(size_t)w;}close(fd);return 1;
}
static int try_namespaces(void) {
    /* Save IDs before unshare(): unmapped IDs can read back as the overflow uid/gid afterwards. */
    uid_t outer_uid = getuid();
    gid_t outer_gid = getgid();
    /* Once inside userns, a failed uid map is fatal: continuing with an unmapped identity
       can silently lose access to profile/cache files. */
    if (unshare(CLONE_NEWUSER) == 0) {
        char map[128];
        snprintf(map, sizeof map, "0 %lu 1\n", (unsigned long)outer_uid);
        if (!write_map("/proc/self/uid_map", map)) return -1;
        (void)write_map("/proc/self/setgroups", "deny\n");
        snprintf(map, sizeof map, "0 %lu 1\n", (unsigned long)outer_gid);
        (void)write_map("/proc/self/gid_map", map);
    }
    if (unshare(CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWUTS) == 0) {
        if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0) return MLB_SANDBOX_NAMESPACES;
        return 0;
    }
    return 0;
}

static int install_filter(void) {
    struct sock_filter f[96];size_t n=0;
#if defined(__x86_64__)
    const unsigned arch=AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    const unsigned arch=AUDIT_ARCH_AARCH64;
#elif defined(__i386__)
    const unsigned arch=AUDIT_ARCH_I386;
#else
    return 0;
#endif
    f[n++]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_W|BPF_ABS,(unsigned)offsetof(struct seccomp_data,arch));
    f[n++]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,arch,1,0);
    f[n++]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS);
    f[n++]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_W|BPF_ABS,(unsigned)offsetof(struct seccomp_data,nr));
#define DENY_SYSCALL(nr_) do { f[n++]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(unsigned)(nr_),0,1); f[n++]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|(EPERM&SECCOMP_RET_DATA)); } while(0)
#ifdef __NR_execve
    DENY_SYSCALL(__NR_execve);
#endif
#ifdef __NR_fork
    DENY_SYSCALL(__NR_fork);
#endif
#ifdef __NR_vfork
    DENY_SYSCALL(__NR_vfork);
#endif
#ifdef __NR_clone
    DENY_SYSCALL(__NR_clone);
#endif
#ifdef __NR_clone3
    DENY_SYSCALL(__NR_clone3);
#endif
#ifdef __NR_execveat
    DENY_SYSCALL(__NR_execveat);
#endif
#ifdef __NR_ptrace
    DENY_SYSCALL(__NR_ptrace);
#endif
#ifdef __NR_mount
    DENY_SYSCALL(__NR_mount);
#endif
#ifdef __NR_umount2
    DENY_SYSCALL(__NR_umount2);
#endif
#ifdef __NR_pivot_root
    DENY_SYSCALL(__NR_pivot_root);
#endif
#ifdef __NR_unshare
    DENY_SYSCALL(__NR_unshare);
#endif
#ifdef __NR_setns
    DENY_SYSCALL(__NR_setns);
#endif
#ifdef __NR_bpf
    DENY_SYSCALL(__NR_bpf);
#endif
#ifdef __NR_keyctl
    DENY_SYSCALL(__NR_keyctl);
#endif
#ifdef __NR_add_key
    DENY_SYSCALL(__NR_add_key);
#endif
#ifdef __NR_request_key
    DENY_SYSCALL(__NR_request_key);
#endif
#ifdef __NR_kexec_load
    DENY_SYSCALL(__NR_kexec_load);
#endif
#ifdef __NR_init_module
    DENY_SYSCALL(__NR_init_module);
#endif
#ifdef __NR_finit_module
    DENY_SYSCALL(__NR_finit_module);
#endif
#ifdef __NR_delete_module
    DENY_SYSCALL(__NR_delete_module);
#endif
#ifdef __NR_reboot
    DENY_SYSCALL(__NR_reboot);
#endif
#ifdef __NR_open_by_handle_at
    DENY_SYSCALL(__NR_open_by_handle_at);
#endif
#ifdef __NR_process_vm_readv
    DENY_SYSCALL(__NR_process_vm_readv);
#endif
#ifdef __NR_process_vm_writev
    DENY_SYSCALL(__NR_process_vm_writev);
#endif
#ifdef __NR_userfaultfd
    DENY_SYSCALL(__NR_userfaultfd);
#endif
#ifdef __NR_perf_event_open
    DENY_SYSCALL(__NR_perf_event_open);
#endif
#ifdef __NR_io_uring_setup
    DENY_SYSCALL(__NR_io_uring_setup);
#endif
#ifdef __NR_chroot
    DENY_SYSCALL(__NR_chroot);
#endif
#ifdef __NR_iopl
    DENY_SYSCALL(__NR_iopl);
#endif
#ifdef __NR_ioperm
    DENY_SYSCALL(__NR_ioperm);
#endif
    f[n++]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
#undef DENY_SYSCALL
    struct sock_fprog prog={(unsigned short)n,f};
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)<0)return 0;
    if(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&prog)<0)return 0;
    return 1;
}
int mlb_sandbox_apply(void) {
    int result=0;
    if (getenv("MLBROWSER_DISABLE_NAMESPACES") == NULL) {
        int ns = try_namespaces();
        if (ns < 0) return MLB_SANDBOX_ERROR;
        result |= ns;
    }
    /* set no_new_privs and a deny-list filter: untrusted workers cannot start processes, ptrace peers,
       load kernel modules, mount filesystems, alter namespaces or use privileged kernel APIs. */
    if (install_filter()) result |= MLB_SANDBOX_SECCOMP;
    else result |= MLB_SANDBOX_ERROR;
    return result;
}
