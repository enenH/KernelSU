#include "linux/capability.h"
#include "linux/cred.h"
#include "linux/dcache.h"
#include "linux/err.h"
#include "linux/init.h"
#include "linux/init_task.h"
#include "linux/kernel.h"
#include "linux/kprobes.h"
#include "linux/lsm_hooks.h"
#include "linux/nsproxy.h"
#include "linux/path.h"
#include "linux/printk.h"
#include "linux/uaccess.h"
#include "linux/uidgid.h"
#include "linux/version.h"
#include "linux/mount.h"

#include "linux/fs.h"
#include "linux/namei.h"
#include "linux/rcupdate.h"

#include "allowlist.h"
#include "arch.h"
#include "core_hook.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "ksud.h"
#include "manager.h"
#include "selinux/selinux.h"
#include "uid_observer.h"
#include "kernel_compat.h"

static bool ksu_module_mounted = false;

extern int handle_sepolicy(unsigned long arg3, void __user *arg4);

static inline bool is_allow_su()
{
	if (is_manager()) {
		// we are manager, allow!
		return true;
	}
	return ksu_is_allow_uid(current_uid().val);
}

static inline bool is_isolated_uid(uid_t uid)
{
#define FIRST_ISOLATED_UID 99000
#define LAST_ISOLATED_UID 99999
#define FIRST_APP_ZYGOTE_ISOLATED_UID 90000
#define LAST_APP_ZYGOTE_ISOLATED_UID 98999
	uid_t appid = uid % 100000;
	return (appid >= FIRST_ISOLATED_UID && appid <= LAST_ISOLATED_UID) ||
	       (appid >= FIRST_APP_ZYGOTE_ISOLATED_UID &&
		appid <= LAST_APP_ZYGOTE_ISOLATED_UID);
}

static struct group_info root_groups = { .usage = ATOMIC_INIT(2) };

static void setup_groups(struct root_profile *profile, struct cred *cred)
{
	if (profile->groups_count > KSU_MAX_GROUPS) {
		pr_warn("Failed to setgroups, too large group: %d!\n",
			profile->uid);
		return;
	}

	if (profile->groups_count == 1 && profile->groups[0] == 0) {
		// setgroup to root and return early.
		if (cred->group_info)
			put_group_info(cred->group_info);
		cred->group_info = get_group_info(&root_groups);
		return;
	}

	u32 ngroups = profile->groups_count;
	struct group_info *group_info = groups_alloc(ngroups);
	if (!group_info) {
		pr_warn("Failed to setgroups, ENOMEM for: %d\n", profile->uid);
		return;
	}

	int i;
	for (i = 0; i < ngroups; i++) {
		gid_t gid = profile->groups[i];
		kgid_t kgid = make_kgid(current_user_ns(), gid);
		if (!gid_valid(kgid)) {
			pr_warn("Failed to setgroups, invalid gid: %d\n", gid);
			put_group_info(group_info);
			return;
		}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
		group_info->gid[i] = kgid;
#else
		GROUP_AT(group_info, i) = kgid;
#endif
	}

	groups_sort(group_info);
	set_groups(cred, group_info);
}

void escape_to_root(void)
{
	struct cred *cred;

	cred = (struct cred *)__task_cred(current);

	if (cred->euid.val == 0) {
		pr_warn("Already root, don't escape!\n");
		return;
	}
	struct root_profile *profile = ksu_get_root_profile(cred->uid.val);

	cred->uid.val = profile->uid;
	cred->suid.val = profile->uid;
	cred->euid.val = profile->uid;
	cred->fsuid.val = profile->uid;

	cred->gid.val = profile->gid;
	cred->fsgid.val = profile->gid;
	cred->sgid.val = profile->gid;
	cred->egid.val = profile->gid;

	BUILD_BUG_ON(sizeof(profile->capabilities.effective) !=
		     sizeof(kernel_cap_t));

	// setup capabilities
	// we need CAP_DAC_READ_SEARCH becuase `/data/adb/ksud` is not accessible for non root process
	// we add it here but don't add it to cap_inhertiable, it would be dropped automaticly after exec!
	u64 cap_for_ksud =
		profile->capabilities.effective | CAP_DAC_READ_SEARCH;
	memcpy(&cred->cap_effective, &cap_for_ksud,
	       sizeof(cred->cap_effective));
	memcpy(&cred->cap_inheritable, &profile->capabilities.effective,
	       sizeof(cred->cap_inheritable));
	memcpy(&cred->cap_permitted, &profile->capabilities.effective,
	       sizeof(cred->cap_permitted));
	memcpy(&cred->cap_bset, &profile->capabilities.effective,
	       sizeof(cred->cap_bset));
	memcpy(&cred->cap_ambient, &profile->capabilities.effective,
	       sizeof(cred->cap_ambient));

	// disable seccomp
#if defined(CONFIG_GENERIC_ENTRY) &&                                           \
	LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	current_thread_info()->syscall_work &= ~SYSCALL_WORK_SECCOMP;
#else
	current_thread_info()->flags &= ~(TIF_SECCOMP | _TIF_SECCOMP);
#endif

#ifdef CONFIG_SECCOMP
	current->seccomp.mode = 0;
	current->seccomp.filter = NULL;
#else
#endif

	setup_groups(profile, cred);

	setup_selinux(profile->selinux_domain);
}

