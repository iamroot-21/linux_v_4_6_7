/*
 *  linux/kernel/fork.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also entry.S and others).
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/memory.c': 'copy_page_range()'
 */

#include <linux/slab.h>
#include <linux/init.h>
#include <linux/unistd.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/completion.h>
#include <linux/personality.h>
#include <linux/mempolicy.h>
#include <linux/sem.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/iocontext.h>
#include <linux/key.h>
#include <linux/binfmts.h>
#include <linux/mman.h>
#include <linux/mmu_notifier.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/vmacache.h>
#include <linux/nsproxy.h>
#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/cgroup.h>
#include <linux/security.h>
#include <linux/hugetlb.h>
#include <linux/seccomp.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
#include <linux/jiffies.h>
#include <linux/futex.h>
#include <linux/compat.h>
#include <linux/kthread.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/rcupdate.h>
#include <linux/ptrace.h>
#include <linux/mount.h>
#include <linux/audit.h>
#include <linux/memcontrol.h>
#include <linux/ftrace.h>
#include <linux/proc_fs.h>
#include <linux/profile.h>
#include <linux/rmap.h>
#include <linux/ksm.h>
#include <linux/acct.h>
#include <linux/tsacct_kern.h>
#include <linux/cn_proc.h>
#include <linux/freezer.h>
#include <linux/delayacct.h>
#include <linux/taskstats_kern.h>
#include <linux/random.h>
#include <linux/tty.h>
#include <linux/blkdev.h>
#include <linux/fs_struct.h>
#include <linux/magic.h>
#include <linux/perf_event.h>
#include <linux/posix-timers.h>
#include <linux/user-return-notifier.h>
#include <linux/oom.h>
#include <linux/khugepaged.h>
#include <linux/signalfd.h>
#include <linux/uprobes.h>
#include <linux/aio.h>
#include <linux/compiler.h>
#include <linux/sysctl.h>
#include <linux/kcov.h>

#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/uaccess.h>
#include <asm/mmu_context.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#include <trace/events/sched.h>

#define CREATE_TRACE_POINTS
#include <trace/events/task.h>

/*
 * Minimum number of threads to boot the kernel
 */
#define MIN_THREADS 20

/*
 * Maximum number of threads
 */
#define MAX_THREADS FUTEX_TID_MASK

/*
 * Protected counters by write_lock_irq(&tasklist_lock)
 */
unsigned long total_forks;	/* Handle normal Linux uptimes. */
int nr_threads;			/* The idle threads do not count.. */

int max_threads;		/* tunable limit on nr_threads */

DEFINE_PER_CPU(unsigned long, process_counts) = 0;

__cacheline_aligned DEFINE_RWLOCK(tasklist_lock);  /* outer */

#ifdef CONFIG_PROVE_RCU
int lockdep_tasklist_lock_is_held(void)
{
	return lockdep_is_held(&tasklist_lock);
}
EXPORT_SYMBOL_GPL(lockdep_tasklist_lock_is_held);
#endif /* #ifdef CONFIG_PROVE_RCU */

int nr_processes(void)
{
	int cpu;
	int total = 0;

	for_each_possible_cpu(cpu)
		total += per_cpu(process_counts, cpu);

	return total;
}

void __weak arch_release_task_struct(struct task_struct *tsk)
{
}

#ifndef CONFIG_ARCH_TASK_STRUCT_ALLOCATOR
static struct kmem_cache *task_struct_cachep;

static inline struct task_struct *alloc_task_struct_node(int node)
{
	return kmem_cache_alloc_node(task_struct_cachep, GFP_KERNEL, node);
}

static inline void free_task_struct(struct task_struct *tsk)
{
	kmem_cache_free(task_struct_cachep, tsk);
}
#endif

void __weak arch_release_thread_info(struct thread_info *ti)
{
}

#ifndef CONFIG_ARCH_THREAD_INFO_ALLOCATOR

/*
 * Allocate pages if THREAD_SIZE is >= PAGE_SIZE, otherwise use a
 * kmemcache based allocator.
 */
# if THREAD_SIZE >= PAGE_SIZE
static struct thread_info *alloc_thread_info_node(struct task_struct *tsk,
						  int node)
{
	struct page *page = alloc_kmem_pages_node(node, THREADINFO_GFP,
						  THREAD_SIZE_ORDER);

	if (page)
		memcg_kmem_update_page_stat(page, MEMCG_KERNEL_STACK,
					    1 << THREAD_SIZE_ORDER);

	return page ? page_address(page) : NULL;
}

static inline void free_thread_info(struct thread_info *ti)
{
	struct page *page = virt_to_page(ti);

	memcg_kmem_update_page_stat(page, MEMCG_KERNEL_STACK,
				    -(1 << THREAD_SIZE_ORDER));
	__free_kmem_pages(page, THREAD_SIZE_ORDER);
}
# else
static struct kmem_cache *thread_info_cache;

static struct thread_info *alloc_thread_info_node(struct task_struct *tsk,
						  int node)
{
	return kmem_cache_alloc_node(thread_info_cache, THREADINFO_GFP, node);
}

static void free_thread_info(struct thread_info *ti)
{
	kmem_cache_free(thread_info_cache, ti);
}

void thread_info_cache_init(void)
{
	thread_info_cache = kmem_cache_create("thread_info", THREAD_SIZE,
					      THREAD_SIZE, 0, NULL);
	BUG_ON(thread_info_cache == NULL);
}
# endif
#endif

/* SLAB cache for signal_struct structures (tsk->signal) */
static struct kmem_cache *signal_cachep;

/* SLAB cache for sighand_struct structures (tsk->sighand) */
struct kmem_cache *sighand_cachep;

/* SLAB cache for files_struct structures (tsk->files) */
struct kmem_cache *files_cachep;

/* SLAB cache for fs_struct structures (tsk->fs) */
struct kmem_cache *fs_cachep;

/* SLAB cache for vm_area_struct structures */
struct kmem_cache *vm_area_cachep;

/* SLAB cache for mm_struct structures (tsk->mm) */
static struct kmem_cache *mm_cachep;

static void account_kernel_stack(struct thread_info *ti, int account)
{
	struct zone *zone = page_zone(virt_to_page(ti));

	mod_zone_page_state(zone, NR_KERNEL_STACK, account);
}

void free_task(struct task_struct *tsk)
{
	account_kernel_stack(tsk->stack, -1);
	arch_release_thread_info(tsk->stack);
	free_thread_info(tsk->stack);
	rt_mutex_debug_task_free(tsk);
	ftrace_graph_exit_task(tsk);
	put_seccomp_filter(tsk);
	arch_release_task_struct(tsk);
	free_task_struct(tsk);
}
EXPORT_SYMBOL(free_task);

static inline void free_signal_struct(struct signal_struct *sig)
{
	taskstats_tgid_free(sig);
	sched_autogroup_exit(sig);
	kmem_cache_free(signal_cachep, sig);
}

static inline void put_signal_struct(struct signal_struct *sig)
{
	if (atomic_dec_and_test(&sig->sigcnt))
		free_signal_struct(sig);
}

void __put_task_struct(struct task_struct *tsk)
{
	WARN_ON(!tsk->exit_state);
	WARN_ON(atomic_read(&tsk->usage));
	WARN_ON(tsk == current);

	cgroup_free(tsk);
	task_numa_free(tsk);
	security_task_free(tsk);
	exit_creds(tsk);
	delayacct_tsk_free(tsk);
	put_signal_struct(tsk->signal);

	if (!profile_handoff_task(tsk))
		free_task(tsk);
}
EXPORT_SYMBOL_GPL(__put_task_struct);

void __init __weak arch_task_cache_init(void) { }

/*
 * set_max_threads
 */
static void set_max_threads(unsigned int max_threads_suggested)
{
	/**
	 * @brief 시스템에서 생성할 수 있는 최대 테스크 개수를 설정한다.
	 */
	u64 threads;

	/*
	 * The number of threads shall be limited such that the thread
	 * structures may only consume a small part of the available memory.
	 */
	if (fls64(totalram_pages) + fls64(PAGE_SIZE) > 64) // 메모리가 일정 수준 이상이 경우
		threads = MAX_THREADS; // thread 최대까지 가능
	else
		threads = div64_u64((u64) totalram_pages * (u64) PAGE_SIZE, // 메모리 크기기에 따라 사용 가능한 최대 threads 개수 계산
				    (u64) THREAD_SIZE * 8UL);

	if (threads > max_threads_suggested) // 호출 시, 최대 설정 값보다 큰 경우 보정
		threads = max_threads_suggested;

	max_threads = clamp_t(u64, threads, MIN_THREADS, MAX_THREADS); // max_threads 값 입력
}

#ifdef CONFIG_ARCH_WANTS_DYNAMIC_TASK_STRUCT
/* Initialized by the architecture: */
int arch_task_struct_size __read_mostly;
#endif

