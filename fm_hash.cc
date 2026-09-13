/*
   FASTMEM storage engine - hash key functions
   ============================================

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   The key hashing/comparison routines are semantic ports of the MEMORY
   engine functions in storage/heap/hp_hash.c of MariaDB 12.1.1 (which no
   longer uses the my_hasher abstraction), operating on the same HA_KEYSEG
   descriptor layout, so that key images produced by the SQL layer
   (mysql_pack_key) hash and compare identically.

   The functions here are pure: they never touch shared state and are
   safe to call concurrently from any thread.
*/

#include "fm_def.h"
#include <m_ctype.h>

static inline size_t
fm_charpos(CHARSET_INFO *cs, const uchar *b, const uchar *e, size_t num)
{
  return my_ci_charpos(cs, (const char*) b, (const char *) e, num);
}

/*
   Hash a *packed* key image (as built by the SQL layer / fm_make_key).
*/
static ulong fm_hashnr(const FM_KEYDEF *keydef, const uchar *key)
{
  my_hasher_st hasher= my_hasher_mysql5x();
  HA_KEYSEG *seg, *endseg;

  for (seg= keydef->seg, endseg= seg + keydef->keysegs; seg < endseg; seg++)
  {
    uchar *pos= (uchar*) key;
    key+= seg->length;
    if (seg->null_bit)
    {
      key++;                                    /* Skip null byte */
      if (*pos)                                 /* Found null */
      {
        hasher.m_nr1 ^= (hasher.m_nr1 << 1) | 1;
        /* Add key pack length (2) to key for VARCHAR segments */
        if (seg->type == HA_KEYTYPE_VARTEXT1)
          key+= 2;
        continue;
      }
      pos++;
    }
    if (seg->type == HA_KEYTYPE_TEXT)
    {
      CHARSET_INFO *cs= seg->charset;
      size_t length= seg->length;
      if (cs->mbmaxlen > 1)
      {
        size_t char_length;
        char_length= fm_charpos(cs, pos, pos + length, length/cs->mbmaxlen);
        set_if_smaller(length, char_length);
      }
      my_ci_hash_sort(&hasher, cs, pos, length);
    }
    else if (seg->type == HA_KEYTYPE_VARTEXT1)   /* Any VARCHAR segments */
    {
      CHARSET_INFO *cs= seg->charset;
      size_t pack_length= 2;             /* key packing is constant */
      size_t length= uint2korr(pos);
      if (cs->mbmaxlen > 1)
      {
        size_t char_length;
        char_length= fm_charpos(cs, pos +pack_length,
                                pos +pack_length + length,
                                seg->length/cs->mbmaxlen);
        set_if_smaller(length, char_length);
      }
      my_ci_hash_sort(&hasher, cs, pos+pack_length, length);
      key+= pack_length;
    }
    else
    {
      for (; pos < (uchar*) key ; pos++)
      {
        MY_HASH_ADD_MARIADB(hasher.m_nr1, hasher.m_nr2, *pos);
      }
    }
  }
  return((ulong) hasher.m_nr1);
}