int ksu_handle_rename(struct dentry *old_dentry, struct dentry *new_dentry)
{
	if (!current->mm) {
		// skip kernel threads
		return 0;
	}

	if (current_uid().val != 1000) {
		// skip non system uid
		return 0;
	}

	if (!old_dentry || !new_dentry) {
		return 0;
	}

	// /data/system/packages.list.tmp -> /data/system/packages.list
	if (strcmp(new_dentry->d_iname, "packages.list")) {
		return 0;
	}

	char path[128];
	char *buf = dentry_path_raw(new_dentry, path, sizeof(path));
	if (IS_ERR(buf)) {
		pr_err("dentry_path_raw failed.\n");
		return 0;
	}

	if (strcmp(buf, "/system/packages.list")) {
		return 0;
	}
	pr_info("renameat: %s -> %s, new path: %s\n", old_dentry->d_iname,
		new_dentry->d_iname, buf);

	update_uid();

	return 0;
}
#include <linux/sched/mm.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <asm/fpsimd.h>
static inline unsigned long size_inside_page(unsigned long start,
					     unsigned long size)
{
	unsigned long sz;
	sz = PAGE_SIZE - (start & (PAGE_SIZE - 1));
	return min(sz, size);
}
static inline bool should_stop_iteration(void)
{
	if (need_resched())
		cond_resched();
	return fatal_signal_pending(current);
}

static inline int page_is_allowed(unsigned long pfn)
{
	return 1;
}
static inline int range_is_allowed(unsigned long pfn, unsigned long size)
{
	return 1;
}

static phys_addr_t translate_linear_address(struct mm_struct *mm, uintptr_t va)
{
	p4d_t *p4d;
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte;
	pud_t *pud;

	phys_addr_t page_addr;
	uintptr_t page_offset;

	pgd = pgd_offset(mm, va);
	if (pgd_none(*pgd) || pgd_bad(*pgd)) {
		return 0;
	}
	p4d = p4d_offset(pgd, va);
	if (p4d_none(*p4d) || p4d_bad(*p4d)) {
		return 0;
	}
	pud = pud_offset(p4d, va);
	if (pud_none(*pud) || pud_bad(*pud)) {
		return 0;
	}
	pmd = pmd_offset(pud, va);
	if (pmd_none(*pmd)) {
		return 0;
	}
	pte = pte_offset_kernel(pmd, va);
	if (pte_none(*pte)) {
		return 0;
	}
	if (!pte_present(*pte)) {
		return 0;
	}
	//页物理地址
	page_addr = (phys_addr_t)(pte_pfn(*pte) << PAGE_SHIFT);
	//页内偏移
	page_offset = va & (PAGE_SIZE - 1);

	return page_addr + page_offset;
}

static size_t read_process_memory(pid_t pid, uintptr_t addr, void *buffer,
				  size_t size)
{
	struct task_struct *task;
	struct mm_struct *mm;
	phys_addr_t pa;
	size_t sz, read = 0;
	void *ptr;
	char *bounce;
	int probe;

	task = find_get_task_by_vpid(pid);
	if (!task)
		return 0;

	mm = get_task_mm(task);
	if (!mm || IS_ERR(mm))
		goto put_task_struct;

	bounce = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!bounce)
		goto mmput;

	while (size > 0) {
		sz = size_inside_page(addr, size);

		pa = translate_linear_address(mm, addr);
		if (!pa)
			goto cc;

		ptr = xlate_dev_mem_ptr(pa);
		if (!ptr)
			goto cc;

		probe = copy_from_kernel_nofault(bounce, ptr, sz);
		unxlate_dev_mem_ptr(pa, ptr);
		if (probe)
			goto cc;

		if (copy_to_user(buffer, bounce, sz))
			goto cc;

		read += sz;
	cc:
		size -= sz;
		addr += sz;
		buffer += sz;
		if (should_stop_iteration())
			break;
	}
	kfree(bounce);
