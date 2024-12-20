/*
 * Slab allocator functions that are independent of the allocator strategy
 *
 * (C) 2012 Christoph Lameter <cl@linux.com>
 */
#include <linux/slab.h>

#include <linux/mm.h>
#include <linux/poison.h>
#include <linux/interrupt.h>
#include <linux/memory.h>
#include <linux/compiler.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/page.h>
#include <linux/memcontrol.h>

#define CREATE_TRACE_POINTS
#include <trace/events/kmem.h>

#include "slab.h"

enum slab_state slab_state;
LIST_HEAD(slab_caches);
DEFINE_MUTEX(slab_mutex);
struct kmem_cache *kmem_cache;

/*
 * Set of flags that will prevent slab merging
 */
#define SLAB_NEVER_MERGE (SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER | \
		SLAB_TRACE | SLAB_DESTROY_BY_RCU | SLAB_NOLEAKTRACE | \
		SLAB_FAILSLAB | SLAB_KASAN)

#define SLAB_MERGE_SAME (SLAB_RECLAIM_ACCOUNT | SLAB_CACHE_DMA | \
			 SLAB_NOTRACK | SLAB_ACCOUNT)

/*
 * Merge control. If this is set then no merging of slab caches will occur.
 * (Could be removed. This was introduced to pacify the merge skeptics.)
 */
static int slab_nomerge;

static int __init setup_slab_nomerge(char *str)
{
	slab_nomerge = 1;
	return 1;
}

#ifdef CONFIG_SLUB
__setup_param("slub_nomerge", slub_nomerge, setup_slab_nomerge, 0);
#endif

__setup("slab_nomerge", setup_slab_nomerge);

/*
 * Determine the size of a slab object
 */
unsigned int kmem_cache_size(struct kmem_cache *s)
{
	return s->object_size;
}
EXPORT_SYMBOL(kmem_cache_size);

#ifdef CONFIG_DEBUG_VM
static int kmem_cache_sanity_check(const char *name, size_t size)
{
	struct kmem_cache *s = NULL;

	if (!name || in_interrupt() || size < sizeof(void *) ||
		size > KMALLOC_MAX_SIZE) {
		pr_err("kmem_cache_create(%s) integrity check failed\n", name);
		return -EINVAL;
	}

	list_for_each_entry(s, &slab_caches, list) {
		char tmp;
		int res;

		/*
		 * This happens when the module gets unloaded and doesn't
		 * destroy its slab cache and no-one else reuses the vmalloc
		 * area of the module.  Print a warning.
		 */
		res = probe_kernel_address(s->name, tmp);
		if (res) {
			pr_err("Slab cache with size %d has lost its name\n",
			       s->object_size);
			continue;
		}
	}

	WARN_ON(strchr(name, ' '));	/* It confuses parsers */
	return 0;
}
#else
static inline int kmem_cache_sanity_check(const char *name, size_t size)
{
	return 0;
}
#endif

void __kmem_cache_free_bulk(struct kmem_cache *s, size_t nr, void **p)
{
	size_t i;

	for (i = 0; i < nr; i++) {
		if (s)
			kmem_cache_free(s, p[i]);
		else
			kfree(p[i]);
	}
}

int __kmem_cache_alloc_bulk(struct kmem_cache *s, gfp_t flags, size_t nr,
								void **p)
{
	size_t i;

	for (i = 0; i < nr; i++) {
		void *x = p[i] = kmem_cache_alloc(s, flags);
		if (!x) {
			__kmem_cache_free_bulk(s, i, p);
			return 0;
		}
	}
	return i;
}

#if defined(CONFIG_MEMCG) && !defined(CONFIG_SLOB)
void slab_init_memcg_params(struct kmem_cache *s)
{
	s->memcg_params.is_root_cache = true;
	INIT_LIST_HEAD(&s->memcg_params.list);
	RCU_INIT_POINTER(s->memcg_params.memcg_caches, NULL);
}

static int init_memcg_params(struct kmem_cache *s,
		struct mem_cgroup *memcg, struct kmem_cache *root_cache)
{
	struct memcg_cache_array *arr;

	if (memcg) {
		s->memcg_params.is_root_cache = false;
		s->memcg_params.memcg = memcg;
		s->memcg_params.root_cache = root_cache;
		return 0;
	}

	slab_init_memcg_params(s);

	if (!memcg_nr_cache_ids)
		return 0;

	arr = kzalloc(sizeof(struct memcg_cache_array) +
		      memcg_nr_cache_ids * sizeof(void *),
		      GFP_KERNEL);
	if (!arr)
		return -ENOMEM;

	RCU_INIT_POINTER(s->memcg_params.memcg_caches, arr);
	return 0;
}

static void destroy_memcg_params(struct kmem_cache *s)
{
	if (is_root_cache(s))
		kfree(rcu_access_pointer(s->memcg_params.memcg_caches));
}

