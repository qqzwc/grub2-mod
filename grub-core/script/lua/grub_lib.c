/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2009  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "grub_lib.h"
#include "gbk.h"

#include <grub/env.h>
#include <grub/parser.h>
#include <grub/command.h>
#include <grub/normal.h>
#include <grub/term.h>
#include <grub/file.h>
#include <grub/menu.h>
#include <grub/device.h>
#include <grub/lib/crc.h>

#ifdef ENABLE_LUA_PCI
#include <grub/pci.h>
#endif

#define UTF_MAX     8
#define DBCS_MAX    2

/* Updates the globals grub_errno and grub_msg, leaving their values on the 
   top of the stack, and clears grub_errno. When grub_errno is zero, grub_msg
   is not left on the stack. The value returned is the number of values left on
   the stack. */
static int
push_result (lua_State *state)
{
  int saved_errno;
  int num_results;

  saved_errno = grub_errno;
  grub_errno = 0;

  /* Push once for setfield, and again to leave on the stack */
  lua_pushinteger (state, saved_errno);
  lua_pushinteger (state, saved_errno);
  lua_setfield (state, LUA_GLOBALSINDEX, "grub_errno");

  if (saved_errno)
  {
    /* Push once for setfield, and again to leave on the stack */
    lua_pushstring (state, grub_errmsg);
    lua_pushstring (state, grub_errmsg);
    num_results = 2;
  }
  else
  {
    lua_pushnil (state);
    num_results = 1;
  }

  lua_setfield (state, LUA_GLOBALSINDEX, "grub_errmsg");

  return num_results;
}

/* Updates the globals grub_errno and grub_msg ( without leaving them on the
   stack ), clears grub_errno,  and returns the value of grub_errno before it
   was cleared. */
static int
save_errno (lua_State *state)
{
  int saved_errno;

  saved_errno = grub_errno;
  lua_pop(state, push_result(state));

  return saved_errno;
}

static unsigned from_utf8(unsigned uni_code) {
    const unsigned short *page = from_uni[(uni_code >> 8) & 0xFF];
    return page == NULL ? DBCS_DEFAULT_CODE : page[uni_code & 0xFF];
}

static unsigned to_utf8(unsigned cp_code) {
    const unsigned short *page = to_uni[(cp_code >> 8) & 0xFF];
    return page == NULL ? UNI_INVALID_CODE : page[cp_code & 0xFF];
}

static size_t utf8_encode(char *s, unsigned ch) {
    if (ch < 0x80) {
        s[0] = (char)ch;
        return 1;
    }
    if (ch <= 0x7FF) {
        s[1] = (char) ((ch | 0x80) & 0xBF);
        s[0] = (char) ((ch >> 6) | 0xC0);
        return 2;
    }
    if (ch <= 0xFFFF) {
three:
        s[2] = (char) ((ch | 0x80) & 0xBF);
        s[1] = (char) (((ch >> 6) | 0x80) & 0xBF);
        s[0] = (char) ((ch >> 12) | 0xE0);
        return 3;
    }
    if (ch <= 0x1FFFFF) {
        s[3] = (char) ((ch | 0x80) & 0xBF);
        s[2] = (char) (((ch >> 6) | 0x80) & 0xBF);
        s[1] = (char) (((ch >> 12) | 0x80) & 0xBF);
        s[0] = (char) ((ch >> 18) | 0xF0);
        return 4;
    }
    if (ch <= 0x3FFFFFF) {
        s[4] = (char) ((ch | 0x80) & 0xBF);
        s[3] = (char) (((ch >> 6) | 0x80) & 0xBF);
        s[2] = (char) (((ch >> 12) | 0x80) & 0xBF);
        s[1] = (char) (((ch >> 18) | 0x80) & 0xBF);
        s[0] = (char) ((ch >> 24) | 0xF8);
        return 5;
    }
    if (ch <= 0x7FFFFFFF) {
        s[5] = (char) ((ch | 0x80) & 0xBF);
        s[4] = (char) (((ch >> 6) | 0x80) & 0xBF);
        s[3] = (char) (((ch >> 12) | 0x80) & 0xBF);
        s[2] = (char) (((ch >> 18) | 0x80) & 0xBF);
        s[1] = (char) (((ch >> 24) | 0x80) & 0xBF);
        s[0] = (char) ((ch >> 30) | 0xFC);
        return 6;
    }

    /* fallback */
    ch = 0xFFFD;
    goto three;
}

