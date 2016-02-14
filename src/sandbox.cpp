#define LOG_PREFIX "sandbox"

#include<cstdio>
#include<cstdlib>
#include<cstdint>
#include<cstddef>
#include<cerrno>
#include<cstring>
#include<unordered_map>
#include<unistd.h>
#include<sys/wait.h>
#include<sys/time.h>
#include<sys/prctl.h>
#include<sys/ptrace.h>
#include<sys/syscall.h>
#include<sys/signal.h>
#include<sys/user.h>
#include<linux/seccomp.h>
#include<linux/filter.h>
#include<linux/audit.h>
#include<libcgroup.h>
#include<uv.h>

#include"utils.h"
#include"sandbox.h"
#include"core.h"

/*
int Sandbox::start() {

    cgroup_set_value_uint64(memcg,"memory.swappiness",0);
    cgroup_set_value_uint64(memcg,"memory.oom_control",1);
    cgroup_set_value_uint64(memcg,"memory.limit_in_bytes",65536 * 1024);
    cgroup_create_cgroup(cg,0);


    if((child_pid = fork()) == 0) {
	ptrace(PTRACE_TRACEME, 0, NULL, NULL);
	kill(getpid(), SIGSTOP);

	if(install_filter()) {
	    printf("test\n");
	    _exit(-1);
	}

	char path[PATH_MAX + 1];
	strncpy(path, exepath.c_str(), sizeof(path));
	char *argv[] = {path, NULL};
	char *envp[] = {NULL};
	execve(argv[0], argv, envp);
	_exit(0);
    }
    if(trace_loop()) {
	ERR("Trace Error\n");
    }
    return 0;
}

int main(int argc, char *argv[]) {
    cgroup_init();
    auto cg = cgroup_new_cgroup("hypex");
    auto memcg = cgroup_add_controller(cg, "memory");

    Sandbox box(argv[1], memcg);
    box.start();

    cgroup_delete_cgroup(cg, 0);
    cgroup_free(&cg);
    return 0;
}
*/

static uv_signal_t sigchld_uvsig;

static void sigchld_callback(uv_signal_t *uvsig, int signo) {
    siginfo_t siginfo;
    siginfo.si_pid = 0;
    while(!waitid(P_ALL, 0, &siginfo,
	WEXITED | WSTOPPED | WCONTINUED | WNOHANG)) {
	if(siginfo.si_pid == 0) {
	    break;
	}
	Sandbox::update_states(&siginfo);
    }
}

void sandbox_init() {
    cgroup_init();
    uv_signal_init(core_uvloop, &sigchld_uvsig);
    uv_signal_start(&sigchld_uvsig, sigchld_callback, SIGCHLD);
}

unsigned long Sandbox::last_sandbox_id = 0;
std::unordered_map<int, Sandbox*> Sandbox::run_map;

Sandbox::Sandbox(const std::string &_exepath)
    : id(++last_sandbox_id), state(SANDBOX_STATE_INIT), exepath(_exepath)
{
    char cg_name[NAME_MAX + 1];

    snprintf(cg_name, sizeof(cg_name), "hypex_%lu", id);
    if((cg = cgroup_new_cgroup(cg_name)) == NULL) {
	throw SandboxException("Create cgroup failed.");
    }
    if((memcg = cgroup_add_controller(cg, "memory")) == NULL) {
	throw SandboxException("Create memory cgroup failed.");
    }
}

Sandbox::~Sandbox() {
    cgroup_delete_cgroup(cg, 1);
    cgroup_free(&cg);
}

void Sandbox::start() {
    INFO("Start task \"%s\".\n", exepath.c_str());
    if((child_pid = fork()) == 0) {
	ptrace(PTRACE_TRACEME, 0, NULL, NULL);
	kill(getpid(), SIGSTOP);

	if(install_filter()) {
	    _exit(0);
	}

	char path[PATH_MAX + 1];
	strncpy(path, exepath.c_str(), sizeof(path));
	char *argv[] = {path, NULL};
	char *envp[] = {NULL};
	execve(path, argv, envp);
	_exit(0);
    }

    state = SANDBOX_STATE_PRERUN;
    run_map[child_pid] = this;
}

void Sandbox::update_states(siginfo_t *siginfo) {
    auto sdbx_it = run_map.find(siginfo->si_pid);
    if(sdbx_it != run_map.end()) {
	auto sdbx = sdbx_it->second;
	try {
	    sdbx->update_state(siginfo);
	} catch(SandboxException &e) {
	    sdbx->terminate();   
	}
    }
}