static int update_memcg_params(struct kmem_cache *s, int new_array_size)
{
	struct memcg_cache_array *old, *new;

	if (!is_root_cache(s))
		return 0;

	new = kzalloc(sizeof(struct memcg_cache_array) +
		      new_array_size * sizeof(void *), GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	old = rcu_dereference_protected(s->memcg_params.memcg_caches,
					lockdep_is_held(&slab_mutex));
	if (old)
		memcpy(new->entries, old->entries,
		       memcg_nr_cache_ids * sizeof(void *));

	rcu_assign_pointer(s->memcg_params.memcg_caches, new);
	if (old)
		kfree_rcu(old, rcu);
	return 0;
}

int memcg_update_all_caches(int num_memcgs)
{
	struct kmem_cache *s;
	int ret = 0;

	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list) {
		ret = update_memcg_params(s, num_memcgs);
		/*
		 * Instead of freeing the memory, we'll just leave the caches
		 * up to this point in an updated state.
		 */
		if (ret)
			break;
	}
	mutex_unlock(&slab_mutex);
	return ret;
}
#else
static inline int init_memcg_params(struct kmem_cache *s,
		struct mem_cgroup *memcg, struct kmem_cache *root_cache)
{
	return 0;
}

static inline void destroy_memcg_params(struct kmem_cache *s)
{
}
#endif /* CONFIG_MEMCG && !CONFIG_SLOB */

/*
 * Find a mergeable slab cache
 */
int slab_unmergeable(struct kmem_cache *s)
{
	/**
	 * @brief unmergeable 인지 확인
	 * @return
	 *  1 - unmergeable
	 *  0 - mergeable
	 */
	if (slab_nomerge || (s->flags & SLAB_NEVER_MERGE)) // no merge 인 경우
		return 1;

	if (!is_root_cache(s))  // root cache인 경우
		return 1;

	if (s->ctor) // 객체 생성자 함수가 있는 경우
		return 1;

	/*
	 * We may have set a slab to be unmergeable during bootstrap.
	 */
	if (s->refcount < 0) // ref count 가 0 이하인 경우 (bootstrap 에서 unmergeable로 설정한 경우)
		return 1;

	return 0;
}

struct kmem_cache *find_mergeable(size_t size, size_t align,
		unsigned long flags, const char *name, void (*ctor)(void *))
{
	/**
	 * @brief list 를 순회하면서 merge 가능한 kmem_cache를 찾는다.
	 * @return
	 *  &kmem_cache - mergeable kmem_cache가 있는 경우
	 *  null - mergeable kmem_cache가 없는 경우
	 */
	struct kmem_cache *s;

	if (slab_nomerge || (flags & SLAB_NEVER_MERGE)) // nomerge 플래그가 있는 경우
		return NULL;

	if (ctor) // constructor가 있는 경우 
		return NULL;

	size = ALIGN(size, sizeof(void *)); // WORD 단위로 size 값을 정렬 (size를 8의 배수로 만듦)
	align = calculate_alignment(flags, align, size); // size, align 값으로 실제 사용할 align 값을 계산
	size = ALIGN(size, align); // 실제로 사용할 size 값 계산
	flags = kmem_cache_flags(size, flags, name, NULL);

	list_for_each_entry_reverse(s, &slab_caches, list) { // list 역순으로 순회
		if (slab_unmergeable(s)) // mergeable 체크
			continue;

		if (size > s->size) // 사이즈 체크
			continue;

		if ((flags & SLAB_MERGE_SAME) != (s->flags & SLAB_MERGE_SAME)) // SLAB_MERGE_SAME flag가 동일하게 있는 경우
			continue;
		/*
		 * Check if alignment is compatible.
		 * Courtesy of Adrian Drzewiecki
		 */
		if ((s->size & ~(align - 1)) != s->size) // cache 크기가 재조정된 align 단위로 정렬이 되지 않는 경우
			continue;

		if (s->size - size >= sizeof(void *)) // 재조정한 size 값이 align 되지 않는 경우
			continue;

		if (IS_ENABLED(CONFIG_SLAB) && align && // SLAB 옵션을 사용 중일 경우, 재조정된 align 값이 캐시의 align 값 보다 큰 경우
			(align > s->align || s->align % align))
			continue;

		return s; // 모든 조건이 충족되는 경우 해당 kmem_cache 리턴
	}
	return NULL;
}

/*
 * Figure out what the alignment of the objects will be given a set of
 * flags, a user specified alignment and the size of the objects.
 */
unsigned long calculate_alignment(unsigned long flags,
		unsigned long align, unsigned long size)
{
	/**
	 * @brief 입력한 size, align 값에 따라 최종적으로 사용할 align 값을 구한다.
	 */
	/*
	 * If the user wants hardware cache aligned objects then follow that
	 * suggestion if the object is sufficiently large.
	 *
	 * The hardware cache alignment cannot override the specified
	 * alignment though. If that is greater then use it.
	 */
	if (flags & SLAB_HWCACHE_ALIGN) { // 하드웨어 정렬 요청이 있는 경우
		unsigned long ralign = cache_line_size();
		while (size <= ralign / 2) // size보다 큰 2의 제곱수를 구한다.
			ralign /= 2;
		align = max(align, ralign); // 요청한 size보다 큰 2의 제곱수로 align 값을 정한다.
	}

	if (align < ARCH_SLAB_MINALIGN) // align 값이 Arm64에서 지정한 최소 값보다 작지 않도록 변경
		align = ARCH_SLAB_MINALIGN;

	return ALIGN(align, sizeof(void *)); // 지정한 align 값 리턴
}

static struct kmem_cache *create_cache(const char *name,
		size_t object_size, size_t size, size_t align,
		unsigned long flags, void (*ctor)(void *),
		struct mem_cgroup *memcg, struct kmem_cache *root_cache)
{
	/**
	 * @brief kmem_cache를 할당받아 slab_caches에 추가함
	 * @return
	 *  &kmem_cache - Pass
	 *  ErrorPointer - Fail
	 */
	struct kmem_cache *s;
	int err;

