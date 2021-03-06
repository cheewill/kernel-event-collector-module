// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2019-2020 VMware, Inc. All rights reserved.
// Copyright (c) 2016-2019 Carbon Black, Inc. All rights reserved.

#include "priv.h"
#include "hash-table-generic.h"
#include "cb-spinlock.h"

#define get_refcountp(hashTblp, ptr)  ((atomic64_t *)((ptr)+(hashTblp)->refcount_offset))
#define get_datap(hashTblp, ptr)  ((void *)((ptr)-(hashTblp)->node_offset))
#define get_nodep(hashTblp, ptr)  ((HashTableNode *)((ptr)+(hashTblp)->node_offset))

static int debug;

static uint64_t g_hashtbl_generic_lock;
static LIST_HEAD(g_hashtbl_generic);

#define HASHTBL_PRINT(fmt, ...)    do { if (debug) pr_err("hash-tbl: " fmt, ##__VA_ARGS__); } while (0)

static int hashtbl_del_generic_lockheld(HashTbl *hashTblp, void *datap, ProcessContext *context);
static int hashtbl_add_generic_lockheld(HashTbl *hashTblp, void *datap, ProcessContext *context);

void debug_on(void)
{
    debug = 1;
}

void debug_off(void)
{
    debug = 0;
}

char *key_in_hex(ProcessContext *context, unsigned char *key, int key_len)
{
    int i;
    char *str = (char *) cb_mem_cache_alloc_generic(key_len * 3, context);

    for (i = 0; i < key_len; i++)
    {
        sprintf(str + i*3, "%02x ", key[i]);
    }
    str[key_len * 3 - 1] = '\0';
    return str;
}

inline void *get_key_ptr(HashTbl *hashTblp, void *datap)
{
    return (void *) datap + hashTblp->key_offset;
}

void hashtbl_generic_init(ProcessContext *context)
{
    cb_spinlock_init(&g_hashtbl_generic_lock, context);
}

void hashtbl_generic_destoy(ProcessContext *context)
{
    cb_spinlock_destroy(&g_hashtbl_generic_lock, context);
}

HashTbl *hashtbl_init_generic(ProcessContext *context,
                              uint64_t numberOfBuckets,
                              uint64_t datasize,
                              uint64_t sizehint,
                              const char *hashtble_name,
                              int key_len,
                              int key_offset,
                              int node_offset,
                              int refcount_offset,
                              hashtbl_delete_cb delete_callback)
{
    HashTbl *hashTblp = NULL;
    int tableSize = ((numberOfBuckets * sizeof(struct hlist_head)) + sizeof(HashTbl));
    unsigned char *tbl_storage_p  = NULL;
    uint64_t cache_elem_size;

    //Since we're not in an atomic context this is an acceptable alternative to
    //kmalloc however, it should be noted that this is a little less efficient. The reason for this is
    //fragmentation that can occur on systems. We noticed this happening in the field, and if highly
    //fragmented, our driver will fail to load with a normal kmalloc
    tbl_storage_p  = cb_mem_cache_valloc_generic(tableSize, context);


    if (tbl_storage_p  == NULL)
    {
        HASHTBL_PRINT("Failed to allocate %lluB at %s:%d.", (numberOfBuckets * sizeof(struct hlist_head)) + sizeof(HashTbl),
                                                            __func__,
                                                            __LINE__);
        return NULL;
    }

    //With kzalloc we get zeroing for free, with vmalloc we need to do it ourself
    memset(tbl_storage_p, 0, tableSize);

    if (sizehint > datasize)
    {
        cache_elem_size = sizehint;
    } else
    {
        cache_elem_size = datasize;
    }

    HASHTBL_PRINT("Cache=%s elemsize=%llu hint=%llu\n", hashtble_name, cache_elem_size, sizehint);

    hashTblp = (HashTbl *)tbl_storage_p;
    hashTblp->tablePtr = (struct hlist_head *)(tbl_storage_p + sizeof(HashTbl));
    hashTblp->numberOfBuckets = numberOfBuckets;
    cb_spinlock_init(&(hashTblp->tableSpinlock), context);
    hashTblp->key_len     = key_len;
    hashTblp->key_offset  = key_offset;
    hashTblp->node_offset = node_offset;
    hashTblp->refcount_offset = refcount_offset;
    hashTblp->base_size   = tableSize + sizeof(HashTbl);
    hashTblp->delete_callback = delete_callback;

    if (cache_elem_size)
    {
        if (!cb_mem_cache_create(&hashTblp->hash_cache, hashtble_name, cache_elem_size, context))
        {
            cb_mem_cache_free_generic(hashTblp);
            return 0;
        }
    }

    cb_write_lock(&g_hashtbl_generic_lock, context);
    list_add(&(hashTblp->genTables), &g_hashtbl_generic);
    cb_write_unlock(&g_hashtbl_generic_lock, context);

    HASHTBL_PRINT("Size=%d NumberOfBuckets=%llu\n", tableSize, numberOfBuckets);
    HASHTBL_PRINT("ADDR=%p TADDR=%p OFFSET=%lu\n", hashTblp, hashTblp->tablePtr, sizeof(HashTbl));
    return hashTblp;
}

