#include <stddef.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <lua.h>
#include <lauxlib.h>
#include <errno.h>
#include <stdbool.h>

#include "include/barlib_v1.h"
#include "include/sayf_macros.h"

#include "libls/string_.h"
#include "libls/vector.h"
#include "libls/cstring_utils.h"
#include "libls/parse_int.h"
#include "libls/errno_utils.h"
#include "libls/io_utils.h"
#include "libls/lua_utils.h"
#include "libls/alloc_utils.h"

#include "markup_utils.h"

// Barlib's private data
typedef struct {
    // Number of widgets.
    size_t nwidgets;

    // Content of the widgets.
    LSString *bufs;

    // A zero-terminated separator string.
    char *sep;

    // /fdopen/'ed input file descriptor.
    FILE *in;

    // /fdopen/'ed output file descriptor.
    FILE *out;

    // Buffer for the /escape/ Lua function.
    LSString luabuf;
} Priv;

static
void
destroy(LuastatusBarlibData *bd)
{
    Priv *p = bd->priv;
    for (size_t i = 0; i < p->nwidgets; ++i) {
        LS_VECTOR_FREE(p->bufs[i]);
    }
    free(p->bufs);
    free(p->sep);
    if (p->in) {
        fclose(p->in);
    }
    if (p->out) {
        fclose(p->out);
    }
    LS_VECTOR_FREE(p->luabuf);
    free(p);
}

static
int
init(LuastatusBarlibData *bd, const char *const *opts, size_t nwidgets)
{
    Priv *p = bd->priv = LS_XNEW(Priv, 1);
    *p = (Priv) {
        .nwidgets = nwidgets,
        .bufs = LS_XNEW(LSString, nwidgets),
        .sep = NULL,
        .in = NULL,
        .out = NULL,
        .luabuf = LS_VECTOR_NEW(),
    };
    for (size_t i = 0; i < nwidgets; ++i) {
        LS_VECTOR_INIT_RESERVE(p->bufs[i], 512);
    }

    // All the options may be passed multiple times!
    const char *sep = NULL;
    int in_fd = -1;
    int out_fd = -1;
    for (const char *const *s = opts; *s; ++s) {
        const char *v;
        if ((v = ls_strfollow(*s, "in_fd="))) {
            if ((in_fd = ls_full_parse_uint_s(v)) < 0) {
                LS_FATALF(bd, "in_fd value is not a valid unsigned integer");
                goto error;
            }
        } else if ((v = ls_strfollow(*s, "out_fd="))) {
            if ((out_fd = ls_full_parse_uint_s(v)) < 0) {
                LS_FATALF(bd, "out_fd value is not a valid unsigned integer");
                goto error;
            }
        } else if ((v = ls_strfollow(*s, "separator="))) {
            sep = v;
        } else {
            LS_FATALF(bd, "unknown option '%s'", *s);
            goto error;
        }
    }
    p->sep = ls_xstrdup(sep ? sep : " | ");

    // we require /in_fd/ and /out_fd/ to be >=3 because making stdin/stdout/stderr CLOEXEC has very
    // bad consequences, and we just don't want to complicate the logic.
    if (in_fd < 3) {
        LS_FATALF(bd, "in_fd is not specified or less than 3");
        goto error;
    }
    if (out_fd < 3) {
        LS_FATALF(bd, "out_fd is not specified or less than 3");
        goto error;
    }

    // open
    if (!(p->in = fdopen(in_fd, "r"))) {
        LS_WITH_ERRSTR(s, errno,
            LS_FATALF(bd, "can't fdopen %d: %s", in_fd, s);
        );
        goto error;
    }
    if (!(p->out = fdopen(out_fd, "w"))) {
        LS_WITH_ERRSTR(s, errno,
            LS_FATALF(bd, "can't fdopen %d: %s", out_fd, s);
        );
        goto error;
    }

    // make CLOEXEC
    if (ls_make_cloexec(in_fd) < 0) {
        LS_WITH_ERRSTR(s, errno,
            LS_FATALF(bd, "can't make fd %d CLOEXEC: %s", in_fd, s);
        );
        goto error;
    }
    if (ls_make_cloexec(out_fd) < 0) {
        LS_WITH_ERRSTR(s, errno,
            LS_FATALF(bd, "can't make fd %d CLOEXEC: %s", out_fd, s);
        );
        goto error;
    }

    return LUASTATUS_OK;

error:
    destroy(bd);
    return LUASTATUS_ERR;
}

static
int
l_escape(lua_State *L)
{
    size_t ns;
    // WARNING: /luaL_check*()/ functions do a long jump on error!
    const char *s = luaL_checklstring(L, 1, &ns);

    LuastatusBarlibData *bd = lua_touserdata(L, lua_upvalueindex(1));
    Priv *p = bd->priv;

    LSString *buf = &p->luabuf;
    LS_VECTOR_CLEAR(*buf);

    lemonbar_ls_string_append_escaped_b(buf, s, ns);

    // L: -
    lua_pushlstring(L, buf->data, buf->size); // L: string
    return 1;
}