mmput:
	mmput(mm);
put_task_struct:
	put_task_struct(task);
	return read;
}

static size_t write_process_memory(pid_t pid, uintptr_t addr, void *buffer,
				   size_t size)
{
	struct task_struct *task;
	struct mm_struct *mm;
	phys_addr_t pa;
	size_t sz, written = 0;
	void *ptr;
	unsigned long copied;

	task = find_get_task_by_vpid(pid);
	if (!task)
		return 0;

	mm = get_task_mm(task);
	if (!mm || IS_ERR(mm))
		goto put_task_struct;

	while (size > 0) {
		sz = size_inside_page(addr, size);

		pa = translate_linear_address(mm, addr);
		if (!pa)
			goto cc;

		ptr = xlate_dev_mem_ptr(pa);
		if (!ptr)
			goto cc;

		copied = copy_from_user(ptr, buffer, sz);
		unxlate_dev_mem_ptr(pa, ptr);
		if (copied)
			goto cc;

		written += sz;
	cc:
		size -= sz;
		addr += sz;
		buffer += sz;
		if (should_stop_iteration())
			break;
	}
	mmput(mm);
put_task_struct:
	put_task_struct(task);
	return written;
}

#pragma pack(1)
struct My_User_Regs_struct {
	__uint128_t vregs[32];
	u32 fpsr;
	u32 fpcr;

	u64 xregs[31];
	u64 sp;
	u64 pc;
	u64 pstate;

	bool vregs_set[32];
	bool fpsr_set;
	bool fpcr_set;
	bool xregs_set[31];
	bool sp_set;
	bool pc_set;
	bool pstate_set;
};

struct My_HW_struct {
	struct perf_event *hbp;
	struct My_User_Regs_struct r;
};
#pragma pack()

struct My_HW_struct my_hw_struct[600];
static int current_index = 0;

static void sample_hbp_handler(struct perf_event *bp,
			       struct perf_sample_data *data,
			       struct pt_regs *regs)
{
	int i;
	bool do_vregs_set = false;
	struct My_User_Regs_struct *s;
	struct user_fpsimd_state newstate;
	struct task_struct *target;

	if (bp->attr.sample_regs_user < 0 || bp->attr.sample_regs_user >= 600) {
		pr_info("sample_hbp_handler: invalid index\n");
		return;
	}

	s = &my_hw_struct[bp->attr.sample_regs_user].r;

	for (i = 0; i < 32; i++) {
		if (s->vregs_set[i]) {
			do_vregs_set = true;
			break;
		}
	}
	if (s->fpsr_set) {
		do_vregs_set = true;
	}
	if (s->fpcr_set) {
		do_vregs_set = true;
	}

	if (do_vregs_set) {
		target = current;
		sve_sync_to_fpsimd(target);
		newstate = target->thread.uw.fpsimd_state;
		for (i = 0; i < 32; i++) {
			if (s->vregs_set[i]) {
				newstate.vregs[i] = s->vregs[i];
			}
		}
		if (s->fpsr_set) {
			newstate.fpsr = s->fpsr;
		}
		if (s->fpcr_set) {
			newstate.fpcr = s->fpcr;
		}

		target->thread.uw.fpsimd_state = newstate;

		sve_sync_from_fpsimd_zeropad(target);
		fpsimd_flush_task_state(target);
	}

	for (i = 0; i < 31; i++) {
		if (s->xregs_set[i]) {
			regs->regs[i] = s->xregs[i];
		}
	}
	if (s->sp_set) {
		regs->sp = s->sp;
	}
	if (s->pstate_set) {
		regs->pstate = s->pstate;
	}
	if (s->pc_set) {
		regs->pc = s->pc;
	} else {
		regs->pc += 4;
	}
}