void hashtbl_shutdown_generic(HashTbl *hashTblp, ProcessContext *context)
{
    atomic64_set(&(hashTblp->tableShutdown), 1);

    cb_write_lock(&g_hashtbl_generic_lock, context);
    list_del(&(hashTblp->genTables));
    cb_write_unlock(&g_hashtbl_generic_lock, context);

    hashtbl_clear_generic(hashTblp, context);

    HASHTBL_PRINT("hash shutdown inst=%ld alloc=%ld\n", atomic64_read(&(hashTblp->tableInstance)),
        atomic64_read(&(hashTblp->hash_cache.allocated_count)));

    cb_spinlock_destroy(&(hashTblp->tableSpinlock), context);
    cb_mem_cache_destroy(&hashTblp->hash_cache, context, NULL);
    cb_mem_cache_free_generic(hashTblp);
}

static int _hashtbl_delete_callback(HashTbl *hashTblp, HashTableNode *nodep, void *priv, ProcessContext *context)
{
    return ACTION_DELETE;
}

void hashtbl_clear_generic(HashTbl *hashTblp, ProcessContext *context)
{
    HASHTBL_PRINT("ADDR=%p TADDR=%p OFFSET=%lu\n", hashTblp, hashTblp->tablePtr, sizeof(HashTbl));

    hashtbl_write_for_each_generic(hashTblp, _hashtbl_delete_callback, NULL, context);
}

static void hashtbl_for_each_generic_locked(HashTbl *hashTblp, hashtbl_for_each_generic_cb callback, void *priv, bool haveWriteLock, ProcessContext *context);

void hashtbl_write_for_each_generic(HashTbl *hashTblp, hashtbl_for_each_generic_cb callback, void *priv, ProcessContext *context)
{
    cb_write_lock(&(hashTblp->tableSpinlock), context);
    hashtbl_for_each_generic_locked(hashTblp, callback, priv, true, context);
    cb_write_unlock(&(hashTblp->tableSpinlock), context);
}

void hashtbl_read_for_each_generic(HashTbl *hashTblp, hashtbl_for_each_generic_cb callback, void *priv, ProcessContext *context)
{
    cb_read_lock(&(hashTblp->tableSpinlock), context);
    hashtbl_for_each_generic_locked(hashTblp, callback, priv, false, context);
    cb_read_unlock(&(hashTblp->tableSpinlock), context);
}