	err = -ENOMEM;
	s = kmem_cache_zalloc(kmem_cache, GFP_KERNEL); // kmem_cache 객체 할당
	if (!s)
		goto out;

	// 값 입력
	s->name = name;
	s->object_size = object_size;
	s->size = size;
	s->align = align;
	s->ctor = ctor;

	err = init_memcg_params(s, memcg, root_cache);
	if (err)
		goto out_free_cache;

	err = __kmem_cache_create(s, flags); // TODO) 4-121
	if (err)
		goto out_free_cache;

	s->refcount = 1; // 참조 카운트 업데이트
	list_add(&s->list, &slab_caches); // slab_caches 에 s->list 추가
out:
	if (err)
		return ERR_PTR(err);
	return s;

out_free_cache:
	destroy_memcg_params(s); // 입력했던 s 를 제거
	kmem_cache_free(kmem_cache, s); // 입력했던 s 를 제거
	goto out;
}

/*
 * kmem_cache_create - Create a cache.
 * @name: A string which is used in /proc/slabinfo to identify this cache.
 * @size: The size of objects to be created in this cache.
 * @align: The required alignment for the objects.
 * @flags: SLAB flags
 * @ctor: A constructor for the objects.
 *
 * Returns a ptr to the cache on success, NULL on failure.
 * Cannot be called within a interrupt, but can be interrupted.
 * The @ctor is run when new pages are allocated by the cache.
 *
 * The flags are
 *
 * %SLAB_POISON - Poison the slab with a known test pattern (a5a5a5a5)
 * to catch references to uninitialised memory.
 *
 * %SLAB_RED_ZONE - Insert `Red' zones around the allocated memory to check
 * for buffer overruns.
 *
 * %SLAB_HWCACHE_ALIGN - Align the objects in this cache to a hardware
 * cacheline.  This can be beneficial if you're counting cycles as closely
 * as davem.
 */
struct kmem_cache *
kmem_cache_create(const char *name, size_t size, size_t align,
		  unsigned long flags, void (*ctor)(void *))
{
	struct kmem_cache *s = NULL;
	const char *cache_name;
	int err;

	get_online_cpus(); // Hot plug 인 경우, cpu 동기화 대기
	get_online_mems(); // Hot plug 인 경우, mem 동기화 대기
	memcg_get_cache_ids(); // memcg rw 세마포어 동기화를 시작

	mutex_lock(&slab_mutex); // mutex lock

	err = kmem_cache_sanity_check(name, size); // DEBUG 옵션이 켜진 경우 cache name, size 체킹 수행
	if (err) {
		goto out_unlock; // err 발생한 경우 unlock 이후 return
	}

	/*
	 * Some allocators will constraint the set of valid flags to a subset
	 * of all flags. We expect them to define CACHE_CREATE_MASK in this
	 * case, and we'll just provide them with a sanitized version of the
	 * passed flags.
	 */
	flags &= CACHE_CREATE_MASK; // create_mask 만 남겨둠

	s = __kmem_cache_alias(name, size, align, flags, ctor); // cache에서 mergeable cache를 사용
	if (s) // 할당이 성공한 경우, 종료
		goto out_unlock;

	cache_name = kstrdup_const(name, GFP_KERNEL); // cache name을 복사함
	if (!cache_name) {
		err = -ENOMEM;
		goto out_unlock;
	}

	s = create_cache(cache_name, size, size, // TODO 4-120)
			 calculate_alignment(flags, align, size), // size, align 값에 맞춰서 최종적으로 사용할 align 값을 가져옴
			 flags, ctor, NULL, NULL);
	if (IS_ERR(s)) {
		err = PTR_ERR(s);
		kfree_const(cache_name);
	}

// mutex unlock 및 동기화 시퀀스 진행
out_unlock:
	mutex_unlock(&slab_mutex); // mutex unlock

	memcg_put_cache_ids(); // memcg sync
	put_online_mems(); // memory sync
	put_online_cpus(); // cpu sync

	if (err) {
		if (flags & SLAB_PANIC)
			panic("kmem_cache_create: Failed to create slab '%s'. Error %d\n",
				name, err);
		else {
			pr_warn("kmem_cache_create(%s) failed with error %d\n",
				name, err);
			dump_stack();
		}
		return NULL;
	}
	return s; // 할당받은 kmem_cache 리턴
}
EXPORT_SYMBOL(kmem_cache_create);

static int shutdown_cache(struct kmem_cache *s,
		struct list_head *release, bool *need_rcu_barrier)
{
	if (__kmem_cache_shutdown(s) != 0)
		return -EBUSY;

	if (s->flags & SLAB_DESTROY_BY_RCU)
		*need_rcu_barrier = true;

	list_move(&s->list, release);
	return 0;
}

static void release_caches(struct list_head *release, bool need_rcu_barrier)
{
	struct kmem_cache *s, *s2;

	if (need_rcu_barrier)
		rcu_barrier();

	list_for_each_entry_safe(s, s2, release, list) {
#ifdef SLAB_SUPPORTS_SYSFS
		sysfs_slab_remove(s);
#else
		slab_kmem_cache_release(s);
#endif
	}
}