/* Hash a key stored in a record image. */
static ulong fm_rec_hashnr(const FM_KEYDEF *keydef, const uchar *rec)
{
  my_hasher_st hasher= my_hasher_mysql5x();
  HA_KEYSEG *seg, *endseg;

  for (seg= keydef->seg, endseg= seg + keydef->keysegs; seg < endseg; seg++)
  {
    uchar *pos= (uchar*) rec+seg->start, *end= pos+seg->length;
    if (seg->null_bit)
    {
      if (rec[seg->null_pos] & seg->null_bit)
      {
        hasher.m_nr1 ^= (hasher.m_nr1 << 1) | 1;
        continue;
      }
    }
    if (seg->type == HA_KEYTYPE_TEXT)
    {
      CHARSET_INFO *cs= seg->charset;
      size_t char_length= seg->length;
      if (cs->mbmaxlen > 1)
      {
        char_length= fm_charpos(cs, pos, pos + char_length,
                                char_length / cs->mbmaxlen);
        set_if_smaller(char_length, seg->length);
      }
      my_ci_hash_sort(&hasher, cs, pos, char_length);
    }
    else if (seg->type == HA_KEYTYPE_VARTEXT1)   /* Any VARCHAR segments */
    {
      CHARSET_INFO *cs= seg->charset;
      size_t pack_length= seg->bit_start;
      size_t length= (pack_length == 1 ? (size_t) *(uchar*) pos
                                       : uint2korr(pos));
      if (cs->mbmaxlen > 1)
      {
        size_t char_length;
        char_length= fm_charpos(cs, pos + pack_length,
                                pos + pack_length + length,
                                seg->length/cs->mbmaxlen);
        set_if_smaller(length, char_length);
      }
      else
        set_if_smaller(length, seg->length);
      my_ci_hash_sort(&hasher, cs, pos+pack_length, length);
    }
    else
    {
      if (seg->type == HA_KEYTYPE_BIT && seg->bit_length)
      {
        uchar bits= get_rec_bits(rec + seg->bit_pos,
                                 seg->bit_start, seg->bit_length);
        MY_HASH_ADD_MARIADB(hasher.m_nr1, hasher.m_nr2, bits);
        end--;
      }
      for (; pos < end ; pos++)
      {
        MY_HASH_ADD_MARIADB(hasher.m_nr1, hasher.m_nr2, *pos);
      }
    }
  }
  return((ulong) hasher.m_nr1);
}

/*
   Compare keys of two record images.  Returns 0 if they are identical.
*/
static int fm_rec_key_cmp(const FM_KEYDEF *keydef, const uchar *rec1,
                          const uchar *rec2)
{
  HA_KEYSEG *seg, *endseg;

  for (seg= keydef->seg, endseg= seg + keydef->keysegs; seg < endseg; seg++)
  {
    if (seg->null_bit)
    {
      if ((rec1[seg->null_pos] & seg->null_bit) !=
          (rec2[seg->null_pos] & seg->null_bit))
        return 1;
      if (rec1[seg->null_pos] & seg->null_bit)
        continue;
    }
    if (seg->type == HA_KEYTYPE_TEXT)
    {
      CHARSET_INFO *cs= seg->charset;
      size_t char_length1;
      size_t char_length2;
      uchar *pos1= (uchar*)rec1 + seg->start;
      uchar *pos2= (uchar*)rec2 + seg->start;
      if (cs->mbmaxlen > 1)
      {
        size_t char_length= seg->length / cs->mbmaxlen;
        char_length1= fm_charpos(cs, pos1, pos1 + seg->length, char_length);
        set_if_smaller(char_length1, seg->length);
        char_length2= fm_charpos(cs, pos2, pos2 + seg->length, char_length);
        set_if_smaller(char_length2, seg->length);
      }
      else
        char_length1= char_length2= seg->length;
      if (my_ci_strnncollsp(seg->charset,
                            pos1, char_length1,
                            pos2, char_length2))
        return 1;
    }
    else if (seg->type == HA_KEYTYPE_VARTEXT1)   /* Any VARCHAR segments */
    {
      uchar *pos1= (uchar*) rec1 + seg->start;
      uchar *pos2= (uchar*) rec2 + seg->start;
      size_t char_length1, char_length2;
      size_t pack_length= seg->bit_start;
      CHARSET_INFO *cs= seg->charset;
      if (pack_length == 1)
      {
        char_length1= (size_t) *(uchar*) pos1++;
        char_length2= (size_t) *(uchar*) pos2++;
      }
      else
      {
        char_length1= uint2korr(pos1);
        char_length2= uint2korr(pos2);
        pos1+= 2;
        pos2+= 2;
      }
      if (cs->mbmaxlen > 1)
      {
        size_t safe_length1= char_length1;
        size_t safe_length2= char_length2;
        size_t char_length= seg->length / cs->mbmaxlen;
        char_length1= fm_charpos(cs, pos1, pos1 + char_length1, char_length);
        set_if_smaller(char_length1, safe_length1);
        char_length2= fm_charpos(cs, pos2, pos2 + char_length2, char_length);
        set_if_smaller(char_length2, safe_length2);
      }
      else
      {
        set_if_smaller(char_length1, seg->length);
        set_if_smaller(char_length2, seg->length);
      }
      if (my_ci_strnncollsp(seg->charset,
                            pos1, char_length1,
                            pos2, char_length2))
        return 1;
    }
    else
    {
      uint dec= 0;
      if (seg->type == HA_KEYTYPE_BIT && seg->bit_length)
      {
        uchar bits1= get_rec_bits(rec1 + seg->bit_pos,
                                  seg->bit_start, seg->bit_length);
        uchar bits2= get_rec_bits(rec2 + seg->bit_pos,
                                  seg->bit_start, seg->bit_length);
        if (bits1 != bits2)
          return 1;
        dec= 1;
      }
      if (bcmp(rec1 + seg->start, rec2 + seg->start, seg->length - dec))
        return 1;
    }
  }
  return 0;
}

