/*
   FASTMEM storage engine - handler
   ================================

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   THD handler for the lock-free in-memory engine.  There is deliberately
   no THR_LOCK: store_lock() contributes no lock data and external_lock()
   does nothing, so the server never takes or releases table locks for
   this engine.  All intra-table synchronization happens inside fm_core.h
   (sequential lock reads, per-slot writer locks, per-bucket spinlocks).
*/

#define MYSQL_SERVER 1
#include "fm_def.h"
#include "sql_priv.h"
#include "sql_plugin.h"
#include "ha_fastmem.h"
#include "sql_base.h"

static handler *fastmem_create_handler(handlerton *, TABLE_SHARE *, MEM_ROOT *);
static int fastmem_drop_table(handlerton *hton, const char *path);

/* See optimizer_costs.txt for how these values were calculated
   (kept identical to the MEMORY engine for a fair comparison). */
#define HEAP_ROW_NEXT_FIND_COST  8.0166e-06           /* table scan        */
#define HEAP_LOOKUP_COST         0.00016097           /* hash lookup       */

static void fastmem_update_optimizer_costs(OPTIMIZER_COSTS *costs)
{
  costs->disk_read_cost= 0;          /* all data in memory */
  costs->disk_read_ratio= 0.0;
  costs->key_next_find_cost= 0;
  costs->key_copy_cost= 0;
  costs->row_copy_cost= 2.334e-06;   /* just a memcpy */
  costs->row_lookup_cost= 0;
  costs->row_next_find_cost= HEAP_ROW_NEXT_FIND_COST;
  costs->key_lookup_cost= 0;
  costs->index_block_copy_cost= 0;
}

int fastmem_init(void *p)
{
  handlerton *fastmem_hton;

  mysql_mutex_init(0, &THR_LOCK_fastmem, MY_MUTEX_INIT_FAST);

  fastmem_hton= (handlerton *) p;
  fastmem_hton->create= fastmem_create_handler;
  fastmem_hton->drop_table= fastmem_drop_table;
  fastmem_hton->update_optimizer_costs= fastmem_update_optimizer_costs;
  fastmem_hton->flags= HTON_CAN_RECREATE;

  return 0;
}

static int fastmem_drop_table(handlerton *hton, const char *path)
{
  int error= fm_delete_table(path);
  return error == ENOENT ? -1 : error;
}

static handler *fastmem_create_handler(handlerton *hton,
                                       TABLE_SHARE *table,
                                       MEM_ROOT *mem_root)
{
  return new (mem_root) ha_fastmem(hton, table);
}

/*****************************************************************************
** FASTMEM tables
*****************************************************************************/

ha_fastmem::ha_fastmem(handlerton *hton, TABLE_SHARE *table_arg)
  : handler(hton, table_arg), file(0), internal_table(0),
    write_stmt(false), row_locked(false), locked_slot(-1)
{
  empty_keys.clear_all();
}

/* ------------------------------------------------------------------ */
/* Row-level read-modify-write serialization (write statements only).  */
/*                                                                     */
/* SQL executes UPDATE/DELETE as: index_read/rnd_pos/rnd_next (copies  */
/* the row image) -> compute -> update_row/delete_row.  Two concurrent */
/* writers would both read the same old image and the last write wins, */
/* losing an update.  To make the cycle atomic we take the slot wlock  */
/* when the row image is read, hold it across the handler calls and    */
/* release it in update_row()/delete_row().  Readers (SELECT) never    */
/* come here: they stay completely lock-free.                          */
/* ------------------------------------------------------------------ */

void ha_fastmem::lock_current_row()
{
  FM_SLOT *sl;
  if (!file || file->current_slot < 0)
    return;
  if (row_locked && locked_slot == file->current_slot)
    return;                                 /* already held for this row */
  if (row_locked)
    unlock_current_row();                   /* move the lock to the new row */
  sl= fm_slot(&file->s->core, file->current_slot);
  sl->wlock.lock();
  row_locked= true;
  locked_slot= file->current_slot;
}

