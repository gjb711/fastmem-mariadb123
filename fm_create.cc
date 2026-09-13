/*
   FASTMEM storage engine - share lifecycle
   ========================================

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   Registry and lifecycle of FM_SHARE structures.  The logic mirrors the
   MEMORY engine (storage/heap/hp_create.c, hp_open.c, hp_close.c,
   hp_rename.c, hp_clear.c) minus everything that belongs to the table
   lock: FASTMEM shares have no THR_LOCK and no lock data at all.

   The single registry mutex THR_LOCK_fastmem protects the share list,
   open counts and delete_on_close flags - it is NEVER taken on any row
   data path.
*/

#define MYSQL_SERVER 1
#include "fm_def.h"
#include <my_sys.h>
#include <my_bit.h>
#include <my_byteorder.h>
#include <m_ctype.h>
#include <sql_priv.h>
#include <field.h>
#include <table.h>
#include <sql_class.h>
#include <mysql/plugin.h>
#include <time.h>

static FM_SHARE *fm_share_list= 0;

extern "C"
{
  mysql_mutex_t THR_LOCK_fastmem;
}

/* Memory allocators handed to the concurrency core. */
static void *fm_core_malloc(size_t n)
{
  return my_malloc(PSI_NOT_INSTRUMENTED, n, MYF(MY_WME));
}
static void fm_core_free(void *p)
{
  my_free(p);
}
static FM_SHARE_CORE_OPS fm_core_ops= { fm_core_malloc, fm_core_free };

/* ------------------------------------------------------------------ */
/* Registry helpers                                                    */
/* ------------------------------------------------------------------ */

FM_SHARE *fm_find_named_share(const char *name)
{
  FM_SHARE *share;
  for (share= fm_share_list; share; share= share->share_next)
  {
    if (!strcmp(name, share->name))
      return share;
  }
  return 0;
}

static void fm_share_list_add(FM_SHARE *share)
{
  share->share_next= fm_share_list;
  fm_share_list= share;
}

static void fm_share_list_remove(FM_SHARE *share)
{
  FM_SHARE **link= &fm_share_list;
  while (*link && *link != share)
    link= &(*link)->share_next;
  if (*link)
    *link= share->share_next;
  share->share_next= 0;
}

/* ------------------------------------------------------------------ */
/* Key definition building (from the SQL TABLE object)                 */
/* ------------------------------------------------------------------ */

