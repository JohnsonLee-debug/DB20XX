#pragma once
#include <cstdint>
#include "masstree-beta/kvthread.hh"
#include "masstree-beta/masstree.hh"
#include "masstree-beta/masstree_scan.hh"
#include "masstree-beta/masstree_tcursor.hh"
#include "record.h"
#include "transaction.h"
#include "utils.h"
#include "version_chain.h"
#include "thread_context.h"

namespace db20xx {
using namespace Masstree;

typedef Str Key;
struct KeyInfo {
  /**
    mysql keypart counted from 1,
    db20xx keypart counted from 0;
  */
  void add_key_part(uint32_t key_part) { key_parts.push_back(key_part - 1); }
  uint32_t get_key_length() { return key_len; }

  Schema schema;
  std::vector<int> key_parts;
  uint32_t key_len = 0; //key length capacity
};

class Index {
  friend class Table;

 public:
  Index(void) {}
  Index(const KeyInfo &keyinfo) : keyinfo_(keyinfo) {}
  ~Index() {}

  virtual bool get(const Key &key, VersionChainHead *&vchain_head,
                   threadinfo &ti) const = 0;

  virtual bool put(const Key &key, VersionChainHead *vchain_head,
                   threadinfo &ti) = 0;

  /**
  @brief
    build key from a db20xx record
    have memory allocation in this function, need to free by others
  @args
    arg1 record: record payload, without record header
  */
  void build_key(const char *record, Key &output_key, ThreadContext *thd_ctx) {
    char *key_data = thd_ctx->get_key_container();
    char *key_cursor = key_data;
    uint32_t key_len = 0;
    for (auto i : keyinfo_.key_parts) {
      const Field &field = keyinfo_.schema.get_field(i);
      const char *field_data = nullptr;
      uint32_t data_len = 0;

      field.get_field_data(record, field_data, data_len);
      memcpy(key_cursor, field_data, data_len);
      key_cursor += data_len;
      key_len += data_len;
    }

    output_key.s = key_data;
    output_key.len = key_len;
  }

  void build_key_from_mysql_record(const char *mysql_record, Key &output_key, ThreadContext *thd_ctx) {
    char *key_data = thd_ctx->get_key_container();
    char *key_cursor = key_data;
    uint32_t key_len = 0;
    for (auto i: keyinfo_.key_parts) {
      const Field &field = keyinfo_.schema.get_field(i);
      const char *field_data = nullptr;
      uint32_t data_len = 0;

      field.get_mysql_field_data(mysql_record, field_data, data_len);
      memcpy(key_cursor, field_data, data_len);
      key_cursor += data_len;
      key_len += data_len;
    }
    output_key.s = key_data;
    output_key.len = key_len;
  }

  uint32_t get_key_length() { return keyinfo_.get_key_length(); }
  const KeyInfo &get_key_info() const { return keyinfo_; }

 protected:
  KeyInfo keyinfo_;
};

struct db20xx_masstree_params : public nodeparams<15, 15> {
  typedef VersionChainHead *value_type;
  typedef value_print<value_type> value_print_type;
  typedef threadinfo threadinfo_type;
};

/*
Mysql的scan操作由scan_range_first和一系列scan_range_next完成,
在此过程中,每个操作上下文需要记录scan的状态.
Masstree索引使用scan_stack_type记录scan的状态.
我们将scan_stack_type存储在class ThreadLocal中.
*/
typedef db20xx_masstree_params nodeparam_type;
typedef scanstackelt<nodeparam_type> scan_stack_type;

class MasstreeIndex : public Index {
  typedef basic_table<db20xx_masstree_params> db20xx_masstree_type;
  typedef typename db20xx_masstree_params::value_type leafvalue_type;
  friend class Table;

 public:
  MasstreeIndex(void) {}
  MasstreeIndex(const KeyInfo &keyinfo) : Index(keyinfo) {}
  ~MasstreeIndex() {}

  void initialize(threadinfo &ti) { masstree_.initialize(ti); }

  void destroy(threadinfo &ti) { masstree_.destroy(ti); }

  /**
  @brief
    put a key-value pair to masstree. key consists of columns, values is
    corresponding RecordLocation of that Record.

  @return values
    @retval1 true: first put
    @retval2 false: not first put, update old value
  */
  bool put(const Key &key, VersionChainHead *vchain_head,
           threadinfo &ti) override {
    typename db20xx_masstree_type::cursor_type lp(masstree_, key);
    bool found = lp.find_insert(ti);
    if (!found) {
      ti.observe_phantoms(lp.node());
    }

    apply_put(lp.value(), vchain_head, ti);
    lp.finish(1, ti);
    return found;
  }

  /**
    @brief
      given key, get the value(RecordLocation of a db20xx row) of the key
    @return values
      @retval1 true: key exists
      @retval2 false: key doesnot exist
    FIXME: same problem with apply_put
  */
  bool get(const Key &key, VersionChainHead *&vchain_head,
           threadinfo &ti) const override {
    typename db20xx_masstree_type::unlocked_cursor_type lp(masstree_, key);
    bool found = lp.find_unlocked(ti);
    if (found) vchain_head = lp.value();

    return found;
  }

  bool scan_range_first(const Key &key, VersionChainHead *&vchain_head,
                        bool emit_firstkey, scan_stack_type &stack,
                        threadinfo &ti) const {
    masstree_.scan_range_first(key, emit_firstkey, stack, ti);
    if (stack.no_value()) {
      return false;
    } else {
      vchain_head = stack.get_value();
      return true;
    }
  }

  bool scan_range_next(VersionChainHead *&vchain_head, scan_stack_type &stack,
                       threadinfo &ti) const {
    masstree_.scan_range_next(stack, ti);
    if (stack.no_value()) {
      return false;
    } else {
      vchain_head = stack.get_value();
      return true;
    }
  }

  bool rscan_range_first(const Key &key, VersionChainHead *&vchain_head,
                         bool emit_firstkey, scan_stack_type &stack,
                         threadinfo &ti) const {
    masstree_.rscan_range_first(key, emit_firstkey, stack, ti);
    if (stack.no_value()) {
      return false;
    } else {
      vchain_head = stack.get_value();
      return true;
    }
  }

  bool rscan_range_next(VersionChainHead *&vchain_head, scan_stack_type &stack,
                        threadinfo &ti) const {
    masstree_.rscan_range_next(stack, ti);
    if (stack.no_value()) {
      return false;
    } else {
      vchain_head = stack.get_value();
      return true;
    }
  }

  // TODO
  // int scan(const Key &key, bool matchfirst, Scanner& scanner, threadinfo &ti)
  // const override {}

  // TODO
  // int rscan(const Key &key, bool matchfirst, Scanner& scanner, threadinfo
  // &ti) const override {}

 private:
  /**
  FIXME: masstree should manage leafvalue carefully to avoid concurrent
  problems.
  */
  void apply_put(leafvalue_type &value, VersionChainHead *vchain_head,
                 threadinfo &ti) {
    (void)ti;  // FIXME thread local内存池分配value的内存
    value = vchain_head;
  }

 private:
  db20xx_masstree_type masstree_;
};

}  // namespace db20xx