void ha_fastmem::unlock_current_row()
{
  if (!row_locked)
    return;
  fm_slot(&file->s->core, locked_slot)->wlock.unlock();
  row_locked= false;
  locked_slot= -1;
}

int ha_fastmem::open(const char *name, int mode, uint test_if_locked)
{
  internal_table= MY_TEST(test_if_locked & HA_OPEN_INTERNAL_TABLE);
  if (internal_table || (!(file= fm_open(name, mode)) && my_errno == ENOENT))
  {
    FM_CREATE_INFO create_info;
    bool created_new_share;
    int rc;

    file= 0;
    if (fm_prepare_create_info(table, internal_table, &create_info))
      goto end;
    create_info.pin_share= true;

    rc= fm_create(name, &create_info, true, &internal_share,
                  &created_new_share);
    my_free(create_info.keydef);
    if (rc)
      goto end;

    implicit_emptied= MY_TEST(created_new_share);
    if (internal_table)
      file= fm_open_from_share(internal_share, mode);
    else
      file= fm_open_from_share_and_register(internal_share, mode);

    if (!file)
    {
      fm_release_share(internal_share, internal_table);
      goto end;
    }
  }

  fm_key_ops_init(file->s);
  ref_length= FM_REF_LENGTH;
end:
  return (file ? 0 : 1);
}

int ha_fastmem::close(void)
{
  return fm_close(file) ? 1 : 0;
}

/*
   Create a copy of this table: open the same share we are already on
   (mirrors the MEMORY engine's clone, which uses the share's name).
*/
handler *ha_fastmem::clone(const char *name, MEM_ROOT *mem_root)
{
  handler *new_handler= get_new_handler(table->s, mem_root, ht);
  if (new_handler && !new_handler->ha_open(table, file->s->name,
                                           table->db_stat,
                                           HA_OPEN_IGNORE_IF_LOCKED))
    return new_handler;
  return NULL;  /* purecov: inspected */
}

IO_AND_CPU_COST ha_fastmem::scan_time()
{
  return { 0, (double) stats.deleted * HEAP_ROW_NEXT_FIND_COST };
}

IO_AND_CPU_COST ha_fastmem::keyread_time(uint index, ulong ranges, ha_rows rows,
                                         ulonglong blocks)
{
  return { 0, ranges * HEAP_LOOKUP_COST +
               (rows - ranges) * HEAP_ROW_NEXT_FIND_COST };
}

IO_AND_CPU_COST ha_fastmem::rnd_pos_time(ha_rows rows)
{
  return { 0, 0 };
}

int ha_fastmem::write_row(const uchar * buf)
{
  int res;
  if (table->next_number_field && buf == table->record[0])
  {
    if ((res= update_auto_increment()))
      return res;
  }
  res= fm_write(file, buf);
  return res;
}

int ha_fastmem::update_row(const uchar * old_data, const uchar * new_data)
{
  /*
    The slot wlock is held from the row read across the whole
    read-modify-write cycle.  Defensively, if a path reached us
    without the lock, try to take it now; only refuse when that is
    impossible.  Never write unlocked: that would lose updates.
  */
  if (!(write_stmt && row_locked && locked_slot == file->current_slot))
  {
    if (write_stmt && file->current_slot >= 0 && !row_locked)
      lock_current_row();
    if (!(row_locked && locked_slot == file->current_slot))
    {
      unlock_current_row();
      my_errno= HA_ERR_RECORD_CHANGED;
      return my_errno;
    }
  }
  int rc= fm_update_locked(file, old_data, new_data);
  unlock_current_row();
  return rc;
}

int ha_fastmem::delete_row(const uchar * buf)
{
  if (!(write_stmt && row_locked && locked_slot == file->current_slot))
  {
    if (write_stmt && file->current_slot >= 0 && !row_locked)
      lock_current_row();
    if (!(row_locked && locked_slot == file->current_slot))
    {
      unlock_current_row();
      my_errno= HA_ERR_RECORD_CHANGED;
      return my_errno;
    }
  }
  int rc= fm_delete_locked(file, buf);
  unlock_current_row();
  return rc;
}