/* Compare a record image to a *packed* key.  Returns 0 if identical. */
static int fm_key_cmp(const FM_KEYDEF *keydef, const uchar *rec,
                      const uchar *key)
{
  HA_KEYSEG *seg, *endseg;

  for (seg= keydef->seg, endseg= seg + keydef->keysegs;
       seg < endseg;
       key+= (seg++)->length)
  {
    if (seg->null_bit)
    {
      int found_null= MY_TEST(rec[seg->null_pos] & seg->null_bit);
      if (found_null != (int) *key++)
        return 1;
      if (found_null)
      {
        /* Add key pack length (2) to key for VARCHAR segments */
        if (seg->type == HA_KEYTYPE_VARTEXT1)
          key+= 2;
        continue;
      }
    }
    if (seg->type == HA_KEYTYPE_TEXT)
    {
      CHARSET_INFO *cs= seg->charset;
      size_t char_length_key;
      size_t char_length_rec;
      uchar *pos= (uchar*) rec + seg->start;
      if (cs->mbmaxlen > 1)
      {
        size_t char_length= seg->length / cs->mbmaxlen;
        char_length_key= fm_charpos(cs, key, key + seg->length, char_length);
        set_if_smaller(char_length_key, seg->length);
        char_length_rec= fm_charpos(cs, pos, pos + seg->length, char_length);
        set_if_smaller(char_length_rec, seg->length);
      }
      else
      {
        char_length_key= seg->length;
        char_length_rec= seg->length;
      }
      if (my_ci_strnncollsp(seg->charset,
                            pos, char_length_rec,
                            key, char_length_key))
        return 1;
    }
    else if (seg->type == HA_KEYTYPE_VARTEXT1)   /* Any VARCHAR segments */
    {
      uchar *pos= (uchar*) rec + seg->start;
      CHARSET_INFO *cs= seg->charset;
      size_t pack_length= seg->bit_start;
      size_t char_length_rec= (pack_length == 1 ? (size_t) *(uchar*) pos
                                                : uint2korr(pos));
      /* Key segments are always packed with 2 bytes */
      size_t char_length_key= uint2korr(key);
      pos+= pack_length;
      key+= 2;                                  /* skip key pack length */
      if (cs->mbmaxlen > 1)
      {
        size_t char_length1, char_length2;
        char_length1= char_length2= seg->length / cs->mbmaxlen;
        char_length1= fm_charpos(cs, key, key + char_length_key, char_length1);
        set_if_smaller(char_length_key, char_length1);
        char_length2= fm_charpos(cs, pos, pos + char_length_rec, char_length2);
        set_if_smaller(char_length_rec, char_length2);
      }
      else
        set_if_smaller(char_length_rec, seg->length);

      if (my_ci_strnncollsp(seg->charset,
                            pos, char_length_rec,
                            key, char_length_key))
        return 1;
    }
    else
    {
      uint dec= 0;
      if (seg->type == HA_KEYTYPE_BIT && seg->bit_length)
      {
        uchar bits= get_rec_bits(rec + seg->bit_pos,
                                 seg->bit_start, seg->bit_length);
        if (bits != (*key))
          return 1;
        dec= 1;
        key++;
      }
      if (bcmp(rec + seg->start, key, seg->length - dec))
        return 1;
    }
  }
  return 0;
}

