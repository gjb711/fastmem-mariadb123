/*
   FASTMEM storage engine - internal definitions (server side)
   =============================================================

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   NOTE: this file is included only by server-side sources.  The
   concurrency core lives in fm_core.h which has no server dependency.
*/

#ifndef FM_DEF_H_INCLUDED
#define FM_DEF_H_INCLUDED

#include <my_global.h>
#include <my_base.h>
#include <my_pthread.h>
#include "my_compare.h"   /* HA_KEYSEG                            */
#include "fm_core.h"

/* Forward declaration: TABLE is defined in sql/table.h, included only
   by translation units that actually use the table object. */
struct TABLE;

C_MODE_START

/* ------------------------------------------------------------------ */
/* Server-side key definition                                          */
/* ------------------------------------------------------------------ */

typedef struct st_fm_keydef
{
  HA_KEYSEG *seg;                 /* key segments                       */
  uint flag;                      /* HA_NOSAME | HA_NULL_PART_KEY etc   */
  uint keysegs;                   /* number of key segments             */
  uint length;                    /* packed key length                  */
  uint8 algorithm;                /* HA_KEY_ALG_HASH or BTREE (BTREE
                                     rejected at CREATE)                */
} FM_KEYDEF;

/* ------------------------------------------------------------------ */
/* Share (one per table, shared by all open handles)                   */
/* ------------------------------------------------------------------ */

typedef struct st_fm_share
{
  char *name;                     /* table name (path)                  */
  FM_SHARE_CORE core;             /* concurrency core                   */
  FM_KEYDEF *keydef;              /* [keys] or NULL                     */
  uint max_key_length;            /* longest packed key (incl null)     */
  uint open_count;                /* open handles (registry mutex)      */
  bool delete_on_close;
  bool internal;                  /* thread-local temporary table       */
  time_t create_time;
  struct st_fm_share *share_next; /* registry list                      */
  struct st_fm_info *open_list;   /* open handles of this share         */
} FM_SHARE;

/* ------------------------------------------------------------------ */
/* Open handle (one per TABLE object)                                  */
/* ------------------------------------------------------------------ */

/* Position reference: slot index + generation, exactly 8 bytes.       */
#define FM_REF_LENGTH 8

typedef struct st_fm_slot_ref
{
  fm_i32 slot;
  fm_u32 gen;
} FM_SLOT_REF;

typedef struct st_fm_info
{
  FM_SHARE *s;
  fm_i32 current_slot;            /* -1 = no active record              */
  fm_u32 current_gen;
  fm_i32 current_hash_slot;       /* position inside a key chain        */
  int lastinx;                    /* last used key, -1 = none           */
  int errkey;
  uint update;                    /* HA_STATE_* flags                   */
  uint opt_flag;                  /* READ_CHECK_USED etc                */
  uint mode;
  fm_u32 scan_pos;                /* scan cursor                        */
  uint lastkey_len;
  uchar *lastkey;                 /* packed key buffer (max_key_length) */
  uchar *scratch;                 /* record image scratch (reclength)   */
  bool open_listed;
  struct st_fm_info *next_open;
} FM_INFO;

/* ------------------------------------------------------------------ */
/* Status info returned by fm_info()                                   */
/* ------------------------------------------------------------------ */

typedef struct st_fminfo
{
  ha_rows records;
  ha_rows deleted;
  ha_rows max_records;
  ulonglong data_length;
  ulonglong index_length;
  uint reclength;
  int errkey;
  ulonglong auto_increment;
  time_t create_time;
} FMINFO;

/* ------------------------------------------------------------------ */
/* Create-time information (built from the SQL TABLE object)           */
/* ------------------------------------------------------------------ */