void __init fork_init(void)
{
#ifndef CONFIG_ARCH_TASK_STRUCT_ALLOCATOR
#ifndef ARCH_MIN_TASKALIGN
#define ARCH_MIN_TASKALIGN	L1_CACHE_BYTES
#endif
	/* create a slab on which task_structs can be allocated */
	task_struct_cachep = kmem_cache_create("task_struct", // task_struct 슬랩 캐시를 생성
			arch_task_struct_size, ARCH_MIN_TASKALIGN
			SLAB_PANIC|SLAB_NOTRACK|SLAB_ACCOUNT, NULL);
#endif

	/* do the arch specific task caches init */
	arch_task_cache_init(); // 추가적인 task 관련 kmem_cache를 초기화한다.

	set_max_threads(MAX_THREADS); // 최대로 생성 가능한 태스크 관련 캐시를 초기화

	init_task.signal->rlim[RLIMIT_NPROC].rlim_cur = max_threads/2; // resource limit 관련 초기화
	init_task.signal->rlim[RLIMIT_NPROC].rlim_max = max_threads/2;
	init_task.signal->rlim[RLIMIT_SIGPENDING] =
		init_task.signal->rlim[RLIMIT_NPROC];
}

int __weak arch_dup_task_struct(struct task_struct *dst,
					       struct task_struct *src)
{
	*dst = *src;
	return 0;
}

void set_task_stack_end_magic(struct task_struct *tsk)
{
	unsigned long *stackend;

	stackend = end_of_stack(tsk);
	*stackend = STACK_END_MAGIC;	/* for overflow detection */
}

static struct task_struct *dup_task_struct(struct task_struct *orig)
{
	/**
	 * @brief 생성된 task의 task_struct, thread_info 구조체 설정
	 */
	struct task_struct *tsk;
	struct thread_info *ti;
	int node = tsk_fork_get_node(orig); // 복사할 task 의 node를 가져옴
	int err;

	tsk = alloc_task_struct_node(node); // node에서 task_struct를 할당
	if (!tsk) // 할당 실패 케이스
		return NULL;

	ti = alloc_thread_info_node(tsk, node); // node에서 thread_info 할당
	if (!ti) // thread info 할당 실패 케이스
		goto free_tsk;

	err = arch_dup_task_struct(tsk, orig); // 할당받은 task_struct 값을 기존 task 값으로 입력
	if (err)
		goto free_ti;

	tsk->stack = ti; // task_struct에 thread_info 입력
#ifdef CONFIG_SECCOMP
	/*
	 * We must handle setting up seccomp filters once we're under
	 * the sighand lock in case orig has changed between now and
	 * then. Until then, filter must be NULL to avoid messing up
	 * the usage counts on the error path calling free_task.
	 */
	tsk->seccomp.filter = NULL;
#endif

	setup_thread_stack(tsk, orig); // 할당받은 thread_info 값을 부모 task의 thread_info 값으로 덮어쓴다.
	clear_user_return_notifier(tsk);
	clear_tsk_need_resched(tsk); // 새로 할당한 thread_info 에서 TIF_NEED_RESCHED 플래그가 제거
	set_task_stack_end_magic(tsk);

#ifdef CONFIG_CC_STACKPROTECTOR
	tsk->stack_canary = get_random_int();
#endif

	/*
	 * One for us, one for whoever does the "release_task()" (usually
	 * parent)
	 */
	atomic_set(&tsk->usage, 2); // task usage count 증가, 추후 task가 종료될 때 release_task() 에서 1 감소시키고, 스케줄러에서 1 감소시키므로 2로 설정
#ifdef CONFIG_BLK_DEV_IO_TRACE
	tsk->btrace_seq = 0;
#endif
	tsk->splice_pipe = NULL;
	tsk->task_frag.page = NULL;
	tsk->wake_q.next = NULL;

	account_kernel_stack(ti, 1);

	kcov_task_init(tsk);

	return tsk; // 할당받은 task_struct 리턴

free_ti:
	free_thread_info(ti);
free_tsk:
	free_task_struct(tsk);
	return NULL;
}

#ifdef CONFIG_MMU
static int dup_mmap(struct mm_struct *mm, struct mm_struct *oldmm)
{
	struct vm_area_struct *mpnt, *tmp, *prev, **pprev;
	struct rb_node **rb_link, *rb_parent;
	int retval;
	unsigned long charge;

	uprobe_start_dup_mmap();
	down_write(&oldmm->mmap_sem);
	flush_cache_dup_mm(oldmm);
	uprobe_dup_mmap(oldmm, mm);
	/*
	 * Not linked in yet - no deadlock potential:
	 */
	down_write_nested(&mm->mmap_sem, SINGLE_DEPTH_NESTING);

	/* No ordering required: file already has been exposed. */
	RCU_INIT_POINTER(mm->exe_file, get_mm_exe_file(oldmm));

	mm->total_vm = oldmm->total_vm;
	mm->data_vm = oldmm->data_vm;
	mm->exec_vm = oldmm->exec_vm;
	mm->stack_vm = oldmm->stack_vm;

	rb_link = &mm->mm_rb.rb_node;
	rb_parent = NULL;
	pprev = &mm->mmap;
	retval = ksm_fork(mm, oldmm);
	if (retval)
		goto out;
	retval = khugepaged_fork(mm, oldmm);
	if (retval)
		goto out;

	prev = NULL;
	for (mpnt = oldmm->mmap; mpnt; mpnt = mpnt->vm_next) {
		struct file *file;

		if (mpnt->vm_flags & VM_DONTCOPY) {
			vm_stat_account(mm, mpnt->vm_flags, -vma_pages(mpnt));
			continue;
		}
		charge = 0;
		if (mpnt->vm_flags & VM_ACCOUNT) {
			unsigned long len = vma_pages(mpnt);

			if (security_vm_enough_memory_mm(oldmm, len)) /* sic */
				goto fail_nomem;
			charge = len;
		}
		tmp = kmem_cache_alloc(vm_area_cachep, GFP_KERNEL);
		if (!tmp)
			goto fail_nomem;
		*tmp = *mpnt;
		INIT_LIST_HEAD(&tmp->anon_vma_chain);
		retval = vma_dup_policy(mpnt, tmp);
		if (retval)
			goto fail_nomem_policy;
		tmp->vm_mm = mm;
		if (anon_vma_fork(tmp, mpnt))
			goto fail_nomem_anon_vma_fork;
		tmp->vm_flags &=
			~(VM_LOCKED|VM_LOCKONFAULT|VM_UFFD_MISSING|VM_UFFD_WP);
		tmp->vm_next = tmp->vm_prev = NULL;
		tmp->vm_userfaultfd_ctx = NULL_VM_UFFD_CTX;
		file = tmp->vm_file;
		if (file) {
			struct inode *inode = file_inode(file);
			struct address_space *mapping = file->f_mapping;

			get_file(file);
			if (tmp->vm_flags & VM_DENYWRITE)
				atomic_dec(&inode->i_writecount);
			i_mmap_lock_write(mapping);
			if (tmp->vm_flags & VM_SHARED)
				atomic_inc(&mapping->i_mmap_writable);
			flush_dcache_mmap_lock(mapping);
			/* insert tmp into the share list, just after mpnt */
			vma_interval_tree_insert_after(tmp, mpnt,
					&mapping->i_mmap);
			flush_dcache_mmap_unlock(mapping);
			i_mmap_unlock_write(mapping);
		}

		/*
		 * Clear hugetlb-related page reserves for children. This only
		 * affects MAP_PRIVATE mappings. Faults generated by the child
		 * are not guaranteed to succeed, even if read-only
		 */
		if (is_vm_hugetlb_page(tmp))
			reset_vma_resv_huge_pages(tmp);

		/*
		 * Link in the new vma and copy the page table entries.
		 */
		*pprev = tmp;
		pprev = &tmp->vm_next;
		tmp->vm_prev = prev;
		prev = tmp;

		__vma_link_rb(mm, tmp, rb_link, rb_parent);
		rb_link = &tmp->vm_rb.rb_right;
		rb_parent = &tmp->vm_rb;

		mm->map_count++;
		retval = copy_page_range(mm, oldmm, mpnt);

		if (tmp->vm_ops && tmp->vm_ops->open)
			tmp->vm_ops->open(tmp);

		if (retval)
			goto out;
	}
	/* a new mm has just been created */
	arch_dup_mmap(oldmm, mm);
	retval = 0;
out:
	up_write(&mm->mmap_sem);
	flush_tlb_mm(oldmm);
	up_write(&oldmm->mmap_sem);
	uprobe_end_dup_mmap();
	return retval;
fail_nomem_anon_vma_fork:
	mpol_put(vma_policy(tmp));
fail_nomem_policy:
	kmem_cache_free(vm_area_cachep, tmp);
fail_nomem:
	retval = -ENOMEM;
	vm_unacct_memory(charge);
	goto out;
}

static inline int mm_alloc_pgd(struct mm_struct *mm)
{
	mm->pgd = pgd_alloc(mm);
	if (unlikely(!mm->pgd))
		return -ENOMEM;
	return 0;
}