static size_t utf8_decode(const char *s, const char *e, unsigned *pch) {
    unsigned ch;

    if (s >= e) {
        *pch = 0;
        return 0;   
    }

    ch = (unsigned char)s[0];
    if (ch < 0xC0) goto fallback;
    if (ch < 0xE0) {
        if (s+1 >= e || (s[1] & 0xC0) != 0x80)
            goto fallback;
        *pch = ((ch   & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (ch < 0xF0) {
        if (s+2 >= e || (s[1] & 0xC0) != 0x80
                || (s[2] & 0xC0) != 0x80)
            goto fallback;
        *pch = ((ch   & 0x0F) << 12) | ((s[1] & 0x3F) <<  6) | (s[2] & 0x3F);
        return 3;
    }
    {
        int count = 0; /* to count number of continuation bytes */
        unsigned res = 0;
        while ((ch & 0x40) != 0) { /* still have continuation bytes? */
            int cc = (unsigned char)s[++count];
            if ((cc & 0xC0) != 0x80) /* not a continuation byte? */
                goto fallback; /* invalid byte sequence, fallback */
            res = (res << 6) | (cc & 0x3F); /* add lower 6 bits from cont. byte */
            ch <<= 1; /* to test next bit */
        }
        if (count > 5)
            goto fallback; /* invalid byte sequence */
        res |= ((ch & 0x7F) << (count * 5)); /* add first byte */
        *pch = res;
        return count+1;
    }

fallback:
    *pch = ch;
    return 1;
}

static void add_utf8char(luaL_Buffer *b, unsigned ch) {
    char buff[UTF_MAX];
    size_t n = utf8_encode(buff, ch);
    luaL_addlstring(b, buff, n);
}

static size_t dbcs_decode(const char *s, const char *e, unsigned *pch) {
    unsigned ch;
    if (s >= e) {
        *pch = 0;
        return 0;
    }

    ch = s[0] & 0xFF;
    if (to_uni_00[ch] != UNI_INVALID_CODE) {
        *pch = ch;
        return 1;
    }

    *pch = (ch << 8) | (s[1] & 0xFF);
    return 2;
}

static void add_dbcschar(luaL_Buffer *b, unsigned ch) {
    if (ch < 0x7F)
        luaL_addchar(b, ch);
    else {
        luaL_addchar(b, (ch >> 8) & 0xFF);
        luaL_addchar(b, ch & 0xFF);
    }
}

static size_t dbcs_length(const char *s, const char *e) {
    size_t dbcslen = 0;
    while (s < e) {
        if ((unsigned char)(*s++) > 0x7F)
            ++s;
        ++dbcslen;
    }
    return dbcslen;
}


/* dbcs module interface */

static const char *check_dbcs(lua_State *L, int idx, const char **pe) {
    size_t len;
    const char *s = luaL_checklstring(L, idx, &len);
    if (pe != NULL) *pe = s + len;
    return s;
}

static int posrelat(int pos, size_t len) {
    if (pos >= 0) return (size_t)pos;
    else if (0u - (size_t)pos > len) return 0;
    else return len - ((size_t)-pos) + 1;
}

static int string_from_utf8(lua_State *L) {
    const char *e, *s = check_dbcs(L, 1, &e);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    while (s < e) {
        unsigned ch;
        s += utf8_decode(s, e, &ch);
        add_dbcschar(&b, from_utf8(ch));
    }
    luaL_pushresult(&b);
    return 1;
}

static int string_to_utf8(lua_State *L) {
    const char *e, *s = check_dbcs(L, 1, &e);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    while (s < e) {
        unsigned ch;
        s += dbcs_decode(s, e, &ch);
        add_utf8char(&b, to_utf8(ch));
    }
    luaL_pushresult(&b);
    return 1;
}

static int
grub_lua_run (lua_State *state)
{
  int n;
  char **args;
  const char *s;

  s = luaL_checkstring (state, 1);
  if ((! grub_parser_split_cmdline (s, 0, 0, &n, &args))
      && (n >= 0))
    {
      grub_command_t cmd;

      cmd = grub_command_find (args[0]);
      if (cmd)
	(cmd->func) (cmd, n-1, &args[1]);
      else
	grub_error (GRUB_ERR_FILE_NOT_FOUND, "command not found");

      grub_free (args[0]);
      grub_free (args);
    }

  return push_result (state);
}

static int
grub_lua_getenv (lua_State *state)
{
  int n, i;

  n = lua_gettop (state);
  for (i = 1; i <= n; i++)
    {
      const char *name, *value;

      name = luaL_checkstring (state, i);
      value = grub_env_get (name);
      if (value)
	lua_pushstring (state, value);
      else
	lua_pushnil (state);
    }

  return n;
}

static int
grub_lua_setenv (lua_State *state)
{
  const char *name, *value;

  name = luaL_checkstring (state, 1);
  value = luaL_checkstring (state, 2);

  if (name[0])
    grub_env_set (name, value);

  return 0;
}

/* Helper for grub_lua_enum_device.  */
static int
grub_lua_enum_device_iter (const char *name, void *data)
{
  lua_State *state = data;
  int result;
  grub_device_t dev;

  result = 0;
  dev = grub_device_open (name);
  if (dev)
    {
      grub_fs_t fs;

      fs = grub_fs_probe (dev);
      if (fs)
	{
	  lua_pushvalue (state, 1);
	  lua_pushstring (state, name);
	  lua_pushstring (state, fs->name);
	  if (! fs->uuid)
	    lua_pushnil (state);
	  else
	    {
	      int err;
	      char *uuid;

	      err = fs->uuid (dev, &uuid);
	      if (err)
		{
		  grub_errno = 0;
		  lua_pushnil (state);
		}
	      else
		{
		  lua_pushstring (state, uuid);
		  grub_free (uuid);
		}
	    }

	  if (! fs->label)
	    lua_pushnil (state);
	  else
	    {
	      int err;
	      char *label = NULL;

	      err = fs->label (dev, &label);
	      if (err)
		{
		  grub_errno = 0;
		  lua_pushnil (state);
		}
	      else
		{
		  if (label == NULL)
		    {
		      lua_pushnil (state);
		    }
		  else
		    {
		      lua_pushstring (state, label);
		    }
		  grub_free (label);
		}
	    }

	  lua_call (state, 4, 1);
	  result = lua_tointeger (state, -1);
	  lua_pop (state, 1);
	}
      else
	grub_errno = 0;
      grub_device_close (dev);
    }
  else
    grub_errno = 0;

  return result;
}

static int
grub_lua_enum_device (lua_State *state)
{
  luaL_checktype (state, 1, LUA_TFUNCTION);
  grub_device_iterate (grub_lua_enum_device_iter, state);
  return push_result (state);
}

static int
enum_file (const char *name, const struct grub_dirhook_info *info,
	   void *data)
{
  int result;
  lua_State *state = data;

  lua_pushvalue (state, 1);
  lua_pushstring (state, name);
  lua_pushinteger (state, info->dir != 0);
  lua_call (state, 2, 1);
  result = lua_tointeger (state, -1);
  lua_pop (state, 1);

  return result;
}

static int
grub_lua_enum_file (lua_State *state)
{
  char *device_name;
  const char *arg;
  grub_device_t dev;

  luaL_checktype (state, 1, LUA_TFUNCTION);
  arg = luaL_checkstring (state, 2);
  device_name = grub_file_get_device_name (arg);
  dev = grub_device_open (device_name);
  if (dev)
    {
      grub_fs_t fs;
      const char *path;

      fs = grub_fs_probe (dev);
      path = grub_strchr (arg, ')');
      if (! path)
	path = arg;
      else
	path++;

      if (fs)
	{
	  (fs->dir) (dev, path, enum_file, state);
	}

      grub_device_close (dev);
    }

  grub_free (device_name);

  return push_result (state);
}

#ifdef ENABLE_LUA_PCI
/* Helper for grub_lua_enum_pci.  */
static int
grub_lua_enum_pci_iter (grub_pci_device_t dev, grub_pci_id_t pciid, void *data)
{
  lua_State *state = data;
  int result;
  grub_pci_address_t addr;
  grub_uint32_t class;

  lua_pushvalue (state, 1);
  lua_pushinteger (state, grub_pci_get_bus (dev));
  lua_pushinteger (state, grub_pci_get_device (dev));
  lua_pushinteger (state, grub_pci_get_function (dev));
  lua_pushinteger (state, pciid);

  addr = grub_pci_make_address (dev, GRUB_PCI_REG_CLASS);
  class = grub_pci_read (addr);
  lua_pushinteger (state, class);

  lua_call (state, 5, 1);
  result = lua_tointeger (state, -1);
  lua_pop (state, 1);

  return result;
}

static int
grub_lua_enum_pci (lua_State *state)
{
  luaL_checktype (state, 1, LUA_TFUNCTION);
  grub_pci_iterate (grub_lua_enum_pci_iter, state);
  return push_result (state);
}
#endif

static int
grub_lua_file_open (lua_State *state)
{
  grub_file_t file;
  const char *name;

  name = luaL_checkstring (state, 1);
  file = grub_file_open (name);
  save_errno (state);

  if (! file)
    return 0;

  lua_pushlightuserdata (state, file);
  return 1;
}

static int
grub_lua_file_close (lua_State *state)
{
  grub_file_t file;

  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);
  grub_file_close (file);

  return push_result (state);
}

static int
grub_lua_file_seek (lua_State *state)
{
  grub_file_t file;
  grub_off_t offset;

  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);
  offset = luaL_checkinteger (state, 2);

  offset = grub_file_seek (file, offset);
  save_errno (state);

  lua_pushinteger (state, offset);
  return 1;
}

static int
grub_lua_file_read (lua_State *state)
{
  grub_file_t file;
  luaL_Buffer b;
  int n;

  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);
  n = luaL_checkinteger (state, 2);

  luaL_buffinit (state, &b);
  while (n)
    {
      char *p;
      int nr;

      nr = (n > LUAL_BUFFERSIZE) ? LUAL_BUFFERSIZE : n;
      p = luaL_prepbuffer (&b);

      nr = grub_file_read (file, p, nr);
      if (nr <= 0)
	break;

      luaL_addsize (&b, nr);
      n -= nr;
    }

  save_errno (state);
  luaL_pushresult (&b);
  return 1;
}

