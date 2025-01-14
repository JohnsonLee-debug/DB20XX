/* Copyright (c) 2004, 2021, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file ha_db20xx.cc

  @brief
  The ha_db20xx engine is a in-memory storage engine
  see also /storage/db20xx/ha_db20xx.h.

  @details
  Once this is done, MySQL will let you create tables with:<br>
  CREATE TABLE \<table name\> (...) ENGINE=DB20XXDB;

  The db20xx storage engine is set up to use table locks. It
  implements an db20xx "SHARE" that is inserted into a hash by table
  name. You can use this to store information of state that any
  db20xx handler object will be able to see when it is using that
  table.

  Please read the object definition in ha_db20xx.h before reading the rest
  of this file.

  @note
  When you create an DB20XXDB table, the MySQL Server creates a table .frm
  (format) file in the database directory, using the table name as the file
  name as is customary with MySQL. No other files are created. To get an idea
  of what occurs, here is an db20xx select that would do a scan of an entire
  table:

  @code
  ha_db20xx::store_lock
  ha_db20xx::external_lock
  ha_db20xx::info
  ha_db20xx::rnd_init
  ha_db20xx::extra
  ha_db20xx::rnd_next
  ha_db20xx::rnd_next
  ha_db20xx::rnd_next
  ha_db20xx::rnd_next
  ha_db20xx::rnd_next
  ha_db20xx::rnd_next
  ha_db20xx::rnd_next
  ha_db20xx::rnd_next
  ha_db20xx::rnd_next
  ha_db20xx::extra
  ha_db20xx::external_lock
  ha_db20xx::extra
  ENUM HA_EXTRA_RESET        Reset database to after open
  @endcode

  Here you see that the db20xx storage engine has 9 rows called before
  rnd_next signals that it has reached the end of its data. Also note that
  the table in question was already opened; had it not been open, a call to
  ha_db20xx::open() would also have been necessary. Calls to
  ha_db20xx::extra() are hints as to what will be occurring to the request.

  A Longer Fulgurdb can be found called the "Skeleton Engine" which can be
  found on TangentOrg. It has both an engine and a full build environment
  for building a pluggable storage engine.

  Happy coding!<br>
    -Brian
*/

#include <cstdint>

#include "ha_db20xx.h"
#include "message_logger.h"
#include "my_dbug.h"
#include "mysql/plugin.h"
#include "return_status.h"
#include "sql/sql_class.h"
#include "sql/sql_plugin.h"
#include "sql/sql_select.h"  // actual_key_parts
#include "thread_context.h"
#include "typelib.h"

#include "engine.h"
#include "ha_db20xx_help.h"
#include "transaction.h"

static handler *db20xx_create_handler(handlerton *hton, TABLE_SHARE *table,
                                        bool partitioned, MEM_ROOT *mem_root);

handlerton *db20xx_hton;

/* Interface to mysqld, to check system tables supported by SE */
static bool db20xx_is_supported_system_table(const char *db,
                                               const char *table_name,
                                               bool is_sql_layer_system_table);

Fulgurdb_share::Fulgurdb_share() { thr_lock_init(&lock); }

/**
  @brief
  Fulgurdb of simple lock controls. The "share" it creates is a
  structure we will pass to each db20xx handler. Do you have to have
  one of these? Well, you have pieces that are used for locking, and
  they are needed to function.
*/

