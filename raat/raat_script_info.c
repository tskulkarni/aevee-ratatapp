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
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <luv.h>

static void info_cb(const char *key, const char *val, void *userdata) {
    lua_State *lua = userdata;
    lua_pushstring(lua, val);
    lua_setfield(lua, -2, key);
}

/** Initialize the info API bindings */
void
RAAT__script_init_info(lua_State *lua, RAAT__Info *info) {
    lua_getglobal(lua, "raat");         // push raat table
    lua_newtable(lua);                  // push table for info dict
    RAAT__info_foreach(info, info_cb, lua);
    lua_setfield(lua, -2, "info");      // raat.info = infotable
    lua_pop(lua, -1);                   // pop raat table
}