static int my_register_breakpoint(struct perf_event **p_sample_hbp, pid_t pid,
				  u64 addr, u64 len, u32 type, u32 given_index)
{
	struct perf_event_attr attr;
	struct task_struct *task;

	task = find_get_task_by_vpid(pid);
	if (!task)
		return -1;

	//hw_breakpoint_init(&attr);
	ptrace_breakpoint_init(&attr);

	/*
		 * Initialise fields to sane defaults
		 * (i.e. values that will pass validation).

		 */
	attr.bp_addr = addr;
	attr.bp_len = len;
	attr.bp_type = type;
	attr.disabled = 0;
	attr.sample_regs_user = given_index;
	*p_sample_hbp = register_user_hw_breakpoint(&attr, sample_hbp_handler,
						   NULL, task);

	put_task_struct(task);

	if (IS_ERR((void __force *)*p_sample_hbp)) {
		int ret = PTR_ERR((void __force *)*p_sample_hbp);
		pr_info(KERN_INFO "register_user_hw_breakpoint failed: %d\n",
			ret);
		*p_sample_hbp = NULL;
		return ret;
	}
	return 0;
}

static int hack_handle_prctl(int option, unsigned long arg2, unsigned long arg3,
			     unsigned long arg4, unsigned long arg5)
{
	if (option <= KERNEL_SU_OPTION) {
		return 0;
	}
	//只给root用，防止被其他应用调用
	if (current_uid().val != 0) {
		return 0;
	}
	if (option == KERNEL_SU_OPTION + 1 || option == KERNEL_SU_OPTION + 2) {
		struct my_struct {
			void *pid;
			void *addr;
			void *buff;
			void *size;
			void *ret;
		} s;
		size_t ret;
		if (copy_from_user(&s, (void *)arg2, sizeof(s))) {
			pr_err("hack_handle_prctl: prctl copy error\n");
			return 0;
		}
		if (option == KERNEL_SU_OPTION + 1) {
			ret = read_process_memory((pid_t)(u64)s.pid,
						  (uintptr_t)s.addr,
						  (void *)s.buff,
						  (size_t)s.size);
		} else {
			ret = write_process_memory((pid_t)(u64)s.pid,
						   (uintptr_t)s.addr,
						   (void *)s.buff,
						   (size_t)s.size);
		}

		if (copy_to_user(s.ret, &ret, sizeof(ret))) {
			pr_err("hack_handle_prctl: prctl reply error\n");
		}
		return 0;
	}

	if (option == KERNEL_SU_OPTION + 3) {
		u32 *result = (u32 *)arg2;
		u32 reply_ok = KERNEL_SU_OPTION;
		if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
			pr_err("hack_handle_prctl: prctl reply error\n");
		}
		return 0;
	}

	if (option == KERNEL_SU_OPTION + 4) {
		struct my_struct {
			void *pid;
			void *addr;
			void *len;
			void *type;
			void *ret;
		} s;
		s64 ret;
		struct My_HW_struct *hw_struct;
		if (copy_from_user(&s, (void *)arg2, sizeof(s))) {
			pr_err("hack_handle_prctl: prctl copy error\n");
			return 0;
		}
		if (current_index >= 600 || current_index < 0) {
			pr_info("my_register_breakpoint: too many breakpoints\n");
			current_index = 0;
		}
		hw_struct = &my_hw_struct[current_index];

		memset(hw_struct, 0, sizeof(struct My_HW_struct));
		ret = my_register_breakpoint(&hw_struct->hbp, (pid_t)(u64)s.pid,
					     (u64)s.addr, (u64)s.len,
					     (u32)(u64)s.type, current_index);
		if (ret != -1) {
			ret = current_index;
		}
		if (copy_to_user(s.ret, &ret, sizeof(ret))) {
			pr_err("hack_handle_prctl: prctl reply error\n");
		}
		current_index++;
		return 0;
	}
	if (option == KERNEL_SU_OPTION + 5) {
		struct my_struct {
			void *index;
			void *regs_buff;
			void *ret;
		} s;
		struct My_User_Regs_struct *r;
		s64 ret = 0;

		if (copy_from_user(&s, (void *)arg2, sizeof(s))) {
			pr_err("hack_handle_prctl: prctl copy error\n");
			return 0;
		}
		if ((s64)(u64)s.index >= 600 || (s64)(u64)s.index < 0) {
			pr_info("my_register_breakpoint: invalid index\n");
			ret = -1;
			goto out5;
		}
		r = &my_hw_struct[(s64)s.index].r;
		if (copy_from_user(r, s.regs_buff,
				   sizeof(struct My_User_Regs_struct))) {
			pr_err("hack_handle_prctl: prctl copy regs error\n");
			ret = -1;
			goto out5;
		}
	out5:
		if (copy_to_user(s.ret, &ret, sizeof(ret))) {
			pr_err("hack_handle_prctl: prctl reply error\n");
		}
		return 0;
	}

	if (option == KERNEL_SU_OPTION + 6) {
		struct my_struct {
			void *index;
			void *ret;
		} s;
		s64 ret = 0;

		if (copy_from_user(&s, (void *)arg2, sizeof(s))) {
			pr_err("hack_handle_prctl: prctl copy error\n");
			return 0;
		}
		if ((s64)s.index >= 600 || (s64)s.index < 0) {
			pr_info("unregister_breakpoint: invalid index\n");
			ret = -1;
			goto out6;
		}
		if (!my_hw_struct[(s64)s.index].hbp) {
			pr_info("unregister_breakpoint: invalid hbp\n");
			ret = -1;
			goto out6;
		}
		unregister_hw_breakpoint(my_hw_struct[(s64)s.index].hbp);
		memset(&my_hw_struct[(s64)s.index], 0,
		       sizeof(struct My_HW_struct));
	out6:
		if (copy_to_user(s.ret, &ret, sizeof(ret))) {
			pr_err("hack_handle_prctl: prctl reply error\n");
		}
		return 0;
	}
	return 0;
}
int ksu_handle_prctl(int option, unsigned long arg2, unsigned long arg3,
		     unsigned long arg4, unsigned long arg5)
{
	hack_handle_prctl(option, arg2, arg3, arg4, arg5);
	// if success, we modify the arg5 as result!
	u32 *result = (u32 *)arg5;
	u32 reply_ok = KERNEL_SU_OPTION;

	if (KERNEL_SU_OPTION != option) {
		return 0;
	}

	// always ignore isolated app uid
	if (is_isolated_uid(current_uid().val)) {
		return 0;
	}

	static uid_t last_failed_uid = -1;
	if (last_failed_uid == current_uid().val) {
		return 0;
	}

#ifdef CONFIG_KSU_DEBUG
	pr_info("option: 0x%x, cmd: %ld\n", option, arg2);
#endif

	if (arg2 == CMD_BECOME_MANAGER) {
		// quick check
		if (is_manager()) {
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("become_manager: prctl reply error\n");
			}
			return 0;
		}
		if (ksu_is_manager_uid_valid()) {
#ifdef CONFIG_KSU_DEBUG
			pr_info("manager already exist: %d\n",
				ksu_get_manager_uid());
#endif	
			return 0;
		}

		// someone wants to be root manager, just check it!
		// arg3 should be `/data/user/<userId>/<manager_package_name>`
		char param[128];
		if (ksu_strncpy_from_user_nofault(param, arg3, sizeof(param)) ==
		    -EFAULT) {
#ifdef CONFIG_KSU_DEBUG
			pr_err("become_manager: copy param err\n");
#endif
			goto block;
		}

		// for user 0, it is /data/data
		// for user 999, it is /data/user/999
		const char *prefix;
		char prefixTmp[64];
		int userId = current_uid().val / 100000;
		if (userId == 0) {
			prefix = "/data/data";
		} else {
			snprintf(prefixTmp, sizeof(prefixTmp), "/data/user/%d",
				 userId);
			prefix = prefixTmp;
		}

		if (startswith(param, (char *)prefix) != 0) {
			pr_info("become_manager: invalid param: %s\n", param);
			goto block;
		}

		// stat the param, app must have permission to do this
		// otherwise it may fake the path!
		struct path path;
		if (kern_path(param, LOOKUP_DIRECTORY, &path)) {
			pr_err("become_manager: kern_path err\n");
			goto block;
		}
		uid_t inode_uid = path.dentry->d_inode->i_uid.val;
		path_put(&path);
		if (inode_uid != current_uid().val) {
			pr_err("become_manager: path uid != current uid\n");
			goto block;
		}
		char *pkg = param + strlen(prefix);
		pr_info("become_manager: param pkg: %s\n", pkg);

		bool success = become_manager(pkg);
		if (success) {
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("become_manager: prctl reply error\n");
			}
			return 0;
		}
	block:
		last_failed_uid = current_uid().val;
		return 0;
	}

	if (arg2 == CMD_GRANT_ROOT) {
		if (is_allow_su()) {
			pr_info("allow root for: %d\n", current_uid().val);
			escape_to_root();
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("grant_root: prctl reply error\n");
			}
		}
		return 0;
	}

	// Both root manager and root processes should be allowed to get version
	if (arg2 == CMD_GET_VERSION) {
		if (is_manager() || 0 == current_uid().val) {
			u32 version = KERNEL_SU_VERSION;
			if (copy_to_user(arg3, &version, sizeof(version))) {
				pr_err("prctl reply error, cmd: %lu\n", arg2);
			}
		}
		return 0;
	}

	if (arg2 == CMD_REPORT_EVENT) {
		if (0 != current_uid().val) {
			return 0;
		}
		switch (arg3) {
		case EVENT_POST_FS_DATA: {
			static bool post_fs_data_lock = false;
			if (!post_fs_data_lock) {
				post_fs_data_lock = true;
				pr_info("post-fs-data triggered\n");
				on_post_fs_data();
			}
			break;
		}
		case EVENT_BOOT_COMPLETED: {
			static bool boot_complete_lock = false;
			if (!boot_complete_lock) {
				boot_complete_lock = true;
				pr_info("boot_complete triggered\n");
			}
			break;
		}
		case EVENT_MODULE_MOUNTED: {
			ksu_module_mounted = true;
			pr_info("module mounted!\n");
			break;
		}
		default:
			break;
		}
		return 0;
	}

	if (arg2 == CMD_SET_SEPOLICY) {
		if (0 != current_uid().val) {
			return 0;
		}
		if (!handle_sepolicy(arg3, arg4)) {
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("sepolicy: prctl reply error\n");
			}
		}

		return 0;
	}

	if (arg2 == CMD_CHECK_SAFEMODE) {
		if (!is_manager() && 0 != current_uid().val) {
			return 0;
		}
		if (ksu_is_safe_mode()) {
			pr_warn("safemode enabled!\n");
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("safemode: prctl reply error\n");
			}
		}
		return 0;
	}

	if (arg2 == CMD_GET_ALLOW_LIST || arg2 == CMD_GET_DENY_LIST) {
		if (is_manager() || 0 == current_uid().val) {
			u32 array[128];
			u32 array_length;
			bool success =
				ksu_get_allow_list(array, &array_length,
						   arg2 == CMD_GET_ALLOW_LIST);
			if (success) {
				if (!copy_to_user(arg4, &array_length,
						  sizeof(array_length)) &&
				    !copy_to_user(arg3, array,
						  sizeof(u32) * array_length)) {
					if (copy_to_user(result, &reply_ok,
							 sizeof(reply_ok))) {
						pr_err("prctl reply error, cmd: %lu\n",
						       arg2);
					}
				} else {
					pr_err("prctl copy allowlist error\n");
				}
			}
		}
		return 0;
	}

	if (arg2 == CMD_UID_GRANTED_ROOT || arg2 == CMD_UID_SHOULD_UMOUNT) {
		if (is_manager() || 0 == current_uid().val) {
			uid_t target_uid = (uid_t)arg3;
			bool allow = false;
			if (arg2 == CMD_UID_GRANTED_ROOT) {
				allow = ksu_is_allow_uid(target_uid);
			} else if (arg2 == CMD_UID_SHOULD_UMOUNT) {
				allow = ksu_uid_should_umount(target_uid);
			} else {
				pr_err("unknown cmd: %lu\n", arg2);
			}
			if (!copy_to_user(arg4, &allow, sizeof(allow))) {
				if (copy_to_user(result, &reply_ok,
						 sizeof(reply_ok))) {
					pr_err("prctl reply error, cmd: %lu\n",
					       arg2);
				}
			} else {
				pr_err("prctl copy err, cmd: %lu\n", arg2);
			}
		}
		return 0;
	}

	// all other cmds are for 'root manager'
	if (!is_manager()) {
		last_failed_uid = current_uid().val;
		return 0;
	}

	// we are already manager
	if (arg2 == CMD_GET_APP_PROFILE) {
		struct app_profile profile;
		if (copy_from_user(&profile, arg3, sizeof(profile))) {
			pr_err("copy profile failed\n");
			return 0;
		}

		bool success = ksu_get_app_profile(&profile);
		if (success) {
			if (copy_to_user(arg3, &profile, sizeof(profile))) {
				pr_err("copy profile failed\n");
				return 0;
			}
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("prctl reply error, cmd: %lu\n", arg2);
			}
		}
		return 0;
	}

	if (arg2 == CMD_SET_APP_PROFILE) {
		struct app_profile profile;
		if (copy_from_user(&profile, arg3, sizeof(profile))) {
			pr_err("copy profile failed\n");
			return 0;
		}

		// todo: validate the params
		if (ksu_set_app_profile(&profile, true)) {
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
				pr_err("prctl reply error, cmd: %lu\n", arg2);
			}
		}
		return 0;
	}

	return 0;
}

