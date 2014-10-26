#include <sys/mman.h>
#include "sm-alloc.h"
#include "sm-common.h"
#include "../txn.h"
#include "../masstree_btree.h"
#include <future>
#include <new>

// RA GC states. Transitions btw these states are racy
// (should be fine assuming gc finishes before the new
// active region depletes).
#define RA_NORMAL       0
#define RA_GC_REQUESTED 1
#define RA_GC_IN_PROG   2
#define RA_GC_FINISHED  3

class region_allocator {
    friend void RA::epoch_reclaimed(void *cookie, void *epoch_cookie);
    friend void* RA::epoch_ended(void *cookie, epoch_num e);
private:
    enum { NUM_SEGMENT_BITS=2 };
    enum { NUM_SEGMENTS=1<<NUM_SEGMENT_BITS };

    // low-contention and read-mostly stuff
    char *_hot_data;
    char *_cold_data;
    uint64_t _segment_bits;
    uint64_t _hot_bits;
    uint64_t _hot_capacity;
    uint64_t _cold_capacity;
    uint64_t _hot_mask;
    uint64_t _cold_mask;
    uint64_t _reclaimed_offset;
    int _socket;

    // high contention, needs its own cache line
    uint64_t __attribute__((aligned(64))) _allocated_hot_offset;
    uint64_t __attribute__((aligned(64))) _allocated_cold_offset;

    // gc related
    std::mutex _reclaim_mutex;
    std::condition_variable _reclaim_cv;
    uint64_t _allocated;
    int _state;

public:
    void* allocate(uint64_t size);
    void* allocate_cold(uint64_t size);
    region_allocator(uint64_t one_segment_bits, int skt);
    ~region_allocator();
    int state() { return volatile_read(_state); }
    void set_state(int s)   { volatile_write(_state, s); }
    inline void trigger_reclaim()  { _reclaim_cv.notify_all(); }
    static void reclaim_daemon(int socket);
};


namespace RA {
    static const uint64_t PAGE_SIZE_BITS = 16; // Windows uses 64kB pages...
    static const uint64_t MEM_SEGMENT_BITS = 30; // 1GB/segment (16 GB total on 4-socket machine)
    static_assert(MEM_SEGMENT_BITS > PAGE_SIZE_BITS,
                  "Region allocator segments can't be smaller than a page");
    static const uint64_t TRIM_MARK = 16 * 1024 * 1024;

	std::vector<concurrent_btree*> tables;
    ra_wrapper ra_w;
    region_allocator *ra;
    int ra_nsock;
    int ra_nthreads;
    LSN trim_lsn;
    bool system_loading;
    __thread region_allocator *tls_ra = 0;

    void register_table(concurrent_btree *t) {
        tables.push_back(t);
    }

    void init() {
        if (ra_nsock)
            return;
        
        trim_lsn = INVALID_LSN;
        system_loading = true;
        int nodes = numa_max_node() + 1;
        ra = (region_allocator *)malloc(sizeof(region_allocator) * nodes);
        std::future<region_allocator*> futures[nodes];
        for (int i = 0; i < nodes; i++) {
            auto f = [=]{ return new (ra + i) region_allocator(MEM_SEGMENT_BITS, i); };
            futures[i] = std::async(std::launch::async, f);
        }

        // make sure the threads finish before we leave
        for (auto &f : futures)
            (void*) f.get();

        ra_nsock = nodes;
    }

    void register_thread() {
        if (tls_ra)
            return;

        auto rnum = __sync_fetch_and_add(&ra_nthreads, 1);
        auto snum = rnum % ra_nsock;
        numa_run_on_node(snum);
        tls_ra = &ra[snum];
    }
    void *allocate(uint64_t size) {
        auto *myra = tls_ra;
        if (not myra)
            myra = &ra[sched_getcpu() % ra_nsock];
        if (likely(!system_loading))
            return myra->allocate(size);
        return myra->allocate_cold(size);
    }

    void *allocate_cold(uint64_t size) {
        auto *myra = tls_ra;
        if (not myra)
            myra = &ra[sched_getcpu() % ra_nsock];
        return myra->allocate_cold(size);
    }

    // epochs related
    __thread struct thread_data epoch_tls;
    epoch_mgr ra_epochs {{nullptr, &global_init, &get_tls,
                        &thread_registered, &thread_deregistered,
                        &epoch_ended, &epoch_ended_thread, &epoch_reclaimed}};

    // epoch mgr callbacks
    epoch_mgr::tls_storage *
    get_tls(void*)
    {
        static __thread epoch_mgr::tls_storage s;
        return &s;
    }