static inline void mm_free_pgd(struct mm_struct *mm)
{
	pgd_free(mm, mm->pgd);
}
#else
static int dup_mmap(struct mm_struct *mm, struct mm_struct *oldmm)
{
	down_write(&oldmm->mmap_sem);
	RCU_INIT_POINTER(mm->exe_file, get_mm_exe_file(oldmm));
	up_write(&oldmm->mmap_sem);
	return 0;
}
#define mm_alloc_pgd(mm)	(0)
#define mm_free_pgd(mm)
#endif /* CONFIG_MMU */

__cacheline_aligned_in_smp DEFINE_SPINLOCK(mmlist_lock);

#define allocate_mm()	(kmem_cache_alloc(mm_cachep, GFP_KERNEL))
#define free_mm(mm)	(kmem_cache_free(mm_cachep, (mm)))

static unsigned long default_dump_filter = MMF_DUMP_FILTER_DEFAULT;

static int __init coredump_filter_setup(char *s)
{
	default_dump_filter =
		(simple_strtoul(s, NULL, 0) << MMF_DUMP_FILTER_SHIFT) &
		MMF_DUMP_FILTER_MASK;
	return 1;
}

__setup("coredump_filter=", coredump_filter_setup);

#include <linux/init_task.h>

static void mm_init_aio(struct mm_struct *mm)
{
#ifdef CONFIG_AIO
	spin_lock_init(&mm->ioctx_lock);
	mm->ioctx_table = NULL;
#endif
}

static void mm_init_owner(struct mm_struct *mm, struct task_struct *p)
{
#ifdef CONFIG_MEMCG
	mm->owner = p;
#endif
}

static struct mm_struct *mm_init(struct mm_struct *mm, struct task_struct *p)
{
	mm->mmap = NULL;
	mm->mm_rb = RB_ROOT;
	mm->vmacache_seqnum = 0;
	atomic_set(&mm->mm_users, 1);
	atomic_set(&mm->mm_count, 1);
	init_rwsem(&mm->mmap_sem);
	INIT_LIST_HEAD(&mm->mmlist);
	mm->core_state = NULL;
	atomic_long_set(&mm->nr_ptes, 0);
	mm_nr_pmds_init(mm);
	mm->map_count = 0;
	mm->locked_vm = 0;
	mm->pinned_vm = 0;
	memset(&mm->rss_stat, 0, sizeof(mm->rss_stat));
	spin_lock_init(&mm->page_table_lock);
	mm_init_cpumask(mm);
	mm_init_aio(mm);
	mm_init_owner(mm, p);
	mmu_notifier_mm_init(mm);
	clear_tlb_flush_pending(mm);
#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && !USE_SPLIT_PMD_PTLOCKS
	mm->pmd_huge_pte = NULL;
#endif

	if (current->mm) {
		mm->flags = current->mm->flags & MMF_INIT_MASK;
		mm->def_flags = current->mm->def_flags & VM_INIT_DEF_MASK;
	} else {
		mm->flags = default_dump_filter;
		mm->def_flags = 0;
	}

	if (mm_alloc_pgd(mm))
		goto fail_nopgd;

	if (init_new_context(p, mm))
		goto fail_nocontext;

	return mm;

fail_nocontext:
	mm_free_pgd(mm);
fail_nopgd:
	free_mm(mm);
	return NULL;
}

static void check_mm(struct mm_struct *mm)
{
	int i;

	for (i = 0; i < NR_MM_COUNTERS; i++) {
		long x = atomic_long_read(&mm->rss_stat.count[i]);

		if (unlikely(x))
			printk(KERN_ALERT "BUG: Bad rss-counter state "
					  "mm:%p idx:%d val:%ld\n", mm, i, x);
	}

	if (atomic_long_read(&mm->nr_ptes))
		pr_alert("BUG: non-zero nr_ptes on freeing mm: %ld\n",
				atomic_long_read(&mm->nr_ptes));
	if (mm_nr_pmds(mm))
		pr_alert("BUG: non-zero nr_pmds on freeing mm: %ld\n",
				mm_nr_pmds(mm));

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && !USE_SPLIT_PMD_PTLOCKS
	VM_BUG_ON_MM(mm->pmd_huge_pte, mm);
#endif
}

/*
 * Allocate and initialize an mm_struct.
 */
struct mm_struct *mm_alloc(void)
{
	struct mm_struct *mm;

	mm = allocate_mm();
	if (!mm)
		return NULL;

	memset(mm, 0, sizeof(*mm));
	return mm_init(mm, current);
}

/*
 * Called when the last reference to the mm
 * is dropped: either by a lazy thread or by
 * mmput. Free the page directory and the mm.
 */
void __mmdrop(struct mm_struct *mm)
{
	BUG_ON(mm == &init_mm);
	mm_free_pgd(mm);
	destroy_context(mm);
	mmu_notifier_mm_destroy(mm);
	check_mm(mm);
	free_mm(mm);
}
EXPORT_SYMBOL_GPL(__mmdrop);

/*
 * Decrement the use count and release all resources for an mm.
 */
void mmput(struct mm_struct *mm)
{
	might_sleep();

	if (atomic_dec_and_test(&mm->mm_users)) {
		uprobe_clear_state(mm);
		exit_aio(mm);
		ksm_exit(mm);
		khugepaged_exit(mm); /* must run before exit_mmap */
		exit_mmap(mm);
		set_mm_exe_file(mm, NULL);
		if (!list_empty(&mm->mmlist)) {
			spin_lock(&mmlist_lock);
			list_del(&mm->mmlist);
			spin_unlock(&mmlist_lock);
		}
		if (mm->binfmt)
			module_put(mm->binfmt->module);
		mmdrop(mm);
	}
}
EXPORT_SYMBOL_GPL(mmput);

/**
 * set_mm_exe_file - change a reference to the mm's executable file
 *
 * This changes mm's executable file (shown as symlink /proc/[pid]/exe).
 *
 * Main users are mmput() and sys_execve(). Callers prevent concurrent
 * invocations: in mmput() nobody alive left, in execve task is single
 * threaded. sys_prctl(PR_SET_MM_MAP/EXE_FILE) also needs to set the
 * mm->exe_file, but does so without using set_mm_exe_file() in order
 * to do avoid the need for any locks.
 */
void set_mm_exe_file(struct mm_struct *mm, struct file *new_exe_file)
{
	struct file *old_exe_file;

	/*
	 * It is safe to dereference the exe_file without RCU as
	 * this function is only called if nobody else can access
	 * this mm -- see comment above for justification.
	 */
	old_exe_file = rcu_dereference_raw(mm->exe_file);

	if (new_exe_file)
		get_file(new_exe_file);
	rcu_assign_pointer(mm->exe_file, new_exe_file);
	if (old_exe_file)
		fput(old_exe_file);
}

/**
 * get_mm_exe_file - acquire a reference to the mm's executable file
 *
 * Returns %NULL if mm has no associated executable file.
 * User must release file via fput().
 */
struct file *get_mm_exe_file(struct mm_struct *mm)
{
	struct file *exe_file;

	rcu_read_lock();
	exe_file = rcu_dereference(mm->exe_file);
	if (exe_file && !get_file_rcu(exe_file))
		exe_file = NULL;
	rcu_read_unlock();
	return exe_file;
}
EXPORT_SYMBOL(get_mm_exe_file);

/**
 * get_task_mm - acquire a reference to the task's mm
 *
 * Returns %NULL if the task has no mm.  Checks PF_KTHREAD (meaning
 * this kernel workthread has transiently adopted a user mm with use_mm,
 * to do its AIO) is not set and if so returns a reference to it, after
 * bumping up the use count.  User must release the mm via mmput()
 * after use.  Typically used by /proc and ptrace.
 */
struct mm_struct *get_task_mm(struct task_struct *task)
{
	struct mm_struct *mm;

	task_lock(task);
	mm = task->mm;
	if (mm) {
		if (task->flags & PF_KTHREAD)
			mm = NULL;
		else
			atomic_inc(&mm->mm_users);
	}
	task_unlock(task);
	return mm;
}
EXPORT_SYMBOL_GPL(get_task_mm);

struct mm_struct *mm_access(struct task_struct *task, unsigned int mode)
{
	struct mm_struct *mm;
	int err;

	err =  mutex_lock_killable(&task->signal->cred_guard_mutex);
	if (err)
		return ERR_PTR(err);

	mm = get_task_mm(task);
	if (mm && mm != current->mm &&
			!ptrace_may_access(task, mode)) {
		mmput(mm);
		mm = ERR_PTR(-EACCES);
	}
	mutex_unlock(&task->signal->cred_guard_mutex);

	return mm;
}

static void complete_vfork_done(struct task_struct *tsk)
{
	struct completion *vfork;

	task_lock(tsk);
	vfork = tsk->vfork_done;
	if (likely(vfork)) {
		tsk->vfork_done = NULL;
		complete(vfork);
	}
	task_unlock(tsk);
}

static int wait_for_vfork_done(struct task_struct *child,
				struct completion *vfork)
{
	int killed;

	freezer_do_not_count();
	killed = wait_for_completion_killable(vfork);
	freezer_count();