static bool is_appuid(kuid_t uid)
{
#define PER_USER_RANGE 100000
#define FIRST_APPLICATION_UID 10000
#define LAST_APPLICATION_UID 19999

	uid_t appid = uid.val % PER_USER_RANGE;
	return appid >= FIRST_APPLICATION_UID && appid <= LAST_APPLICATION_UID;
}

static bool should_umount(struct path *path)
{
	if (!path) {
		return false;
	}

	if (current->nsproxy->mnt_ns == init_nsproxy.mnt_ns) {
		pr_info("ignore global mnt namespace process: %d\n",
			current_uid().val);
		return false;
	}

	if (path->mnt && path->mnt->mnt_sb && path->mnt->mnt_sb->s_type) {
		const char *fstype = path->mnt->mnt_sb->s_type->name;
		return strcmp(fstype, "overlay") == 0;
	}
	return false;
}

static void ksu_umount_mnt(struct path *path, int flags)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	int err = path_umount(path, flags);
	if (err) {
		pr_info("umount %s failed: %d\n", path->dentry->d_iname, err);
	}
#else
	// TODO: umount for non GKI kernel
#endif
}

static void try_umount(const char *mnt, bool check_mnt, int flags)
{
	struct path path;
	int err = kern_path(mnt, 0, &path);
	if (err) {
		return;
	}

	if (path.dentry != path.mnt->mnt_root) {
		// it is not root mountpoint, maybe umounted by others already.
		return;
	}

	// we are only interest in some specific mounts
	if (check_mnt && !should_umount(&path)) {
		return;
	}

	ksu_umount_mnt(&path, flags);
}

