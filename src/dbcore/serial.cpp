#include "serial.h"
#if defined(USE_PARALLEL_SSN) || defined(USE_PARALLEL_SSI)
namespace TXN {

/* The read optimization for SSN

   Versions with some LSN delta from a tx's begin timestamp are considered "old"
   by that transaction. This reader then will not track these versions in its
   read set, betting it won't be updated soon (at least not before it commits),
   saving the effort of maintaining a potentially large read set.

   The updater then has all the burden of detecting such a reader's existance.
   The basic idea is to let readers mark in the tuple their existance (a bool),
   and the updater will read this mark to know if some reader who thinks this
   tuple is "old" ever existed. If so, the updater will have to do an educated
   guess on what the reader's commit timestamp would be and use it to adjust
   its pstamp (more on this later).

   Implementation is much messier then the above idea, due to the non-blocking
   paradigm we use. Two basic problems are:
   1. How to make sure both the reader and writer think it's an old version;
   2. How to tell a reader who thinks it read an old version from an innocent
      reader who happens to inherit the same bit position in the raeders bitmap.

   Problem 1 is simple to deal with. The discrepancy comes from the way we
   calculate the age of a version:

      [ accessing transaction's begin timestamp - version creation timestamp ]

   Suppose T1's begin stamp=5, and thinks a version is old. Meanwhile another
   older T2 with begin stamp=2 updates the version, thinking it's a young
   version (b/c 2-clsn < threshold, but 5-clsn >= threshold). The updater will
   then process the update assuming the read is accounted for by the reader.

   The solution is to rely on the reader to determine the age only, the writer
   will only need to see "if some reader thinks this version is an old one".
   For this to work, in each old tuple there's a "persistent reader" marker
   (currently occupying 8 bits, but really we only need 2). The reader will
   change the value of this mark to 0x1 if it thinks this is an old version.
   (This marking might fail if some updater already "locked" it by setting
   the marker's MSB to 1, more on this later).

   After the above marking, the reader then claim its position in the readers
   bitmap as usual, and continue, without adding this tuple to its read set.

   Porblem 2 is more subtle and puts more burden on the updater. At pre-commit
   the updater needs to know two things and consider what to do:
   1. Whether this version was considered to be "old" by some reader;
   2. What's the current status of the potential reader (if exists)

   We can know 1 by looking at the "persistent reader" marker in the version.
   To avoid races, the updater needs to "lockout" all incoming readers who
   will think this is an old version, before looking at the marker - this is
   why we need 2 bits for the marker. New reader saw this mark's MSB=1 will
   abort. The updater then reads the marker, if it's set, then some reader who
   thought this was an old version read it. Then it's time to tackle the other
   need-to-know thing above.

   Recall that the reader will claim its position on the readers bitmap.
   However, if the reader thinks it's accessing an old version, it won't track
   the read, consequently it won't be able to clear that bit when finished.
   So the burden of figuring the situation out falls on the writer's shoulder.
   There're several cases here:

   (Obviously the updater will find the reader's bitmap bit is set and the
   persistent reader marker's LSB=1, and set the marker to 0x81. Note that
   the reader will not release its bit position once finished - the read
   isn't even tracked)

   1. The tx represented by the corresponding bit in the readers bitmap is
      valid and running.

      In this case, the reader might be (a) the guy who did the marking, or
      (b) a new guy who also thinks this is an old version but there's no need
      to mark, or (c) an innocent guy who happened to inherit that unlucky
      bitmap position.

    2. The tx represented by the corresponding bit in the readers bitmap is
       invalid.

       This means there has been some context change after the updater has read
       the bitmap, and before it retrived the xid_context represented by the bit.
       Perhaps the transaction just left before we can retrieve its context.

    For both 1 and 2, without any further information, we can't determine the
    potential reader's cstamp and use it as the updater's pstamp. Especially
    if it is the case that the reader just finished before we can retrieve
    its context: we don't even have a chance to know its cstamp.

    The solution is to for each thread bit position in the centralized xid
    list, record a "last commit timestamp". In general it looks like this:

        bitmap:  000011000111...
        array of xids: [x0] [x1] [x2] ...
        array of lsns: [l0] [l1] [l2] ...
        x0, x1, x2... correspond to bit 0, 1, 2... in the bitmap (per version)
        Each bit in the bitmap corresponds to the threads that's accessing the
        version. So each thread can actually find its xid through this xid
        array and its position on the bitmap. l0, l1, l2... then correspond
        to each bit/xid.

    When a reader is sure it can commit, it will set its last commit lsn
    in the array to its commit timestamp. So the array of lsn actually
    records each thread's latest commit stamp.

    With the above infrastructure, it becomes easy for the updater to figure
    out its potential pstamp value: if a potential reader exists, find the
    corresponding thread's latest committed lsn, and this stamp in worst
    case will be the reader's cstamp, which would be the tuple's xstamp if
    the reader did track this read.

    In real implementation, we further look at the potential reader's state/
    commit order relative to the updater to determine what to do.

    For 1 above:
      * If this reader's cstamp is > 0 but < updater's cstamp:
        The updater might actually catch the reader right on (a)
        or there were some older readers already finished (b-c). The updater
        should continue as if this were a "normal" reader: spin on its result.
        But the difference is that the updater will need to update its pstamp
        to the reader's cstamp if it committed (as in normal SSN), to the last
        cstamp on that thread if aborted.

      * If this reader is not in pre-commit or has a cstamp > updater's cstamp:
        Basically this means the reader will (attempt to) commit after the
        updater, forming an read/write dependency (updater->reader).
        - If we don't allow any back-edges, we can either spin on it to see the
          result, or tell it to abort. We settled on the latter before, as the
          former will tend to give potentially higher pstamp (=more false+ves,
          but haven't measured). The updater will try to notify the reader
          "hey yo, you need to abort!", betting that the reader will later use
          the updater's cstamp as its sstamp which will be low. But this makes
          it very tricky to choose the threshold and can abort lots of read-mostly
          transactions.

          (The implementation is like: use a boolean (set by the updater) in the
          reader's context (xc.should_abort) to indicate whether it needs to abort.
          The reader will examine this flag before post-commit (if it survived),
          and abort accordingly. The updater should read the reader's state
          (e.g., ACTIVE) before setting the flag, then re-read it after setting it.
          If the reader's state didn't change, it means the reader will know it
          should abort later; otherwise the updater considers it missed this precious
          opportunity. Then the updater will have two choice: spin on the reader or
          abort. The former might cause deadlock - an reader might be spinning on
          the updater already hoping to use its cstamp as sstamp. So here we let
          the updater abort.)

        - But actually we can allow back-edges - simply let the updater to set
          the reader's sstamp to the updater's sstamp. This implies that we need
          to go over the read first for the updater to have a stable sstamp; we
          also need to use a CAS to set sstamp because now xc.sstamp is now not
          only updated by the owner any more. We follow a similar optimistic
          read-set-validate paradigm to make sure that the reader will get this
          (like what we did in the above "should_abort" implementation).
          This appears to be working well; it preserves most of the read-mostly
          transactions and does not abort too many updaters, either.

    For 2 above:
       This basically can be considered as the cstamp < updater's cstamp case,
       because the reader might have already gone. So the updater should use
       the most recent cstamp indicated by that thread as its pstamp.
*/

readers_list rlist;

typedef dbtuple::rl_bitmap_t rl_bitmap_t;
static __thread rl_bitmap_t tls_bitmap_entry = 0;
static rl_bitmap_t claimed_bitmap_entries = 0;

/* Return a bitmap with 1's representing active readers.
 */
readers_list::bitmap_t serial_get_tuple_readers(dbtuple *tup, bool exclude_self)
{
    if (exclude_self)
        return volatile_read(tup->rl_bitmap) & ~tls_bitmap_entry;
    return volatile_read(tup->rl_bitmap);
}

void assign_reader_bitmap_entry() {
    if (tls_bitmap_entry)
        return;

    rl_bitmap_t old_bitmap = volatile_read(claimed_bitmap_entries);
 retry:
    rl_bitmap_t new_bitmap = old_bitmap | (old_bitmap+1);
    rl_bitmap_t cur_bitmap = __sync_val_compare_and_swap(&claimed_bitmap_entries, old_bitmap, new_bitmap);
    if (old_bitmap != cur_bitmap) {
        old_bitmap = cur_bitmap;
        goto retry;
    }
    tls_bitmap_entry = new_bitmap ^ old_bitmap;
    // FIXME: GCC warns if XIDS_PER_READER_KEY == 64
    //rl_bitmap_t forbidden_bits = -(rl_bitmap_t(1) << readers_list::XIDS_PER_READER_KEY);
    //ASSERT(not (tls_bitmap_entry & forbidden_bits));
}

void deassign_reader_bitmap_entry() {
    ASSERT(tls_bitmap_entry);
    ASSERT(claimed_bitmap_entries & tls_bitmap_entry);
    __sync_fetch_and_xor(&claimed_bitmap_entries, tls_bitmap_entry);
    tls_bitmap_entry = 0;
}

bool
serial_register_reader_tx(dbtuple *t, XID xid)
{
    rl_bitmap_t old_bitmap = volatile_read(t->rl_bitmap);
    if (not (old_bitmap & tls_bitmap_entry)) {
#if CHECK_INVARIANTS
        int xid_pos = __builtin_ctzll(tls_bitmap_entry);
        ASSERT(xid_pos >= 0 and xid_pos < readers_list::XIDS_PER_READER_KEY);
#endif
        __sync_fetch_and_or(&t->rl_bitmap, tls_bitmap_entry);
        ASSERT(t->rl_bitmap & tls_bitmap_entry);
    }
    return true;
}

void
serial_deregister_reader_tx(dbtuple *t)
{
    ASSERT(tls_bitmap_entry);
    // if a tx reads a tuple multiple times (e.g., 3 times),
    // then during post-commit it will call this function
    // multiple times, so we need to prevent it flipping the
    // bit an even number of times - leaving a 1 there.
    if (volatile_read(t->rl_bitmap) & tls_bitmap_entry)
        __sync_fetch_and_xor(&t->rl_bitmap, tls_bitmap_entry);
    ASSERT(not (t->rl_bitmap & tls_bitmap_entry));
}

// register tx in the global rlist (called at tx start)
void
serial_register_tx(XID xid)
{
    ASSERT(not rlist.xids[__builtin_ctzll(tls_bitmap_entry)]._val);
    volatile_write(rlist.xids[__builtin_ctzll(tls_bitmap_entry)]._val, xid._val);
}

// deregister tx in the global rlist (called at tx end)
void
serial_deregister_tx(XID xid)
{
    volatile_write(rlist.xids[__builtin_ctzll(tls_bitmap_entry)]._val, 0);
    ASSERT(not rlist.xids[__builtin_ctzll(tls_bitmap_entry)]._val);
}

void
serial_stamp_last_committed_lsn(LSN lsn)
{
    volatile_write(rlist.last_read_mostly_clsns[__builtin_ctzll(tls_bitmap_entry)]._val, lsn._val);
}

uint64_t
serial_get_last_read_mostly_cstamp(int xid_idx)
{
    return volatile_read(rlist.last_read_mostly_clsns[xid_idx]).offset();
}

};  // end of namespace
#endif