/*
   Build the FM_CREATE_INFO used both by create() and open() of a table
   that has no live FASTMEM share yet.  Only HASH indexed keys are
   accepted; any BTREE key is rejected (FASTMEM is single-algorithm).

   Returns 0 on success, or a handler error code.
*/
int fm_prepare_create_info(TABLE *table_arg, bool internal_table,
                           FM_CREATE_INFO *fm_create_info)
{
  TABLE_SHARE *share= table_arg->s;
  uint key, parts, mem_per_row= 0, keys= share->keys;
  uint auto_key= 0, auto_key_type= 0;
  ulong max_records;
  ulonglong max_table_size;
  FM_KEYDEF *keydef;
  HA_KEYSEG *seg;
  bool found_real_auto_increment= false;

  bzero(fm_create_info, sizeof(*fm_create_info));

  if (share->total_keys > keys)
  {
    my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0), "FASTMEM", "VECTOR");
    return HA_ERR_UNSUPPORTED;
  }

  for (key= parts= 0; key < keys; key++)
    parts+= table_arg->key_info[key].user_defined_key_parts;

  /* One buffer: keydef array followed by all key segments (the caller
     frees it with a single my_free(create_info->keydef)). */
  if (!(keydef= (FM_KEYDEF *) my_malloc(PSI_NOT_INSTRUMENTED,
                                        keys * sizeof(FM_KEYDEF) +
                                        parts * sizeof(HA_KEYSEG),
                                        MYF(MY_WME))))
    return my_errno;
  seg= (HA_KEYSEG *) (keydef + keys);

  for (key= 0; key < keys; key++)
  {
    KEY *pos= table_arg->key_info + key;
    KEY_PART_INFO *key_part= pos->key_part;
    KEY_PART_INFO *key_part_end= key_part + pos->user_defined_key_parts;

    switch (pos->algorithm)
    {
    case HA_KEY_ALG_UNDEF:
    case HA_KEY_ALG_HASH:
      keydef[key].algorithm= HA_KEY_ALG_HASH;
      break;
    case HA_KEY_ALG_BTREE:
      my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0), "FASTMEM", "BTREE");
      my_free(keydef);
      my_free(seg);
      return HA_ERR_UNSUPPORTED;
    default:
      DBUG_ASSERT(0);
      break;
    }

    keydef[key].keysegs= (uint) pos->user_defined_key_parts;
    keydef[key].flag= (pos->flags & (HA_NOSAME | HA_NULL_ARE_EQUAL));
    keydef[key].seg= seg;
    seg+= keydef[key].keysegs;

    for (; key_part != key_part_end; key_part++)
    {
      Field *field= key_part->field;
      HA_KEYSEG *s= key_part - pos->key_part + keydef[key].seg;

      if ((s->type= field->key_type()) != (int) HA_KEYTYPE_TEXT &&
          s->type != HA_KEYTYPE_VARTEXT1 &&
          s->type != HA_KEYTYPE_VARTEXT2 &&
          s->type != HA_KEYTYPE_VARBINARY1 &&
          s->type != HA_KEYTYPE_VARBINARY2 &&
          s->type != HA_KEYTYPE_BIT)
        s->type= HA_KEYTYPE_BINARY;
      s->start= (uint) key_part->offset;
      s->length= (uint) key_part->length;
      s->flag= key_part->key_part_flag;

      if (field->flags & (ENUM_FLAG | SET_FLAG))
        s->charset= &my_charset_bin;
      else
        s->charset= field->charset_for_protocol();

      if (field->null_ptr)
      {
        s->null_bit= field->null_bit;
        s->null_pos= (uint) (field->null_ptr - (uchar*) table_arg->record[0]);
      }
      else
      {
        s->null_bit= 0;
        s->null_pos= 0;
      }

      if (field->flags & AUTO_INCREMENT_FLAG &&
          table_arg->found_next_number_field &&
          key == share->next_number_index)
      {
        auto_key= key + 1;
        auto_key_type= field->key_type();
      }

      if (s->type == HA_KEYTYPE_BIT)
      {
        s->bit_length= ((Field_bit *) field)->bit_len;
        s->bit_start= ((Field_bit *) field)->bit_ofs;
        s->bit_pos= (uint) (((Field_bit *) field)->bit_ptr -
                            (uchar*) table_arg->record[0]);
      }
      else
      {
        s->bit_length= 0;
        s->bit_start= 0;
        s->bit_pos= 0;
      }
    }
  }

  if (table_arg->found_next_number_field)
  {
    keydef[share->next_number_index].flag|= HA_AUTO_KEY;
    found_real_auto_increment= share->next_number_key_offset == 0;
  }

  max_table_size= MY_MAX(current_thd->variables.max_heap_table_size,
                         (ulonglong) sizeof(char *));

  fm_create_info->auto_key= auto_key;
  fm_create_info->auto_key_type= auto_key_type;
  fm_create_info->max_table_size= max_table_size;
  fm_create_info->with_auto_increment= found_real_auto_increment;
  fm_create_info->internal_table= internal_table;
  fm_create_info->keydef= keydef;
  fm_create_info->keys= keys;
  fm_create_info->reclength= share->reclength;

  /*
     Estimate how many rows fit into max_table_size: each row occupies a
     slot plus one 32-bit chain cell per key, plus hash bucket space.
  */
  {
    size_t slot_size= fm_align8(FM_SLOT_DATA_OFFSET + share->reclength);
    size_t mem_per_row= slot_size + keys * 4 + 16;
    max_records= (ulong) (max_table_size /
                          (mem_per_row ? mem_per_row : 1));
    if (share->max_rows && (ha_rows) max_records > share->max_rows)
      max_records= (ulong) share->max_rows;
    if (!max_records)
      max_records= 1;
  }
  fm_create_info->max_records= max_records;

  /* Longest packed key over all keys (= info->lastkey buffer size). */
  {
    uint max_key_length= 0;
    for (key= 0; key < keys; key++)
    {
      uint length= 0;
      for (uint j= 0; j < keydef[key].keysegs; j++)
      {
        HA_KEYSEG *s= &keydef[key].seg[j];
        length+= s->length;
        if (s->null_bit)
          length++;                      /* null marker byte           */
        switch (s->type)
        {
        case HA_KEYTYPE_VARBINARY1:
          s->type= HA_KEYTYPE_VARTEXT1;
          /* fall through */
        case HA_KEYTYPE_VARTEXT1:
          length+= 2;                    /* 2-byte packed length        */
          s->bit_start= 1;
          break;
        case HA_KEYTYPE_VARBINARY2:
          /* fall through */
        case HA_KEYTYPE_VARTEXT2:
          s->type= HA_KEYTYPE_VARTEXT1;
          length+= 2;
          s->bit_start= 2;
          break;
        default:
          break;
        }
      }
      keydef[key].length= length;
      if (length > max_key_length)
        max_key_length= length;
    }
    fm_create_info->max_key_length= max_key_length;
  }

  return 0;
}

/* ------------------------------------------------------------------ */
/* Core creation                                                       */
/* ------------------------------------------------------------------ */