#if defined(CONFIG_MEMCG) && !defined(CONFIG_SLOB)
/*
 * memcg_create_kmem_cache - Create a cache for a memory cgroup.
 * @memcg: The memory cgroup the new cache is for.
 * @root_cache: The parent of the new cache.
 *
 * This function attempts to create a kmem cache that will serve allocation
 * requests going from @memcg to @root_cache. The new cache inherits properties
 * from its parent.
 */
void memcg_create_kmem_cache(struct mem_cgroup *memcg,
			     struct kmem_cache *root_cache)
{
	static char memcg_name_buf[NAME_MAX + 1]; /* protected by slab_mutex */
	struct cgroup_subsys_state *css = &memcg->css;
	struct memcg_cache_array *arr;
	struct kmem_cache *s = NULL;
	char *cache_name;
	int idx;

	get_online_cpus();
	get_online_mems();

	mutex_lock(&slab_mutex);

	/*
	 * The memory cgroup could have been offlined while the cache
	 * creation work was pending.
	 */
	if (memcg->kmem_state != KMEM_ONLINE)
		goto out_unlock;

	idx = memcg_cache_id(memcg);
	arr = rcu_dereference_protected(root_cache->memcg_params.memcg_caches,
					lockdep_is_held(&slab_mutex));

	/*
	 * Since per-memcg caches are created asynchronously on first
	 * allocation (see memcg_kmem_get_cache()), several threads can try to
	 * create the same cache, but only one of them may succeed.
	 */
	if (arr->entries[idx])
		goto out_unlock;

	cgroup_name(css->cgroup, memcg_name_buf, sizeof(memcg_name_buf));
	cache_name = kasprintf(GFP_KERNEL, "%s(%d:%s)", root_cache->name,
			       css->id, memcg_name_buf);
	if (!cache_name)
		goto out_unlock;

	s = create_cache(cache_name, root_cache->object_size,
			 root_cache->size, root_cache->align,
			 root_cache->flags, root_cache->ctor,
			 memcg, root_cache);
	/*
	 * If we could not create a memcg cache, do not complain, because
	 * that's not critical at all as we can always proceed with the root
	 * cache.
	 */
	if (IS_ERR(s)) {
		kfree(cache_name);
		goto out_unlock;
	}

	list_add(&s->memcg_params.list, &root_cache->memcg_params.list);

	/*
	 * Since readers won't lock (see cache_from_memcg_idx()), we need a
	 * barrier here to ensure nobody will see the kmem_cache partially
	 * initialized.
	 */
	smp_wmb();
	arr->entries[idx] = s;

out_unlock:
	mutex_unlock(&slab_mutex);

	put_online_mems();
	put_online_cpus();
}

void memcg_deactivate_kmem_caches(struct mem_cgroup *memcg)
{
	int idx;
	struct memcg_cache_array *arr;
	struct kmem_cache *s, *c;

	idx = memcg_cache_id(memcg);

	get_online_cpus();
	get_online_mems();

	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list) {
		if (!is_root_cache(s))
			continue;

		arr = rcu_dereference_protected(s->memcg_params.memcg_caches,
						lockdep_is_held(&slab_mutex));
		c = arr->entries[idx];
		if (!c)
			continue;

		__kmem_cache_shrink(c, true);
		arr->entries[idx] = NULL;
	}
	mutex_unlock(&slab_mutex);

	put_online_mems();
	put_online_cpus();
}

static int __shutdown_memcg_cache(struct kmem_cache *s,
		struct list_head *release, bool *need_rcu_barrier)
{
	BUG_ON(is_root_cache(s));

	if (shutdown_cache(s, release, need_rcu_barrier))
		return -EBUSY;

	list_del(&s->memcg_params.list);
	return 0;
}

void memcg_destroy_kmem_caches(struct mem_cgroup *memcg)
{
	LIST_HEAD(release);
	bool need_rcu_barrier = false;
	struct kmem_cache *s, *s2;

	get_online_cpus();
	get_online_mems();

	mutex_lock(&slab_mutex);
	list_for_each_entry_safe(s, s2, &slab_caches, list) {
		if (is_root_cache(s) || s->memcg_params.memcg != memcg)
			continue;
		/*
		 * The cgroup is about to be freed and therefore has no charges
		 * left. Hence, all its caches must be empty by now.
		 */
		BUG_ON(__shutdown_memcg_cache(s, &release, &need_rcu_barrier));
	}
	mutex_unlock(&slab_mutex);

	put_online_mems();
	put_online_cpus();

	release_caches(&release, need_rcu_barrier);
}

static int shutdown_memcg_caches(struct kmem_cache *s,
		struct list_head *release, bool *need_rcu_barrier)
{
	struct memcg_cache_array *arr;
	struct kmem_cache *c, *c2;
	LIST_HEAD(busy);
	int i;

	BUG_ON(!is_root_cache(s));

	/*
	 * First, shutdown active caches, i.e. caches that belong to online
	 * memory cgroups.
	 */
	arr = rcu_dereference_protected(s->memcg_params.memcg_caches,
					lockdep_is_held(&slab_mutex));
	for_each_memcg_cache_index(i) {
		c = arr->entries[i];
		if (!c)
			continue;
		if (__shutdown_memcg_cache(c, release, need_rcu_barrier))
			/*
			 * The cache still has objects. Move it to a temporary
			 * list so as not to try to destroy it for a second
			 * time while iterating over inactive caches below.
			 */
			list_move(&c->memcg_params.list, &busy);
		else
			/*
			 * The cache is empty and will be destroyed soon. Clear
			 * the pointer to it in the memcg_caches array so that
			 * it will never be accessed even if the root cache
			 * stays alive.
			 */
			arr->entries[i] = NULL;
	}

