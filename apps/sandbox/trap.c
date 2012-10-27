/*
 * trap.c - handles system calls, page faults, and other traps
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <utime.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>
#include <linux/sched.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <pthread.h>
#include <err.h>
#include <linux/futex.h>
#include <linux/unistd.h>

#include "sandbox.h"
#include "boxer.h"

struct thread_arg {
	pthread_cond_t	ta_cnd;
	pthread_mutex_t	ta_mtx;
	pid_t		ta_tid;
	struct dune_tf	*ta_tf;
};

int exec_execev(const char *filename, char *const argv[], char *const envp[]);

static boxer_syscall_cb _syscall_monitor;
static pthread_mutex_t _syscall_mtx;

static void
pgflt_handler(uintptr_t addr, uint64_t fec, struct dune_tf *tf)
{
	int ret;
	ptent_t *pte;
	bool was_user = (tf->cs & 0x3);

	if (was_user) {
		printf("sandbox: got unexpected G3 page fault at addr %lx, fec %lx\n",
		       addr, fec);
		dune_dump_trap_frame(tf);
		dune_ret_from_user(-EFAULT);
	} else {
		ret = dune_vm_lookup(pgroot, (void *) addr, CREATE_NORMAL, &pte);
		assert(!ret);
		*pte = PTE_P | PTE_W | PTE_ADDR(dune_va_to_pa((void *) addr));
	}
}

int check_extent(const void *ptr, size_t len)
{
	if (!mem_ref_is_safe(ptr, len)) {
		printf("sandbox: mem ref with addr %p, len %lx is out of range\n",
		       ptr, len);
		return -EFAULT;
	}

	return 0;
}

int check_string(const void *ptr)
{
	void *pos;
	size_t maxlen;

	if ((uintptr_t) ptr < APP_MAX_ELF_VADDR)
		maxlen = APP_MAX_ELF_VADDR - ((uintptr_t) ptr);
	else if ((uintptr_t) ptr >= mmap_base) {
		if ((uintptr_t) ptr >= mmap_base + APP_MMAP_LEN)
			goto fault;
		maxlen = mmap_base + APP_MMAP_LEN - ((uintptr_t) ptr);
	} else
		goto fault;

	pos = memchr(ptr, 0, maxlen);
	if (!pos)
		goto fault;

	return 0;

fault:
	printf("str ref addr %p is out of range\n", ptr);
	return -EFAULT;
}

void boxer_register_syscall_monitor(boxer_syscall_cb cb)
{
	_syscall_monitor = cb;
}

void do_enter_thread(struct dune_tf *tf)
{
	long rc;

	rc = dune_jump_to_user(tf);

	syscall(SYS_exit, rc);
}

void thread_entry(unsigned long sp, unsigned long rc, struct dune_tf *t)
{
	struct dune_tf tf;
	unsigned char *stack;
	int stacklen = PGSIZE * 10;

	dune_enter();

	/* fork */
	if (t->rip == 0)
		return;

        memset(&tf, 0, sizeof(struct dune_tf));
        tf.rip = t->rip;
	tf.rax = rc;
	tf.rsp = sp;

	stack = dune_mmap(NULL, stacklen, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED) {
		dune_printf("Can't alloc stack\n");
		abort();
	}

        dune_vm_map_phys(pgroot, stack, stacklen, (void*) dune_va_to_pa(stack),
	                         PERM_R | PERM_W);

	stack += stacklen;

	asm volatile ("mov %0, %%rdi\n\t"
		      "mov %1, %%rsp\n\t"
		      "call do_enter_thread\n\t"
		      : 
		      : "r" (&tf), "r" (stack)
		      );

	abort();
}

static void map_stack(void)
{
	unsigned long sp;
	char buf[4096 * 100];
	int rc, fd;
	char *p, *p2;

	asm ("mov %%rsp, %0" : "=r" (sp));

	if ((fd = open("/proc/self/maps", O_RDONLY)) == -1)
		err(1, "open()");

	rc = read(fd, buf, sizeof(buf) - 1);
	if (rc < 0)
		err(1, "read()");

	if (rc == (sizeof(buf) - 1))
		errx(1, "need more space dude");

	buf[rc] = 0;

	close(fd);

	p = buf;

	while (*p) {
		unsigned long start, end;

		p2 = strchr(p, '\n');
		if (*p2)
			*p2++ = 0;
		else
			p2 = p + strlen(p);

		if (sscanf(p, "%lx-%lx ", &start, &end) != 2)
			errx(1, "sscanf()");

		if (sp >= start && sp < end) {
//			printf("SP %lx start %lx end %lx\n", sp, start, end);
			dune_map_ptr((void*) start, end - start);
			return;
		}

		p = p2;
	}

	errx(1, "stack not found");
}

