/* Code for loading Linux executables.  Mostly linux kernel code.  */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include "qemu.h"

#define NGROUPS 32

/* ??? This should really be somewhere else.  */
abi_long memcpy_to_target(abi_ulong dest, const void *src,
                          unsigned long len)
{
    void *host_ptr;

    host_ptr = lock_user(VERIFY_WRITE, dest, len, 0);
    if (!host_ptr)
        return -TARGET_EFAULT;
    memcpy(host_ptr, src, len);
    unlock_user(host_ptr, dest, 1);
    return 0;
}

static int count(char ** vec)
{
    int		i;

    for(i = 0; *vec; i++) {
        vec++;
    }

    return(i);
}

static int prepare_binprm(struct linux_binprm *bprm)
{
    struct stat		st;
    int mode;
    int retval;

    if(fstat(bprm->fd, &st) < 0) {
	return(-errno);
    }

    mode = st.st_mode;
    if(!S_ISREG(mode)) {	/* Must be regular file */
	return(-EACCES);
    }
    if(!(mode & 0111)) {	/* Must have at least one execute bit set */
	return(-EACCES);
    }

    bprm->e_uid = geteuid();
    bprm->e_gid = getegid();

    /* Set-uid? */
    if(mode & S_ISUID) {
    	bprm->e_uid = st.st_uid;
    }

    /* Set-gid? */
    /*
     * If setgid is set but no group execute bit then this
     * is a candidate for mandatory locking, not a setgid
     * executable.
     */
    if ((mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) {
	bprm->e_gid = st.st_gid;
    }

    retval = read(bprm->fd, bprm->buf, BPRM_BUF_SIZE);
    if (retval < 0) {
	perror("prepare_binprm");
	exit(-1);
    }
    if (retval < BPRM_BUF_SIZE) {
        /* Make sure the rest of the loader won't read garbage.  */
        memset(bprm->buf + retval, 0, BPRM_BUF_SIZE - retval);
    }
    return retval;
}

/* Construct the envp and argv tables on the target stack.  */
abi_ulong loader_build_argptr(int envc, int argc, abi_ulong sp,
                              abi_ulong stringp, int push_ptr)
{
    TaskState *ts = (TaskState *)thread_env->opaque;
    int n = sizeof(abi_ulong);
    abi_ulong envp;
    abi_ulong argv;

    sp -= (envc + 1) * n;
    envp = sp;
    sp -= (argc + 1) * n;
    argv = sp;
    if (push_ptr) {
        /* FIXME - handle put_user() failures */
        sp -= n;
        put_user_ual(envp, sp);
        sp -= n;
        put_user_ual(argv, sp);
    }
    sp -= n;
    /* FIXME - handle put_user() failures */
    put_user_ual(argc, sp);
    ts->info->arg_start = stringp;
    while (argc-- > 0) {
        /* FIXME - handle put_user() failures */
        put_user_ual(stringp, argv);
        argv += n;
        stringp += target_strlen(stringp) + 1;
    }
    ts->info->arg_end = stringp;
    /* FIXME - handle put_user() failures */
    put_user_ual(0, argv);
    while (envc-- > 0) {
        /* FIXME - handle put_user() failures */
        put_user_ual(stringp, envp);
        envp += n;
        stringp += target_strlen(stringp) + 1;
    }
    /* FIXME - handle put_user() failures */
    put_user_ual(0, envp);

    return sp;
}

#ifdef DEBUG_PAGE_ALLOC
void debug_page_alloc() {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        fprintf(stderr, "Could not open mem maps %s\n", __FUNCTION__);
        exit(-1);
    }

    char buffer[512];
    while (fgets(buffer, sizeof(buffer), fp)) {
        uint64_t s, e;
        unsigned int pgoff, major, minor;
	unsigned long ino;
        //char r, w, x;
        char r, w, x, p;
	char name[200];
        //sscanf(buffer, "%" PRIx64 "-%" PRIx64 " %c%c%c", &s, &e, &r, &w, &x);
        sscanf(buffer, "%" PRIx64 "-%" PRIx64 " %c%c%c%c" " %x %x:%x %lu\t%s", &s, &e, &r, &w, &x, &p, &pgoff, &major, &minor, &ino, name);
        fprintf(stderr, "    Area %" PRIx64 "-%" PRIx64 "%c%c%c name=%s\n", s, e, r, w, x, name);
    }

    fclose(fp);
}
#else
void debug_page_alloc() {};
#endif

int loader_exec(const char * filename, char ** argv, char ** envp,
             struct target_pt_regs * regs, struct image_info *infop,
             struct linux_binprm *bprm)
{
    int retval;
    int i;

    bprm->p = TARGET_PAGE_SIZE*MAX_ARG_PAGES-sizeof(unsigned int);
    memset(bprm->page, 0, sizeof(bprm->page));
    retval = open(filename, O_RDONLY);
    if (retval < 0)
        return retval;
    bprm->fd = retval;
    bprm->filename = (char *)filename;
    bprm->argc = count(argv);
    bprm->argv = argv;
    bprm->envc = count(envp);
    bprm->envp = envp;

    retval = prepare_binprm(bprm);

    if(retval>=0) {
        if (bprm->buf[0] == 0x7f
                && bprm->buf[1] == 'E'
                && bprm->buf[2] == 'L'
                && bprm->buf[3] == 'F') {
            retval = load_elf_binary(bprm, regs, infop);
#if defined(TARGET_HAS_BFLT)
        } else if (bprm->buf[0] == 'b'
                && bprm->buf[1] == 'F'
                && bprm->buf[2] == 'L'
                && bprm->buf[3] == 'T') {
            retval = load_flt_binary(bprm,regs,infop);
#endif
        } else {
            fprintf(stderr, "Unknown binary format\n");
            return -1;
        }
    }
//user mode: initialize registers
    if(retval>=0) {
        /* success.  Initialize important registers */
        do_init_thread(regs, infop);
	debug_page_alloc();
        return retval;
    }

    /* Something went wrong, return the inode and free the argument pages*/
    for (i=0 ; i<MAX_ARG_PAGES ; i++) {
        g_free(bprm->page[i]);
    }
    return(retval);
}