/*
   Create (or find) the FASTMEM share for a table.  The caller keeps
   ownership of fm_create_info->keydef and frees it afterwards.

   pin_share: when set, open_count is incremented on behalf of the
   caller (used while the handler is being constructed).
*/
int fm_create(const char *name, const FM_CREATE_INFO *create_info,
              bool pin_share, FM_SHARE **res, bool *created_new_share)
{
  uint key;
  FM_SHARE *share= 0;
  FM_KEYDEF *keydef= create_info->keydef;
  uint reclength= create_info->reclength;
  uint keys= create_info->keys;
  ulong max_records= create_info->max_records;
  FM_KEYDEF *keyinfo;
  HA_KEYSEG *keyseg;
  ulong nbuckets;

  if (!create_info->internal_table)
  {
    mysql_mutex_lock(&THR_LOCK_fastmem);
    share= fm_find_named_share(name);
    if (share && share->open_count == 0)
    {
      fm_free(share);
      share= 0;
    }
  }

  *created_new_share= (share == NULL);

  if (!share)
  {
    uint key_segs= 0;
    uint max_key_length= create_info->max_key_length;

    /* Number of buckets = at least max_records / 4, power of two. */
    nbuckets= 1024;
    while (nbuckets < (ulong) (max_records / 4) && nbuckets < (1UL << 20))
      nbuckets<<= 1;

    for (key= 0; key < keys; key++)
      key_segs+= keydef[key].keysegs;

    /* One allocation: FM_SHARE + keydef array + all HA_KEYSEGs. */
    if (!(share= (FM_SHARE *) my_malloc(PSI_NOT_INSTRUMENTED,
                                        sizeof(FM_SHARE) +
                                        keys * sizeof(FM_KEYDEF) +
                                        key_segs * sizeof(HA_KEYSEG),
                                        MYF(MY_ZEROFILL |
                                            (create_info->internal_table ?
                                             MY_THREAD_SPECIFIC : 0)))))
      goto err;

    share->keydef= (FM_KEYDEF *) (share + 1);
    memcpy(share->keydef, keydef, sizeof(keydef[0]) * keys);
    keyseg= (HA_KEYSEG *) ((uchar *) (share->keydef + keys));
    for (key= 0, keyinfo= share->keydef; key < keys; key++, keyinfo++)
    {
      keyinfo->seg= keyseg;
      memcpy(keyseg, keydef[key].seg,
             sizeof(keyseg[0]) * keydef[key].keysegs);
      keyseg+= keydef[key].keysegs;
      key_segs++;
    }

    /* Initialize the concurrency core. */
    fm_core_init_share(&share->core, reclength, keys, max_records,
                       nbuckets, &fm_core_ops);
    if (!share->core.chunks)
      goto err;
    share->core.auto_key= create_info->auto_key;
    share->core.auto_key_type= create_info->auto_key_type;
    share->core.auto_inc.store(create_info->auto_increment,
                               std::memory_order_relaxed);

    share->max_key_length= max_key_length;
    share->create_time= (time_t) time((time_t*) 0);
    share->internal= create_info->internal_table;
    share->delete_on_close= create_info->internal_table;

    /* Must be allocated separately for rename to work. */
    if (!(share->name= my_strdup(PSI_NOT_INSTRUMENTED, name, MYF(0))))
      goto err;

    if (!create_info->internal_table)
      fm_share_list_add(share);
  }
  if (!create_info->internal_table)
  {
    if (pin_share)
      ++share->open_count;
    mysql_mutex_unlock(&THR_LOCK_fastmem);
  }

  *res= share;
  return 0;

err:
  if (!create_info->internal_table)
    mysql_mutex_unlock(&THR_LOCK_fastmem);
  if (share)
  {
    if (share->name)
      my_free(share->name);
    fm_core_free_share(&share->core);
    my_free(share);
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */

/* Open a share that is already registered; keep the registry mutex state
   of the caller (fm_open holds it, fm_open_from_share_and_register too). */
static FM_INFO *fm_open_from_share_locked(FM_SHARE *share, int mode)
{
  FM_INFO *info= (FM_INFO *) my_malloc(PSI_NOT_INSTRUMENTED,
                                       sizeof(FM_INFO) +
                                       share->max_key_length +
                                       share->core.reclength,
                                       MYF(MY_ZEROFILL |
                                           (share->internal ?
                                            MY_THREAD_SPECIFIC : 0)));
  if (!info)
    return 0;
  share->open_count++;
  info->s= share;
  info->lastkey= (uchar *) (info + 1);
  info->scratch= info->lastkey + share->max_key_length;
  info->mode= mode;
  info->current_slot= -1;
  info->current_hash_slot= -1;
  info->lastinx= -1;
  info->errkey= -1;
#ifndef DBUG_OFF
  info->opt_flag= READ_CHECK_USED;   /* verify records on update */
#endif
  info->open_listed= false;
  info->next_open= 0;
  return info;
}

FM_INFO *fm_open_from_share(FM_SHARE *share, int mode)
{
  return fm_open_from_share_locked(share, mode);
}

FM_INFO *fm_open_from_share_and_register(FM_SHARE *share, int mode)
{
  FM_INFO *info;
  mysql_mutex_lock(&THR_LOCK_fastmem);
  if ((info= fm_open_from_share_locked(share, mode)))
  {
    info->next_open= share->open_list;
    share->open_list= info;
    info->open_listed= true;
    /* Unpin the share, it is now pinned by the file. */
    share->open_count--;
  }
  mysql_mutex_unlock(&THR_LOCK_fastmem);
  return info;
}

/* Open by name; the share must exist (handler creates it on ENOENT). */
FM_INFO *fm_open(const char *name, int mode)
{
  FM_INFO *info= 0;
  FM_SHARE *share;
  mysql_mutex_lock(&THR_LOCK_fastmem);
  if (!(share= fm_find_named_share(name)))
  {
    my_errno= ENOENT;
    mysql_mutex_unlock(&THR_LOCK_fastmem);
    return 0;
  }
  if ((info= fm_open_from_share_locked(share, mode)))
  {
    info->next_open= share->open_list;
    share->open_list= info;
    info->open_listed= true;
  }
  mysql_mutex_unlock(&THR_LOCK_fastmem);
  return info;
}

/*
   Unregister an open handle.  Frees the share when it was marked
   delete_on_close and this was the last open handle.
 */
int fm_close(FM_INFO *info)
{
  int error= 0;
  FM_SHARE *share= info->s;
  mysql_mutex_lock(&THR_LOCK_fastmem);
  if (info->open_listed)
  {
    FM_INFO **link= &share->open_list;
    while (*link && *link != info)
      link= &(*link)->next_open;
    if (*link)
      *link= info->next_open;
    info->open_listed= false;
  }
  if (!--share->open_count && share->delete_on_close)
    fm_free(share);                     /* Table was deleted */
  my_free(info);
  mysql_mutex_unlock(&THR_LOCK_fastmem);
  return error;
}

/* Dereference a share and free it when unreferenced. */
void fm_release_share(FM_SHARE *share, bool internal_table)
{
  if (internal_table)
    fm_free(share);
  else
  {
    mysql_mutex_lock(&THR_LOCK_fastmem);
    if (--share->open_count == 0)
      fm_free(share);
    mysql_mutex_unlock(&THR_LOCK_fastmem);
  }
}

/* ------------------------------------------------------------------ */
/* Drop / rename / free                                                */
/* ------------------------------------------------------------------ */

static void fm_try_free(FM_SHARE *share)
{
  if (share->open_count == 0)
    fm_free(share);
  else
    share->delete_on_close= 1;      /* free when the last handle closes */
}

int fm_delete_table(const char *name)
{
  int result;
  FM_SHARE *share;
  mysql_mutex_lock(&THR_LOCK_fastmem);
  if ((share= fm_find_named_share(name)))
  {
    fm_try_free(share);
    result= 0;
  }
  else
  {
    result= my_errno= ENOENT;
  }
  mysql_mutex_unlock(&THR_LOCK_fastmem);
  return result;
}

void fm_drop_table(FM_INFO *info)
{
  mysql_mutex_lock(&THR_LOCK_fastmem);
  fm_try_free(info->s);
  mysql_mutex_unlock(&THR_LOCK_fastmem);
}

int fm_rename(const char *old_name, const char *new_name)
{
  FM_SHARE *info;
  char *name_buff;
  mysql_mutex_lock(&THR_LOCK_fastmem);
  if ((info= fm_find_named_share(old_name)))
  {
    if (!(name_buff= my_strdup(PSI_NOT_INSTRUMENTED, new_name,
                             MYF(MY_WME))))
    {
      mysql_mutex_unlock(&THR_LOCK_fastmem);
      return my_errno;
    }
    my_free(info->name);
    info->name= name_buff;
  }
  mysql_mutex_unlock(&THR_LOCK_fastmem);
  return 0;
}

/* Free all table memory.  Caller must hold THR_LOCK_fastmem when the
   share is a registered (non-internal) one. */
void fm_free(FM_SHARE *share)
{
  if (!share->internal)
    fm_share_list_remove(share);
  fm_core_free_share(&share->core);
  my_free(share->name);
  my_free(share);
}

/* ------------------------------------------------------------------ */
/* Clear (TRUNCATE)                                                     */
/* ------------------------------------------------------------------ */

void fm_clear(FM_INFO *info)
{
  fm_core_clear(&info->s->core);
  info->current_slot= -1;
  info->current_hash_slot= -1;
  info->lastinx= -1;
  info->update= 0;
}