static int fm_if_null_in_key(const FM_KEYDEF *keydef, const uchar *record)
{
  HA_KEYSEG *seg, *endseg;
  for (seg= keydef->seg, endseg= seg + keydef->keysegs; seg < endseg; seg++)
  {
    if (seg->null_bit && (record[seg->null_pos] & seg->null_bit))
      return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Core callbacks (ud = server-side FM_KEYDEF)                         */
/* ------------------------------------------------------------------ */

extern "C" {

static uint32 fm_ops_hash_rec(const void *ud, const fm_u8 *rec)
{
  return (uint32) fm_rec_hashnr((const FM_KEYDEF *) ud, rec);
}

static uint32 fm_ops_hash_key(const void *ud, const fm_u8 *packed_key)
{
  return (uint32) fm_hashnr((const FM_KEYDEF *) ud, packed_key);
}

static int fm_ops_cmp_rec_rec(const void *ud, const fm_u8 *a, const fm_u8 *b)
{
  return fm_rec_key_cmp((const FM_KEYDEF *) ud, a, b);
}

static int fm_ops_cmp_rec_key(const void *ud, const fm_u8 *rec,
                              const fm_u8 *packed_key)
{
  return fm_key_cmp((const FM_KEYDEF *) ud, rec, packed_key);
}

static int fm_ops_has_null_part(const void *ud, const fm_u8 *rec)
{
  return fm_if_null_in_key((const FM_KEYDEF *) ud, rec);
}

}   /* extern "C" */

/*
   Wire the core's per-key ops to the server-side FM_KEYDEF table.
   Call once after the share's keydef array is populated (create/open).
*/
void fm_key_ops_init(FM_SHARE *share)
{
  for (uint k= 0; k < share->core.keys; k++)
  {
    FM_KEY_CORE *kc= &share->core.keydef[k];
    FM_KEYDEF *kd= &share->keydef[k];
    kc->ops.ud= kd;
    kc->unique= (kd->flag & HA_NOSAME) != 0;
    kc->ops.hash_rec= fm_ops_hash_rec;
    kc->ops.hash_key= fm_ops_hash_key;
    kc->ops.cmp_rec_rec= fm_ops_cmp_rec_rec;
    kc->ops.cmp_rec_key= fm_ops_cmp_rec_key;
    kc->ops.has_null_part= fm_ops_has_null_part;
  }
}

/*
   Pack a key from a record image into the SQL-layer packed format
   (port of hp_make_key).  The packed buffers passed to index_read_map
   have exactly this layout, so the hash/comparison functions above see
   identical images.
*/
void fm_make_key(const FM_KEYDEF *keydef, uchar *key, const uchar *rec)
{
  HA_KEYSEG *seg, *endseg;

  for (seg= keydef->seg, endseg= seg + keydef->keysegs; seg < endseg; seg++)
  {
    CHARSET_INFO *cs= seg->charset;
    size_t char_length= seg->length;
    uchar *pos= (uchar*) rec + seg->start;
    if (seg->null_bit)
      *key++= MY_TEST(rec[seg->null_pos] & seg->null_bit);
    if (cs->mbmaxlen > 1)
    {
      char_length= fm_charpos(cs, pos, pos + seg->length,
                              char_length / cs->mbmaxlen);
      set_if_smaller(char_length, seg->length);
    }
    if (seg->type == HA_KEYTYPE_VARTEXT1)
      char_length+= seg->bit_start;             /* copy also length */
    else if (seg->type == HA_KEYTYPE_BIT && seg->bit_length)
    {
      *key++= get_rec_bits(rec + seg->bit_pos,
                           seg->bit_start, seg->bit_length);
      char_length--;
    }
    memcpy(key, rec + seg->start, (size_t) char_length);
    key+= char_length;
  }
}