	/*
	 * Second, shutdown all caches left from memory cgroups that are now
	 * offline.
	 */
	list_for_each_entry_safe(c, c2, &s->memcg_params.list,
				 memcg_params.list)
		__shutdown_memcg_cache(c, release, need_rcu_barrier);

	list_splice(&busy, &s->memcg_params.list);

	/*
	 * A cache being destroyed must be empty. In particular, this means
	 * that all per memcg caches attached to it must be empty too.
	 */
	if (!list_empty(&s->memcg_params.list))
		return -EBUSY;
	return 0;
}
#else
static inline int shutdown_memcg_caches(struct kmem_cache *s,
		struct list_head *release, bool *need_rcu_barrier)
{
	return 0;
}
#endif /* CONFIG_MEMCG && !CONFIG_SLOB */

void slab_kmem_cache_release(struct kmem_cache *s)
{
	__kmem_cache_release(s);
	destroy_memcg_params(s);
	kfree_const(s->name);
	kmem_cache_free(kmem_cache, s);
}

void kmem_cache_destroy(struct kmem_cache *s)
{
	LIST_HEAD(release);
	bool need_rcu_barrier = false;
	int err;

	if (unlikely(!s))
		return;

	get_online_cpus();
	get_online_mems();

	mutex_lock(&slab_mutex);

	s->refcount--;
	if (s->refcount)
		goto out_unlock;

	err = shutdown_memcg_caches(s, &release, &need_rcu_barrier);
	if (!err)
		err = shutdown_cache(s, &release, &need_rcu_barrier);

	if (err) {
		pr_err("kmem_cache_destroy %s: Slab cache still has objects\n",
		       s->name);
		dump_stack();
	}
out_unlock:
	mutex_unlock(&slab_mutex);

	put_online_mems();
	put_online_cpus();

	release_caches(&release, need_rcu_barrier);
}
EXPORT_SYMBOL(kmem_cache_destroy);

/**
 * kmem_cache_shrink - Shrink a cache.
 * @cachep: The cache to shrink.
 *
 * Releases as many slabs as possible for a cache.
 * To help debugging, a zero exit status indicates all slabs were released.
 */
int kmem_cache_shrink(struct kmem_cache *cachep)
{
	int ret;

	get_online_cpus();
	get_online_mems();
	ret = __kmem_cache_shrink(cachep, false);
	put_online_mems();
	put_online_cpus();
	return ret;
}
EXPORT_SYMBOL(kmem_cache_shrink);

bool slab_is_available(void)
{
	return slab_state >= UP;
}

#ifndef CONFIG_SLOB
/* Create a cache during boot when no slab services are available yet */
void __init create_boot_cache(struct kmem_cache *s, const char *name, size_t size,
		unsigned long flags)
{
	int err;

	s->name = name;
	s->size = s->object_size = size;
	s->align = calculate_alignment(flags, ARCH_KMALLOC_MINALIGN, size); // align 값 계산

	slab_init_memcg_params(s); // s->memcg_param 값을 초기화

	err = __kmem_cache_create(s, flags); // kmem 캐시 생성

	if (err)
		panic("Creation of kmalloc slab %s size=%zu failed. Reason %d\n",
					name, size, err);

	s->refcount = -1;	/* Exempt from merging for now */ // 참조 카운터는 추후 업데이트
}

struct kmem_cache *__init create_kmalloc_cache(const char *name, size_t size,
				unsigned long flags)
{
	struct kmem_cache *s = kmem_cache_zalloc(kmem_cache, GFP_NOWAIT); // slab 객체를 kmem_cache에서 받아옴

	if (!s)
		panic("Out of memory when creating slab %s\n", name);

	create_boot_cache(s, name, size, flags); // cpu cache를 per-cpu 자료형으로 할당 받음
	list_add(&s->list, &slab_caches); // slab_caches list에 추가
	s->refcount = 1; // refcount 추가
	return s;
}

struct kmem_cache *kmalloc_caches[KMALLOC_SHIFT_HIGH + 1];
EXPORT_SYMBOL(kmalloc_caches);

#ifdef CONFIG_ZONE_DMA
struct kmem_cache *kmalloc_dma_caches[KMALLOC_SHIFT_HIGH + 1];
EXPORT_SYMBOL(kmalloc_dma_caches);
#endif

/*
 * Conversion table for small slabs sizes / 8 to the index in the
 * kmalloc array. This is necessary for slabs < 192 since we have non power
 * of two cache sizes there. The size of larger slabs can be determined using
 * fls.
 */
static s8 size_index[24] = {
	3,	/* 8 */
	4,	/* 16 */
	5,	/* 24 */
	5,	/* 32 */
	6,	/* 40 */
	6,	/* 48 */
	6,	/* 56 */
	6,	/* 64 */
	1,	/* 72 */
	1,	/* 80 */
	1,	/* 88 */
	1,	/* 96 */
	7,	/* 104 */
	7,	/* 112 */
	7,	/* 120 */
	7,	/* 128 */
	2,	/* 136 */
	2,	/* 144 */
	2,	/* 152 */
	2,	/* 160 */
	2,	/* 168 */
	2,	/* 176 */
	2,	/* 184 */
	2	/* 192 */
};

static inline int size_index_elem(size_t bytes)
{
	return (bytes - 1) / 8;
}