    void global_init(void*)
    {
    }

    void*
    thread_registered(void*)
    {
        epoch_tls.initialized = true;
        return &epoch_tls;
    }

    void
    thread_deregistered(void *cookie, void *thread_cookie)
    {
        auto *t = (thread_data*) thread_cookie;
        ASSERT(t == &epoch_tls);
        t->initialized = false;
    }

    void*
    epoch_ended(void *cookie, epoch_num e)
    {
        // So we need this rcu_is_active here because
        // epoch_ended is called not only when an epoch is eneded,
        // but also when threads exit (see epoch.cpp:274-283 in function
        // epoch_mgr::thread_init(). So we need to avoid the latter case
        // as when thread exits it will no longer be in the rcu region
        // created by the scoped_rcu_region in the transaction class.
        for (int i = 0; i < ra_nsock; i++) {
            region_allocator *r = RA::ra + i;
            int s = r->state();
            if (s == RA_GC_REQUESTED || s == RA_GC_FINISHED) {
                LSN *lsn = (LSN *)malloc(sizeof(LSN));
                if (likely(RCU::rcu_is_active()))
                    *lsn = transaction_base::logger->cur_lsn();
                else
                    *lsn = INVALID_LSN;
                return lsn;
            }
        }
        return NULL;
    }

    void*
    epoch_ended_thread(void *cookie, void *epoch_cookie, void *thread_cookie)
    {
        //return NULL;
        return epoch_cookie;
        //return thread_cookie;
    }

    void
    epoch_reclaimed(void *cookie, void *epoch_cookie)
    {
        LSN lsn = *(LSN *)epoch_cookie;
        if (lsn != INVALID_LSN)
            trim_lsn = *(LSN *)epoch_cookie;
        free(epoch_cookie);

        for (int i = 0; i < ra_nsock; i++) {
            region_allocator *r = RA::ra + i;
            int s = r->state();
            if (s == RA_GC_REQUESTED) {
                r->set_state(RA_GC_IN_PROG);
                r->trigger_reclaim();
            }
            else if (s == RA_GC_FINISHED) {
                std::cout << "region allocator: spared for socket " << r->_socket << "\n";
                volatile_write(r->_reclaimed_offset,
                    r->_reclaimed_offset + (1 << r->_segment_bits));  // no need to %
                r->set_state(RA_NORMAL);
            }
        }
    }

    void
    epoch_enter(void)
    {
        if (!epoch_tls.initialized) {
            ra_epochs.thread_init();
        }
        ra_epochs.thread_enter();
    }

    void
    epoch_exit(void)
    {
        ra_epochs.thread_quiesce();
        ra_epochs.thread_exit();
    }

    void
    epoch_thread_quiesce(void)
    {
        ra_epochs.thread_quiesce();
    }
};

region_allocator::region_allocator(uint64_t one_segment_bits, int skt)
    : _segment_bits(one_segment_bits)
    , _hot_bits(NUM_SEGMENT_BITS + _segment_bits)
    , _hot_capacity(uint64_t{1} << _hot_bits)
    , _cold_capacity((uint64_t{1} << _segment_bits) * 2)
    , _hot_mask(_hot_capacity - 1)
    , _cold_mask(_cold_capacity - 1)
    , _reclaimed_offset(_hot_capacity)
    , _socket(skt)
    , _allocated_hot_offset(0)
    , _allocated_cold_offset(0)
    , _allocated(0)
    , _state(RA_NORMAL)
{
#warning Thread that runs region_allocator::region_allocator will be pinned to that socket
    numa_run_on_node(skt);
    _hot_data = (char*) numa_alloc_local(_hot_capacity);
    _cold_data = (char*) numa_alloc_local(_cold_capacity);
    ASSERT(_hot_data);
    std::cout << "memory region: faulting " << _hot_capacity << " bytes"
              << " on node " << skt << std::endl;
    memset(_hot_data, '\0', _hot_capacity);
    memset(_cold_data, '\0', _cold_capacity);
    std::thread reclaim_thd(reclaim_daemon, skt);
    reclaim_thd.detach();
}

region_allocator::~region_allocator()
{
    numa_free(_hot_data, _hot_capacity);
    numa_free(_cold_data, _cold_capacity);
}