static
void
register_funcs(LuastatusBarlibData *bd, lua_State *L)
{
    // L: table
    lua_pushlightuserdata(L, bd); // L: table bd
    lua_pushcclosure(L, l_escape, 1); // L: table bd l_escape
    ls_lua_rawsetf(L, "escape"); // L: table
}

static
bool
redraw(LuastatusBarlibData *bd)
{
    Priv *p = bd->priv;
    FILE *out = p->out;
    size_t n = p->nwidgets;
    LSString *bufs = p->bufs;
    const char *sep = p->sep;

    bool first = true;
    for (size_t i = 0; i < n; ++i) {
        if (bufs[i].size) {
            if (!first) {
                fputs(sep, out);
            }
            fwrite(bufs[i].data, 1, bufs[i].size, out);
            first = false;
        }
    }
    fputc('\n', out);
    fflush(out);
    if (ferror(out)) {
        LS_WITH_ERRSTR(s, errno,
            LS_FATALF(bd, "write error: %s", s);
        );
        return false;
    }
    return true;
}

static
int
set(LuastatusBarlibData *bd, lua_State *L, size_t widget_idx)
{
    Priv *p = bd->priv;
    LSString *buf = &p->bufs[widget_idx];

    LS_VECTOR_CLEAR(*buf);
    switch (lua_type(L, -1)) {
    case LUA_TNIL:
        break;
    case LUA_TSTRING:
        {
            size_t ns;
            const char *s = lua_tolstring(L, -1, &ns);
            lemonbar_ls_string_append_sanitized_b(buf, widget_idx, s, ns);
        }
        break;
    case LUA_TTABLE:
        {
            const char *sep = p->sep;
            LS_LUA_TRAVERSE(L, -1) {
                if (!lua_isnumber(L, LS_LUA_KEY)) {
                    LS_ERRF(bd, "table key: expected number, found %s",
                        luaL_typename(L, LS_LUA_KEY));
                    return LUASTATUS_NONFATAL_ERR;
                }
                if (!lua_isstring(L, LS_LUA_VALUE)) {
                    LS_ERRF(bd, "table value: expected string, found %s",
                        luaL_typename(L, LS_LUA_VALUE));
                    return LUASTATUS_NONFATAL_ERR;
                }
                size_t ns;
                const char *s = lua_tolstring(L, LS_LUA_VALUE, &ns);
                if (buf->size && ns) {
                    ls_string_append_s(buf, sep);
                }
                lemonbar_ls_string_append_sanitized_b(buf, widget_idx, s, ns);
            }
        }
        break;
    default:
        LS_ERRF(bd, "expected string or nil, found %s", luaL_typename(L, -1));
        return LUASTATUS_NONFATAL_ERR;
    }

    if (!redraw(bd)) {
        return LUASTATUS_ERR;
    }
    return LUASTATUS_OK;
}

static
int
set_error(LuastatusBarlibData *bd, size_t widget_idx)
{
    Priv *p = bd->priv;
    ls_string_assign_s(&p->bufs[widget_idx], "%{B#f00}%{F#fff}(Error)%{B-}%{F-}");
    if (!redraw(bd)) {
        return LUASTATUS_ERR;
    }
    return LUASTATUS_OK;
}

static
int
event_watcher(LuastatusBarlibData *bd, LuastatusBarlibEWFuncs funcs)
{
    Priv *p = bd->priv;

    char *buf = NULL;
    size_t nbuf = 256;

    for (ssize_t nread; (nread = getline(&buf, &nbuf, p->in)) >= 0;) {
        if (nread == 0 || buf[nread - 1] != '\n') {
            continue;
        }
        size_t ncommand;
        size_t widget_idx;
        const char *command = lemonbar_parse_command(buf, nread - 1, &ncommand, &widget_idx);
        if (!command) {
            continue;
        }
        lua_State *L = funcs.call_begin(bd->userdata, widget_idx);
        lua_pushlstring(L, command, ncommand);
        funcs.call_end(bd->userdata, widget_idx);
    }

    if (feof(p->in)) {
        LS_ERRF(bd, "lemonbar closed its pipe end");
    } else {
        LS_WITH_ERRSTR(s, errno,
            LS_ERRF(bd, "read error: %s", s);
        );
    }

    free(buf);

    return LUASTATUS_ERR;
}

LuastatusBarlibIface luastatus_barlib_iface_v1 = {
    .init = init,
    .register_funcs = register_funcs,
    .set = set,
    .set_error = set_error,
    .event_watcher = event_watcher,
    .destroy = destroy,
};