	if (killed) {
		task_lock(child);
		child->vfork_done = NULL;
		task_unlock(child);
	}

	put_task_struct(child);
	return killed;
}

/* Please note the differences between mmput and mm_release.
 * mmput is called whenever we stop holding onto a mm_struct,
 * error success whatever.
 *
 * mm_release is called after a mm_struct has been removed
 * from the current process.
 *
 * This difference is important for error handling, when we
 * only half set up a mm_struct for a new process and need to restore
 * the old one.  Because we mmput the new mm_struct before
 * restoring the old one. . .
 * Eric Biederman 10 January 1998
 */
void mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	/* Get rid of any futexes when releasing the mm */
#ifdef CONFIG_FUTEX
	if (unlikely(tsk->robust_list)) {
		exit_robust_list(tsk);
		tsk->robust_list = NULL;
	}
#ifdef CONFIG_COMPAT
	if (unlikely(tsk->compat_robust_list)) {
		compat_exit_robust_list(tsk);
		tsk->compat_robust_list = NULL;
	}
#endif
	if (unlikely(!list_empty(&tsk->pi_state_list)))
		exit_pi_state_list(tsk);
#endif

	uprobe_free_utask(tsk);

	/* Get rid of any cached register state */
	deactivate_mm(tsk, mm);

	/*
	 * If we're exiting normally, clear a user-space tid field if
	 * requested.  We leave this alone when dying by signal, to leave
	 * the value intact in a core dump, and to save the unnecessary
	 * trouble, say, a killed vfork parent shouldn't touch this mm.
	 * Userland only wants this done for a sys_exit.
	 */
	if (tsk->clear_child_tid) {
		if (!(tsk->flags & PF_SIGNALED) &&
		    atomic_read(&mm->mm_users) > 1) {
			/*
			 * We don't check the error code - if userspace has
			 * not set up a proper pointer then tough luck.
			 */
			put_user(0, tsk->clear_child_tid);
			sys_futex(tsk->clear_child_tid, FUTEX_WAKE,
					1, NULL, NULL, 0);
		}
		tsk->clear_child_tid = NULL;
	}

	/*
	 * All done, finally we can wake up parent and return this mm to him.
	 * Also kthread_stop() uses this completion for synchronization.
	 */
	if (tsk->vfork_done)
		complete_vfork_done(tsk);
}

/*
 * Allocate a new mm structure and copy contents from the
 * mm structure of the passed in task structure.
 */
static struct mm_struct *dup_mm(struct task_struct *tsk)
{
	struct mm_struct *mm, *oldmm = current->mm;
	int err;

	mm = allocate_mm();
	if (!mm)
		goto fail_nomem;

	memcpy(mm, oldmm, sizeof(*mm));

	if (!mm_init(mm, tsk))
		goto fail_nomem;

	err = dup_mmap(mm, oldmm);
	if (err)
		goto free_pt;

	mm->hiwater_rss = get_mm_rss(mm);
	mm->hiwater_vm = mm->total_vm;

	if (mm->binfmt && !try_module_get(mm->binfmt->module))
		goto free_pt;

	return mm;

free_pt:
	/* don't put binfmt in mmput, we haven't got module yet */
	mm->binfmt = NULL;
	mmput(mm);

fail_nomem:
	return NULL;
}

static int copy_mm(unsigned long clone_flags, struct task_struct *tsk)
{
	struct mm_struct *mm, *oldmm;
	int retval;

	tsk->min_flt = tsk->maj_flt = 0;
	tsk->nvcsw = tsk->nivcsw = 0;
#ifdef CONFIG_DETECT_HUNG_TASK
	tsk->last_switch_count = tsk->nvcsw + tsk->nivcsw;
#endif

	tsk->mm = NULL;
	tsk->active_mm = NULL;

	/*
	 * Are we cloning a kernel thread?
	 *
	 * We need to steal a active VM for that..
	 */
	oldmm = current->mm;
	if (!oldmm)
		return 0;

	/* initialize the new vmacache entries */
	vmacache_flush(tsk);

	if (clone_flags & CLONE_VM) {
		atomic_inc(&oldmm->mm_users);
		mm = oldmm;
		goto good_mm;
	}

	retval = -ENOMEM;
	mm = dup_mm(tsk);
	if (!mm)
		goto fail_nomem;

good_mm:
	tsk->mm = mm;
	tsk->active_mm = mm;
	return 0;

fail_nomem:
	return retval;
}

static int copy_fs(unsigned long clone_flags, struct task_struct *tsk)
{
	/**
	 * @brief 현재 task의 fs_struct를 복사해 tsk에 입력
	 */
	struct fs_struct *fs = current->fs;
	if (clone_flags & CLONE_FS) {
		/* tsk->fs is already what we want */
		spin_lock(&fs->lock);
		if (fs->in_exec) {
			spin_unlock(&fs->lock);
			return -EAGAIN;
		}
		fs->users++; // 사용자 카운터 증가
		spin_unlock(&fs->lock);
		return 0;
	}
	tsk->fs = copy_fs_struct(fs); // fs_struct 복사 후 tsk->fs에 입력력
	if (!tsk->fs)
		return -ENOMEM;
	return 0;
}

static int copy_files(unsigned long clone_flags, struct task_struct *tsk)
{
	/**
	 * @brief 현재 task 의 file_struct를 tsk 로 복사
	 */
	struct files_struct *oldf, *newf;
	int error = 0;

	/*
	 * A background process may not have any files ...
	 */
	oldf = current->files; // 현재 task의 file struct
	if (!oldf) // file_struct가 없을 경우 에러 처리
		goto out;

	if (clone_flags & CLONE_FILES) {
		atomic_inc(&oldf->count);
		goto out;
	}

	newf = dup_fd(oldf, &error); // old file struct를 복사
	if (!newf) // 복사 실패인 경우 에러 처리
		goto out;

	tsk->files = newf; // 복사한 file struct를 tsk 에 입력
	error = 0; // return 값 처리
out:
	return error;
}

static int copy_io(unsigned long clone_flags, struct task_struct *tsk)
{
#ifdef CONFIG_BLOCK
	struct io_context *ioc = current->io_context;
	struct io_context *new_ioc;

	if (!ioc)
		return 0;
	/*
	 * Share io context with parent, if CLONE_IO is set
	 */
	if (clone_flags & CLONE_IO) {
		ioc_task_link(ioc);
		tsk->io_context = ioc;
	} else if (ioprio_valid(ioc->ioprio)) {
		new_ioc = get_task_io_context(tsk, GFP_KERNEL, NUMA_NO_NODE);
		if (unlikely(!new_ioc))
			return -ENOMEM;

		new_ioc->ioprio = ioc->ioprio;
		put_io_context(new_ioc);
	}
#endif
	return 0;
}

static int copy_sighand(unsigned long clone_flags, struct task_struct *tsk)
{
	/**
	 * @brief current task의 signal handler 정보를 복사해 tsk에 입력
	 */
	struct sighand_struct *sig;

	if (clone_flags & CLONE_SIGHAND) {
		atomic_inc(&current->sighand->count);
		return 0;
	}
	sig = kmem_cache_alloc(sighand_cachep, GFP_KERNEL); // kmem_cache 할당
	rcu_assign_pointer(tsk->sighand, sig); // 할당받은 signal handler를 task에 입력
	if (!sig)
		return -ENOMEM;

	atomic_set(&sig->count, 1); // 참조 카운터 증가
	memcpy(sig->action, current->sighand->action, sizeof(sig->action));
	return 0;
}

void __cleanup_sighand(struct sighand_struct *sighand)
{
	if (atomic_dec_and_test(&sighand->count)) {
		signalfd_cleanup(sighand);
		/*
		 * sighand_cachep is SLAB_DESTROY_BY_RCU so we can free it
		 * without an RCU grace period, see __lock_task_sighand().
		 */
		kmem_cache_free(sighand_cachep, sighand);
	}
}

/*
 * Initialize POSIX timer handling for a thread group.
 */
static void posix_cpu_timers_init_group(struct signal_struct *sig)
{
	unsigned long cpu_limit;

	cpu_limit = READ_ONCE(sig->rlim[RLIMIT_CPU].rlim_cur);
	if (cpu_limit != RLIM_INFINITY) {
		sig->cputime_expires.prof_exp = secs_to_cputime(cpu_limit);
		sig->cputimer.running = true;
	}

	/* The timer lists. */
	INIT_LIST_HEAD(&sig->cpu_timers[0]);
	INIT_LIST_HEAD(&sig->cpu_timers[1]);
	INIT_LIST_HEAD(&sig->cpu_timers[2]);
}