static void hashtbl_for_each_generic_locked(HashTbl *hashTblp, hashtbl_for_each_generic_cb callback, void *priv, bool haveWriteLock, ProcessContext *context)
{
    int i;
    uint64_t numberOfBuckets;
    struct hlist_head *hashtbl_tbl  = NULL;

    if (!hashTblp) return;

    hashtbl_tbl = hashTblp->tablePtr;
    numberOfBuckets  = hashTblp->numberOfBuckets;

    // May need to walk the lists too
    for (i = 0; i < numberOfBuckets; ++i)
    {
        struct hlist_head *bucketp = &hashtbl_tbl[i];
        HashTableNode *nodep = 0;
        struct hlist_node *tmp;

        if (!hlist_empty(bucketp))
        {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
            hlist_for_each_entry_safe(nodep, tmp, bucketp, link)
#else
            struct hlist_node *_nodep;

            hlist_for_each_entry_safe(nodep, _nodep, tmp, bucketp, link)
#endif
            {

                switch ((*callback)(hashTblp, get_datap(hashTblp, nodep), priv, context))
                {
                case ACTION_DELETE:
                    // This should never be called with only a read lock
                    BUG_ON(!haveWriteLock);
                    hlist_del(&nodep->link);
                    atomic64_dec(&(hashTblp->tableInstance));
                    hashtbl_free_generic(hashTblp, nodep, context);
                    break;
                case ACTION_STOP:
                    goto Exit;
                    break;
                case ACTION_CONTINUE:
                default:
                    break;
                }
            }
        }
    }

Exit:
    // Signal the callback we are done.  It may need to clean up something in the context
    (*callback)(hashTblp, NULL, priv, context);
    return;
}

static int hash_key(void *key, int len, int bucket_num)
{
    int i;
    char *data = (char *) key;
    unsigned int hash = 5381;

    for (i = 0; i < len; i++)
    {
        hash = ((hash << 5) + hash) + data[i]; // hash * 33 + data[i]
    }
    return hash % bucket_num;
}

static int hashtbl_add_generic_lockheld(HashTbl *hashTblp, void *datap, ProcessContext *context)
{
    uint64_t bucket_indx;
    struct hlist_head *bucketp = NULL;
    char *key_str;

    if (datap == NULL)
    {
        return -1;
    }

    bucket_indx = hash_key(get_key_ptr(hashTblp, datap), hashTblp->key_len, hashTblp->numberOfBuckets);
    bucketp = &(hashTblp->tablePtr[bucket_indx]);


    hlist_add_head(&get_nodep(hashTblp, datap)->link, bucketp);
    if (debug)
    {
        key_str = key_in_hex(context, get_key_ptr(hashTblp, datap), hashTblp->key_len);
        HASHTBL_PRINT("%s: bucket=%llu key=%s\n", __func__, bucket_indx, key_str);
        cb_mem_cache_free_generic(key_str);
    }

    atomic64_inc(&(hashTblp->tableInstance));
    return 0;
}

int hashtbl_add_generic(HashTbl *hashTblp, void *datap, ProcessContext *context)
{
    int ret = 0;

    if (atomic64_read(&(hashTblp->tableShutdown)) == 1)
    {
        return -1;
    }

    if (hashTblp->refcount_offset != HASHTBL_DISABLE_REF_COUNT)
    {
        atomic64_inc(get_refcountp(hashTblp, datap));
    }

    cb_write_lock(&(hashTblp->tableSpinlock), context);
    ret = hashtbl_add_generic_lockheld(hashTblp, datap, context);
    cb_write_unlock(&(hashTblp->tableSpinlock), context);

    return ret;
}

void *hashtbl_get_generic(HashTbl *hashTblp, void *key, ProcessContext *context)
{
    uint64_t bucket_indx;
    struct hlist_head *bucketp;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
    struct hlist_node *_nodep = NULL;
#endif
    HashTableNode *nodep = NULL;
    char *key_str;
    void *datap = NULL;

    if (atomic64_read(&(hashTblp->tableShutdown)) == 1)
    { goto ng_exit; }

    bucket_indx = hash_key(key, hashTblp->key_len, hashTblp->numberOfBuckets);
    bucketp = &(hashTblp->tablePtr[bucket_indx]);

    if (debug)
    {
        key_str = key_in_hex(context, key, hashTblp->key_len);
        HASHTBL_PRINT("%s: bucket=%llu key=%s\n", __func__, bucket_indx, key_str);
        cb_mem_cache_free_generic(key_str);
    }

    cb_read_lock(&(hashTblp->tableSpinlock), context);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
    hlist_for_each_entry(nodep, bucketp, link)
#else
    hlist_for_each_entry(nodep, _nodep, bucketp, link)
#endif
    {
        if (memcmp(key, get_key_ptr(hashTblp, get_datap(hashTblp, nodep)), hashTblp->key_len) == 0)
        {
            cb_read_unlock(&(hashTblp->tableSpinlock), context);
            datap = get_datap(hashTblp, nodep);

            if (hashTblp->refcount_offset != HASHTBL_DISABLE_REF_COUNT)
            {
                atomic64_inc(get_refcountp(hashTblp, datap));
            }
            return datap;
        }
    }
    cb_read_unlock(&(hashTblp->tableSpinlock), context);

ng_exit:
    return NULL;
}