static int
grub_lua_file_getline (lua_State *state)
{
  grub_file_t file;
  char *line;

  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);

  line = grub_file_getline (file);
  save_errno (state);

  if (! line)
    return 0;

  lua_pushstring (state, line);
  grub_free (line);
  return 1;
}

static int
grub_lua_file_getsize (lua_State *state)
{
  grub_file_t file;

  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);

  lua_pushinteger (state, file->size);
  return 1;
}

static int
grub_lua_file_getpos (lua_State *state)
{
  grub_file_t file;

  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);

  lua_pushinteger (state, file->offset);
  return 1;
}

static int
grub_lua_file_eof (lua_State *state)
{
  grub_file_t file;

  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);

  lua_pushboolean (state, file->offset >= file->size);
  return 1;
}

static int
grub_lua_file_exist (lua_State *state)
{
  grub_file_t file;
  const char *name;
  int result;

  result = 0;
  name = luaL_checkstring (state, 1);
  file = grub_file_open (name);
  if (file)
    {
      result++;
      grub_file_close (file);
    }
  else
    grub_errno = 0;

  lua_pushboolean (state, result);
  return 1;
}

static int
grub_lua_file_crc32 (lua_State *state)
{
  grub_file_t file;
  const char *name;
  int crc;
  char buf[GRUB_DISK_SECTOR_SIZE];
  grub_ssize_t size;
  name = luaL_checkstring (state, 1);
  file = grub_file_open (name);
  if (file)
    {
      crc = 0;
      while ((size = grub_file_read (file, buf, sizeof (buf))) > 0)
        crc = grub_getcrc32c (crc, buf, size);
      lua_pushinteger (state, crc);
    }
  return 1;
}

