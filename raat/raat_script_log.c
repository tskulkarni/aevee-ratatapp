//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_script.h"
#include "raat_session.h"

#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <luv.h>

static void format_log_message(lua_State *lua, char *buf, int buflen) {
    RAAT__Session *session = RAAT__script_get_registry(lua, "raat_session");
    int n = lua_gettop(lua);  /* number of arguments */
    int i;
    int left = buflen;
    const char *displayaddr = RAAT__session_get_display_addr(session);

    lua_getglobal(lua, "tostring");

    char prefixbuf[1024];
    int threadbuf_len = snprintf(prefixbuf, 1024, "[lua@%p] [%s] ", lua, displayaddr);
    strncpy(buf+buflen-left, prefixbuf, left); left -= threadbuf_len;

    for (i=1; i<=n; i++) {
        const char *s;
        size_t len;
        lua_pushvalue(lua, -1);  /* function to be called */
        lua_pushvalue(lua, i);   /* value to print */
        lua_call(lua, 1, 1);
        s = lua_tolstring(lua, -1, &len);  /* get result */
        if (s == NULL) continue;
        if (left > 0) {
            strncpy(buf+buflen-left, " ", left);
            left -= 1;
            strncpy(buf+buflen-left, s, left);
            left -= len;
        }
        lua_pop(lua, 1);  /* pop result */
    }
    buf[buflen-1] = 0;
}

#define RAAT__CURRENT_LOG log
static int script_log_trace(lua_State *lua) {
    RAAT__Log *log = RAAT__script_get_registry(lua, "raat_log");
    char logbuf[1024] = {0,};
    format_log_message(lua, logbuf, sizeof(logbuf));
    RAAT__TRACE("%s", logbuf);
    return 0;
}

static int script_log_debug(lua_State *lua) {
    RAAT__Log *log = RAAT__script_get_registry(lua, "raat_log");
    char logbuf[1024] = {0,};
    format_log_message(lua, logbuf, sizeof(logbuf));
    RAAT__DEBUG("%s", logbuf);
    return 0;
}

static int script_log_info(lua_State *lua) {
    RAAT__Log *log = RAAT__script_get_registry(lua, "raat_log");
    char logbuf[1024] = {0,};
    format_log_message(lua, logbuf, sizeof(logbuf));
    RAAT__INFO("%s", logbuf);
    return 0;
}

static int script_log_error(lua_State *lua) {
    RAAT__Log *log = RAAT__script_get_registry(lua, "raat_log");
    char logbuf[1024] = {0,};
    format_log_message(lua, logbuf, sizeof(logbuf));
    RAAT__ERROR("%s", logbuf);
    return 0;
}

static int script_log_critical(lua_State *lua) {
    RAAT__Log *log = RAAT__script_get_registry(lua, "raat_log");
    char logbuf[1024] = {0,};
    format_log_message(lua, logbuf, sizeof(logbuf));
    RAAT__CRITICAL("%s", logbuf);
    return 0;
}

static int script_log_warning(lua_State *lua) {
    RAAT__Log *log = RAAT__script_get_registry(lua, "raat_log");
    char logbuf[1024] = {0,};
    format_log_message(lua, logbuf, sizeof(logbuf));
    RAAT__WARNING("%s", logbuf);
    return 0;
}
#undef RAAT__CURRENT_LOG

static const luaL_Reg loglib[] = {
    { "trace",          script_log_trace                },
    { "info",           script_log_info                 },
    { "warning",        script_log_warning              },
    { "debug",          script_log_debug                },
    { "critical",       script_log_critical             },
    { "error",          script_log_error                },
    { NULL,             NULL                            }
};

/** Initialize the log API bindings */
void
RAAT__script_init_log(lua_State *lua, RAAT__Log *log) {
    RAAT__script_set_registry(lua, "raat_log", log);
    lua_getglobal(lua, "raat");         // push raat table
    luaL_newlib(lua, loglib);           // push table for log functions
    lua_setfield(lua, -2, "log");       // raat.log = logtable
    lua_pop(lua, 1);                    // pop raat table
}