Fulgurdb_share *ha_db20xx::get_share() {
  Fulgurdb_share *tmp_share;

  DBUG_TRACE;

  lock_shared_ha_data();
  if (!(tmp_share = static_cast<Fulgurdb_share *>(get_ha_share_ptr()))) {
    tmp_share = new Fulgurdb_share;
    if (!tmp_share) goto err;

    set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  return tmp_share;
}

static handler *db20xx_create_handler(handlerton *hton, TABLE_SHARE *table,
                                        bool, MEM_ROOT *mem_root) {
  return new (mem_root) ha_db20xx(hton, table);
}

ha_db20xx::ha_db20xx(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg) {}

/*
  List of all system tables specific to the SE.
  Array element would look like below,
     { "<database_name>", "<system table name>" },
  The last element MUST be,
     { (const char*)NULL, (const char*)NULL }

  This array is optional, so every SE need not implement it.
*/
static st_handler_tablename ha_db20xx_system_tables[] = {
    {(const char *)nullptr, (const char *)nullptr}};

/**
  @brief Check if the given db.tablename is a system table for this SE.

  @param db                         Database name to check.
  @param table_name                 table name to check.
  @param is_sql_layer_system_table  if the supplied db.table_name is a SQL
                                    layer system table.

  @retval true   Given db.table_name is supported system table.
  @retval false  Given db.table_name is not a supported system table.
*/
static bool db20xx_is_supported_system_table(const char *db,
                                               const char *table_name,
                                               bool is_sql_layer_system_table) {
  st_handler_tablename *systab;

  // Does this SE support "ALL" SQL layer system tables ?
  if (is_sql_layer_system_table) return false;

  // Check if this is SE layer system tables
  systab = ha_db20xx_system_tables;
  while (systab && systab->db) {
    if (systab->db == db && strcmp(systab->tablename, table_name) == 0)
      return true;
    systab++;
  }

  return false;
}

/**
  @brief
  Used for opening tables. The name will be the name of the file.

  @details
  A table is opened when it needs to be opened; e.g. when a request comes in
  for a SELECT on the table (tables are not open and closed for each request,
  they are cached).

  Called from handler.cc by handler::ha_open(). The server opens all tables by
  calling ha_open() which then calls the handler specific open().

  @see
  handler::ha_open() in handler.cc
*/

int ha_db20xx::open(const char *name, int, uint, const dd::Table *) {
  DBUG_TRACE;
  // db20xx::threadinfo_type *ti = get_threadinfo();
  LEX_CSTRING sl_db_name = table->s->db;
  std::string db_name(sl_db_name.str, sl_db_name.length);
  std::string table_name(name);

  db20xx::Database *database = db20xx::Engine::get_database(db_name);
  if (database == nullptr)
    return HA_ERR_NO_SUCH_TABLE;  // 是否存在类似于no such db的error code？

  db20xx_table_ = database->get_table(table_name);
  if (db20xx_table_ == nullptr) return HA_ERR_NO_SUCH_TABLE;

  return 0;
}

/**
  @brief
  Closes a table.

  @details
  Called from sql_base.cc, sql_select.cc, and table.cc. In sql_select.cc it is
  only used to close up temporary tables or during the process where a
  temporary table is converted over to being a myisam table.

  For sql_base.cc look at close_data_tables().

  @see
  sql_base.cc, sql_select.cc and table.cc
*/

int ha_db20xx::close(void) {
  DBUG_TRACE;
  return 0;
}

/**
  @brief
  write_row() inserts a row.

  @param sl_row(server layer row): a byte array of data

  @details
  Called from item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc, and sql_update.cc.

  @see
  item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc and sql_update.cc
*/

int ha_db20xx::write_row(uchar *sl_record) {
  DBUG_TRACE;
  db20xx::ThreadContext *thd_ctx = get_thread_ctx();
  int ret = db20xx_table_->insert_record_from_mysql((char *)sl_record, thd_ctx);
  if (ret == db20xx::DB20XX_KEY_EXIST)
    return HA_ERR_FOUND_DUPP_KEY;
  else if (ret == db20xx::DB20XX_ABORT)
    return HA_ERR_GENERIC;

  return 0;
}

/**
  @brief
  update_row() updates a row. old_data will have the previous row record in it,
  while new_data will have the newest data in it. Keep in mind that the server
  can do updates based on ordering if an ORDER BY clause was used. Consecutive
  ordering is not guaranteed.

  @details
  Currently new_data will not have an updated auto_increament record. You can
  do this for db20xx by doing:
  @code
  if (table->next_number_field && record == table->record[0])
    update_auto_increment();
  @endcode
  Called from sql_select.cc, sql_acl.cc, sql_update.cc, and sql_insert.cc.
  @see
  sql_select.cc, sql_acl.cc, sql_update.cc and sql_insert.cc
*/
int ha_db20xx::update_row(const uchar *old_row, uchar *new_row) {
  (void)old_row;
  DBUG_TRACE;
  db20xx::ThreadContext *thd_ctx = get_thread_ctx();
  db20xx_table_->update_record_from_mysql(current_record_, (char *)new_row,
                                          thd_ctx);
  return 0;
}

/**
  @brief
  This will delete a row. buf will contain a copy of the row to be deleted.
  The server will call this right after the current row has been called (from
  either a previous rnd_nexT() or index call).

  @details
  If you keep a pointer to the last row or can access a primary key it will
  make doing the deletion quite a bit easier. Keep in mind that the server does
  not guarantee consecutive deletions. ORDER BY clauses can be used.

  Called in sql_acl.cc and sql_udf.cc to manage internal table
  information.  Called in sql_delete.cc, sql_insert.cc, and
  sql_select.cc. In sql_select it is used for removing duplicates
  while in insert it is used for REPLACE calls.

  @see
  sql_acl.cc, sql_udf.cc, sql_delete.cc, sql_insert.cc and sql_select.cc
*/

int ha_db20xx::delete_row(const uchar *) {
  DBUG_TRACE;
  db20xx::ThreadContext *thd_ctx = get_thread_ctx();
  db20xx_table_->delete_record(current_record_, thd_ctx);

  return 0;
}

void ha_db20xx::build_key_from_mysql_key(const uchar *mysql_key,
                                           key_part_map keypart_map,
                                           db20xx::Key &db20xx_key,
                                           bool &full_key_search) {
  /* works only with key prefixes */
  assert(((keypart_map + 1) & keypart_map) == 0);

  KEY *key_info = table->key_info + active_index;
  KEY_PART_INFO *key_part = key_info->key_part;
  KEY_PART_INFO *end_key_part = key_part + actual_key_parts(key_info);
  uint full_key_part_num = end_key_part - key_part;
  uint used_key_part_num = 0;
  db20xx::ThreadContext *thd_ctx = get_thread_ctx();

  char *materized_key = thd_ctx->get_key_container();
  uint key_len = 0;

  char *p = materized_key;
  while (key_part < end_key_part && keypart_map) {
    uint part_len = 0;
    if (key_part->store_length == key_part->length) {
      part_len = key_part->length;
      memcpy(p, mysql_key, part_len);
      p += part_len;
      mysql_key += part_len;
    } else {
      uint len_bytes = key_part->store_length - key_part->length;
      memcpy(&part_len, mysql_key, len_bytes);
      mysql_key += len_bytes;
      memcpy(p, mysql_key, part_len);
      p += part_len;
      mysql_key += key_part->length;
    }

    key_len += part_len;
    keypart_map >>= 1;
    key_part++;
    used_key_part_num++;
  }

  db20xx_key.assign((const char *)materized_key, key_len);
  full_key_search = (used_key_part_num == full_key_part_num ? true : false);
}

/**
   @brief
   Positions an index cursor to the index specified in the handle
   ('active_index'). Fetches the row if available. If the key value is null,
   begin at the first key of the index.
   @returns 0 if success (found a record); non-zero if no record.
*/
int ha_db20xx::index_read_map(uchar *mysql_record, const uchar *key,
                                key_part_map keypart_map,
                                enum ha_rkey_function find_flag) {
  bool full_key_search = true;
  db20xx::Record *record = nullptr;
  db20xx::ThreadContext *thd_ctx = get_thread_ctx();
  int found = db20xx::DB20XX_SUCCESS;
  scan_direction_ = find_flag;  // flag的定义见include/my_base.h
  build_key_from_mysql_key(key, keypart_map, index_key_, full_key_search);

  if (!full_key_search) {
    assert(find_flag == HA_READ_KEY_EXACT);
    found = db20xx_table_->index_prefix_key_search(
        active_index, index_key_, record, masstree_scan_stack_, *thd_ctx,
        read_own_statement_);
  } else if (find_flag == HA_READ_KEY_EXACT) {
    found = db20xx_table_->get_record_from_index(
        active_index, index_key_, record, *thd_ctx, read_own_statement_);
  } else if (find_flag == HA_READ_KEY_OR_NEXT) {
    found = db20xx_table_->index_scan_range_first(
        active_index, index_key_, record, true, masstree_scan_stack_, *thd_ctx,
        read_own_statement_);
  } else if (find_flag == HA_READ_AFTER_KEY) {
    found = db20xx_table_->index_scan_range_first(
        active_index, index_key_, record, false, masstree_scan_stack_, *thd_ctx,
        read_own_statement_);
  } else if (find_flag == HA_READ_KEY_OR_PREV) {
    found = db20xx_table_->index_rscan_range_first(
        active_index, index_key_, record, true, masstree_scan_stack_, *thd_ctx,
        read_own_statement_);
  } else if (find_flag == HA_READ_BEFORE_KEY) {
    found = db20xx_table_->index_rscan_range_first(
        active_index, index_key_, record, false, masstree_scan_stack_, *thd_ctx,
        read_own_statement_);
  } else {
    // TODO:panic
    assert(false);
  }

  if (found == db20xx::DB20XX_SUCCESS) {
    record->load_data_to_mysql((char *)mysql_record,
                               db20xx_table_->get_schema());
    current_record_ = record;
    return 0;
  } else if (found == db20xx::DB20XX_ABORT) {
    return HA_ERR_GENERIC;
  } else
    return HA_ERR_KEY_NOT_FOUND;
}

/**
  @brief
  Used to read forward through the index.
*/

int ha_db20xx::index_next(uchar *mysql_record) {
  db20xx::Record *record;
  db20xx::ThreadContext *thd_ctx = get_thread_ctx();
  int found = false;

  switch (scan_direction_) {
    case HA_READ_KEY_OR_NEXT:
    case HA_READ_AFTER_KEY:
      found = db20xx_table_->index_scan_range_next(
          active_index, record, masstree_scan_stack_, *thd_ctx,
          read_own_statement_);
      break;
    case HA_READ_KEY_OR_PREV:
    case HA_READ_BEFORE_KEY:
      found = db20xx_table_->index_rscan_range_next(
          active_index, record, masstree_scan_stack_, *thd_ctx,
          read_own_statement_);
      break;
    case HA_READ_KEY_EXACT:
      found = db20xx_table_->index_prefix_search_next(
          active_index, index_key_, record, masstree_scan_stack_, *thd_ctx,
          read_own_statement_);
      break;
    default:
      // TODO:panic
      assert(false);
      break;
  }

  if (found == db20xx::DB20XX_SUCCESS) {
    record->load_data_to_mysql((char *)mysql_record,
                               db20xx_table_->get_schema());
    current_record_ = record;
    return 0;
  } else
    return HA_ERR_KEY_NOT_FOUND;
}

/**
  @brief
  Used to read backwards through the index.
*/

int ha_db20xx::index_prev(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  index_first() asks for the first key in the index.

  @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

  @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_db20xx::index_first(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  index_last() asks for the last key in the index.

  @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

  @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_db20xx::index_last(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  rnd_init() is called when the system wants the storage engine to do a table
  scan. See the Example in the introduction at the top of this file to see when
  rnd_init() is called.

  @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc,
  sql_table.cc, and sql_update.cc.

  @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and
  sql_update.cc
*/
int ha_db20xx::rnd_init(bool) {
  DBUG_TRACE;
  seq_scan_cursor_.reset();

  return 0;
}

int ha_db20xx::rnd_end() {
  DBUG_TRACE;
  // do nothing
  return 0;
}

/**
  @brief
  This is called for each row of the table scan. When you run out of records
  you should return HA_ERR_END_OF_FILE. Fill buff up with the row information.
  The Field structure for the table is the key to getting data into buf
  in a manner that will allow the server to understand it.

  @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc,
  sql_table.cc, and sql_update.cc.

  @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and
  sql_update.cc
*/
int ha_db20xx::rnd_next(uchar *sl_record) {
  DBUG_TRACE;
  int ret = db20xx::DB20XX_SUCCESS;
  db20xx::ThreadContext *thd_ctx = get_thread_ctx();

  ret = db20xx_table_->table_scan_get(seq_scan_cursor_, read_own_statement_,
                                      thd_ctx);
  if (ret == db20xx::DB20XX_END_OF_TABLE) return HA_ERR_END_OF_FILE;

  if (ret == db20xx::DB20XX_RETRY || ret == db20xx::DB20XX_FAIL
      || ret == db20xx::DB20XX_ABORT) {
    // db20xx::LOG_DEBUG("can not read a visible version, abort");
    return HA_ERR_GENERIC;
  }

  if (ret == db20xx::DB20XX_INVISIBLE_VERSION ||
      ret == db20xx::DB20XX_DELETED_VERSION) {
    seq_scan_cursor_.inc_cursor();
    return rnd_next(sl_record);
  }

  assert(ret == db20xx::DB20XX_SUCCESS);

  // At this point, we've got a visible record version
  seq_scan_cursor_.record_->load_data_to_mysql((char *)sl_record,
                                               db20xx_table_->get_schema());
  table->set_found_row();
  seq_scan_cursor_.inc_cursor();
  current_record_ = seq_scan_cursor_.record_;

  return 0;
}

/**
  @brief
  position() is called after each call to rnd_next() if the data needs
  to be ordered. You can do something like the following to store
  the position:
  @code
  my_store_ptr(ref, ref_length, current_position);
  @endcode

  @details
  The server uses ref to store data. ref_length in the above case is
  the size needed to store current_position. ref is just a byte array
  that the server will maintain. If you are using offsets to mark rows, then
  current_position should be the offset. If it is a primary key like in
  BDB, then it needs to be a primary key.

  Called from filesort.cc, sql_select.cc, sql_delete.cc, and sql_update.cc.

  @see
  filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc
*/
void ha_db20xx::position(const uchar *) { DBUG_TRACE; }

/**
  @brief
  This is like rnd_next, but you are given a position to use
  to determine the row. The position will be of the type that you stored in
  ref. You can use ha_get_ptr(pos,ref_length) to retrieve whatever key
  or position you saved when position() was called.

  @details
  Called from filesort.cc, records.cc, sql_insert.cc, sql_select.cc, and
  sql_update.cc.

  @see
  filesort.cc, records.cc, sql_insert.cc, sql_select.cc and sql_update.cc
*/
int ha_db20xx::rnd_pos(uchar *, uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  ::info() is used to return information to the optimizer. See my_base.h for
  the complete description.

  @details
  Currently this table handler doesn't implement most of the fields really
  needed. SHOW also makes use of this data.

  You will probably want to have the following in your code:
  @code
  if (records < 2)
    records = 2;
  @endcode
  The reason is that the server will optimize for cases of only a single
  record. If, in a table scan, you don't know the number of records, it
  will probably be better to set records to two so you can return as many
  records as you need. Along with records, a few more variables you may wish
  to set are:
    records
    deleted
    data_file_length
    index_file_length
    delete_length
    check_time
  Take a look at the public variables in handler.h for more information.

  Called in filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
  sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc,
  sql_show.cc, sql_table.cc, sql_union.cc, and sql_update.cc.

  @see
  filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
  sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc,
  sql_show.cc, sql_table.cc, sql_union.cc and sql_update.cc
*/
int ha_db20xx::info(uint) {
  DBUG_TRACE;
  return 0;
}

/**
  @brief
  extra() is called whenever the server wishes to send a hint to
  the storage engine. The myisam engine implements the most hints.
  ha_innodb.cc has the most exhaustive list of these hints.

    @see
  ha_innodb.cc
*/
int ha_db20xx::extra(enum ha_extra_function) {
  DBUG_TRACE;
  return 0;
}

/**
  @brief
  Used to delete all rows in a table, including cases of truncate and cases
  where the optimizer realizes that all rows will be removed as a result of an
  SQL statement.

  @details
  Called from item_sum.cc by Item_func_group_concat::clear(),
  Item_sum_count_distinct::clear(), and Item_func_group_concat::clear().
  Called from sql_delete.cc by mysql_delete().
  Called from sql_select.cc by JOIN::reinit().
  Called from sql_union.cc by st_query_block_query_expression::exec().

  @see
  Item_func_group_concat::clear(), Item_sum_count_distinct::clear() and
  Item_func_group_concat::clear() in item_sum.cc;
  mysql_delete() in sql_delete.cc;
  JOIN::reinit() in sql_select.cc and
  st_query_block_query_expression::exec() in sql_union.cc.
*/
int ha_db20xx::delete_all_rows() {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

/**
  @brief
  This create a lock on the table. If you are implementing a storage engine
  that can handle transacations look at ha_berkely.cc to see how you will
  want to go about doing this. Otherwise you should consider calling flock()
  here. Hint: Read the section "locking functions for mysql" in lock.cc to
  understand this.

  @details
  Called from lock.cc by lock_external() and unlock_external(). Also called
  from sql_table.cc by copy_data_between_tables().

  @see
  lock.cc by lock_external() and unlock_external() in lock.cc;
  the section "locking functions for mysql" in lock.cc;
  copy_data_between_tables() in sql_table.cc.
*/
int ha_db20xx::external_lock(THD *thd, int lock_type) {
  DBUG_TRACE;
  enum_sql_command sql_command = (enum_sql_command)thd_sql_command(thd);

  // First time use the table, instead of  close/unclock the table
  if (lock_type != F_UNLCK) {
    // FIXME set and reset read_own_statement_ carefully
    read_own_statement_ =
        (sql_command == SQLCOM_UPDATE || sql_command == SQLCOM_DELETE ||
         sql_command == SQLCOM_UPDATE_MULTI ||
         sql_command == SQLCOM_DELETE_MULTI);

    db20xx::ThreadContext *thd_ctx = get_thread_ctx();
    db20xx::TransactionContext *txn_ctx = thd_ctx->get_transaction_context();
    if (!txn_ctx->on_going()) {
      uint64_t thread_id = thd_ctx->get_thread_id();
      txn_ctx->begin_transaction(thread_id);
      // register in statement level
      // FIXME: set 4th arg correctly (pointer to transaction id)
      trans_register_ha(thd, false, ht, nullptr);

      if ((thd->in_multi_stmt_transaction_mode())) {
        // register in session level
        trans_register_ha(thd, true, ht, nullptr);
      }
    }
  }

  return 0;
}

/**
  @brief
  The idea with handler::store_lock() is: The statement decides which locks
  should be needed for the table. For updates/deletes/inserts we get WRITE
  locks, for SELECT... we get read locks.

  @details
  Before adding the lock into the table lock handler (see thr_lock.c),
  mysqld calls store lock with the requested locks. Store lock can now
  modify a write lock to a read lock (or some other lock), ignore the
  lock (if we don't want to use MySQL table locks at all), or add locks
  for many tables (like we do when we are using a MERGE handler).

  Berkeley DB, for db20xx, changes all WRITE locks to TL_WRITE_ALLOW_WRITE
  (which signals that we are doing WRITES, but are still allowing other
  readers and writers).

  When releasing locks, store_lock() is also called. In this case one
  usually doesn't have to do anything.

  In some exceptional cases MySQL may send a request for a TL_IGNORE;
  This means that we are requesting the same lock as last time and this
  should also be ignored. (This may happen when someone does a flush
  table when we have opened a part of the tables, in which case mysqld
  closes and reopens the tables and tries to get the same locks at last
  time). In the future we will probably try to remove this.

  Called from lock.cc by get_lock_data().

  @note
  In this method one should NEVER rely on table->in_use, it may, in fact,
  refer to a different thread! (this happens if get_lock_data() is called
  from mysql_lock_abort_for_thread() function)

  @see
  get_lock_data() in lock.cc
*/
THR_LOCK_DATA **ha_db20xx::store_lock(THD *, THR_LOCK_DATA **to,
                                        enum thr_lock_type lock_type) {
  (void)lock_type;
  return to;
}

/**
  @brief
  Used to delete a table. By the time delete_table() has been called all
  opened references to this table will have been closed (and your globally
  shared references released). The variable name will just be the name of
  the table. You will need to remove any files you have created at this point.

  @details
  If you do not implement this, the default delete_table() is called from
  handler.cc and it will delete all files with the file extensions from
  handlerton::file_extensions.

  Called from handler.cc by delete_table and ha_create_table(). Only used
  during create if the table_flag HA_DROP_BEFORE_CREATE was specified for
  the storage engine.

  @see
  delete_table and ha_create_table() in handler.cc
*/
int ha_db20xx::delete_table(const char *, const dd::Table *) {
  DBUG_TRACE;
  /* This is not implemented but we want someone to be able that it works. */
  return 0;
}

/**
  @brief
  Renames a table from one name to another via an alter table call.

  @details
  If you do not implement this, the default rename_table() is called from
  handler.cc and it will delete all files with the file extensions from
  handlerton::file_extensions.

  Called from sql_table.cc by mysql_rename_table().

  @see
  mysql_rename_table() in sql_table.cc
*/
int ha_db20xx::rename_table(const char *, const char *, const dd::Table *,
                              dd::Table *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

/**
  @brief
  Given a starting key and an ending key, estimate the number of rows that
  will exist between the two keys.

  @details
  end_key may be empty, in which case determine if start_key matches any rows.

  Called from opt_range.cc by check_quick_keys().

  @see
  check_quick_keys() in opt_range.cc
*/
ha_rows ha_db20xx::records_in_range(uint, key_range *, key_range *) {
  DBUG_TRACE;
  return 10;  // low number to force index usage
}

static MYSQL_THDVAR_STR(last_create_thdvar, PLUGIN_VAR_MEMALLOC, nullptr,
                        nullptr, nullptr, nullptr);

static MYSQL_THDVAR_UINT(create_count_thdvar, 0, nullptr, nullptr, nullptr, 0,
                         0, 1000, 0);

/**
  @brief
  create a db20xx table
  @param[in] form :table structure
  @param[in] create_info :more information on the table
  @param[in,out] table_def :dd::Table describing table to be
    created. Can be adjusted by SE, the changes will be saved into
  data-dictionary at statement commit time.
  @return error number
    @retval 0 on success
  @see
  ha_create_table() in handle.cc
*/

int ha_db20xx::create(const char *name, TABLE *form,
                        HA_CREATE_INFO *create_info, dd::Table *table_def) {
  DBUG_TRACE;
  (void)create_info;
  (void)table_def;
  int ret = 0;
  // THD *thd = ha_thd();
  LEX_CSTRING sl_dbname = form->s->db;  // sl means server layer
  // LEX_CSTRING sl_table_name = form->s->table_name;
  std::string fgdb_dbname(
      sl_dbname.str, sl_dbname.length);  // fulg is the abbreviation of db20xx
  std::string fgdb_table_name(name);
  db20xx::Database *db = nullptr;

  if (db20xx::Engine::check_database_existence(fgdb_dbname) == false)
    db = db20xx::Engine::create_new_database(fgdb_dbname);
  else
    db = db20xx::Engine::get_database(fgdb_dbname);

  // generate table schema at create table time
  uint32_t sl_row_null_bytes = table->s->null_bytes;
  db20xx::Schema schema;
  schema.set_null_byte_length(sl_row_null_bytes);
  generate_db20xx_schema(form, schema);

  auto fgdb_table = db->create_table(fgdb_table_name, schema);
  if (fgdb_table == nullptr) {
    ret = HA_ERR_GENERIC;
    return ret;
  }

  db20xx::threadinfo_type *ti = get_threadinfo();
  // TABLE_SHARE::keys表示索引的个数
  // TABLE::key_info[]中保存了索引键的信息
  for (size_t i = 0; i < table->s->keys; i++) {
    db20xx::KeyInfo keyinfo;
    keyinfo.schema = schema;

    KEY &mysql_key_info = table->key_info[i];
    KEY_PART_INFO *keypart_end =
        mysql_key_info.key_part + mysql_key_info.user_defined_key_parts;
    for (KEY_PART_INFO *keypart = mysql_key_info.key_part;
         keypart != keypart_end; keypart++) {
      keyinfo.add_key_part(keypart->fieldnr);
      keyinfo.key_len += keypart->length;
    }

    fgdb_table->build_index(keyinfo, *ti);
  }

  return ret;
}

/**
  'all' is true if it's a real commit, that makes persistent changes
  'all' is false if it's not in fact a commit but an end of the
  statement that is part of the transaction.
  NOTE 'all' is also false in auto-commit mode where 'end of statement'
  and 'real commit' mean the same event.
*/
int db20xx_commit(handlerton *hton, THD *thd, bool all) {
  (void)hton;
  db20xx::ThreadContext *thd_ctx = get_thread_ctx();
  db20xx::TransactionContext *txn_ctx = thd_ctx->get_transaction_context();

  if (txn_ctx->get_transaction_status() == db20xx::DB20XX_TRANSACTION_ABORT) {
    txn_ctx->abort();
    return HA_ERR_LOCK_DEADLOCK;  // DB_FORCE_ABORT: same as innodb
  }

  bool real_commit = all || !thd->in_multi_stmt_transaction_mode();
  if (real_commit) {
    txn_ctx->commit();
  }

  return 0;
}

// FIXME: <NOT SURE>
int db20xx_rollback(handlerton *hton, THD *thd, bool all) {
  (void)hton;
  db20xx::ThreadContext *thd_ctx = get_thread_ctx();
  db20xx::TransactionContext *txn_ctx = thd_ctx->get_transaction_context();

  bool real_commit = all || thd->in_active_multi_stmt_transaction();
  if (real_commit) {
    txn_ctx->abort();
  }
  return 0;
}

static int db20xx_init_func(void *p) {
  DBUG_TRACE;

  db20xx_hton = (handlerton *)p;
  db20xx_hton->state = SHOW_OPTION_YES;
  db20xx_hton->create = db20xx_create_handler;
  db20xx_hton->commit = db20xx_commit;
  db20xx_hton->rollback = db20xx_rollback;
  db20xx_hton->flags = HTON_CAN_RECREATE;
  db20xx_hton->is_supported_system_table = db20xx_is_supported_system_table;

  db20xx::Engine::init();
  return 0;
}

struct st_mysql_storage_engine db20xx_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

static ulong srv_enum_var = 0;
static ulong srv_ulong_var = 0;
static double srv_double_var = 0;
static int srv_signed_int_var = 0;
static long srv_signed_long_var = 0;
static longlong srv_signed_longlong_var = 0;

const char *enum_var_names[] = {"e1", "e2", NullS};

TYPELIB enum_var_typelib = {array_elements(enum_var_names) - 1,
                            "enum_var_typelib", enum_var_names, nullptr};

static MYSQL_SYSVAR_ENUM(enum_var,                        // name
                         srv_enum_var,                    // varname
                         PLUGIN_VAR_RQCMDARG,             // opt
                         "Sample ENUM system variable.",  // comment
                         nullptr,                         // check
                         nullptr,                         // update
                         0,                               // def
                         &enum_var_typelib);              // typelib

static MYSQL_SYSVAR_ULONG(ulong_var, srv_ulong_var, PLUGIN_VAR_RQCMDARG,
                          "0..1000", nullptr, nullptr, 8, 0, 1000, 0);

static MYSQL_SYSVAR_DOUBLE(double_var, srv_double_var, PLUGIN_VAR_RQCMDARG,
                           "0.500000..1000.500000", nullptr, nullptr, 8.5, 0.5,
                           1000.5,
                           0);  // reserved always 0

static MYSQL_THDVAR_DOUBLE(double_thdvar, PLUGIN_VAR_RQCMDARG,
                           "0.500000..1000.500000", nullptr, nullptr, 8.5, 0.5,
                           1000.5, 0);

static MYSQL_SYSVAR_INT(signed_int_var, srv_signed_int_var, PLUGIN_VAR_RQCMDARG,
                        "INT_MIN..INT_MAX", nullptr, nullptr, -10, INT_MIN,
                        INT_MAX, 0);

static MYSQL_THDVAR_INT(signed_int_thdvar, PLUGIN_VAR_RQCMDARG,
                        "INT_MIN..INT_MAX", nullptr, nullptr, -10, INT_MIN,
                        INT_MAX, 0);

static MYSQL_SYSVAR_LONG(signed_long_var, srv_signed_long_var,
                         PLUGIN_VAR_RQCMDARG, "LONG_MIN..LONG_MAX", nullptr,
                         nullptr, -10, LONG_MIN, LONG_MAX, 0);

static MYSQL_THDVAR_LONG(signed_long_thdvar, PLUGIN_VAR_RQCMDARG,
                         "LONG_MIN..LONG_MAX", nullptr, nullptr, -10, LONG_MIN,
                         LONG_MAX, 0);

static MYSQL_SYSVAR_LONGLONG(signed_longlong_var, srv_signed_longlong_var,
                             PLUGIN_VAR_RQCMDARG, "LLONG_MIN..LLONG_MAX",
                             nullptr, nullptr, -10, LLONG_MIN, LLONG_MAX, 0);

static MYSQL_THDVAR_LONGLONG(signed_longlong_thdvar, PLUGIN_VAR_RQCMDARG,
                             "LLONG_MIN..LLONG_MAX", nullptr, nullptr, -10,
                             LLONG_MIN, LLONG_MAX, 0);

static SYS_VAR *db20xx_system_variables[] = {
    MYSQL_SYSVAR(enum_var),
    MYSQL_SYSVAR(ulong_var),
    MYSQL_SYSVAR(double_var),
    MYSQL_SYSVAR(double_thdvar),
    MYSQL_SYSVAR(last_create_thdvar),
    MYSQL_SYSVAR(create_count_thdvar),
    MYSQL_SYSVAR(signed_int_var),
    MYSQL_SYSVAR(signed_int_thdvar),
    MYSQL_SYSVAR(signed_long_var),
    MYSQL_SYSVAR(signed_long_thdvar),
    MYSQL_SYSVAR(signed_longlong_var),
    MYSQL_SYSVAR(signed_longlong_thdvar),
    nullptr};

// this is an db20xx of SHOW_FUNC
static int show_func_db20xx(MYSQL_THD, SHOW_VAR *var, char *buf) {
  var->type = SHOW_CHAR;
  var->value = buf;  // it's of SHOW_VAR_FUNC_BUFF_SIZE bytes
  snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE,
           "enum_var is %lu, ulong_var is %lu, "
           "double_var is %f, signed_int_var is %d, "
           "signed_long_var is %ld, signed_longlong_var is %lld",
           srv_enum_var, srv_ulong_var, srv_double_var, srv_signed_int_var,
           srv_signed_long_var, srv_signed_longlong_var);
  return 0;
}

struct db20xx_vars_t {
  ulong var1;
  double var2;
  char var3[64];
  bool var4;
  bool var5;
  ulong var6;
};

db20xx_vars_t db20xx_vars = {100,  20.01, "three hundred",
                                 true, false, 8250};

static SHOW_VAR show_status_db20xx[] = {
    {"var1", (char *)&db20xx_vars.var1, SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"var2", (char *)&db20xx_vars.var2, SHOW_DOUBLE, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF,
     SHOW_SCOPE_UNDEF}  // null terminator required
};

static SHOW_VAR show_array_db20xx[] = {
    {"array", (char *)show_status_db20xx, SHOW_ARRAY, SHOW_SCOPE_GLOBAL},
    {"var3", (char *)&db20xx_vars.var3, SHOW_CHAR, SHOW_SCOPE_GLOBAL},
    {"var4", (char *)&db20xx_vars.var4, SHOW_BOOL, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

static SHOW_VAR func_status[] = {
    {"db20xx_func_db20xx", (char *)show_func_db20xx, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"db20xx_status_var5", (char *)&db20xx_vars.var5, SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    {"db20xx_status_var6", (char *)&db20xx_vars.var6, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"db20xx_status", (char *)show_array_db20xx, SHOW_ARRAY,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

mysql_declare_plugin(db20xx){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &db20xx_storage_engine,
    "DB20XXDB",
    PLUGIN_AUTHOR_ORACLE,
    "Fulgurdb storage engine",
    PLUGIN_LICENSE_GPL,
    db20xx_init_func, /* Plugin Init */
    nullptr,            /* Plugin check uninstall */
    nullptr,            /* Plugin Deinit */
    0x0001 /* 0.1 */,
    func_status,               /* status variables */
    db20xx_system_variables, /* system variables */
    nullptr,                   /* config options */
    0,                         /* flags */
} mysql_declare_plugin_end;