static int
grub_lua_add_menu (lua_State *state)
{
  int n;
  const char *source;

  source = luaL_checklstring (state, 1, 0);
  n = lua_gettop (state) - 1;
  if (n > 0)
    {
      const char **args;
      char *p;
      int i;

      args = grub_malloc (n * sizeof (args[0]));
      if (!args)
	return push_result (state);
      for (i = 0; i < n; i++)
	args[i] = luaL_checkstring (state, 2 + i);

      p = grub_strdup (source);
      if (! p)
	return push_result (state);

      grub_normal_add_menu_entry (n, args, NULL, NULL, NULL, NULL, NULL, p, 0, 0);
    }
  else
    {
      lua_pushstring (state, "not enough parameter");
      lua_error (state);
    }

  return push_result (state);
}

static int
grub_lua_read_byte (lua_State *state)
{
  grub_addr_t addr;

  addr = luaL_checkinteger (state, 1);
  lua_pushinteger (state, *((grub_uint8_t *) addr));
  return 1;
}

static int
grub_lua_read_word (lua_State *state)
{
  grub_addr_t addr;

  addr = luaL_checkinteger (state, 1);
  lua_pushinteger (state, *((grub_uint16_t *) addr));
  return 1;
}

static int
grub_lua_read_dword (lua_State *state)
{
  grub_addr_t addr;

  addr = luaL_checkinteger (state, 1);
  lua_pushinteger (state, *((grub_uint32_t *) addr));
  return 1;
}

