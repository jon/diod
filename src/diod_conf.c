/*****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security, LLC.
 *  Written by Jim Garlick <garlick@llnl.gov> LLNL-CODE-423279
 *  All Rights Reserved.
 *
 *  This file is part of the Distributed I/O Daemon (diod).
 *  For details, see <http://code.google.com/p/diod/>.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License (as published by the
 *  Free Software Foundation) version 2, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA or see
 *  <http://www.gnu.org/licenses/>.
 *****************************************************************************/

/* diod_conf.c - config registry for distributed I/O daemon */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#if HAVE_LUA_H
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#endif

#include "list.h"

#include "diod_conf.h"
#include "diod_log.h"


typedef struct {
    int          debuglevel;
    int          nwthreads;
    int          foreground;
    int          munge;
    int          tcpwrappers;
    int          readahead;
    int          exit_on_lastuse;
    List         listen;
    List         exports;
} Conf;

static Conf config = {
    .debuglevel     = 0,
    .nwthreads      = 16,
    .foreground     = 0,
    .munge          = 1,
    .tcpwrappers    = 1,
    .readahead      = 0,
    .exit_on_lastuse= 0,
    .listen         = NULL,     /* diod_conf_init initializes */
    .exports        = NULL,
};


void diod_conf_init (void) { diod_conf_set_listen ("0.0.0.0:564"); }

int diod_conf_get_debuglevel (void) { return config.debuglevel; }
void diod_conf_set_debuglevel (int i) { config.debuglevel = i; }

int diod_conf_get_nwthreads (void) { return config.nwthreads; }
void diod_conf_set_nwthreads (int i) { config.nwthreads = i; }

int diod_conf_get_foreground (void) { return config.foreground; }
void diod_conf_set_foreground (int i) { config.foreground = i; }

int diod_conf_get_munge (void) { return config.munge; }
void diod_conf_set_munge (int i) { config.munge = i; }

int diod_conf_get_tcpwrappers (void) { return config.tcpwrappers; }
void diod_conf_set_tcpwrappers (int i) { config.tcpwrappers = i; }

int diod_conf_get_readahead (void) { return config.readahead; }
void diod_conf_set_readahead (int i) { config.readahead = i; }

int diod_conf_get_exit_on_lastuse (void) { return config.exit_on_lastuse; }
void diod_conf_set_exit_on_lastuse (int i) { config.exit_on_lastuse = i; }

List diod_conf_get_listen (void) { return config.listen; }
void diod_conf_set_listen (char *s)
{
    char *cpy;

    if (config.listen) {
        list_destroy (config.listen);
        config.listen = NULL;
    }
    if (s) {
        if (!(cpy = strdup (s)))
            msg_exit ("out of memory");
        config.listen = list_create ((ListDelF)free);
        list_append (config.listen, cpy);
    }
}

/* Tattach verifies path against configured exports.
 * Return 1 on ALLOWED, 0 on DENIED.  On DENjIED, put errno in *errp.
 * FIXME: verify host/ip/uid once we parse those in config file
 * FIXME: verify path contains no symlinks below export dir
 */
int
diod_conf_match_export (char *path, char *host, char *ip, uid_t uid, int *errp)
{
    ListIterator itr;
    char *el;
    int res = 0;
    int plen = strlen(path);

    if (strstr (path, "/..") != NULL) {
        *errp = EPERM;
        goto done;
    }
    if ((itr = list_iterator_create (config.exports)) == NULL) {
        *errp = ENOMEM;
        goto done;
    }
    while ((el = list_next (itr))) {
        int len = strlen (el);

        if (strcmp (el, "/") == 0) {
            res = 1;
            break;
        }
        while (len > 0 && el[len - 1] == '/')
            len--; 
        if (plen == len && strncmp (el, path, len) == 0) {
            res = 1;
            break;
        }
        if (plen > len && path[len] == '/') {
            res = 1;
            break;
        }
    }
    list_iterator_destroy (itr);
    if (res == 0)
        *errp = EPERM;
done:
    msg ("attach user %d path %s host %s(%s): %s", uid, path, host, ip,
         res ? "ALLOWED" : "DENIED");

    return res;
}

char *
diod_conf_cat_exports (void)
{
    ListIterator itr = NULL;
    char *ret = NULL;
    int len = 0, elen;
    char *el;

    if ((itr = list_iterator_create (config.exports)) == NULL)
        goto done;        
    while ((el = list_next (itr))) {
        elen = strlen (el) + 1;
        ret = ret ? realloc (ret, len + elen + 1) : malloc (elen + 1);
        if (ret == NULL)
            goto done;
        snprintf (ret + len, elen + 1, "%s\n", el);
        len += elen;
    }
    ret[len] = '\0';
done:
    if (itr)
        list_iterator_destroy (itr);
    return ret;
}

