#ifndef _NDB_TXN_BTREE_H_
#define _NDB_TXN_BTREE_H_

#include "btree.h"
#include "txn.h"

// XXX: hacky
extern void txn_btree_test();

// each Transaction implementation should specialize this for special
// behavior- the default implementation is just nops
template <template <typename> class Transaction>
struct txn_btree_handler {
  inline void on_construct(btree *underlying) {} // get a handle to the underying btree
  inline void on_destruct() {} // called at the beginning of the txn_btree's dtor
  static const bool has_background_task = false;
};

/**
 * This class implements a serializable, multi-version b-tree
 *
 * It presents mostly same interface as the underlying concurrent b-tree,
 * but the interface is serializable. The main interface differences are,
 * insert() and put() do not return a boolean value to indicate whether or not
 * they caused the tree to mutate
 *
 * A txn_btree does not allow keys to map to NULL records, even though the
 * underlying concurrent btree does- this simplifies some of the book-keeping
 * Additionally, keys cannot map to zero length records.
 *
 * Note that the txn_btree *manages* the memory of both keys and values internally.
 * See the specific notes on search()/insert() about memory ownership
 */
template <template <typename> class Transaction>
class txn_btree {

  // XXX: not ideal
  template <template <typename> class P, typename T>
    friend class transaction;

  // XXX: would like to declare friend wth all Transaction<T> classes, but
  // doesn't seem like an easy way to do that for template template parameters

public:
  typedef transaction_base::key_type key_type;
  typedef transaction_base::string_type string_type;
  typedef const uint8_t * value_type;
  typedef size_t size_type;

  struct search_range_callback {
  public:
    virtual ~search_range_callback() {}
    virtual bool invoke(const string_type &k, const string_type &v) = 0;
  };

private:
  template <typename T>
  class type_callback_wrapper : public search_range_callback {
  public:
    type_callback_wrapper(T *callback) : callback(callback) {}
    virtual bool
    invoke(const string_type &k, const string_type &v)
    {
      return callback->operator()(k, v);
    }
  private:
    T *const callback;
  };

  static inline ALWAYS_INLINE string_type
  to_string_type(const varkey &k)
  {
    return string_type((const char *) k.data(), k.size());
  }

public:

  txn_btree(size_type value_size_hint = 128,
            bool mostly_append = false)
    : value_size_hint(value_size_hint),
      mostly_append(mostly_append),
      been_destructed(false)
  {
    handler.on_construct(&underlying_btree);
  }

  ~txn_btree()
  {
    if (!been_destructed)
      unsafe_purge(false);
  }

  // either returns false or v is set to not-empty with value
  // precondition: max_bytes_read > 0
  template <typename Traits>
  bool search(Transaction<Traits> &t, const string_type &k, string_type &v,
              size_t max_bytes_read = string_type::npos);

  // memory:
  // k - assumed to point to valid memory, *not* managed by btree
  // v - if k is found, points to a region of (immutable) memory of size sz which
  //     is guaranteed to be valid memory as long as transaction t is in scope
  template <typename Traits>
  inline bool
  search(Transaction<Traits> &t, const key_type &k, string_type &v,
         size_t max_bytes_read = string_type::npos)
  {
    return search(t, to_string_type(k), v, max_bytes_read);
  }

  struct default_string_allocator {
    inline ALWAYS_INLINE string_type *
    operator()()
    {
      return nullptr;
    }
  };

  // StringAllocator needs to be CopyConstructable
  template <typename Traits, typename StringAllocator = default_string_allocator>
  void
  search_range_call(Transaction<Traits> &t,
                    const string_type &lower,
                    const string_type *upper,
                    search_range_callback &callback,
                    const StringAllocator &sa = StringAllocator());

  template <typename Traits, typename StringAllocator = default_string_allocator>
  inline void
  search_range_call(Transaction<Traits> &t,
                    const key_type &lower,
                    const key_type *upper,
                    search_range_callback &callback,
                    const StringAllocator &sa = StringAllocator())
  {
    string_type s;
    if (upper)
      s = to_string_type(*upper);
    search_range_call(t, to_string_type(lower), upper ? &s : NULL, callback, sa);
  }