typedef struct st_fm_create_info
{
  FM_KEYDEF *keydef;              /* temporary array, freed by caller   */
  uint keys;
  uint reclength;
  uint max_key_length;
  ulong max_records;
  ulonglong max_table_size;
  ulonglong auto_increment;       /* "last handed out" value            */
  uint auto_key;                  /* 1-based index of the auto key, or 0 */
  uint auto_key_type;
  bool with_auto_increment;
  bool internal_table;
  bool pin_share;
} FM_CREATE_INFO;

/* ------------------------------------------------------------------ */
/* Internal API                                                        */
/* ------------------------------------------------------------------ */

extern mysql_mutex_t THR_LOCK_fastmem;

extern int  fm_prepare_create_info(TABLE *table_arg, bool internal_table,
                                   FM_CREATE_INFO *fm_create_info);
extern FM_SHARE *fm_find_named_share(const char *name);
extern FM_INFO  *fm_open(const char *name, int mode);
extern FM_INFO  *fm_open_from_share(FM_SHARE *share, int mode);
extern FM_INFO  *fm_open_from_share_and_register(FM_SHARE *share,
                                                 int mode);
extern void fm_release_share(FM_SHARE *share, bool internal_table);
extern int  fm_close(FM_INFO *info);
extern int  fm_create(const char *name, const FM_CREATE_INFO *create_info,
                      bool pin_share, FM_SHARE **res, bool *created_new_share);
extern int  fm_delete_table(const char *name);
extern void fm_drop_table(FM_INFO *info);
extern int  fm_rename(const char *old_name, const char *new_name);
extern void fm_free(FM_SHARE *share);

/* data */
extern int  fm_write(FM_INFO *info, const uchar *record);
extern int  fm_update(FM_INFO *info, const uchar *old, const uchar *newdata);
extern int  fm_delete(FM_INFO *info, const uchar *buf);
/* Row already locked variants: the caller holds the slot wlock for the
   whole read-modify-write cycle (see ha_fastmem write_stmt locking).
   fm_update_locked()/fm_delete_locked() keep the wlock owned by the
   caller - the caller must release it after the call. */
extern int  fm_update_locked(FM_INFO *info, const uchar *old,
                             const uchar *newdata);
extern int  fm_delete_locked(FM_INFO *info, const uchar *buf);
/* Re-read the current row under the already-held slot wlock so the
   read-modify-write cycle starts from a fresh image. */
extern int  fm_reread_locked(FM_INFO *info, uchar *record);
extern int  fm_rrnd(FM_INFO *info, uchar *record, const uchar *pos);
extern int  fm_scan_init(FM_INFO *info);
extern int  fm_scan(FM_INFO *info, uchar *record);
extern int  fm_info(FM_INFO *info, FMINFO *x, int flag);
extern int  fm_extra(FM_INFO *info, enum ha_extra_function function);
extern int  fm_reset(FM_INFO *info);
extern void fm_clear(FM_INFO *info);
extern void fm_set_position(FM_INFO *info, uchar *ref);
extern bool fm_read_from_ref(FM_INFO *info, uchar *record,
                             const uchar *ref, bool *was_deleted);
extern int  fm_find_key(FM_INFO *info, int inx, const uchar *key,
                        uchar *record);
extern int  fm_find_next_key(FM_INFO *info, int inx, const uchar *key,
                             fm_i32 cur_slot, uchar *record);
extern int  fm_update_auto_increment(FM_INFO *info, const uchar *record);
extern ulonglong fm_get_auto_increment(FM_INFO *info, ulonglong offset,
                                       ulonglong increment,
                                       ulonglong nb_desired_values,
                                       ulonglong *nb_reserved_values);
extern void fm_set_auto_increment(FM_SHARE *share, ulonglong value);

/* hash (fm_hash.cc) - key callbacks + packing helpers */
extern void fm_key_ops_init(FM_SHARE *share);
extern void fm_make_key(const FM_KEYDEF *keydef, uchar *key,
                        const uchar *rec);

C_MODE_END

#endif /* FM_DEF_H_INCLUDED */