void Sandbox::update_state(siginfo_t *siginfo) {
    if(state == SANDBOX_STATE_PRERUN) {
	if(siginfo->si_code != CLD_TRAPPED || siginfo->si_status != SIGSTOP) {
	    throw SandboxException("Trace task failed.");
	}
	if(ptrace(PTRACE_SETOPTIONS, child_pid, NULL,
	    PTRACE_O_EXITKILL | PTRACE_O_TRACEEXIT | PTRACE_O_TRACESECCOMP |
	    PTRACE_O_TRACESYSGOOD)) {
	    throw SandboxException("Trace task failed.");
	}
	kill(child_pid, SIGCONT);
	ptrace(PTRACE_CONT, child_pid, NULL, NULL);
	state = SANDBOX_STATE_RUNNING;

    } else if(state == SANDBOX_STATE_RUNNING) {
	if(siginfo->si_code == CLD_EXITED) {
	    statistic(false);
	    return;
	}
	if(siginfo->si_code == CLD_DUMPED || siginfo->si_code == CLD_KILLED) {
	    statistic(true);
	    return;
	}
	if(siginfo->si_code != CLD_TRAPPED) {
	    throw SandboxException("Unexpected signal.");
	    return;
	}

	if(siginfo->si_status == (SIGTRAP | (PTRACE_EVENT_SECCOMP << 8))) {
	    unsigned long nr;

	    if(ptrace(PTRACE_GETEVENTMSG, child_pid ,NULL, &nr)) {
		throw SandboxException("PTRACE_GETEVENTMSG failed.");
		return;
	    }
	    switch(nr) {
		case __NR_rt_sigprocmask:
		{
		    char path[PATH_MAX + 1];
		    int mem_fd;
		    struct user_regs_struct regs;
		    sigset_t sigset;

		    snprintf(path, sizeof(path), "/proc/%d/mem", child_pid);
		    if((mem_fd = open(path, O_RDWR | O_CLOEXEC)) < 0) {
			throw SandboxException("Can't open /proc/[pid]/mem");
		    }

		    ptrace(PTRACE_GETREGS, child_pid, NULL, &regs);
		    if(pread64(mem_fd, &sigset, sizeof(sigset), regs.rsi)
			!= sizeof(sigset)) {
			throw SandboxException("Read sigprocmask failed.");
		    }
		    sigdelset(&sigset,SIGVTALRM);
		    if(pwrite64(mem_fd, &sigset, sizeof(sigset), regs.rsi)
			!= sizeof(sigset)) {
			throw SandboxException("Write sigprocmask failed.");
		    }

		    close(mem_fd);
		    break;
		}
		default:
		    throw SandboxException("Unexpected signal.");
		    return;
	    }
	    ptrace(PTRACE_CONT, child_pid, NULL, NULL);

	} else {
	    ptrace(PTRACE_CONT, child_pid, NULL, NULL);
	}
    }
}

void Sandbox::statistic(bool exit_error) {
    state = SANDBOX_STATE_STOP;
    run_map.erase(child_pid);
    INFO("Task finished.\n");
}

void Sandbox::terminate() {
    kill(child_pid, SIGKILL);
}

int Sandbox::install_filter() {
    unsigned int upper_nr_limit = 0x40000000 - 1;
    struct sock_filter filter[] = {
	//get arch
	BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
	    (offsetof(struct seccomp_data, arch))),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, AUDIT_ARCH_X86_64, 0, 2),
	//get syscall nr
	BPF_STMT(BPF_LD + BPF_W + BPF_ABS, (offsetof(struct seccomp_data, nr))),
	BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, upper_nr_limit, 0, 1),
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_KILL),

	//prctl
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_prctl, 0, 4),
	BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
	    (offsetof(struct seccomp_data, args[0]))),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, PR_SET_SECCOMP, 0, 1),
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ERRNO | EINVAL),
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

	//seccomp
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_seccomp, 0, 1),
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ERRNO | EINVAL),

	//rt_sigaction
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_rt_sigaction, 0, 4),
	BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
	    (offsetof(struct seccomp_data, args[0]))),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, SIGVTALRM, 0, 1),
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ERRNO | EINVAL),
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

	//setitimer
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_setitimer, 0, 4),
	BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
	    (offsetof(struct seccomp_data, args[0]))),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ITIMER_VIRTUAL, 0, 1),
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ERRNO | EINVAL),
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

	//rt_sigprocmask
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_rt_sigprocmask, 0, 1),
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRACE | __NR_rt_sigprocmask),

	//other
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {
	.len = (unsigned short)(sizeof(filter) / sizeof(*filter)),
	.filter = filter,
    };
    if(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
	return -1;
    }
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0);
}