static int
grub_lua_write_byte (lua_State *state)
{
  grub_addr_t addr;

  addr = luaL_checkinteger (state, 1);
  *((grub_uint8_t *) addr) = luaL_checkinteger (state, 2);
  return 1;
}

static int
grub_lua_write_word (lua_State *state)
{
  grub_addr_t addr;

  addr = luaL_checkinteger (state, 1);
  *((grub_uint16_t *) addr) = luaL_checkinteger (state, 2);
  return 1;
}

static int
grub_lua_write_dword (lua_State *state)
{
  grub_addr_t addr;

  addr = luaL_checkinteger (state, 1);
  *((grub_uint32_t *) addr) = luaL_checkinteger (state, 2);
  return 1;
}

static int
grub_lua_cls (lua_State *state __attribute__ ((unused)))
{
  grub_cls ();
  return 0;
}

static int
grub_lua_setcolorstate (lua_State *state)
{
  grub_setcolorstate (luaL_checkinteger (state, 1));
  return 0;
}

static int
grub_lua_refresh (lua_State *state __attribute__ ((unused)))
{
  grub_refresh ();
  return 0;
}


static int grub_lua_len (lua_State *L) {
    const char *e, *s = check_dbcs(L, 1, &e);
    lua_pushinteger(L, dbcs_length(s, e));
    return 1;
}

static int grub_lua_byte(lua_State *L) {
    const char *e, *s = check_dbcs(L, 1, &e);
    size_t len = dbcs_length(s, e);
    int posi = posrelat((int)luaL_optinteger(L, 2, 1), len);
    int pose = posrelat((int)luaL_optinteger(L, 3, posi), len);
    const char *start = s;
    int i, n;
    if (posi < 1) posi = 1;
    if (pose > (int)len) pose = len;
    if (pose > pose) return 0;
    n = (int)(pose - posi + 1);
    if (posi + n <= pose) /* (size_t -> int) overflow? */
        return luaL_error(L, "string slice too long");
    luaL_checkstack(L, n, "string slice too long");
    for (i = 0; i < posi; ++i) {
        unsigned ch;
        start += dbcs_decode(start, e, &ch);
    }
    for (i = 0; i < n; ++i) {
        unsigned ch;
        start += dbcs_decode(start, e, &ch);
        lua_pushinteger(L, ch);
    }
    return n;
}