static void
_str_list_append (List l, char *s)
{
    char *cpy = strdup (s);

    if (!cpy)
        msg_exit ("out of memory");
    if (!list_append (l, cpy)) {
        free (cpy);
        msg_exit ("out of memory");
    }
}

static List
_str_list_create (void)
{
    List l = list_create ((ListDelF)free);

    if (!l)
        msg_exit ("out of memory");
    return l;
}

void
diod_conf_set_export (char *path)
{
    if (config.exports)
        list_destroy (config.exports);
    config.exports = _str_list_create ();
    _str_list_append (config.exports, path);
}

void
diod_conf_validate_exports (void)
{
    ListIterator itr;
    char *el;
    struct stat sb;

    if (config.exports == NULL)
        msg_exit ("no exports defined");
    if ((itr = list_iterator_create (config.exports)) == NULL)
        msg_exit ("out of memory");
    while ((el = list_next (itr))) {
        if (*el != '/')
            msg_exit ("exports should begin with '/'");
        if (strstr (el, "/..") != 0)
            msg_exit ("exports should not contain '/..'"); /* FIXME */
        if (stat (el, &sb) < 0)
            err_exit ("could not stat %s", el);
        if (!S_ISDIR (sb.st_mode))
            msg_exit ("%s does not refer to a directory", el);
    }
    list_iterator_destroy (itr);
}

static int
_lua_getglobal_int (char *path, lua_State *L, char *key, int *ip)
{
    int res = 0;

    lua_getglobal (L, key);
    if (!lua_isnil (L, -1)) {
        if (!lua_isnumber (L, -1))
            msg_exit ("%s: `%s' should be number", path, key);
        if (ip)
            *ip = (int)lua_tonumber (L, -1);
        res = 1;
    }
    lua_pop (L, 1);

    return res;
}

#if 0
static int
_lua_getglobal_string (char *path, lua_State *L, char *key, char **sp)
{
    int res = 0;
    char *cpy;

    lua_getglobal (L, key);
    if (!lua_isnil (L, -1)) {
        if (!lua_isstring (L, -1))
            msg_exit ("%s: `%s' should be string", path, key);
        if (sp) {
            cpy = strdup ((char *)lua_tostring (L, -1));
            if (!cpy)
                msg_exit ("out of memory");
            *sp = cpy;
        }
        res = 1;
    }
    lua_pop (L, 1);

    return res;
}
#endif

static int
_lua_getglobal_list_of_strings (char *path, lua_State *L, char *key, List *lp)
{
    int res = 0;
    int i;
    List l;

    lua_getglobal (L, key);
    if (!lua_isnil (L, -1)) {
        if (!lua_istable(L, -1))
            msg_exit ("%s: `%s' should be table", path, key);
        l = _str_list_create();
        for (i = 1; ;i++) {
            lua_pushinteger(L, (lua_Integer)i);
            lua_gettable (L, -2);
            if (lua_isnil (L, -1))
                break;
            if (!lua_isstring (L, -1))
                msg_exit ("%s: `%s[%d]' should be string", path, key, i);
            _str_list_append (l, (char *)lua_tostring (L, -1));
            lua_pop (L, 1);
        }
        lua_pop (L, 1);
        if (lp) {
            if (*lp != NULL)
                list_destroy (*lp);
            *lp = l;
        } else
            list_destroy (l);
        res = 1;
    }
    lua_pop (L, 1);

    return res;
}

void
diod_conf_init_config_file (char *path)
{
    lua_State *L;
    static char buf[PATH_MAX];

    if (!path) {
        snprintf (buf, sizeof (buf), "%s/diod.conf", X_SYSCONFDIR);
        if (access (buf, R_OK) == 0)
            path = buf;  /* missing default config file is not fatal */
    }
    if (path) {
        L = lua_open ();
        luaopen_base (L);
        luaopen_table (L);
        //luaopen_io (L);
        luaopen_string (L);
        luaopen_math (L);

        if (luaL_loadfile (L, path) || lua_pcall (L, 0, 0, 0))
            msg_exit ("%s", lua_tostring (L, -1));
        
        _lua_getglobal_int (path, L, "nwthreads", &config.nwthreads);
        _lua_getglobal_int (path, L, "munge", &config.munge);
        _lua_getglobal_int (path, L, "tcpwrappers", &config.tcpwrappers);
        _lua_getglobal_int (path, L, "readahead", &config.readahead);
        _lua_getglobal_int (path, L, "exit_on_lastuse",
                            &config.exit_on_lastuse);
        _lua_getglobal_list_of_strings (path, L, "listen", &config.listen);
        _lua_getglobal_list_of_strings (path, L, "exports", &config.exports);

        lua_close(L);
    }
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