int ksu_handle_setuid(struct cred *new, const struct cred *old)
{
	// this hook is used for umounting overlayfs for some uid, if there isn't any module mounted, just ignore it!
	if (!ksu_module_mounted) {
		return 0;
	}

	if (!new || !old) {
		return 0;
	}

	kuid_t new_uid = new->uid;
	kuid_t old_uid = old->uid;

	if (0 != old_uid.val) {
		// old process is not root, ignore it.
		return 0;
	}

	if (!is_appuid(new_uid) || is_isolated_uid(new_uid.val)) {
		// pr_info("handle setuid ignore non application or isolated uid: %d\n", new_uid.val);
		return 0;
	}

	if (ksu_is_allow_uid(new_uid.val)) {
		// pr_info("handle setuid ignore allowed application: %d\n", new_uid.val);
		return 0;
	}

	if (!ksu_uid_should_umount(new_uid.val)) {
		return 0;
	} else {
#ifdef CONFIG_KSU_DEBUG
		pr_info("uid: %d should not umount!\n", current_uid().val);
#endif
	}

	// check old process's selinux context, if it is not zygote, ignore it!
	// because some su apps may setuid to untrusted_app but they are in global mount namespace
	// when we umount for such process, that is a disaster!
	bool is_zygote_child = is_zygote(old->security);
	if (!is_zygote_child) {
		pr_info("handle umount ignore non zygote child: %d\n",
			current->pid);
		return 0;
	}
	// umount the target mnt
	pr_info("handle umount for uid: %d, pid: %d\n", new_uid.val,
		current->pid);

	// fixme: use `collect_mounts` and `iterate_mount` to iterate all mountpoint and
	// filter the mountpoint whose target is `/data/adb`
	try_umount("/system", true, 0);
	try_umount("/vendor", true, 0);
	try_umount("/product", true, 0);
	try_umount("/data/adb/modules", false, MNT_DETACH);

	// try umount ksu temp path
	try_umount("/debug_ramdisk", false, MNT_DETACH);
	try_umount("/sbin", false, MNT_DETACH);

	return 0;
}

