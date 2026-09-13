/*
   FASTMEM storage engine - row data operations
   =============================================

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   Thin layer between a handler open handle (FM_INFO) and the lock-free
   concurrency core (fm_core.h).  All row access on the hot path is done
   here; registry-level operations live in fm_create.cc.
*/

#include "fm_def.h"
#include <my_sys.h>
#include <m_ctype.h>

/* ------------------------------------------------------------------ */
/* Error mapping                                                       */
/* ------------------------------------------------------------------ */

static int fm_core_error_to_ha(int rc)
{
  switch (rc)
  {
  case FM_ERR_FULL:
    my_errno= HA_ERR_RECORD_FILE_FULL;
    break;
  case FM_ERR_NOMEM:
    my_errno= HA_ERR_OUT_OF_MEM;
    break;
  case FM_ERR_DELETED:
    my_errno= HA_ERR_RECORD_DELETED;
    break;
  case FM_ERR_DUP_KEY:
    my_errno= HA_ERR_FOUND_DUPP_KEY;
    break;
  case FM_ERR_NOT_FOUND:
    my_errno= HA_ERR_KEY_NOT_FOUND;
    break;
  default:
    my_errno= HA_ERR_CRASHED;
    break;
  }
  return my_errno;
}

static inline fm_u32 fm_slot_gen(FM_SHARE *share, fm_i32 idx)
{
  return fm_slot(&share->core, idx)->gen.load(std::memory_order_acquire);
}

/* ------------------------------------------------------------------ */
/* Write (INSERT)                                                      */
/* ------------------------------------------------------------------ */