void ha_fastmem::get_auto_increment(ulonglong offset, ulonglong increment,
                                    ulonglong nb_desired_values,
                                    ulonglong *first_value,
                                    ulonglong *nb_reserved_values)
{
  *first_value= fm_get_auto_increment(file, offset, increment,
                                      nb_desired_values, nb_reserved_values);
  if (*first_value == ~(ulonglong) 0)
    my_errno= HA_ERR_AUTOINC_READ_FAILED;
}

int ha_fastmem::index_read_map(uchar *buf, const uchar *key,
                               key_part_map keypart_map,
                               enum ha_rkey_function find_flag)
{
  DBUG_ASSERT(inited==INDEX);
  /* HASH keys: one whole-key exact lookup; everything else is ignored
     (mirrors heap_rkey semantics for hash indexes). */
  int rc= fm_find_key(file, active_index, key, buf);
  if (rc == 0 && write_stmt)
  {
    lock_current_row();
    /* Re-read under the lock: the pre-lock image may already be stale
       (another writer may have updated the row between our lookup and
       the lock acquisition). */
    if (fm_reread_locked(file, buf))
    {
      unlock_current_row();
      return my_errno= HA_ERR_KEY_NOT_FOUND;
    }
  }
  return rc;
}

int ha_fastmem::index_read_last_map(uchar *buf, const uchar *key,
                                    key_part_map keypart_map)
{
  DBUG_ASSERT(inited==INDEX);
  int rc= fm_find_key(file, active_index, key, buf);
  if (rc == 0 && write_stmt)
  {
    lock_current_row();
    /* Re-read under the lock: the pre-lock image may already be stale
       (another writer may have updated the row between our lookup and
       the lock acquisition). */
    if (fm_reread_locked(file, buf))
    {
      unlock_current_row();
      return my_errno= HA_ERR_KEY_NOT_FOUND;
    }
  }
  return rc;
}

int ha_fastmem::index_read_idx_map(uchar *buf, uint index, const uchar *key,
                                   key_part_map keypart_map,
                                   enum ha_rkey_function find_flag)
{
  int rc= fm_find_key(file, index, key, buf);
  if (rc == 0 && write_stmt)
  {
    lock_current_row();
    /* Re-read under the lock: the pre-lock image may already be stale
       (another writer may have updated the row between our lookup and
       the lock acquisition). */
    if (fm_reread_locked(file, buf))
    {
      unlock_current_row();
      return my_errno= HA_ERR_KEY_NOT_FOUND;
    }
  }
  return rc;
}

int ha_fastmem::index_next(uchar * buf)
{
  DBUG_ASSERT(inited==INDEX);
  if (file->lastinx < 0)
    return my_errno= HA_ERR_WRONG_INDEX;
  int error= fm_find_next_key(file, file->lastinx, file->lastkey,
                              file->current_slot, buf);
  if (error == HA_ERR_KEY_NOT_FOUND && !(file->update & HA_STATE_NO_KEY))
    return my_errno= HA_ERR_END_OF_FILE;
  if (error == 0 && write_stmt)
  {
    lock_current_row();
    /* Re-read under the lock: the pre-lock image may be stale. */
    if (fm_reread_locked(file, buf))
    {
      unlock_current_row();
      return my_errno= HA_ERR_KEY_NOT_FOUND;
    }
  }
  return error;
}

/* The engine has no ordered indexes: prev/first/last are not supported. */
int ha_fastmem::index_prev(uchar * buf)
{
  DBUG_ASSERT(inited==INDEX);
  return my_errno= HA_ERR_WRONG_COMMAND;
}

int ha_fastmem::index_first(uchar * buf)
{
  DBUG_ASSERT(inited==INDEX);
  return my_errno= HA_ERR_WRONG_COMMAND;
}