static void *pthread_entry(void *arg)
{
	struct thread_arg *a = arg;
	struct dune_tf *tf = a->ta_tf;
	struct dune_tf child_tf;
	int *tidp = NULL;
	pid_t tid;
	int flags = ARG0(tf);

	map_stack();

	dune_enter();

	tid = syscall(SYS_gettid);

	/* XXX validate */
	/* set up tls */
	if (flags & CLONE_SETTLS)
		dune_set_user_fs(ARG4(tf));

	if (flags & CLONE_PARENT_SETTID) {
		tidp = (int*) ARG2(tf);
		*tidp = tid;
	}

	if (flags & CLONE_CHILD_CLEARTID) {
		tidp = (int*) ARG3(tf);
		syscall(SYS_set_tid_address, tidp);
	}

	/* tell parent tid */
	pthread_mutex_lock(&a->ta_mtx);
	a->ta_tid = tid;
	pthread_mutex_unlock(&a->ta_mtx);
	pthread_cond_signal(&a->ta_cnd);

	/* enter thread */
	memcpy(&child_tf, tf, sizeof(child_tf));
        child_tf.rip = tf->rip;
	child_tf.rax = 0;
	child_tf.rsp = ARG1(tf);

	do_enter_thread(&child_tf);

	return NULL;
}

static long dune_pthread_create(struct dune_tf *tf)
{
	pthread_t pt;
	struct thread_arg arg;

	arg.ta_tf  = tf;
	arg.ta_tid = 0;

	if (pthread_cond_init(&arg.ta_cnd, NULL))
		return -1;

	if (pthread_mutex_init(&arg.ta_mtx, NULL))
		return -1;

	if (pthread_create(&pt, NULL, pthread_entry, &arg))
		return -1;

	pthread_mutex_lock(&arg.ta_mtx);
	if (arg.ta_tid == 0)
		pthread_cond_wait(&arg.ta_cnd, &arg.ta_mtx);
	pthread_mutex_unlock(&arg.ta_mtx);

	pthread_mutex_destroy(&arg.ta_mtx);
	pthread_cond_destroy(&arg.ta_cnd);

	return arg.ta_tid;
}

static long dune_clone(struct dune_tf *tf)
{
	long rc;
	void *ptr = NULL;
	unsigned long sp = ARG1(tf);
	unsigned long *x;
	struct dune_tf t;

	if (ARG1(tf) != 0)
		return dune_pthread_create(tf);

	if (ARG1(tf) == 0)
		t.rip = 0; /* fork */
	else
		memcpy(&t, tf, sizeof(t)); /* thread */

	ptr = &t;

	if (!sp)
		asm volatile ("movq %%rsp, %0" : "=a" (sp));

	/* XXX check stack */
	x = (unsigned long*) (sp - 80);
	*x = (unsigned long) ptr;

        asm volatile(
		     "movq %7,-80(%%rsp)\n\t"
		     "movq %2, %%rdi \n\t"
                     "movq %3, %%rsi \n\t"
                     "movq %4, %%rdx \n\t"
                     "movq %5, %%r10 \n\t"
                     "movq %6, %%r8 \n\t"
                     "vmcall \n\t"
		     "testq %%rax,%%rax\n\t"
		     "jz _thread_start\n\t"
                     "movq %%rax, %0 \n\t" 
		     "jmp _out\n\t"
		     "_thread_start:\n\t"
		     "movq %%rsp, %%rdi\n\t"
		     "movq %%rax, %%rsi\n\t"
		     "movq -80(%%rsp), %%rdx\n\t"
		     "call thread_entry\n\t"
		     "movq $0, %0\n\t"
		     "_out:\n\t"
		     : "=a" (rc) 
                     : "a" (SYS_clone), "r" (ARG0(tf)), "r" (ARG1(tf)),
                     "r" (ARG2(tf)), "r" (ARG3(tf)), "r" (ARG4(tf)), "r" (ptr)
                     : "rdi", "rsi", "rdx", "r10",                                               
                     "r8", "r9", "memory");

	if (rc < 0)
		return rc;

	return rc;
}