static int hashtbl_del_generic_lockheld(HashTbl *hashTblp, void *datap, ProcessContext *context)
{
    HashTableNode *nodep = get_nodep(hashTblp, datap);

    // This protects against hashtbl_del_generic being called twice for the same datap
    if ((&nodep->link)->pprev != NULL)
    {
        hlist_del_init(&nodep->link);

        if (atomic64_read(&(hashTblp->tableInstance)) == 0)
        {
            HASHTBL_PRINT("hashtbl_del: underflow!!\n");
        } else
        {
            atomic64_dec(&(hashTblp->tableInstance));
        }

        // The only reason this should happen is if hashtbl_del_generic and
        // hashtbl_put_generic are called out of order,
        // e.g. hashtbl_put_generic -> hashtbl_del_generic
        if (hashTblp->refcount_offset != HASHTBL_DISABLE_REF_COUNT)
        {
            WARN(atomic64_read(get_refcountp(hashTblp, datap)) == 1, "hashtbl will free while lock held");
        }

        hashtbl_put_generic(hashTblp, datap, context);

        return 0;
    } else
    {
        pr_err("Attempt to delete a NULL object from the hash table");
    }

    return -1;
}

void *hashtbl_del_by_key_generic(HashTbl *hashTblp, void *key, ProcessContext *context)
{
    void *datap = hashtbl_get_generic(hashTblp, key, context);

    if (datap)
    {
        // We remove the item from the table, and remove the reference held by the table.
        // The item will not be freed because the reference count was incremented by get.
        hashtbl_del_generic(hashTblp, datap, context);
    }
    // caller must put or free (if no reference count)
    return datap;
}

void hashtbl_del_generic(HashTbl *hashTblp, void *datap, ProcessContext *context)
{
    CANCEL_VOID(atomic64_read(&(hashTblp->tableShutdown)) != 1);

    cb_write_lock(&(hashTblp->tableSpinlock), context);
    hashtbl_del_generic_lockheld(hashTblp, datap, context);
    cb_write_unlock(&(hashTblp->tableSpinlock), context);
}

void *hashtbl_alloc_generic(HashTbl *hashTblp, ProcessContext *context)
{
    void *datap;

    CANCEL(atomic64_read(&(hashTblp->tableShutdown)) != 1, NULL);

    datap = cb_mem_cache_alloc(&hashTblp->hash_cache, context);
    CANCEL(datap, NULL);

    INIT_HLIST_NODE(&get_nodep(hashTblp, datap)->link);
    return datap;
}

void hashtbl_put_generic(HashTbl *hashTblp, void *datap, ProcessContext *context)
{
    if (hashTblp->refcount_offset != HASHTBL_DISABLE_REF_COUNT)
    {
         IF_ATOMIC64_DEC_AND_TEST__CHECK_NEG(get_refcountp(hashTblp, datap), {
            hashtbl_free_generic(hashTblp, datap, context);
        });
    }
}

void hashtbl_free_generic(HashTbl *hashTblp, void *datap, ProcessContext *context)
{
    if (datap)
    {
        if (hashTblp->delete_callback)
        {
            hashTblp->delete_callback(get_datap(hashTblp, datap), context);
        }
        cb_mem_cache_free(&hashTblp->hash_cache, datap, context);
    }
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
#define CACHE_SIZE(a)      a->object_size
#else
#define CACHE_SIZE(a)      a->buffer_size
#endif

// Loop over each hash table and calculate the memory used
size_t hashtbl_get_memory(ProcessContext *context)
{
    HashTbl *hashTblp;
    size_t   size = 0;

    cb_read_lock(&g_hashtbl_generic_lock, context);
    list_for_each_entry(hashTblp, &g_hashtbl_generic, genTables) {
            size += hashTblp->base_size;
    }
    cb_read_unlock(&g_hashtbl_generic_lock, context);

    return size;
}