/*
 * Find the kmem_cache structure that serves a given size of
 * allocation
 */
struct kmem_cache *kmalloc_slab(size_t size, gfp_t flags)
{
	/**
	 * @brief size 값으로 적절한 kmem_cache 배열에 사용할 index 값을 구한 뒤, kmalloc_caches 에서 index에 해당하는 kmem_cache 포인터를 리턴한다.
	 * @param[in] size 할당할 사이즈
	 * @param[in] flags 할당 flags
	 * @return
	 *  kmem_cache* - Pass  
	 *  ZERO_SIZE_PTR, NULL - Fail
	 */
	int index;

	if (unlikely(size > KMALLOC_MAX_SIZE)) { // Malloc 최대 사이즈를 넘어가는 경우
		WARN_ON_ONCE(!(flags & __GFP_NOWARN)); // NOWARN 플래그가 없는 경우 WARN 발생
		return NULL; // Fail
	}

	if (size <= 192) {
		if (!size) // size가 192 이하인 경우에는 size_index 테이블을 사용한다.
			return ZERO_SIZE_PTR; // Fail

		index = size_index[size_index_elem(size)]; // size index 값 계산
	} else
		index = fls(size - 1); // 필요한 비트수 계산

#ifdef CONFIG_ZONE_DMA
	if (unlikely((flags & GFP_DMA))) // DMA zone을 사용하는 경우
		return kmalloc_dma_caches[index]; // DMA Cache index 값 리턴

#endif
	return kmalloc_caches[index]; // cache에서 지정된 index 값 리턴
}

/*
 * kmalloc_info[] is to make slub_debug=,kmalloc-xx option work at boot time.
 * kmalloc_index() supports up to 2^26=64MB, so the final entry of the table is
 * kmalloc-67108864.
 */
static struct {
	const char *name;
	unsigned long size;
} const kmalloc_info[] __initconst = {
	{NULL,                      0},		{"kmalloc-96",             96},
	{"kmalloc-192",           192},		{"kmalloc-8",               8},
	{"kmalloc-16",             16},		{"kmalloc-32",             32},
	{"kmalloc-64",             64},		{"kmalloc-128",           128},
	{"kmalloc-256",           256},		{"kmalloc-512",           512},
	{"kmalloc-1024",         1024},		{"kmalloc-2048",         2048},
	{"kmalloc-4096",         4096},		{"kmalloc-8192",         8192},
	{"kmalloc-16384",       16384},		{"kmalloc-32768",       32768},
	{"kmalloc-65536",       65536},		{"kmalloc-131072",     131072},
	{"kmalloc-262144",     262144},		{"kmalloc-524288",     524288},
	{"kmalloc-1048576",   1048576},		{"kmalloc-2097152",   2097152},
	{"kmalloc-4194304",   4194304},		{"kmalloc-8388608",   8388608},
	{"kmalloc-16777216", 16777216},		{"kmalloc-33554432", 33554432},
	{"kmalloc-67108864", 67108864}
};

/*
 * Patch up the size_index table if we have strange large alignment
 * requirements for the kmalloc array. This is only the case for
 * MIPS it seems. The standard arches will not generate any code here.
 *
 * Largest permitted alignment is 256 bytes due to the way we
 * handle the index determination for the smaller caches.
 *
 * Make sure that nothing crazy happens if someone starts tinkering
 * around with ARCH_KMALLOC_MINALIGN
 */
void __init setup_kmalloc_cache_index_table(void)
{
	int i;

	BUILD_BUG_ON(KMALLOC_MIN_SIZE > 256 ||
		(KMALLOC_MIN_SIZE & (KMALLOC_MIN_SIZE - 1))); // KMALLOC MIN SIZE가 L1 Cache Size 보다 클 경우 Build error

	for (i = 8; i < KMALLOC_MIN_SIZE; i += 8) { // 8 byte 단위 계산
		int elem = size_index_elem(i); // size index 값 계산

		if (elem >= ARRAY_SIZE(size_index)) // out_of_range 방지
			break;
		size_index[elem] = KMALLOC_SHIFT_LOW; // log2(L1 cache)
	}

	if (KMALLOC_MIN_SIZE >= 64) { // MIN_SIZE가 64byte 이상인 경우 cache 최적화에서 96byte 지원이 필요 없음
		/*
		 * The 96 byte size cache is not used if the alignment
		 * is 64 byte.
		 */
		for (i = 64 + 8; i <= 96; i += 8)
			size_index[size_index_elem(i)] = 7;

	}

	if (KMALLOC_MIN_SIZE >= 128) { // MIN_SIZE가 128byte 이상인 경우 cache 최적화에서 192byte 지원이 필요 없음
		/*
		 * The 192 byte sized cache is not used if the alignment
		 * is 128 byte. Redirect kmalloc to use the 256 byte cache
		 * instead.
		 */
		for (i = 128 + 8; i <= 192; i += 8)
			size_index[size_index_elem(i)] = 8;
	}
}

static void __init new_kmalloc_cache(int idx, unsigned long flags)
{
	kmalloc_caches[idx] = create_kmalloc_cache(kmalloc_info[idx].name,
					kmalloc_info[idx].size, flags);
}

/*
 * Create the kmalloc array. Some of the regular kmalloc arrays
 * may already have been created because they were needed to
 * enable allocations for slab creation.
 */