void*
region_allocator::allocate(uint64_t size)
{
    __builtin_prefetch(&_segment_bits);
    
 retry:
    auto noffset = __sync_add_and_fetch(&_allocated_hot_offset, size);
    THROW_IF(volatile_read(_reclaimed_offset) < noffset, std::bad_alloc);
    __sync_add_and_fetch(&_allocated, size);

    auto sbits = _segment_bits;
    if (((noffset-1) >> sbits) != ((noffset-size)  >> sbits)) {
        // chunk spans a segment boundary, unusable
        std::cout << "opening segment " << (noffset >> sbits) << " of memory region for socket " << _socket << std::endl;
        //while(state() != RA_NORMAL);
        if (state() != RA_NORMAL)
            throw std::runtime_error("GC requested before last round finishes.");
        set_state(RA_GC_REQUESTED);
        goto retry;
    }

    if (_allocated >= RA::TRIM_MARK) {
        if (RA::ra_epochs.new_epoch_possible()) {
            if (RA::ra_epochs.new_epoch())
                __sync_add_and_fetch(&_allocated, -_allocated);
        }
    }

    return &_hot_data[(noffset - size) & _hot_mask];
}

void*
region_allocator::allocate_cold(uint64_t size)
{
    auto noffset = __sync_add_and_fetch(&_allocated_cold_offset, size);
    THROW_IF(volatile_read(_cold_capacity) < noffset, std::bad_alloc);
    return &_cold_data[(noffset - size) & _cold_mask];
}

void
region_allocator::reclaim_daemon(int socket)
{
    std::cout << "Allocator daemon started for socket " << socket << std::endl;
    region_allocator *myra = RA::ra + socket;
    std::unique_lock<std::mutex> lock(myra->_reclaim_mutex);
    uint64_t seg_size = 1 << myra->_segment_bits;

forever:
    myra->_reclaim_cv.wait(lock);
    LSN tlsn = volatile_read(RA::trim_lsn);
    uint64_t start_offset = (volatile_read(myra->_reclaimed_offset)) & myra->_hot_mask;
    uint64_t end_offset = start_offset + seg_size;
    ASSERT(!(start_offset & (seg_size - 1)));
    ASSERT(!(end_offset & (seg_size - 1)));

    std::cout << "region allocator: start to reclaim for socket "
              << socket << std::endl;

    uint64_t cold_copy_amt = 0, hot_copy_amt = 0;
    for (uint i = 0; i < RA::tables.size(); i++) {
        concurrent_btree *t = RA::tables[i];
        concurrent_btree::tuple_vector_type *v = t->get_tuple_vector();
        INVARIANT(v);

        for (oid_type oid = 1; oid < v->size(); oid++) {
start_over:
            object *head, *cur, *prev = NULL;
            cur = head = v->begin(oid);
            if (!cur)
                continue;

            object *new_obj = NULL;
            size_t size = cur->_size;
            uint64_t offset = (char *)cur - myra->_hot_data;

            dbtuple *version = reinterpret_cast<dbtuple *>(cur->payload());
            auto clsn = volatile_read(version->clsn);

            if (offset >= start_offset && offset + size <= end_offset && LSN::from_ptr(clsn) < tlsn) {
                new_obj = (object *)myra->allocate_cold(size);
                memcpy(new_obj, cur, size);
                new_obj->_next = NULL;
                cold_copy_amt += size;
                if (!__sync_bool_compare_and_swap(v->begin_ptr(oid), cur, new_obj))
                    goto start_over;
                continue;
            }

            while (cur) {
                size = cur->_size;
                offset = (char *)cur - myra->_hot_data;
                if (offset >= start_offset && offset + size <= end_offset) {
                    version = reinterpret_cast<dbtuple *>(cur->payload());
                    clsn = volatile_read(version->clsn);
                    if (LSN::from_ptr(clsn) < tlsn && prev) {
                        if (!__sync_bool_compare_and_swap(&prev->_next, cur, NULL))
                            goto start_over;
                        break;
                    }
                    new_obj = (object *)myra->allocate(size);
                    memcpy(new_obj, cur, size);
                    hot_copy_amt += size;

                    if (cur == head) {
                        ASSERT(!prev);
                        if (!__sync_bool_compare_and_swap(v->begin_ptr(oid), cur, new_obj))
                            goto start_over;
                    }
                    else {
                        ASSERT(prev);
                        if (!__sync_bool_compare_and_swap(&prev->_next, cur, new_obj))
                            goto start_over;
                    }
                }
                prev = volatile_read(cur);
                cur = volatile_read(cur->_next);
            }
        }
    }

    ASSERT(myra->state() == RA_GC_IN_PROG);
    myra->set_state(RA_GC_FINISHED);
    std::cout << "socket " << socket << " cold copy=" << cold_copy_amt
             << " bytes, hot copy=" << hot_copy_amt << " bytes\n";
    goto forever;
}