int ha_fastmem::index_last(uchar * buf)
{
  DBUG_ASSERT(inited==INDEX);
  return my_errno= HA_ERR_WRONG_COMMAND;
}

int ha_fastmem::rnd_init(bool scan)
{
  if (scan)
  {
    unlock_current_row();               /* drop any sticky row lock */
    return fm_scan_init(file);
  }
  return 0;
}

int ha_fastmem::rnd_next(uchar *buf)
{
  int error= fm_scan(file, buf);
  if (error == HA_ERR_END_OF_FILE)
    return HA_ERR_END_OF_FILE;
  if (error == 0 && write_stmt)
  {
    lock_current_row();                 /* moves the lock to the new row */
    /* Re-read under the lock: the pre-lock image may be stale. */
    if (fm_reread_locked(file, buf))
    {
      unlock_current_row();
      return my_errno= HA_ERR_RECORD_DELETED;
    }
  }
  return error;
}

int ha_fastmem::rnd_pos(uchar * buf, uchar *pos)
{
  int rc= fm_rrnd(file, buf, pos);
  if (rc == 0 && write_stmt)
  {
    lock_current_row();
    /* Re-read under the lock: the pre-lock image may already be stale
       (another writer may have updated the row between our lookup and
       the lock acquisition). */
    if (fm_reread_locked(file, buf))
    {
      unlock_current_row();
      return my_errno= HA_ERR_RECORD_DELETED;
    }
  }
  return rc;
}

void ha_fastmem::position(const uchar *record)
{
  fm_set_position(file, ref);
}

int ha_fastmem::info(uint flag)
{
  FMINFO fm;

  (void) fm_info(file, &fm, flag);

  errkey=                     fm.errkey;
  stats.records=              fm.records;
  stats.deleted=              fm.deleted;
  stats.mean_rec_length=      fm.reclength;
  stats.data_file_length=     fm.data_length;
  stats.index_file_length=    fm.index_length;
  stats.max_data_file_length= fm.max_records * fm.reclength;
  stats.delete_length=        fm.deleted * fm.reclength;
  stats.create_time=          (ulong) fm.create_time;
  if (flag & HA_STATUS_AUTO)
    stats.auto_increment_value= fm.auto_increment;

  /* Simple hash-index selectivity estimates. */
  for (uint i= 0; i < table->s->keys; i++)
  {
    KEY *key= table->key_info + i;
    if (!key->rec_per_key)
      continue;
    if (key->flags & HA_NOSAME)
      key->rec_per_key[key->user_defined_key_parts-1]= 1;
    else
    {
      ha_rows per_bucket= 2;
      if (stats.records > 1)
        per_bucket= (ha_rows)
          (stats.records / file->s->core.keydef[i].nbuckets);
      if (per_bucket < 2)
        per_bucket= 2;
      key->rec_per_key[key->user_defined_key_parts-1]= (ulong) per_bucket;
    }
  }
  return 0;
}

int ha_fastmem::extra(enum ha_extra_function operation)
{
  return fm_extra(file, operation);
}

int ha_fastmem::reset()
{
  unlock_current_row();               /* statement boundary: drop lock */
  return fm_reset(file);
}

int ha_fastmem::delete_all_rows()
{
  fm_clear(file);
  return 0;
}

int ha_fastmem::reset_auto_increment(ulonglong value)
{
  fm_set_auto_increment(file->s, value);
  return 0;
}

int ha_fastmem::disable_indexes(key_map map, bool persist)
{
  /* Not implemented: this engine cannot repair indexes after data load. */
  return HA_ERR_WRONG_COMMAND;
}

int ha_fastmem::enable_indexes(key_map map, bool persist)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_fastmem::indexes_are_disabled(void)
{
  return 0;
}