// Init functons

static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
	struct pt_regs *real_regs = (struct pt_regs *)PT_REGS_PARM1(regs);
#else
	struct pt_regs *real_regs = regs;
#endif
	int option = (int)PT_REGS_PARM1(real_regs);
	unsigned long arg2 = (unsigned long)PT_REGS_PARM2(real_regs);
	unsigned long arg3 = (unsigned long)PT_REGS_PARM3(real_regs);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
	// PRCTL_SYMBOL is the arch-specificed one, which receive raw pt_regs from syscall
	unsigned long arg4 = (unsigned long)PT_REGS_SYSCALL_PARM4(real_regs);
#else
	// PRCTL_SYMBOL is the common one, called by C convention in do_syscall_64
	// https://elixir.bootlin.com/linux/v4.15.18/source/arch/x86/entry/common.c#L287
	unsigned long arg4 = (unsigned long)PT_REGS_CCALL_PARM4(real_regs);
#endif
	unsigned long arg5 = (unsigned long)PT_REGS_PARM5(real_regs);

	return ksu_handle_prctl(option, arg2, arg3, arg4, arg5);
}

static struct kprobe prctl_kp = {
	.symbol_name = PRCTL_SYMBOL,
	.pre_handler = handler_pre,
};

static int renameat_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
	// https://elixir.bootlin.com/linux/v5.12-rc1/source/include/linux/fs.h
	struct renamedata *rd = PT_REGS_PARM1(regs);
	struct dentry *old_entry = rd->old_dentry;
	struct dentry *new_entry = rd->new_dentry;