static int copy_signal(unsigned long clone_flags, struct task_struct *tsk)
{
	struct signal_struct *sig;

	if (clone_flags & CLONE_THREAD)
		return 0;

	sig = kmem_cache_zalloc(signal_cachep, GFP_KERNEL);
	tsk->signal = sig;
	if (!sig)
		return -ENOMEM;

	sig->nr_threads = 1;
	atomic_set(&sig->live, 1);
	atomic_set(&sig->sigcnt, 1);

	/* list_add(thread_node, thread_head) without INIT_LIST_HEAD() */
	sig->thread_head = (struct list_head)LIST_HEAD_INIT(tsk->thread_node);
	tsk->thread_node = (struct list_head)LIST_HEAD_INIT(sig->thread_head);

	init_waitqueue_head(&sig->wait_chldexit);
	sig->curr_target = tsk;
	init_sigpending(&sig->shared_pending);
	INIT_LIST_HEAD(&sig->posix_timers);
	seqlock_init(&sig->stats_lock);
	prev_cputime_init(&sig->prev_cputime);

	hrtimer_init(&sig->real_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sig->real_timer.function = it_real_fn;

	task_lock(current->group_leader);
	memcpy(sig->rlim, current->signal->rlim, sizeof sig->rlim);
	task_unlock(current->group_leader);

	posix_cpu_timers_init_group(sig);

	tty_audit_fork(sig);
	sched_autogroup_fork(sig);

	sig->oom_score_adj = current->signal->oom_score_adj;
	sig->oom_score_adj_min = current->signal->oom_score_adj_min;

	sig->has_child_subreaper = current->signal->has_child_subreaper ||
				   current->signal->is_child_subreaper;

	mutex_init(&sig->cred_guard_mutex);

	return 0;
}

static void copy_seccomp(struct task_struct *p)
{
#ifdef CONFIG_SECCOMP
	/*
	 * Must be called with sighand->lock held, which is common to
	 * all threads in the group. Holding cred_guard_mutex is not
	 * needed because this new task is not yet running and cannot
	 * be racing exec.
	 */
	assert_spin_locked(&current->sighand->siglock);

	/* Ref-count the new filter user, and assign it. */
	get_seccomp_filter(current);
	p->seccomp = current->seccomp;

	/*
	 * Explicitly enable no_new_privs here in case it got set
	 * between the task_struct being duplicated and holding the
	 * sighand lock. The seccomp state and nnp must be in sync.
	 */
	if (task_no_new_privs(current))
		task_set_no_new_privs(p);

	/*
	 * If the parent gained a seccomp mode after copying thread
	 * flags and between before we held the sighand lock, we have
	 * to manually enable the seccomp thread flag here.
	 */
	if (p->seccomp.mode != SECCOMP_MODE_DISABLED)
		set_tsk_thread_flag(p, TIF_SECCOMP);
#endif
}

SYSCALL_DEFINE1(set_tid_address, int __user *, tidptr)
{
	current->clear_child_tid = tidptr;

	return task_pid_vnr(current);
}

static void rt_mutex_init_task(struct task_struct *p)
{
	raw_spin_lock_init(&p->pi_lock);
#ifdef CONFIG_RT_MUTEXES
	p->pi_waiters = RB_ROOT;
	p->pi_waiters_leftmost = NULL;
	p->pi_blocked_on = NULL;
#endif
}

/*
 * Initialize POSIX timer handling for a single task.
 */
static void posix_cpu_timers_init(struct task_struct *tsk)
{
	tsk->cputime_expires.prof_exp = 0;
	tsk->cputime_expires.virt_exp = 0;
	tsk->cputime_expires.sched_exp = 0;
	INIT_LIST_HEAD(&tsk->cpu_timers[0]);
	INIT_LIST_HEAD(&tsk->cpu_timers[1]);
	INIT_LIST_HEAD(&tsk->cpu_timers[2]);
}

static inline void
init_task_pid(struct task_struct *task, enum pid_type type, struct pid *pid)
{
	 task->pids[type].pid = pid;
}

/*
 * This creates a new process as a copy of the old one,
 * but does not actually start it yet.
 *
 * It copies the registers, and all the appropriate
 * parts of the process environment (as per the clone
 * flags). The actual kick-off is left to the caller.
 */
static struct task_struct *copy_process(unsigned long clone_flags,
					unsigned long stack_start,
					unsigned long stack_size,
					int __user *child_tidptr,
					struct pid *pid,
					int trace,
					unsigned long tls)
{
	int retval;
	struct task_struct *p;

	if ((clone_flags & (CLONE_NEWNS|CLONE_FS)) == (CLONE_NEWNS|CLONE_FS))
		return ERR_PTR(-EINVAL);

	if ((clone_flags & (CLONE_NEWUSER|CLONE_FS)) == (CLONE_NEWUSER|CLONE_FS))
		return ERR_PTR(-EINVAL);

	/*
	 * Thread groups must share signals as well, and detached threads
	 * can only be started up within the thread group.
	 */
	if ((clone_flags & CLONE_THREAD) && !(clone_flags & CLONE_SIGHAND))
		return ERR_PTR(-EINVAL);

	/*
	 * Shared signal handlers imply shared VM. By way of the above,
	 * thread groups also imply shared VM. Blocking this case allows
	 * for various simplifications in other code.
	 */
	if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
		return ERR_PTR(-EINVAL);

	/*
	 * Siblings of global init remain as zombies on exit since they are
	 * not reaped by their parent (swapper). To solve this and to avoid
	 * multi-rooted process trees, prevent global and container-inits
	 * from creating siblings.
	 */
	if ((clone_flags & CLONE_PARENT) &&
				current->signal->flags & SIGNAL_UNKILLABLE)
		return ERR_PTR(-EINVAL);

	/*
	 * If the new process will be in a different pid or user namespace
	 * do not allow it to share a thread group with the forking task.
	 */
	if (clone_flags & CLONE_THREAD) {
		if ((clone_flags & (CLONE_NEWUSER | CLONE_NEWPID)) ||
		    (task_active_pid_ns(current) !=
				current->nsproxy->pid_ns_for_children))
			return ERR_PTR(-EINVAL);
	}
// ... 5-15)
	retval = security_task_create(clone_flags);
	if (retval)
		goto fork_out;

	retval = -ENOMEM;
	p = dup_task_struct(current);
	if (!p)
		goto fork_out;

	ftrace_graph_init_task(p);

	rt_mutex_init_task(p);

#ifdef CONFIG_PROVE_LOCKING
	DEBUG_LOCKS_WARN_ON(!p->hardirqs_enabled);
	DEBUG_LOCKS_WARN_ON(!p->softirqs_enabled);
#endif
	retval = -EAGAIN;
	if (atomic_read(&p->real_cred->user->processes) >=
			task_rlimit(p, RLIMIT_NPROC)) {
		if (p->real_cred->user != INIT_USER &&
		    !capable(CAP_SYS_RESOURCE) && !capable(CAP_SYS_ADMIN))
			goto bad_fork_free;
	}
	current->flags &= ~PF_NPROC_EXCEEDED;

	retval = copy_creds(p, clone_flags);
	if (retval < 0)
		goto bad_fork_free;

	/*
	 * If multiple threads are within copy_process(), then this check
	 * triggers too late. This doesn't hurt, the check is only there
	 * to stop root fork bombs.
	 */
	retval = -EAGAIN;
	if (nr_threads >= max_threads)
		goto bad_fork_cleanup_count;

	delayacct_tsk_init(p);	/* Must remain after dup_task_struct() */
	p->flags &= ~(PF_SUPERPRIV | PF_WQ_WORKER);
	p->flags |= PF_FORKNOEXEC;
// 5-18)
	INIT_LIST_HEAD(&p->children); // 생성한 task의 children 필드 초기화
	INIT_LIST_HEAD(&p->sibling); // 생성한 task의 sibling 필드 초기화 (부모 task의 children 필드에 연결될 예정)
	rcu_copy_process(p); 
	p->vfork_done = NULL;
	spin_lock_init(&p->alloc_lock);

	init_sigpending(&p->pending);

	p->utime = p->stime = p->gtime = 0;
	p->utimescaled = p->stimescaled = 0;
	prev_cputime_init(&p->prev_cputime);

#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
	seqcount_init(&p->vtime_seqcount);
	p->vtime_snap = 0;
	p->vtime_snap_whence = VTIME_INACTIVE;
#endif

#if defined(SPLIT_RSS_COUNTING)
	memset(&p->rss_stat, 0, sizeof(p->rss_stat));
#endif

	p->default_timer_slack_ns = current->timer_slack_ns;

	task_io_accounting_init(&p->ioac);
	acct_clear_integrals(p);

	posix_cpu_timers_init(p);

	p->start_time = ktime_get_ns();
	p->real_start_time = ktime_get_boot_ns();
	p->io_context = NULL;
	p->audit_context = NULL;
	threadgroup_change_begin(current);
	cgroup_fork(p);
#ifdef CONFIG_NUMA
	p->mempolicy = mpol_dup(p->mempolicy);
	if (IS_ERR(p->mempolicy)) {
		retval = PTR_ERR(p->mempolicy);
		p->mempolicy = NULL;
		goto bad_fork_cleanup_threadgroup_lock;
	}
#endif
#ifdef CONFIG_CPUSETS
	p->cpuset_mem_spread_rotor = NUMA_NO_NODE;
	p->cpuset_slab_spread_rotor = NUMA_NO_NODE;
	seqcount_init(&p->mems_allowed_seq);
#endif
#ifdef CONFIG_TRACE_IRQFLAGS
	p->irq_events = 0;
	p->hardirqs_enabled = 0;
	p->hardirq_enable_ip = 0;
	p->hardirq_enable_event = 0;
	p->hardirq_disable_ip = _THIS_IP_;
	p->hardirq_disable_event = 0;
	p->softirqs_enabled = 1;
	p->softirq_enable_ip = _THIS_IP_;
	p->softirq_enable_event = 0;
	p->softirq_disable_ip = 0;
	p->softirq_disable_event = 0;
	p->hardirq_context = 0;
	p->softirq_context = 0;
#endif

	p->pagefault_disabled = 0;

#ifdef CONFIG_LOCKDEP
	p->lockdep_depth = 0; /* no locks held yet */
	p->curr_chain_key = 0;
	p->lockdep_recursion = 0;
#endif

#ifdef CONFIG_DEBUG_MUTEXES
	p->blocked_on = NULL; /* not blocked yet */
#endif
#ifdef CONFIG_BCACHE
	p->sequential_io	= 0;
	p->sequential_io_avg	= 0;
#endif

	/* Perform scheduler related setup. Assign this task to a CPU. */
	retval = sched_fork(clone_flags, p);
// ... 5-15 end)
	if (retval)
		goto bad_fork_cleanup_policy;

	retval = perf_event_init_task(p);
	if (retval)
		goto bad_fork_cleanup_policy;
	retval = audit_alloc(p);
	if (retval)
		goto bad_fork_cleanup_perf;
	/* copy all the process information */
	shm_init_task(p);
	retval = copy_semundo(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_audit;
// ... 5-16)
	retval = copy_files(clone_flags, p); // Task가 open한 파일 정보를 관리하는 files_struct 구조를 생성해 부모의 file_struct 구조체 내용을 복사
	if (retval) // file struct copy fail case
		goto bad_fork_cleanup_semundo;
	retval = copy_fs(clone_flags, p); // 디렉토리 경로를 저장하는 fs를 복사
	if (retval)
		goto bad_fork_cleanup_files;
	retval = copy_sighand(clone_flags, p); // signal handler 정보 복사
	if (retval)
		goto bad_fork_cleanup_fs;
	retval = copy_signal(clone_flags, p); // signal_struct 복사
	if (retval)
		goto bad_fork_cleanup_sighand;
	retval = copy_mm(clone_flags, p); // mm_struct 복사
	if (retval)
		goto bad_fork_cleanup_signal;
	retval = copy_namespaces(clone_flags, p); // namespaces 복사
	if (retval)
		goto bad_fork_cleanup_mm;
	retval = copy_io(clone_flags, p); // io_context 복사
	if (retval)
		goto bad_fork_cleanup_namespaces;
	retval = copy_thread_tls(clone_flags, stack_start, stack_size, p, tls); // cpu_context 복사
	if (retval)
		goto bad_fork_cleanup_io;
// ... 5-17)
	if (pid != &init_struct_pid) { // swapper가 사용하는 pid 구조체와 다른지 확인
		pid = alloc_pid(p->nsproxy->pid_ns_for_children); // 다른 경우 pid 할당
		if (IS_ERR(pid)) { // 할당 실패 케이스
			retval = PTR_ERR(pid);
			goto bad_fork_cleanup_io;
		}
	}

	p->set_child_tid = (clone_flags & CLONE_CHILD_SETTID) ? child_tidptr : NULL; // SETTID 플래그 처리
	/*
	 * Clear TID on mm_release()?
	 */
	p->clear_child_tid = (clone_flags & CLONE_CHILD_CLEARTID) ? child_tidptr : NULL; //  CLEARTID 플래그 처리
#ifdef CONFIG_BLOCK
	p->plug = NULL;
#endif
#ifdef CONFIG_FUTEX
	p->robust_list = NULL;
#ifdef CONFIG_COMPAT
	p->compat_robust_list = NULL;
#endif
	INIT_LIST_HEAD(&p->pi_state_list);
	p->pi_state_cache = NULL;
#endif
	/*
	 * sigaltstack should be cleared when sharing the same VM
	 */
	if ((clone_flags & (CLONE_VM|CLONE_VFORK)) == CLONE_VM) // CLONE_VM 를 사용하고 CLONE_VFORK 를 사용하지 않는 경우
		p->sas_ss_sp = p->sas_ss_size = 0; // 새로 생성할 task의 signal stack을 0으로 초기화

	/*
	 * Syscall tracing and stepping should be turned off in the
	 * child regardless of CLONE_PTRACE.
	 */
	user_disable_single_step(p);
	clear_tsk_thread_flag(p, TIF_SYSCALL_TRACE);
#ifdef TIF_SYSCALL_EMU
	clear_tsk_thread_flag(p, TIF_SYSCALL_EMU);
#endif
	clear_all_latency_tracing(p);

	/* ok, now we should be set up.. */
	p->pid = pid_nr(pid); // global pid 를 구한다.
	if (clone_flags & CLONE_THREAD) { // Thread 일 경우
		p->exit_signal = -1;
		p->group_leader = current->group_leader; // 부모 task가 group leader
		p->tgid = current->tgid; // 부모 task의 tgid를 가져옴
	} else { // process 일 경우
		if (clone_flags & CLONE_PARENT) // 부모 task를 복사하는 경우
			p->exit_signal = current->group_leader->exit_signal; // 부모 task의 exit_signal을 가져옴
		else // 복사가 아닌 생성인 경우
			p->exit_signal = (clone_flags & CSIGNAL); // exit_signal  입력
		p->group_leader = p; // 프로세스 이므로 생성한 pid가 group_leader
		p->tgid = p->pid; // task group id = pid
	}

	p->nr_dirtied = 0; // 태스크 단위로 누적해서 관리되는 더티 페이지 개수 초기화
	p->nr_dirtied_pause = 128 >> (PAGE_SHIFT - 10); // 페이지의 변경사항을 backing device로 writeback 해야하는 pre-task threshold
	p->dirty_paused_when = 0; // writeback을 시도한 가장 최근의 jiffies

	p->pdeath_signal = 0;
	INIT_LIST_HEAD(&p->thread_group); // thread 그룹 안의 모든 스레드를 연결하는 필드인 thread_group을 초기화한다.
	p->task_works = NULL;

	/*
	 * Ensure that the cgroup subsystem policies allow the new process to be
	 * forked. It should be noted the the new process's css_set can be changed
	 * between here and cgroup_post_fork() if an organisation operation is in
	 * progress.
	 */
	retval = cgroup_can_fork(p); // 서브시스템이 구현한 can_fork 를 각각 호출
	if (retval)
		goto bad_fork_free_pid;

	/*
	 * Make it visible to the rest of the system, but dont wake it up yet.
	 * Need tasklist lock for parent etc handling!
	 */
	write_lock_irq(&tasklist_lock);

	/* CLONE_PARENT re-uses the old parent */
	if (clone_flags & (CLONE_PARENT|CLONE_THREAD)) { // 부모 task 의 real_parent, parent_exec_id를 받음
		p->real_parent = current->real_parent;
		p->parent_exec_id = current->parent_exec_id;
	} else { // 생성하는 task가 부모 task
		p->real_parent = current;
		p->parent_exec_id = current->self_exec_id;
	}

	spin_lock(&current->sighand->siglock);

	/*
	 * Copy seccomp details explicitly here, in case they were changed
	 * before holding sighand lock.
	 */
	copy_seccomp(p);

	/*
	 * Process group and session signals need to be delivered to just the
	 * parent before the fork or both the parent and the child after the
	 * fork. Restart if a signal comes in before we add the new process to
	 * it's process group.
	 * A fatal signal pending means that current will exit, so the new
	 * thread can't slip out of an OOM kill (or normal SIGKILL).
	*/
	recalc_sigpending();
	if (signal_pending(current)) { // current task 가 처리해야 할 signal이 있을 경우 생성 중지
		spin_unlock(&current->sighand->siglock);
		write_unlock_irq(&tasklist_lock);
		retval = -ERESTARTNOINTR;
		goto bad_fork_cancel_cgroup; // error case
	}

	if (likely(p->pid)) { // pid 할당이 성공한 경우
		ptrace_init_task(p, (clone_flags & CLONE_PTRACE) || trace);

		init_task_pid(p, PIDTYPE_PID, pid); // pid 구조체를 태스크의 pids 필드에 연결
		if (thread_group_leader(p)) { // 해당 프로세스를 생성 중인 경우 (thread group leader 인 경우)
			init_task_pid(p, PIDTYPE_PGID, task_pgrp(current)); // PGID 초기화
			init_task_pid(p, PIDTYPE_SID, task_session(current)); // SID 초기화

			if (is_child_reaper(pid)) { // pid 구조체를 특정 네임스페이스의 init 태스크가 사용한다면 해당 pid ns의 child reaper로 등록
				ns_of_pid(pid)->child_reaper = p;
				p->signal->flags |= SIGNAL_UNKILLABLE;
			}

			p->signal->leader_pid = pid;
			p->signal->tty = tty_kref_get(current->signal->tty);
			list_add_tail(&p->sibling, &p->real_parent->children); //  parent->children 리스트 마지막에 p->sibling 추가
			list_add_tail_rcu(&p->tasks, &init_task.tasks); // init_task.tasks 의 마지막에 p->tasks 추가
			attach_pid(p, PIDTYPE_PGID); // pid 구조체의 tasks[] 필드를 설정, 
			attach_pid(p, PIDTYPE_SID);
			__this_cpu_inc(process_counts); // process count 증가
		} else { // thread를 생성하는 경우
			current->signal->nr_threads++; // thread count 증가
			atomic_inc(&current->signal->live); // signal_struct 구조체의 nr_threads 필드 증가
			atomic_inc(&current->signal->sigcnt); // signal count 증가
			list_add_tail_rcu(&p->thread_group, // 부모 task의 group leader에 현재 task 의 thread_group 추가
					  &p->group_leader->thread_group);
			list_add_tail_rcu(&p->thread_node, // 해당 task 에서 발생하는 signal을 받을 signal_handler를 연결
					  &p->signal->thread_head);
		}
// ... 5-18 end)
		attach_pid(p, PIDTYPE_PID); // pid 구조체의 tasks 필드를 설정
		nr_threads++; // 현재 시스템에서 사용 중인 thread 개수 증가
	}

	total_forks++; // fork count 증가
	spin_unlock(&current->sighand->siglock);
	syscall_tracepoint_update(p);
	write_unlock_irq(&tasklist_lock); // irq unlock

	proc_fork_connector(p); // 유저에게 task가 fork 되었음을 알림
	cgroup_post_fork(p);
	threadgroup_change_end(current);
	perf_event_fork(p);

	trace_task_newtask(p, clone_flags);
	uprobe_copy_process(p, clone_flags);

	return p; // 생성한 task return

bad_fork_cancel_cgroup:
	cgroup_cancel_fork(p);
bad_fork_free_pid:
	if (pid != &init_struct_pid)
		free_pid(pid);
bad_fork_cleanup_io:
	if (p->io_context)
		exit_io_context(p);
bad_fork_cleanup_namespaces:
	exit_task_namespaces(p);
bad_fork_cleanup_mm:
	if (p->mm)
		mmput(p->mm);
bad_fork_cleanup_signal:
	if (!(clone_flags & CLONE_THREAD))
		free_signal_struct(p->signal);
bad_fork_cleanup_sighand:
	__cleanup_sighand(p->sighand);
bad_fork_cleanup_fs:
	exit_fs(p); /* blocking */
bad_fork_cleanup_files:
	exit_files(p); /* blocking */
bad_fork_cleanup_semundo:
	exit_sem(p);
bad_fork_cleanup_audit:
	audit_free(p);
bad_fork_cleanup_perf:
	perf_event_free_task(p);
bad_fork_cleanup_policy:
#ifdef CONFIG_NUMA
	mpol_put(p->mempolicy);
bad_fork_cleanup_threadgroup_lock:
#endif
	threadgroup_change_end(current);
	delayacct_tsk_free(p);
bad_fork_cleanup_count:
	atomic_dec(&p->cred->user->processes);
	exit_creds(p);
bad_fork_free:
	free_task(p);
fork_out:
	return ERR_PTR(retval);
}

static inline void init_idle_pids(struct pid_link *links)
{
	enum pid_type type;

	for (type = PIDTYPE_PID; type < PIDTYPE_MAX; ++type) {
		INIT_HLIST_NODE(&links[type].node); /* not really needed */
		links[type].pid = &init_struct_pid;
	}
}

struct task_struct *fork_idle(int cpu)
{
	/**
	 * @brief idle 스레드를 생성하고 초기화한다.
	 */
	struct task_struct *task;
	task = copy_process(CLONE_VM, 0, 0, NULL, &init_struct_pid, 0, 0); // process copy
	if (!IS_ERR(task)) { // copy가 정상적으로 진행된 경우
		init_idle_pids(task->pids);
		init_idle(task, cpu); // TODO 5-29
	}

	return task;
}

/*
 *  Ok, this is the main fork-routine.
 *
 * It copies the process, and if successful kick-starts
 * it and waits for it to finish using the VM if required.
 */
long _do_fork(unsigned long clone_flags,
	      unsigned long stack_start,
	      unsigned long stack_size,
	      int __user *parent_tidptr,
	      int __user *child_tidptr,
	      unsigned long tls)
{
	struct task_struct *p;
	int trace = 0;
	long nr;

	/*
	 * Determine whether and which event to report to ptracer.  When
	 * called from kernel_thread or CLONE_UNTRACED is explicitly
	 * requested, no event is reported; otherwise, report if the event
	 * for the type of forking is enabled.
	 */
	if (!(clone_flags & CLONE_UNTRACED)) {
		if (clone_flags & CLONE_VFORK)
			trace = PTRACE_EVENT_VFORK;
		else if ((clone_flags & CSIGNAL) != SIGCHLD)
			trace = PTRACE_EVENT_CLONE;
		else
			trace = PTRACE_EVENT_FORK;

		if (likely(!ptrace_event_enabled(current, trace)))
			trace = 0;
	}

	p = copy_process(clone_flags, stack_start, stack_size, // current task 를 부모로 갖는 task 생성
			 child_tidptr, NULL, trace, tls);
	/*
	 * Do this prior waking up the new thread - the thread pointer
	 * might get invalid after that point, if the thread exits quickly.
	 */
	if (!IS_ERR(p)) {
		struct completion vfork;
		struct pid *pid;

		trace_sched_process_fork(current, p);

		pid = get_task_pid(p, PIDTYPE_PID); // struct pid 를 가져옴
		nr = pid_vnr(pid); // 정수형 pid 를 가져옴 

		if (clone_flags & CLONE_PARENT_SETTID)
			put_user(nr, parent_tidptr);

		if (clone_flags & CLONE_VFORK) {
			p->vfork_done = &vfork;
			init_completion(&vfork);
			get_task_struct(p);
		}

		wake_up_new_task(p); // 새로 생성된 task 가 실행 될 수 있도록 queue 에 넣어줌

		/* forking complete and child started to run, tell ptracer */
		if (unlikely(trace))
			ptrace_event_pid(trace, pid);

		if (clone_flags & CLONE_VFORK) {
			if (!wait_for_vfork_done(p, &vfork))
				ptrace_event_pid(PTRACE_EVENT_VFORK_DONE, pid);
		}

		put_pid(pid);
	} else {
		nr = PTR_ERR(p);
	}
	return nr; // 정수형 PID 리턴
}

#ifndef CONFIG_HAVE_COPY_THREAD_TLS
/* For compatibility with architectures that call do_fork directly rather than
 * using the syscall entry points below. */
long do_fork(unsigned long clone_flags,
	      unsigned long stack_start,
	      unsigned long stack_size,
	      int __user *parent_tidptr,
	      int __user *child_tidptr)
{
	return _do_fork(clone_flags, stack_start, stack_size,
			parent_tidptr, child_tidptr, 0);
}
#endif

/*
 * Create a kernel thread.
 */
pid_t kernel_thread(int (*fn)(void *), void *arg, unsigned long flags)
{
	return _do_fork(flags|CLONE_VM|CLONE_UNTRACED, (unsigned long)fn,
		(unsigned long)arg, NULL, NULL, 0);
}

#ifdef __ARCH_WANT_SYS_FORK
SYSCALL_DEFINE0(fork)
{
#ifdef CONFIG_MMU
	return _do_fork(SIGCHLD, 0, 0, NULL, NULL, 0);
#else
	/* can not support in nommu mode */
	return -EINVAL;
#endif
}
#endif

#ifdef __ARCH_WANT_SYS_VFORK
SYSCALL_DEFINE0(vfork)
{
	return _do_fork(CLONE_VFORK | CLONE_VM | SIGCHLD, 0,
			0, NULL, NULL, 0);
}
#endif

#ifdef __ARCH_WANT_SYS_CLONE
#ifdef CONFIG_CLONE_BACKWARDS
SYSCALL_DEFINE5(clone, unsigned long, clone_flags, unsigned long, newsp,
		 int __user *, parent_tidptr,
		 unsigned long, tls,
		 int __user *, child_tidptr)
#elif defined(CONFIG_CLONE_BACKWARDS2)
SYSCALL_DEFINE5(clone, unsigned long, newsp, unsigned long, clone_flags,
		 int __user *, parent_tidptr,
		 int __user *, child_tidptr,
		 unsigned long, tls)
#elif defined(CONFIG_CLONE_BACKWARDS3)
SYSCALL_DEFINE6(clone, unsigned long, clone_flags, unsigned long, newsp,
		int, stack_size,
		int __user *, parent_tidptr,
		int __user *, child_tidptr,
		unsigned long, tls)
#else
SYSCALL_DEFINE5(clone, unsigned long, clone_flags, unsigned long, newsp,
		 int __user *, parent_tidptr,
		 int __user *, child_tidptr,
		 unsigned long, tls)
#endif
{
	return _do_fork(clone_flags, newsp, 0, parent_tidptr, child_tidptr, tls);
}
#endif

#ifndef ARCH_MIN_MMSTRUCT_ALIGN
#define ARCH_MIN_MMSTRUCT_ALIGN 0
#endif

static void sighand_ctor(void *data)
{
	struct sighand_struct *sighand = data;

	spin_lock_init(&sighand->siglock);
	init_waitqueue_head(&sighand->signalfd_wqh);
}

void __init proc_caches_init(void)
{
	/**
	 * @brief kmem_cache 초기화
	 */
	sighand_cachep = kmem_cache_create("sighand_cache", // sighand_cache 생성
			sizeof(struct sighand_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_DESTROY_BY_RCU|
			SLAB_NOTRACK|SLAB_ACCOUNT, sighand_ctor);
	signal_cachep = kmem_cache_create("signal_cache", // signal_cache 생성
			sizeof(struct signal_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_NOTRACK|SLAB_ACCOUNT,
			NULL);
	files_cachep = kmem_cache_create("files_cache", // files_cache 생성
			sizeof(struct files_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_NOTRACK|SLAB_ACCOUNT,
			NULL);
	fs_cachep = kmem_cache_create("fs_cache", // fs_cache 생성
			sizeof(struct fs_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_NOTRACK|SLAB_ACCOUNT,
			NULL);
	/*
	 * FIXME! The "sizeof(struct mm_struct)" currently includes the
	 * whole struct cpumask for the OFFSTACK case. We could change
	 * this to *only* allocate as much of it as required by the
	 * maximum number of CPU's we can ever have.  The cpumask_allocation
	 * is at the end of the structure, exactly for that reason.
	 */
	mm_cachep = kmem_cache_create("mm_struct", // mm_struct
			sizeof(struct mm_struct), ARCH_MIN_MMSTRUCT_ALIGN,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_NOTRACK|SLAB_ACCOUNT,
			NULL);
	vm_area_cachep = KMEM_CACHE(vm_area_struct, SLAB_PANIC|SLAB_ACCOUNT); // vm_area_struct 생성
	mmap_init(); // vm_committed_as 초기화
	nsproxy_cache_init(); // nsproxy cache 초기화
}

/*
 * Check constraints on flags passed to the unshare system call.
 */
static int check_unshare_flags(unsigned long unshare_flags)
{
	if (unshare_flags & ~(CLONE_THREAD|CLONE_FS|CLONE_NEWNS|CLONE_SIGHAND|
				CLONE_VM|CLONE_FILES|CLONE_SYSVSEM|
				CLONE_NEWUTS|CLONE_NEWIPC|CLONE_NEWNET|
				CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWCGROUP))
		return -EINVAL;
	/*
	 * Not implemented, but pretend it works if there is nothing
	 * to unshare.  Note that unsharing the address space or the
	 * signal handlers also need to unshare the signal queues (aka
	 * CLONE_THREAD).
	 */
	if (unshare_flags & (CLONE_THREAD | CLONE_SIGHAND | CLONE_VM)) {
		if (!thread_group_empty(current))
			return -EINVAL;
	}
	if (unshare_flags & (CLONE_SIGHAND | CLONE_VM)) {
		if (atomic_read(&current->sighand->count) > 1)
			return -EINVAL;
	}
	if (unshare_flags & CLONE_VM) {
		if (!current_is_single_threaded())
			return -EINVAL;
	}

	return 0;
}

/*
 * Unshare the filesystem structure if it is being shared
 */
static int unshare_fs(unsigned long unshare_flags, struct fs_struct **new_fsp)
{
	struct fs_struct *fs = current->fs;

	if (!(unshare_flags & CLONE_FS) || !fs)
		return 0;

	/* don't need lock here; in the worst case we'll do useless copy */
	if (fs->users == 1)
		return 0;

	*new_fsp = copy_fs_struct(fs);
	if (!*new_fsp)
		return -ENOMEM;

	return 0;
}

/*
 * Unshare file descriptor table if it is being shared
 */
static int unshare_fd(unsigned long unshare_flags, struct files_struct **new_fdp)
{
	struct files_struct *fd = current->files;
	int error = 0;

	if ((unshare_flags & CLONE_FILES) &&
	    (fd && atomic_read(&fd->count) > 1)) {
		*new_fdp = dup_fd(fd, &error);
		if (!*new_fdp)
			return error;
	}

	return 0;
}

/*
 * unshare allows a process to 'unshare' part of the process
 * context which was originally shared using clone.  copy_*
 * functions used by do_fork() cannot be used here directly
 * because they modify an inactive task_struct that is being
 * constructed. Here we are modifying the current, active,
 * task_struct.
 */
SYSCALL_DEFINE1(unshare, unsigned long, unshare_flags)
{
	struct fs_struct *fs, *new_fs = NULL;
	struct files_struct *fd, *new_fd = NULL;
	struct cred *new_cred = NULL;
	struct nsproxy *new_nsproxy = NULL;
	int do_sysvsem = 0;
	int err;

	/*
	 * If unsharing a user namespace must also unshare the thread group
	 * and unshare the filesystem root and working directories.
	 */
	if (unshare_flags & CLONE_NEWUSER)
		unshare_flags |= CLONE_THREAD | CLONE_FS;
	/*
	 * If unsharing vm, must also unshare signal handlers.
	 */
	if (unshare_flags & CLONE_VM)
		unshare_flags |= CLONE_SIGHAND;
	/*
	 * If unsharing a signal handlers, must also unshare the signal queues.
	 */
	if (unshare_flags & CLONE_SIGHAND)
		unshare_flags |= CLONE_THREAD;
	/*
	 * If unsharing namespace, must also unshare filesystem information.
	 */
	if (unshare_flags & CLONE_NEWNS)
		unshare_flags |= CLONE_FS;

	err = check_unshare_flags(unshare_flags);
	if (err)
		goto bad_unshare_out;
	/*
	 * CLONE_NEWIPC must also detach from the undolist: after switching
	 * to a new ipc namespace, the semaphore arrays from the old
	 * namespace are unreachable.
	 */
	if (unshare_flags & (CLONE_NEWIPC|CLONE_SYSVSEM))
		do_sysvsem = 1;
	err = unshare_fs(unshare_flags, &new_fs);
	if (err)
		goto bad_unshare_out;
	err = unshare_fd(unshare_flags, &new_fd);
	if (err)
		goto bad_unshare_cleanup_fs;
	err = unshare_userns(unshare_flags, &new_cred);
	if (err)
		goto bad_unshare_cleanup_fd;
	err = unshare_nsproxy_namespaces(unshare_flags, &new_nsproxy,
					 new_cred, new_fs);
	if (err)
		goto bad_unshare_cleanup_cred;

	if (new_fs || new_fd || do_sysvsem || new_cred || new_nsproxy) {
		if (do_sysvsem) {
			/*
			 * CLONE_SYSVSEM is equivalent to sys_exit().
			 */
			exit_sem(current);
		}
		if (unshare_flags & CLONE_NEWIPC) {
			/* Orphan segments in old ns (see sem above). */
			exit_shm(current);
			shm_init_task(current);
		}

		if (new_nsproxy)
			switch_task_namespaces(current, new_nsproxy);

		task_lock(current);

		if (new_fs) {
			fs = current->fs;
			spin_lock(&fs->lock);
			current->fs = new_fs;
			if (--fs->users)
				new_fs = NULL;
			else
				new_fs = fs;
			spin_unlock(&fs->lock);
		}

		if (new_fd) {
			fd = current->files;
			current->files = new_fd;
			new_fd = fd;
		}

		task_unlock(current);

		if (new_cred) {
			/* Install the new user namespace */
			commit_creds(new_cred);
			new_cred = NULL;
		}
	}

bad_unshare_cleanup_cred:
	if (new_cred)
		put_cred(new_cred);
bad_unshare_cleanup_fd:
	if (new_fd)
		put_files_struct(new_fd);

bad_unshare_cleanup_fs:
	if (new_fs)
		free_fs_struct(new_fs);

bad_unshare_out:
	return err;
}

/*
 *	Helper to unshare the files of the current task.
 *	We don't want to expose copy_files internals to
 *	the exec layer of the kernel.
 */

int unshare_files(struct files_struct **displaced)
{
	struct task_struct *task = current;
	struct files_struct *copy = NULL;
	int error;

	error = unshare_fd(CLONE_FILES, &copy);
	if (error || !copy) {
		*displaced = NULL;
		return error;
	}
	*displaced = task->files;
	task_lock(task);
	task->files = copy;
	task_unlock(task);
	return 0;
}

int sysctl_max_threads(struct ctl_table *table, int write,
		       void __user *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	int ret;
	int threads = max_threads;
	int min = MIN_THREADS;
	int max = MAX_THREADS;

	t = *table;
	t.data = &threads;
	t.extra1 = &min;
	t.extra2 = &max;

	ret = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	set_max_threads(threads);

	return 0;
}