void __init create_kmalloc_caches(unsigned long flags)
{
	int i;

	for (i = KMALLOC_SHIFT_LOW; i <= KMALLOC_SHIFT_HIGH; i++) {
		if (!kmalloc_caches[i]) // cache 할당이 안되어 있을 경우 할당
			new_kmalloc_cache(i, flags);

		/*
		 * Caches that are not of the two-to-the-power-of size.
		 * These have to be created immediately after the
		 * earlier power of two caches
		 */
		if (KMALLOC_MIN_SIZE <= 32 && !kmalloc_caches[1] && i == 6)
			new_kmalloc_cache(1, flags);
		if (KMALLOC_MIN_SIZE <= 64 && !kmalloc_caches[2] && i == 7)
			new_kmalloc_cache(2, flags);
	}

	/* Kmalloc array is now usable */
	slab_state = UP;

#ifdef CONFIG_ZONE_DMA
	for (i = 0; i <= KMALLOC_SHIFT_HIGH; i++) {
		struct kmem_cache *s = kmalloc_caches[i];

		if (s) {
			int size = kmalloc_size(i);
			char *n = kasprintf(GFP_NOWAIT,
				 "dma-kmalloc-%d", size);

			BUG_ON(!n);
			kmalloc_dma_caches[i] = create_kmalloc_cache(n,
				size, SLAB_CACHE_DMA | flags);
		}
	}
#endif
}
#endif /* !CONFIG_SLOB */

/*
 * To avoid unnecessary overhead, we pass through large allocation requests
 * directly to the page allocator. We use __GFP_COMP, because we will need to
 * know the allocation order to free the pages properly in kfree.
 */
void *kmalloc_order(size_t size, gfp_t flags, unsigned int order)
{
	/**
	 * @param[in] size 할당할 사이즈
	 * @param[in] flags 할당할 때 사용할 flag
	 * @param[in] order memory order
	 * @return
	 *  *page - Pass
	 *  null - Fail
	 */
	void *ret;
	struct page *page;

	flags |= __GFP_COMP; // 메타데이터 또는 연속된 복합 페이지를 구성하도록 요청
	page = alloc_kmem_pages(flags, order); // TODO) 4-153
	ret = page ? page_address(page) : NULL; // 페이지가 정상적으로 할당되었는 지 확인
	kmemleak_alloc(ret, size, 1, flags); // 새로 할당한 페이지를 object로 등록
	kasan_kmalloc_large(ret, size, flags); // 디버깅 코드
	return ret;
}
EXPORT_SYMBOL(kmalloc_order);

#ifdef CONFIG_TRACING
void *kmalloc_order_trace(size_t size, gfp_t flags, unsigned int order)
{
	void *ret = kmalloc_order(size, flags, order);
	trace_kmalloc(_RET_IP_, ret, size, PAGE_SIZE << order, flags);
	return ret;
}
EXPORT_SYMBOL(kmalloc_order_trace);
#endif

#ifdef CONFIG_SLABINFO

#ifdef CONFIG_SLAB
#define SLABINFO_RIGHTS (S_IWUSR | S_IRUSR)
#else
#define SLABINFO_RIGHTS S_IRUSR
#endif

static void print_slabinfo_header(struct seq_file *m)
{
	/*
	 * Output format version, so at least we can change it
	 * without _too_ many complaints.
	 */
#ifdef CONFIG_DEBUG_SLAB
	seq_puts(m, "slabinfo - version: 2.1 (statistics)\n");
#else
	seq_puts(m, "slabinfo - version: 2.1\n");
#endif
	seq_puts(m, "# name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab>");
	seq_puts(m, " : tunables <limit> <batchcount> <sharedfactor>");
	seq_puts(m, " : slabdata <active_slabs> <num_slabs> <sharedavail>");
#ifdef CONFIG_DEBUG_SLAB
	seq_puts(m, " : globalstat <listallocs> <maxobjs> <grown> <reaped> <error> <maxfreeable> <nodeallocs> <remotefrees> <alienoverflow>");
	seq_puts(m, " : cpustat <allochit> <allocmiss> <freehit> <freemiss>");
#endif
	seq_putc(m, '\n');
}

void *slab_start(struct seq_file *m, loff_t *pos)
{
	mutex_lock(&slab_mutex);
	return seq_list_start(&slab_caches, *pos);
}

void *slab_next(struct seq_file *m, void *p, loff_t *pos)
{
	return seq_list_next(p, &slab_caches, pos);
}

void slab_stop(struct seq_file *m, void *p)
{
	mutex_unlock(&slab_mutex);
}

static void
memcg_accumulate_slabinfo(struct kmem_cache *s, struct slabinfo *info)
{
	struct kmem_cache *c;
	struct slabinfo sinfo;

	if (!is_root_cache(s))
		return;

	for_each_memcg_cache(c, s) {
		memset(&sinfo, 0, sizeof(sinfo));
		get_slabinfo(c, &sinfo);

		info->active_slabs += sinfo.active_slabs;
		info->num_slabs += sinfo.num_slabs;
		info->shared_avail += sinfo.shared_avail;
		info->active_objs += sinfo.active_objs;
		info->num_objs += sinfo.num_objs;
	}
}