int fm_write(FM_INFO *info, const uchar *record)
{
  FM_SHARE *share= info->s;
  fm_i32 idx= -1;
  int ek= 0;
  int rc= fm_core_insert_row(&share->core, (const fm_u8 *) record, &idx,
                             &ek, info->scratch);
  if (rc != FM_ERR_OK)
  {
    if (rc == FM_ERR_DUP_KEY)
      info->errkey= ek;
    DBUG_PRINT("error", ("Write row: %d", rc));
    return fm_core_error_to_ha(rc);
  }
  if (share->core.auto_key)
    fm_update_auto_increment(info, record);

  info->lastinx= -1;
  info->current_hash_slot= -1;
  info->current_slot= idx;
  info->current_gen= fm_slot_gen(share, idx);
  info->update= HA_STATE_AKTIV;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Re-read under the slot wlock (read-modify-write atomicity)          */
/* ------------------------------------------------------------------ */

/*
  Re-read the current row while the caller already holds the slot
  wlock.  The point lookup that located the row ran BEFORE the lock was
  taken, so its image may already be stale; after this call the record
  buffer holds the value guaranteed current for the whole
  read-modify-write cycle.

  Returns 0 on success; FM_ERR_DELETED when the row disappeared
  between the pre-lock read and the lock acquisition (delete/recycle).
*/
int fm_reread_locked(FM_INFO *info, uchar *record)
{
  FM_SHARE *share= info->s;
  fm_i32 idx= info->current_slot;
  fm_u32 g= fm_slot_gen(share, idx);
  int rc= fm_core_read_row(&share->core, idx, true, g, (fm_u8 *) record);
  if (rc == FM_ERR_OK)
    info->current_gen= g;
  return rc;
}

/* ------------------------------------------------------------------ */
/* Update                                                              */
/* ------------------------------------------------------------------ */

int fm_update(FM_INFO *info, const uchar *old, const uchar *newdata)
{
  FM_SHARE *share= info->s;
  int rc;

  /*
    Verify the record image if the caller asked for read checks.  A
    relaxed seqlock read is enough: any concurrent key change also
    rewrites the whole image, so comparing gives "record changed".
  */
  if (info->opt_flag & READ_CHECK_USED)
  {
    rc= fm_core_read_row(&share->core, info->current_slot, true,
                         info->current_gen, info->scratch);
    if (rc != FM_ERR_OK)
      return fm_core_error_to_ha(rc);
    if (bcmp(old, info->scratch, share->core.reclength))
    {
      my_errno= HA_ERR_RECORD_CHANGED;
      return my_errno;
    }
  }

  int ek= 0;
  rc= fm_core_update_row(&share->core, info->current_slot,
                         info->current_gen, (const fm_u8 *) old,
                         (const fm_u8 *) newdata, &ek,
                         info->scratch);
  if (rc != FM_ERR_OK)
  {
    if (rc == FM_ERR_DUP_KEY)
      info->errkey= ek;
    return fm_core_error_to_ha(rc);
  }
  if (share->core.auto_key)
    fm_update_auto_increment(info, newdata);

  info->update= HA_STATE_AKTIV;
  info->current_hash_slot= -1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Delete                                                              */
/* ------------------------------------------------------------------ */

int fm_delete(FM_INFO *info, const uchar *buf)
{
  FM_SHARE *share= info->s;
  int rc;

  if (info->opt_flag & READ_CHECK_USED)
  {
    rc= fm_core_read_row(&share->core, info->current_slot, true,
                         info->current_gen, info->scratch);
    if (rc != FM_ERR_OK)
      return fm_core_error_to_ha(rc);
    if (bcmp(buf, info->scratch, share->core.reclength))
    {
      my_errno= HA_ERR_RECORD_CHANGED;
      return my_errno;
    }
  }

  rc= fm_core_delete_row(&share->core, info->current_slot,
                         info->current_gen, info->scratch);
  if (rc != FM_ERR_OK)
    return fm_core_error_to_ha(rc);
  info->update= HA_STATE_DELETED;
  info->current_slot= -1;
  info->current_hash_slot= -1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Update / delete with the slot wlock already held by the caller      */
/* (handler-level read-modify-write serialization).  The caller owns   */
/* the wlock - these functions must NOT release it.                    */
/* ------------------------------------------------------------------ */

int fm_update_locked(FM_INFO *info, const uchar *old, const uchar *newdata)
{
  FM_SHARE *share= info->s;
  int rc;

  if (info->opt_flag & READ_CHECK_USED)
  {
    rc= fm_core_read_row(&share->core, info->current_slot, true,
                         info->current_gen, info->scratch);
    if (rc != FM_ERR_OK)
      return fm_core_error_to_ha(rc);
    if (bcmp(old, info->scratch, share->core.reclength))
    {
      my_errno= HA_ERR_RECORD_CHANGED;
      return my_errno;
    }
  }

  int ek= 0;
  rc= fm_core_update_row_locked(&share->core, info->current_slot,
                                info->current_gen, (const fm_u8 *) old,
                                (const fm_u8 *) newdata, &ek,
                                info->scratch);
  if (rc != FM_ERR_OK)
  {
    if (rc == FM_ERR_DUP_KEY)
      info->errkey= ek;
    return fm_core_error_to_ha(rc);
  }
  if (share->core.auto_key)
    fm_update_auto_increment(info, newdata);

  info->update= HA_STATE_AKTIV;
  info->current_hash_slot= -1;
  return 0;
}

int fm_delete_locked(FM_INFO *info, const uchar *buf)
{
  FM_SHARE *share= info->s;
  int rc;

  if (info->opt_flag & READ_CHECK_USED)
  {
    rc= fm_core_read_row(&share->core, info->current_slot, true,
                         info->current_gen, info->scratch);
    if (rc != FM_ERR_OK)
      return fm_core_error_to_ha(rc);
    if (bcmp(buf, info->scratch, share->core.reclength))
    {
      my_errno= HA_ERR_RECORD_CHANGED;
      return my_errno;
    }
  }

  rc= fm_core_delete_row_locked(&share->core, info->current_slot,
                                info->current_gen, info->scratch);
  if (rc != FM_ERR_OK)
    return fm_core_error_to_ha(rc);
  info->update= HA_STATE_DELETED;
  info->current_slot= -1;
  info->current_hash_slot= -1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Random access                                                       */
/* ------------------------------------------------------------------ */

/* Pack the current scan position into an 8-byte ref. */
void fm_set_position(FM_INFO *info, uchar *ref)
{
  FM_SLOT_REF r;
  r.slot= info->current_slot;
  r.gen= info->current_gen;
  memcpy(ref, &r, FM_REF_LENGTH);
}

int fm_rrnd(FM_INFO *info, uchar *record, const uchar *pos)
{
  FM_SHARE *share= info->s;
  FM_SLOT_REF r;
  memcpy(&r, pos, FM_REF_LENGTH);
  info->lastinx= -1;
  if (r.slot == -1)
  {
    info->update= 0;
    return my_errno= HA_ERR_END_OF_FILE;
  }
  if (fm_core_read_row(&share->core, r.slot, true, r.gen,
                       (fm_u8 *) record) != FM_ERR_OK)
  {
    info->update= HA_STATE_PREV_FOUND | HA_STATE_NEXT_FOUND;
    return my_errno= HA_ERR_RECORD_DELETED;
  }
  info->current_slot= r.slot;
  info->current_gen= r.gen;
  info->current_hash_slot= -1;
  info->update= HA_STATE_PREV_FOUND | HA_STATE_NEXT_FOUND | HA_STATE_AKTIV;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Scan (loose): rows may appear/disappear during the scan             */
/* ------------------------------------------------------------------ */

int fm_scan_init(FM_INFO *info)
{
  info->lastinx= -1;
  info->scan_pos= 0;
  info->current_slot= -1;
  info->current_hash_slot= -1;
  info->update= 0;
  return 0;
}

int fm_scan(FM_INFO *info, uchar *record)
{
  FM_SHARE *share= info->s;
  int rc= fm_core_scan_next(&share->core, &info->scan_pos,
                            (fm_u8 *) record);
  if (rc != FM_ERR_OK)
  {
    info->update= HA_STATE_PREV_FOUND | HA_STATE_NEXT_FOUND;
    return my_errno= HA_ERR_END_OF_FILE;
  }
  info->current_slot= (fm_i32) (info->scan_pos - 1);
  info->current_gen= fm_slot_gen(share, info->current_slot);
  info->current_hash_slot= -1;
  info->update= HA_STATE_PREV_FOUND | HA_STATE_NEXT_FOUND | HA_STATE_AKTIV;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Point lookups by hash key                                           */
/* ------------------------------------------------------------------ */

int fm_find_key(FM_INFO *info, int inx, const uchar *key, uchar *record)
{
  FM_SHARE *share= info->s;
  FM_KEY_CORE *kc= &share->core.keydef[inx];
  fm_i32 idx= -1;
  int rc= fm_core_find(&share->core, (fm_u32) inx, (const fm_u8 *) key,
                       (fm_u8 *) record, &idx);
  if (rc != FM_ERR_OK)
  {
    info->update= HA_STATE_NO_KEY;
    return my_errno= HA_ERR_KEY_NOT_FOUND;
  }
  /* Keep the packed key when a future index_next may need it. */
  if ((kc->ops.hash_rec != 0) &&
      (share->keydef[inx].flag & (HA_NOSAME | HA_NULL_PART_KEY)) != HA_NOSAME)
  {
    memcpy(info->lastkey, key, share->keydef[inx].length);
    info->lastkey_len= share->keydef[inx].length;
  }
  info->lastinx= inx;
  info->current_slot= idx;
  info->current_gen= fm_slot_gen(share, idx);
  info->update= HA_STATE_AKTIV;
  return 0;
}

int fm_find_next_key(FM_INFO *info, int inx, const uchar *key,
                     fm_i32 cur_slot, uchar *record)
{
  FM_SHARE *share= info->s;
  fm_i32 idx= -1;
  int rc= fm_core_find_next(&share->core, (fm_u32) inx,
                            (const fm_u8 *) key, cur_slot,
                            (fm_u8 *) record, &idx);
  if (rc != FM_ERR_OK)
  {
    info->update= HA_STATE_NEXT_FOUND;
    return my_errno= HA_ERR_KEY_NOT_FOUND;
  }
  /* Verified above to be a non-unique key path. */
  if (share->keydef[inx].length != info->lastkey_len ||
      bcmp(info->lastkey, key, share->keydef[inx].length))
  {
    memcpy(info->lastkey, key, share->keydef[inx].length);
    info->lastkey_len= share->keydef[inx].length;
  }
  info->lastinx= inx;
  info->current_slot= idx;
  info->current_gen= fm_slot_gen(share, idx);
  info->update= HA_STATE_AKTIV | HA_STATE_NEXT_FOUND;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Extra / reset                                                       */
/* ------------------------------------------------------------------ */

int fm_extra(FM_INFO *info, enum ha_extra_function function)
{
  switch (function)
  {
  case HA_EXTRA_RESET_STATE:
    fm_reset(info);
    break;                              /* keep READ_CHECK_USED: the read
                                           check is a safety net and must
                                           not be dropped by a statement
                                           reset */
  case HA_EXTRA_NO_READCHECK:
    info->opt_flag&= ~READ_CHECK_USED;
    break;
  case HA_EXTRA_READCHECK:
    info->opt_flag|= READ_CHECK_USED;
    break;
  case HA_EXTRA_CHANGE_KEY_TO_UNIQUE:
  case HA_EXTRA_CHANGE_KEY_TO_DUP:
  {
    uint idx;
    for (idx= 0; idx < info->s->core.keys; idx++)
    {
      if (function == HA_EXTRA_CHANGE_KEY_TO_UNIQUE)
      {
        info->s->keydef[idx].flag|= HA_NOSAME;
        info->s->core.keydef[idx].unique= true;
      }
      else
      {
        info->s->keydef[idx].flag&= ~(HA_NOSAME);
        info->s->core.keydef[idx].unique= false;
      }
    }
    break;
  }
  default:
    break;
  }
  return 0;
}

int fm_reset(FM_INFO *info)
{
  info->lastinx= -1;
  info->scan_pos= 0;
  info->current_slot= -1;
  info->current_hash_slot= -1;
  info->update= 0;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

int fm_info(FM_INFO *info, FMINFO *x, int flag)
{
  FM_SHARE *share= info->s;
  x->records= (ha_rows) share->core.records.load(std::memory_order_relaxed);
  x->deleted= 0;
  x->max_records= share->core.max_records;
  x->reclength= share->core.reclength;
  x->errkey= info->errkey;
  x->auto_increment= share->core.auto_inc.load(std::memory_order_relaxed) + 1;
  x->create_time= share->create_time;

  {
    fm_u32 slot_count= fm_core_slot_count(&share->core);
    ulonglong data_length= (ulonglong) slot_count * share->core.slot_size +
                           (ulonglong) slot_count * share->core.keys * 4;
    ulonglong index_length= 0;
    for (uint k= 0; k < share->core.keys; k++)
      index_length+= (ulonglong) share->core.keydef[k].nbuckets *
                     (sizeof(fm_i32) + sizeof(FM_SPIN));
    x->data_length= data_length;
    x->index_length= index_length;
  }
  info->errkey= 0;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Auto increment                                                      */
/* ------------------------------------------------------------------ */

/*
   Update the table's auto counter after a row containing an explicit
   auto-increment value was written (port of heap_update_auto_increment).
*/
int fm_update_auto_increment(FM_INFO *info, const uchar *record)
{
  FM_SHARE *share= info->s;
  ulonglong value= 0;
  longlong s_value= 0;
  FM_KEYDEF *keydef;
  const uchar *key;

  if (!share->core.auto_key)
    return 0;
  keydef= &share->keydef[share->core.auto_key - 1];
  key= record + keydef->seg->start;

  switch (share->core.auto_key_type)
  {
  case HA_KEYTYPE_INT8:
    s_value= (longlong) *(const signed char *) key;
    break;
  case HA_KEYTYPE_BINARY:
    value= (ulonglong) *(uchar *) key;
    break;
  case HA_KEYTYPE_SHORT_INT:
    s_value= (longlong) sint2korr(key);
    break;
  case HA_KEYTYPE_USHORT_INT:
    value= (ulonglong) uint2korr(key);
    break;
  case HA_KEYTYPE_LONG_INT:
    s_value= (longlong) sint4korr(key);
    break;
  case HA_KEYTYPE_ULONG_INT:
    value= (ulonglong) uint4korr(key);
    break;
  case HA_KEYTYPE_INT24:
    s_value= (longlong) sint3korr(key);
    break;
  case HA_KEYTYPE_UINT24:
    value= (ulonglong) uint3korr(key);
    break;
  case HA_KEYTYPE_FLOAT:
  {
    float f_1;
    float4get(f_1, key);
    value= (f_1 < (float) 0.0) ? 0 : (ulonglong) f_1;
    break;
  }
  case HA_KEYTYPE_DOUBLE:
  {
    double f_1;
    float8get(f_1, key);
    value= (f_1 < 0.0) ? 0 : (ulonglong) f_1;
    break;
  }
  case HA_KEYTYPE_LONGLONG:
    s_value= sint8korr(key);
    break;
  case HA_KEYTYPE_ULONGLONG:
    value= uint8korr(key);
    break;
  default:
    value= 0;
    break;
  }
  fm_core_auto_inc_observe(&share->core,
                           (s_value > 0) ? (ulonglong) s_value : value);
  return 0;
}

/*
   Reserve the next auto increment value(s).  This is the lock-free
   replacement for heap's "there is only one writer anyway" shortcut:
   concurrent statements get disjoint intervals from the shared counter.
*/
ulonglong fm_get_auto_increment(FM_INFO *info, ulonglong offset,
                                ulonglong increment,
                                ulonglong nb_desired_values,
                                ulonglong *nb_reserved_values)
{
  fm_u64 reserved= 0;
  fm_u64 first= fm_core_auto_inc_reserve(&info->s->core, offset,
                                         increment, nb_desired_values,
                                         &reserved);
  if (!first)
  {
    /* Overflow: report an error to the caller (mapped to
       HA_ERR_AUTOINC_READ_FAILED by the handler). */
    *nb_reserved_values= 0;
    return ~(ulonglong) 0;
  }
  *nb_reserved_values= reserved;
  return first;
}

void fm_set_auto_increment(FM_SHARE *share, ulonglong value)
{
  /* value is the next value to use; the counter stores last handed out */
  fm_u64 next= value ? value - 1 : 0;
  fm_u64 cur= share->core.auto_inc.load(std::memory_order_relaxed);
  while (next > cur &&
         !share->core.auto_inc.compare_exchange_weak(
             cur, next, std::memory_order_acq_rel,
             std::memory_order_acquire))
    ;
}