static int check_iovec(struct iovec *iov, int num)
{
	if (check_extent(iov, sizeof(*iov) * num))
		return -EFAULT;

	while (num--) {
		if (check_extent(iov->iov_base, iov->iov_len))
			return -EFAULT;

		iov++;
	}

	return 0;
}

static int syscall_check_params(struct dune_tf *tf)
{
	void *ptr = NULL;
	uint64_t len = 0;
	char *str = NULL;
	int err = 0;

	switch (tf->rax) {
	case SYS_uname:
		ptr = (void*) ARG0(tf);
		len = sizeof(struct utsname);
		break;

	case SYS_arch_prctl:
		if (ARG0(tf) == ARCH_GET_FS) {
			ptr = (void*) ARG1(tf);
			len = sizeof(unsigned long);
		}
		break;

	case SYS_access:
	case SYS_open:
	case SYS_unlink:
		str = (char*) ARG0(tf);
		break;

	case SYS_openat:
		str = (char*) ARG1(tf);
		break;

	case SYS_getdents:
	case SYS_read:
	case SYS_write:
	case SYS_connect:
	case SYS_bind:
		ptr = (void*) ARG1(tf);
		len = ARG2(tf);
		break;

	case SYS_writev:
	case SYS_readv:
		err = check_iovec((struct iovec*) ARG1(tf), ARG2(tf));
		break;

	case SYS_stat:
	case SYS_lstat:
		str = (char*) ARG0(tf);
	case SYS_fstat:
		ptr = (void*) ARG1(tf);
		len = sizeof(struct stat);
		break;

	case SYS_statfs:
		str = (char*) ARG0(tf);
		ptr = (void*) ARG1(tf);
		len = sizeof(struct statfs);
		break;

	case SYS_time:
		ptr = (void*) ARG0(tf);
		len = sizeof(time_t);
		break;

	case SYS_epoll_ctl:
		ptr = (void*) ARG3(tf);
		len = sizeof(struct epoll_event);
		break;

	case SYS_epoll_wait:
		ptr = (void*) ARG1(tf);
		len = ARG2(tf) * sizeof(struct epoll_event);
		break;

	case SYS_setsockopt:
		ptr = (void*) ARG3(tf);
		len = ARG4(tf);
		break;

	case SYS_accept:
		if (ARG2(tf)) {
			socklen_t *l = (socklen_t*) ARG2(tf);

			if (check_extent(l, sizeof(*l)))
				err = -EFAULT;
			else {
				len = *l;
				ptr = (void*) ARG1(tf);
				assert(ptr);
			}
		}
		break;

	case SYS_fcntl:
		switch (ARG1(tf)) {
		case F_DUPFD:
		case F_DUPFD_CLOEXEC:
		case F_GETFD:
		case F_SETFD:
		case F_GETFL:
		case F_SETFL:
		case F_GETOWN:
		case F_SETOWN:
			break;

		case F_SETLKW:
		case F_GETLK:
		case F_SETLK:
			ptr = (void*) ARG2(tf);
			len = sizeof(struct flock);
			break;

		default:
			err = -EFAULT;
			break;
		}
		break;

	case SYS_ioctl:
		/* XXX unreliable */
		if (_IOC_DIR(ARG1(tf))) {
			ptr = (void*) ARG2(tf);
			len = _IOC_SIZE(ARG1(tf));
		}
		break;

	case SYS_setgroups:
		ptr = (void*) ARG1(tf);
		len = sizeof(gid_t) * ARG0(tf);
		break;

	case SYS_rt_sigaction:
		ptr = (void*) ARG1(tf);
		len = sizeof(struct sigaction);

		if (ARG2(tf)) {
			if (check_extent((void*) ARG2(tf), len)) {
				tf->rax = -EFAULT;
				return -1;
			}
		}
		break;

	/* umm_ checks for correctness */
	case SYS_brk:
		break;

	case SYS_mprotect:
	case SYS_munmap:
	case SYS_mmap:
//		ptr = (void*) ARG0(tf);
//		len = ARG1(tf);
		break;

	case SYS_getcwd:
		ptr = (void*) ARG0(tf);
		len = ARG1(tf);
		break;

	case SYS_getrlimit:
		ptr = (void*) ARG1(tf);
		len = sizeof(struct rlimit);
		break;

	case SYS_sendfile:
		ptr = (void*) ARG2(tf);
		len = sizeof(off_t);
		break;

	case SYS_getuid:
	case SYS_setuid:
	case SYS_getgid:
	case SYS_setgid:
	case SYS_getpid:
	case SYS_epoll_create:
	case SYS_dup2:
	case SYS_socket:
	case SYS_shutdown:
	case SYS_listen:
	case SYS_lseek:
		break;

	/* XXX - doesn't belong here */
	case SYS_close:
		if (ARG0(tf) < 3) {
			tf->rax = 0;
			return -1;
		}
		break;

	case SYS_execve:
	{
                char *p = (char*)ARG0(tf);
                
                if (check_string(p)) {
                        err = -EFAULT;
                        break;
                }

                // XXX: Check arrays

		break;
	}

	default:
#if 0
		{
			static FILE *_out;

			if (!_out)
				_out = fopen("/tmp/syscall.log", "w");

			fprintf(_out, "Syscall %d\n", tf->rax);
			fflush(_out);
		}
#endif
		break;
	}

	if (ptr != NULL && len != 0 && check_extent(ptr, len))
		err = -EFAULT;

	if (str != NULL && check_string(str))
		err = -EFAULT;

	if (err) {
		tf->rax = err;
		return -1;
	}

	return 0;
}