ha_rows ha_fastmem::records_in_range(uint inx, const key_range *min_key,
                                     const key_range *max_key,
                                     page_range *pages)
{
  KEY *key= table->key_info + inx;

  if (!min_key || !max_key ||
      min_key->length != max_key->length ||
      min_key->length != key->key_length ||
      min_key->flag != HA_READ_KEY_EXACT ||
      max_key->flag != HA_READ_AFTER_KEY)
    return HA_POS_ERROR;               /* only exact whole keys */

  if (stats.records <= 1)
    return stats.records;
  return key->rec_per_key[key->user_defined_key_parts-1];
}

int ha_fastmem::delete_table(const char *name)
{
  return fastmem_drop_table(0, name);
}

void ha_fastmem::drop_table(const char *name)
{
  file->s->delete_on_close= 1;
  ha_close();
}

int ha_fastmem::rename_table(const char * from, const char * to)
{
  return fm_rename(from, to);
}

int ha_fastmem::create(const char *name, TABLE *table_arg,
                       HA_CREATE_INFO *create_info)
{
  int error;
  bool created;
  FM_CREATE_INFO fm_create_info;

  error= fm_prepare_create_info(table_arg, internal_table, &fm_create_info);
  if (error)
    return error;
  fm_create_info.auto_increment= (create_info->auto_increment_value ?
                                  create_info->auto_increment_value - 1 : 0);
  error= fm_create(name, &fm_create_info, false, &internal_share, &created);
  my_free(fm_create_info.keydef);
  DBUG_ASSERT(file == 0);
  return error;
}

void ha_fastmem::update_create_info(HA_CREATE_INFO *create_info)
{
  table->file->info(HA_STATUS_AUTO);
  if (!(create_info->used_fields & HA_CREATE_USED_AUTO))
    create_info->auto_increment_value= stats.auto_increment_value;
}

bool ha_fastmem::check_if_incompatible_data(HA_CREATE_INFO *info,
                                            uint table_changes)
{
  /* Check that auto_increment value was not changed */
  if ((info->used_fields & HA_CREATE_USED_AUTO &&
       info->auto_increment_value != 0) ||
      table_changes == IS_EQUAL_NO ||
      table_changes & IS_EQUAL_PACK_LENGTH)      // Not implemented yet
    return COMPATIBLE_DATA_NO;
  return COMPATIBLE_DATA_YES;
}

/*
   Find a row by its unique key, given a full record image (used by the
   server for duplicate checks on internal temporary tables).
*/
int ha_fastmem::find_unique_row(uchar *record, uint unique_idx)
{
  FM_SHARE *share= file->s;
  FM_KEYDEF *kd= &share->keydef[unique_idx];
  fm_i32 idx= -1;
  int rc;

  DBUG_ASSERT(inited==NONE);
  if (!(kd->flag & HA_NOSAME))
    return 1;

  fm_make_key(kd, file->lastkey, record);
  rc= fm_core_find(&share->core, unique_idx, file->lastkey, file->scratch,
                   &idx);
  if (rc != FM_ERR_OK)
    return 1;                               /* not found */

  file->current_slot= idx;
  file->current_gen= fm_slot(&share->core, idx)->gen.load(
      std::memory_order_acquire);
  file->lastinx= (int) unique_idx;
  file->update= HA_STATE_AKTIV;
  memcpy(record, file->scratch, share->core.reclength);
  if (write_stmt)
  {
    lock_current_row();
    /* Re-read under the lock: the pre-lock image may be stale. */
    if (fm_reread_locked(file, record))
    {
      unlock_current_row();
      return 1;
    }
  }
  return 0;
}

struct st_mysql_storage_engine fastmem_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(fastmem)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &fastmem_storage_engine,
  "FASTMEM",
  "FASTMEM developers",
  "Lock-free in-memory tables (seqlock row images, per-slot writers)",
  PLUGIN_LICENSE_GPL,
  fastmem_init,
  NULL,
  0x0101,                                  /* 1.1 */
  NULL,                                    /* status variables */
  NULL,                                    /* system variables */
  "1.1",                                   /* string version */
  MariaDB_PLUGIN_MATURITY_GAMMA            /* maturity */
}
maria_declare_plugin_end;