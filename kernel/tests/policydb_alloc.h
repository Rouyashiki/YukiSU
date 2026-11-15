#ifndef POLICYDB_TEST_ALLOC_H
#define POLICYDB_TEST_ALLOC_H

#include <linux/slab.h>
#include <linux/vmalloc.h>

void *test_alloc(size_t size, gfp_t flags, struct kmem_cache *cache,
		 bool virtual, const char *site);
void test_free(const void *ptr, struct kmem_cache *cache, bool virtual);
void *test_copy(const void *ptr, size_t size, gfp_t flags, const char *site);

#define test_kmalloc(n, g) test_alloc(n, g, NULL, false, __func__)
#define test_kzalloc(n, g) test_kmalloc(n, (g) | __GFP_ZERO)
#define test_kmalloc_array(n, s, g) test_kmalloc(array_size(n, s), g)
#define test_kcalloc(n, s, g) test_kzalloc(array_size(n, s), g)
#define test_kvcalloc(n, s, g)                                                 \
	test_alloc(array_size(n, s), (g) | __GFP_ZERO, NULL, true, __func__)
#define test_vmalloc(n) test_alloc(n, GFP_KERNEL, NULL, true, __func__)
#define test_kmem_cache_zalloc(c, g)                                           \
	test_alloc(0, (g) | __GFP_ZERO, c, false, __func__)
#define test_kmem_cache_free(c, p) test_free(p, c, false)
#define test_kfree(p) test_free(p, NULL, false)
#define test_kvfree(p) test_free(p, NULL, true)
#define test_vfree(p) test_free(p, NULL, true)
#define test_kstrdup(p, g) test_copy(p, strlen(p) + 1, g, __func__)
#define test_kmemdup(p, n, g) test_copy(p, n, g, __func__)

#endif