  template <typename Traits, typename T, typename StringAllocator = default_string_allocator>
  inline void
  search_range(
      Transaction<Traits> &t, const string_type &lower,
      const string_type *upper, T callback,
      const StringAllocator &sa = StringAllocator())
  {
    type_callback_wrapper<T> w(&callback);
    search_range_call(t, lower, upper, w, sa);
  }

  template <typename Traits, typename T, typename StringAllocator = default_string_allocator>
  inline void
  search_range(
      Transaction<Traits> &t, const key_type &lower,
      const key_type *upper, T callback,
      const StringAllocator &sa = StringAllocator())
  {
    type_callback_wrapper<T> w(&callback);
    search_range_call(t, lower, upper, w, sa);
  }

  template <typename Traits>
  inline void
  put(Transaction<Traits> &t, const string_type &k, const string_type &v)
  {
    INVARIANT(!v.empty());
    do_tree_put(t, k, v, false);
  }

  // XXX: other put variants

  template <typename Traits>
  inline void
  insert(Transaction<Traits> &t, const string_type &k, const string_type &v)
  {
    INVARIANT(!v.empty());
    do_tree_put(t, k, v, true);
  }

  template <typename Traits>
  inline void
  insert(Transaction<Traits> &t, string_type &&k, string_type &&v)
  {
    INVARIANT(!v.empty());
    do_tree_put(t, std::move(k), std::move(v), true);
  }

  // insert() methods below are for legacy use

  template <typename Traits>
  inline void
  insert(Transaction<Traits> &t, const string_type &k, value_type v, size_type sz)
  {
    INVARIANT(v);
    INVARIANT(sz);
    do_tree_put(t, k, string_type((const char *) v, sz), true);
  }

  template <typename Traits>
  inline void
  insert(Transaction<Traits> &t, const key_type &k, value_type v, size_type sz)
  {
    INVARIANT(v);
    INVARIANT(sz);
    do_tree_put(t, to_string_type(k), string_type((const char *) v, sz), true);
  }

  template <typename Traits, typename T>
  inline void
  insert_object(Transaction<Traits> &t, const key_type &k, const T &obj)
  {
    insert(t, k, (value_type) &obj, sizeof(obj));
  }

  template <typename Traits, typename T>
  inline void
  insert_object(Transaction<Traits> &t, const string_type &k, const T &obj)
  {
    insert(t, k, (value_type) &obj, sizeof(obj));
  }

  template <typename Traits>
  inline void
  remove(Transaction<Traits> &t, const string_type &k)
  {
    // XXX: assume empty string is efficient to construct
    do_tree_put(t, k, string_type(), false);
  }

  template <typename Traits>
  inline void
  remove(Transaction<Traits> &t, string_type &&k)
  {
    // XXX: assume empty string is efficient to construct
    do_tree_put(t, std::move(k), string_type(), false);
  }

  template <typename Traits>
  inline void
  remove(Transaction<Traits> &t, const key_type &k)
  {
    do_tree_put(t, to_string_type(k), string_type(), false);
  }

  inline size_t
  size_estimate() const
  {
    return underlying_btree.size();
  }

  inline size_type
  get_value_size_hint() const
  {
    return value_size_hint;
  }

  inline void
  set_value_size_hint(size_type value_size_hint)
  {
    this->value_size_hint = value_size_hint;
  }

  inline bool
  is_mostly_append() const
  {
    return mostly_append;
  }

  inline void
  set_mostly_append(bool mostly_append)
  {
    this->mostly_append = mostly_append;
  }

  /**
   * only call when you are sure there are no concurrent modifications on the
   * tree. is neither threadsafe nor transactional
   *
   * Note that when you call unsafe_purge(), this txn_btree becomes
   * completely invalidated and un-usable. Any further operations
   * (other than calling the destructor) are undefined
   */
  void unsafe_purge(bool dump_stats = false);