static void cache_show(struct kmem_cache *s, struct seq_file *m)
{
	struct slabinfo sinfo;

	memset(&sinfo, 0, sizeof(sinfo));
	get_slabinfo(s, &sinfo);

	memcg_accumulate_slabinfo(s, &sinfo);

	seq_printf(m, "%-17s %6lu %6lu %6u %4u %4d",
		   cache_name(s), sinfo.active_objs, sinfo.num_objs, s->size,
		   sinfo.objects_per_slab, (1 << sinfo.cache_order));

	seq_printf(m, " : tunables %4u %4u %4u",
		   sinfo.limit, sinfo.batchcount, sinfo.shared);
	seq_printf(m, " : slabdata %6lu %6lu %6lu",
		   sinfo.active_slabs, sinfo.num_slabs, sinfo.shared_avail);
	slabinfo_show_stats(m, s);
	seq_putc(m, '\n');
}

static int slab_show(struct seq_file *m, void *p)
{
	struct kmem_cache *s = list_entry(p, struct kmem_cache, list);

	if (p == slab_caches.next)
		print_slabinfo_header(m);
	if (is_root_cache(s))
		cache_show(s, m);
	return 0;
}

#if defined(CONFIG_MEMCG) && !defined(CONFIG_SLOB)
int memcg_slab_show(struct seq_file *m, void *p)
{
	struct kmem_cache *s = list_entry(p, struct kmem_cache, list);
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));

	if (p == slab_caches.next)
		print_slabinfo_header(m);
	if (!is_root_cache(s) && s->memcg_params.memcg == memcg)
		cache_show(s, m);
	return 0;
}
#endif

/*
 * slabinfo_op - iterator that generates /proc/slabinfo
 *
 * Output layout:
 * cache-name
 * num-active-objs
 * total-objs
 * object size
 * num-active-slabs
 * total-slabs
 * num-pages-per-slab
 * + further values on SMP and with statistics enabled
 */
static const struct seq_operations slabinfo_op = {
	.start = slab_start,
	.next = slab_next,
	.stop = slab_stop,
	.show = slab_show,
};

static int slabinfo_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &slabinfo_op);
}

static const struct file_operations proc_slabinfo_operations = {
	.open		= slabinfo_open,
	.read		= seq_read,
	.write          = slabinfo_write,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int __init slab_proc_init(void)
{
	proc_create("slabinfo", SLABINFO_RIGHTS, NULL,
						&proc_slabinfo_operations);
	return 0;
}
module_init(slab_proc_init);
#endif /* CONFIG_SLABINFO */

static __always_inline void *__do_krealloc(const void *p, size_t new_size,
					   gfp_t flags)
{
	void *ret;
	size_t ks = 0;

	if (p)
		ks = ksize(p);

	if (ks >= new_size) {
		kasan_krealloc((void *)p, new_size, flags);
		return (void *)p;
	}

	ret = kmalloc_track_caller(new_size, flags);
	if (ret && p)
		memcpy(ret, p, ks);

	return ret;
}

/**
 * __krealloc - like krealloc() but don't free @p.
 * @p: object to reallocate memory for.
 * @new_size: how many bytes of memory are required.
 * @flags: the type of memory to allocate.
 *
 * This function is like krealloc() except it never frees the originally
 * allocated buffer. Use this if you don't want to free the buffer immediately
 * like, for example, with RCU.
 */
void *__krealloc(const void *p, size_t new_size, gfp_t flags)
{
	if (unlikely(!new_size))
		return ZERO_SIZE_PTR;

	return __do_krealloc(p, new_size, flags);

}
EXPORT_SYMBOL(__krealloc);

/**
 * krealloc - reallocate memory. The contents will remain unchanged.
 * @p: object to reallocate memory for.
 * @new_size: how many bytes of memory are required.
 * @flags: the type of memory to allocate.
 *
 * The contents of the object pointed to are preserved up to the
 * lesser of the new and old sizes.  If @p is %NULL, krealloc()
 * behaves exactly like kmalloc().  If @new_size is 0 and @p is not a
 * %NULL pointer, the object pointed to is freed.
 */
void *krealloc(const void *p, size_t new_size, gfp_t flags)
{
	void *ret;

	if (unlikely(!new_size)) {
		kfree(p);
		return ZERO_SIZE_PTR;
	}

	ret = __do_krealloc(p, new_size, flags);
	if (ret && p != ret)
		kfree(p);

	return ret;
}
EXPORT_SYMBOL(krealloc);

/**
 * kzfree - like kfree but zero memory
 * @p: object to free memory of
 *
 * The memory of the object @p points to is zeroed before freed.
 * If @p is %NULL, kzfree() does nothing.
 *
 * Note: this function zeroes the whole allocated buffer which can be a good
 * deal bigger than the requested buffer size passed to kmalloc(). So be
 * careful when using this function in performance sensitive code.
 */
void kzfree(const void *p)
{
	size_t ks;
	void *mem = (void *)p;

	if (unlikely(ZERO_OR_NULL_PTR(mem)))
		return;
	ks = ksize(mem);
	memset(mem, 0, ks);
	kfree(mem);
}
EXPORT_SYMBOL(kzfree);

/* Tracepoints definitions. */
EXPORT_TRACEPOINT_SYMBOL(kmalloc);
EXPORT_TRACEPOINT_SYMBOL(kmem_cache_alloc);
EXPORT_TRACEPOINT_SYMBOL(kmalloc_node);
EXPORT_TRACEPOINT_SYMBOL(kmem_cache_alloc_node);
EXPORT_TRACEPOINT_SYMBOL(kfree);
EXPORT_TRACEPOINT_SYMBOL(kmem_cache_free);