static int grub_lua_char(lua_State *L) {
    luaL_Buffer b;
    int i, top = lua_gettop(L);
    luaL_buffinit(L, &b);
    for (i = 1; i <= top; ++i)
        add_dbcschar(&b, (unsigned)luaL_checkinteger(L, i));
    luaL_pushresult(&b);
    return 1;
}

static int grub_lua_fromutf8(lua_State *L) {
    int i, top;
    switch (lua_type(L, 1)) {
    case LUA_TSTRING:
        return string_from_utf8(L);
    case LUA_TNUMBER:
        top = lua_gettop(L);
        for (i = 1; i <= top; ++i) {
            unsigned code = (unsigned)luaL_checkinteger(L, i);
            lua_pushinteger(L, (lua_Integer)from_utf8(code));
            lua_replace(L, i);
        }
        return top;
    }
    return luaL_error(L, "string/number expected, got %s",
            luaL_typename(L, 1));
}

static int grub_lua_toutf8(lua_State *L) {
    int i, top;
    switch (lua_type(L, 1)) {
    case LUA_TSTRING:
        return string_to_utf8(L);
    case LUA_TNUMBER:
        top = lua_gettop(L);
        for (i = 1; i <= top; ++i) {
            unsigned code = (unsigned)luaL_checkinteger(L, i);
            lua_pushinteger(L, to_utf8(code));
            lua_replace(L, i);
        }
        return top;
    }
    return luaL_error(L, "string/number expected, got %s",
            luaL_typename(L, 1));
}

static int
grub_lua_getkey_noblock (lua_State *state __attribute__ ((unused)))
{
  int key;
  key = grub_getkey_noblock ();
  lua_pushinteger (state, key);
  return 1;
}

static int
grub_lua_getkey (lua_State *state __attribute__ ((unused)))
{
  int key;
  key = grub_getkey ();
  lua_pushinteger (state, key);
  return 1;
}

luaL_Reg grub_lua_lib[] =
  {
    {"run", grub_lua_run},
    {"getenv", grub_lua_getenv},
    {"setenv", grub_lua_setenv},
    {"enum_device", grub_lua_enum_device},
    {"enum_file", grub_lua_enum_file},
#ifdef ENABLE_LUA_PCI
    {"enum_pci", grub_lua_enum_pci},
#endif
    {"file_open", grub_lua_file_open},
    {"file_close", grub_lua_file_close},
    {"file_seek", grub_lua_file_seek},
    {"file_read", grub_lua_file_read},
    {"file_getline", grub_lua_file_getline},
    {"file_getsize", grub_lua_file_getsize},
    {"file_getpos", grub_lua_file_getpos},
    {"file_eof", grub_lua_file_eof},
    {"file_exist", grub_lua_file_exist},
    {"file_crc32", grub_lua_file_crc32},
    {"add_menu", grub_lua_add_menu},
    {"read_byte", grub_lua_read_byte},
    {"read_word", grub_lua_read_word},
    {"read_dword", grub_lua_read_dword},
    {"write_byte", grub_lua_write_byte},
    {"write_word", grub_lua_write_word},
    {"write_dword", grub_lua_write_dword},
    {"cls", grub_lua_cls},
    {"setcolorstate", grub_lua_setcolorstate},
    {"refresh", grub_lua_refresh},
    {"len", grub_lua_len},
    {"byte", grub_lua_byte},
    {"char", grub_lua_char},
    {"fromutf8", grub_lua_fromutf8},
    {"toutf8", grub_lua_toutf8},
    {"getkey_noblock", grub_lua_getkey_noblock},
    {"getkey", grub_lua_getkey},
    {0, 0}
  };