#else
	struct dentry *old_entry = (struct dentry *)PT_REGS_PARM2(regs);
	struct dentry *new_entry = (struct dentry *)PT_REGS_CCALL_PARM4(regs);
#endif

	return ksu_handle_rename(old_entry, new_entry);
}

static struct kprobe renameat_kp = {
	.symbol_name = "vfs_rename",
	.pre_handler = renameat_handler_pre,
};

__maybe_unused int ksu_kprobe_init(void)
{
	int rc = 0;
	rc = register_kprobe(&prctl_kp);

	if (rc) {
		pr_info("prctl kprobe failed: %d.\n", rc);
		return rc;
	}

	rc = register_kprobe(&renameat_kp);
	pr_info("renameat kp: %d\n", rc);

	return rc;
}

__maybe_unused int ksu_kprobe_exit(void)
{
	unregister_kprobe(&prctl_kp);
	unregister_kprobe(&renameat_kp);
	return 0;
}

static int ksu_task_prctl(int option, unsigned long arg2, unsigned long arg3,
			  unsigned long arg4, unsigned long arg5)
{
	ksu_handle_prctl(option, arg2, arg3, arg4, arg5);
	return -ENOSYS;
}
// kernel 4.4 and 4.9
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
static int ksu_key_permission(key_ref_t key_ref, const struct cred *cred,
			      unsigned perm)
{
	if (init_session_keyring != NULL) {
		return 0;
	}
	if (strcmp(current->comm, "init")) {
		// we are only interested in `init` process
		return 0;
	}
	init_session_keyring = cred->session_keyring;
	pr_info("kernel_compat: got init_session_keyring\n");
	return 0;
}
#endif
static int ksu_inode_rename(struct inode *old_inode, struct dentry *old_dentry,
			    struct inode *new_inode, struct dentry *new_dentry)
{
	return ksu_handle_rename(old_dentry, new_dentry);
}

static int ksu_task_fix_setuid(struct cred *new, const struct cred *old,
			       int flags)
{
	return ksu_handle_setuid(new, old);
}

static struct security_hook_list ksu_hooks[] = {
	LSM_HOOK_INIT(task_prctl, ksu_task_prctl),
	LSM_HOOK_INIT(inode_rename, ksu_inode_rename),
	LSM_HOOK_INIT(task_fix_setuid, ksu_task_fix_setuid),
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	LSM_HOOK_INIT(key_permission, ksu_key_permission)
#endif
};

void __init ksu_lsm_hook_init(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	security_add_hooks(ksu_hooks, ARRAY_SIZE(ksu_hooks), "ksu");
#else
	// https://elixir.bootlin.com/linux/v4.10.17/source/include/linux/lsm_hooks.h#L1892
	security_add_hooks(ksu_hooks, ARRAY_SIZE(ksu_hooks));
#endif
}

void __init ksu_core_init(void)
{
#ifndef MODULE
	pr_info("ksu_lsm_hook_init\n");
	ksu_lsm_hook_init();
#else
	pr_info("ksu_kprobe_init\n");
	ksu_kprobe_init();
#endif
}

void ksu_core_exit(void)
{
#ifndef MODULE
	pr_info("ksu_kprobe_exit\n");
	ksu_kprobe_exit();
#endif
}