static int syscall_allow(struct dune_tf *tf)
{
	if (!_syscall_monitor) {
		tf->rax = -EPERM;
		return 0;
	}

	return _syscall_monitor(tf);
}

static void syscall_do_foreal(struct dune_tf *tf)
{
	switch (tf->rax) {
	case SYS_arch_prctl:
		switch (ARG0(tf)) {
		case ARCH_GET_FS:
			*((unsigned long*) ARG1(tf)) = dune_get_user_fs();
			tf->rax = 0;
			break;

		case ARCH_SET_FS:
			dune_set_user_fs(ARG1(tf));
			tf->rax = 0;
			break;

		default:
			tf->rax = -EINVAL;
			break;
		}
		break;

	case SYS_brk:
		tf->rax = umm_brk((unsigned long) ARG0(tf));
		break;

	case SYS_mmap:
		tf->rax = (unsigned long) umm_mmap((void *) ARG0(tf),
			(size_t) ARG1(tf), (int) ARG2(tf), (int) ARG3(tf),
			(int) ARG4(tf), (off_t) ARG5(tf));
		break;

	case SYS_mprotect:
		tf->rax = umm_mprotect((void *) ARG0(tf),
				       (size_t) ARG1(tf),
				       ARG2(tf));
		break;

	case SYS_munmap:
		tf->rax = umm_munmap((void *) ARG0(tf), (size_t) ARG1(tf)); 
		break;

	case SYS_mremap:
		tf->rax = (unsigned long) umm_mremap((void*) ARG0(tf),
				(size_t) ARG1(tf), (size_t) ARG2(tf),
				(int) ARG3(tf), (void*) ARG4(tf));
		break;

	case SYS_shmat:
		tf->rax = (unsigned long) umm_shmat((int) ARG0(tf),
				(void*) ARG1(tf), (int) ARG2(tf));
		break;

	case SYS_clone:
		tf->rax = dune_clone(tf);
		break;

        case SYS_execve:
                tf->rax = exec_execev((const char *)ARG0(tf),
                                      (char **const)ARG1(tf),
                                      (char **const)ARG2(tf));
                break;


	/* ignore signals for now */
	case SYS_rt_sigaction:
	case SYS_rt_sigprocmask:
		tf->rax = 0;
		break;

	case SYS_exit_group:
	case SYS_exit:
		dune_ret_from_user(ARG0(tf));
		break;

	default:
		dune_passthrough_syscall(tf);
		break;
	}
}

static void syscall_do(struct dune_tf *tf)
{
	int need_lock = 0;

	switch (tf->rax) {
	case SYS_mmap:
	case SYS_mprotect:
	case SYS_munmap:
		need_lock = 1;
		break;
	}

	if (need_lock && pthread_mutex_lock(&_syscall_mtx))
		err(1, "pthread_mutex_lock()");

	syscall_do_foreal(tf);

	if (need_lock && pthread_mutex_unlock(&_syscall_mtx))
		err(1, "pthread_mutex_unlock()");
}

static void syscall_handler(struct dune_tf *tf)
{
//	printf("Syscall No. %d\n", tf->rax);

	if (syscall_check_params(tf) == -1)
		return;

	if (!syscall_allow(tf))
		return;

	syscall_do(tf);
}

int trap_init(void)
{
	if (pthread_mutex_init(&_syscall_mtx, NULL))
		return -1;

	dune_register_pgflt_handler(pgflt_handler);
	dune_register_syscall_handler(&syscall_handler);

	return 0;
}