  static void Test();

  // XXX: only exists because can't declare friend of template parameter
  // Transaction
  inline btree *
  get_underlying_btree()
  {
    return &underlying_btree;
  }

private:

  struct purge_tree_walker : public btree::tree_walk_callback {
    virtual void on_node_begin(const btree::node_opaque_t *n);
    virtual void on_node_success();
    virtual void on_node_failure();
#ifdef TXN_BTREE_DUMP_PURGE_STATS
    purge_tree_walker()
      : purge_stats_nodes(0),
        purge_stats_nosuffix_nodes(0) {}
    std::map<size_t, size_t> purge_stats_ln_record_size_counts; // just the record
    std::map<size_t, size_t> purge_stats_ln_alloc_size_counts; // includes overhead
    std::vector<uint16_t> purge_stats_nkeys_node;
    size_t purge_stats_nodes;
    size_t purge_stats_nosuffix_nodes;
#endif
  private:
    std::vector< std::pair<btree::value_type, bool> > spec_values;
  };

  template <typename Traits, typename StringAllocator>
  struct txn_search_range_callback : public btree::low_level_search_range_callback {
    txn_search_range_callback(Transaction<Traits> *t,
                              typename Transaction<Traits>::txn_context *ctx,
                              const key_type &lower,
                              search_range_callback *caller_callback,
                              const StringAllocator &sa)
      : t(t), ctx(ctx), lower(lower), prev_key(),
        invoked(false), caller_callback(caller_callback),
        caller_stopped(false), sa(sa) {}
    virtual void on_resp_node(const btree::node_opaque_t *n, uint64_t version);
    virtual bool invoke(const btree::string_type &k, btree::value_type v,
                        const btree::node_opaque_t *n, uint64_t version);
    Transaction<Traits> *const t;
    typename Transaction<Traits>::txn_context *const ctx;
    const key_type lower;
    string_type prev_key;
    bool invoked;
    search_range_callback *const caller_callback;
    bool caller_stopped;
    StringAllocator sa;
    string_type temp_buf0;
    string_type temp_buf1;
  };

  template <typename Traits>
  struct absent_range_validation_callback : public btree::search_range_callback {
    absent_range_validation_callback(typename Transaction<Traits>::txn_context *ctx,
                                     transaction_base::tid_t commit_tid)
      : ctx(ctx), commit_tid(commit_tid), failed_flag(false) {}
    inline bool failed() const { return failed_flag; }
    virtual bool
    invoke(const btree::string_type &k, btree::value_type v)
    {
      dbtuple *ln = (dbtuple *) v;
      INVARIANT(ln);
      VERBOSE(std::cerr << "absent_range_validation_callback: key " << util::hexify(k)
          << " found dbtuple 0x" << util::hexify(ln) << std::endl);
      const bool did_write = ctx->write_set.find(k) != ctx->write_set.end();
      failed_flag = did_write ? !ln->latest_value_is_nil() : !ln->stable_latest_value_is_nil();
      if (failed_flag)
        VERBOSE(std::cerr << "absent_range_validation_callback: key " << util::hexify(k)
            << " found dbtuple 0x" << util::hexify(ln) << std::endl);
      return !failed_flag;
    }
    typename Transaction<Traits>::txn_context *const ctx;
    const transaction_base::tid_t commit_tid;
    bool failed_flag;
  };

  // remove() is just do_tree_put() with empty-string
  // expect_new indicates if we expect the record to not exist in the tree-
  // is just a hint that affects perf, not correctness
  template <typename Traits>
  void do_tree_put(Transaction<Traits> &t, const string_type &k,
                   const string_type &v, bool expect_new);

  template <typename Traits>
  void do_tree_put(Transaction<Traits> &t, string_type &&k,
                   string_type &&v, bool expect_new);

  btree underlying_btree;
  size_type value_size_hint;
  bool mostly_append;
  bool been_destructed;
  txn_btree_handler<Transaction> handler;
};

#endif /* _NDB_TXN_BTREE_H_ */
