/*
   FASTMEM storage engine - handler
   ================================

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   The distinguishing feature of this engine is the total absence of
   table level locking: store_lock() returns zero THR_LOCK_DATA entries,
   external_lock() is a no-op, and every row access goes through the
   lock-free seqlock core in fm_core.h.
*/

#include "fm_def.h"
#include "sql_class.h"                        /* THD */

class ha_fastmem final : public handler
{
  FM_INFO *file;
  FM_SHARE *internal_share;
  key_map empty_keys;
  my_bool internal_table;
  /* Row-level read-modify-write serialization for write statements.
     While a write statement is active (external_lock got a write lock)
     every row read takes the slot wlock; update_row()/delete_row()
     run under that lock and release it.  Pure reads never lock. */
  bool write_stmt;
  bool row_locked;
  fm_i32 locked_slot;

  void lock_current_row();
  void unlock_current_row();
public:
  ha_fastmem(handlerton *hton, TABLE_SHARE *table);
  ~ha_fastmem() override = default;
  handler *clone(const char *name, MEM_ROOT *mem_root) override;
  /* Rows also use a fixed-size format */
  enum row_type get_row_type() const override { return ROW_TYPE_FIXED; }
  ulonglong table_flags() const override
  {
    return (HA_FAST_KEY_READ | HA_NO_BLOBS | HA_NULL_IN_KEY |
            HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE |
            HA_CAN_SQL_HANDLER | HA_CAN_ONLINE_BACKUPS |
            HA_REC_NOT_IN_SEQ | HA_CAN_INSERT_DELAYED | HA_NO_TRANSACTIONS |
            HA_HAS_RECORDS | HA_STATS_RECORDS_IS_EXACT | HA_CAN_HASH_KEYS);
  }
  ulong index_flags(uint inx, uint part, bool all_parts) const override
  {
    return HA_ONLY_WHOLE_INDEX | HA_KEY_SCAN_NOT_ROR;
  }
  const key_map *keys_to_use_for_scanning() override { return &empty_keys; }
  uint max_supported_keys()          const override { return MAX_KEY; }
  uint max_supported_key_part_length() const override { return MAX_KEY_LENGTH; }
  IO_AND_CPU_COST scan_time() override;
  IO_AND_CPU_COST keyread_time(uint index, ulong ranges, ha_rows rows,
                               ulonglong blocks) override;
  IO_AND_CPU_COST rnd_pos_time(ha_rows rows) override;

  int open(const char *name, int mode, uint test_if_locked) override;
  int close(void) override;
  int write_row(const uchar * buf) override;
  int update_row(const uchar * old_data, const uchar * new_data) override;
  int delete_row(const uchar * buf) override;
  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values,
                          ulonglong *first_value,
                          ulonglong *nb_reserved_values) override;
  int index_read_map(uchar * buf, const uchar * key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_read_last_map(uchar *buf, const uchar *key, key_part_map keypart_map)
    override;
  int index_read_idx_map(uchar * buf, uint index, const uchar * key,
                         key_part_map keypart_map,
                         enum ha_rkey_function find_flag) override;
  int index_next(uchar * buf) override;
  int index_prev(uchar * buf) override;
  int index_first(uchar * buf) override;
  int index_last(uchar * buf) override;
  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar * buf, uchar *pos) override;
  void position(const uchar *record) override;
  int can_continue_handler_scan() override { return 0; }
  int info(uint) override;
  int extra(enum ha_extra_function operation) override;
  int reset() override;
  int external_lock(THD *thd, int lock_type) override
  {
    /*
      Track write statements so row reads can serialize the whole
      read-modify-write cycle (see lock_current_row).  There is still
      no table-level lock: this only flips an in-handler flag.

      NOTE: lock_type is fcntl-style (F_RDLCK=1 / F_WRLCK=2 /
      F_UNLCK=3, see include/my_global.h), NOT thr_lock_type.  Earlier
      builds compared against thr_lock_type values and never set
      write_stmt, silently falling back to read-checked but unlocked
      writes.
    */
    if (lock_type == F_WRLCK)
      write_stmt= true;
    else if (lock_type == F_UNLCK)
    {
      write_stmt= false;
      unlock_current_row();
    }
    return 0;
  }
  int delete_all_rows(void) override;
  int reset_auto_increment(ulonglong value) override;
  int disable_indexes(key_map map, bool persist) override;
  int enable_indexes(key_map map, bool persist) override;
  int indexes_are_disabled(void) override;
  ha_rows records_in_range(uint inx, const key_range *start_key,
                           const key_range *end_key, page_range *pages) override;
  int delete_table(const char *from) override;
  void drop_table(const char *name) override;
  int rename_table(const char * from, const char * to) override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info) override;
  void update_create_info(HA_CREATE_INFO *create_info) override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override
  {
    /* FASTMEM has no table locks: return zero THR_LOCK_DATA entries. */
    return to;
  }
  int cmp_ref(const uchar *ref1, const uchar *ref2) override
  {
    return memcmp(ref1, ref2, FM_REF_LENGTH);
  }
  bool check_if_incompatible_data(HA_CREATE_INFO *info, uint table_changes)
    override;
  int find_unique_row(uchar *record, uint unique_idx) override;
};