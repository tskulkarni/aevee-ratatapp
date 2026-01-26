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

#if PLATFORM_MACOSX
#include <CoreServices/CoreServices.h>
#endif

#if PLATFORM_LINUX || PLATFORM_IOS || PLATFORM_ANDROID || PLATFORM_MACOSX
#include <sys/utsname.h>
#endif


void *RAAT__script_get_registry(lua_State *lua, const char *key) {
    void *val;
    lua_pushstring(lua, key);
    lua_rawget(lua, LUA_REGISTRYINDEX);
    if (lua_isnil(lua, -1)) val = NULL;
    else                    val = lua_touserdata(lua, -1);
    lua_pop(lua, 1);
    return val;
}

void RAAT__script_set_registry(lua_State *lua, const char *key, void *val) {
    lua_pushstring(lua, key);
    if (val == NULL) lua_pushnil(lua);
    else             lua_pushlightuserdata(lua, val);
    lua_rawset(lua, LUA_REGISTRYINDEX);
}

void *lua_alloc(lua_State *L, void *ptr, int osize, int nsize) {
    void *ud;
    lua_Alloc fn = lua_getallocf(L, &ud);
    return fn(ud, ptr, osize, nsize);
}


void RAAT__unpack_stream_format(lua_State *L, int n, RAAT__StreamFormat *out_format) {
    const char *sample_type_str;
    const char *sample_subtype_str;

    memset(out_format, 0, sizeof(RAAT__StreamFormat));

    lua_getfield(L, n, "sample_rate");
    out_format->sample_rate = lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, n, "bits_per_sample");
    out_format->bits_per_sample = lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, n, "channels");
    out_format->channels = lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, n, "sample_type");
    sample_type_str = lua_tostring(L, -1);
    lua_pop(L, 1);

    if      (!strcmp(sample_type_str, "pcm")) out_format->sample_type = RAAT__SAMPLE_TYPE_PCM;
    else if (!strcmp(sample_type_str, "dsd")) out_format->sample_type = RAAT__SAMPLE_TYPE_DSD;
    else {
        lua_pushliteral(L, "Invalid sample type");
        lua_error(L);
    }

    lua_getfield(L, n, "sample_subtype");

    if (lua_isnil(L, -1)) {
        out_format->sample_subtype = RAAT__SAMPLE_SUBTYPE_NONE;
    } else {
        sample_subtype_str = lua_tostring(L, -1);

        if      (!strcmp(sample_subtype_str, "mqa"))      out_format->sample_subtype = RAAT__SAMPLE_SUBTYPE_MQA;
        else if (!strcmp(sample_subtype_str, "mqa_core")) out_format->sample_subtype = RAAT__SAMPLE_SUBTYPE_MQA_CORE;
        else if (!strcmp(sample_subtype_str, "none"))     out_format->sample_subtype = RAAT__SAMPLE_SUBTYPE_NONE;
        else {
            lua_pushliteral(L, "Invalid sample subtype");
            lua_error(L);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, n, "mqa_original_sample_rate");
    if (!lua_isnil(L, -1)) {
        out_format->mqa_original_sample_rate = lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
}

#if PLATFORM_MACOSX
static void 
push_gestalt(lua_State *L) {
    lua_newtable(L);
    SInt32 versMaj, versMin, versBugFix;

    Gestalt(gestaltSystemVersionMajor, &versMaj);
    Gestalt(gestaltSystemVersionMinor, &versMin);
    Gestalt(gestaltSystemVersionBugFix, &versBugFix);

    lua_pushinteger(L, versMaj);
    lua_setfield(L, -2, "major");     

    lua_pushinteger(L, versMin);
    lua_setfield(L, -2, "minor");     

    lua_pushinteger(L, versBugFix);
    lua_setfield(L, -2, "bugfix");     

    lua_setfield(L, -2, "osx_version");
}
#endif


#if PLATFORM_LINUX || PLATFORM_IOS || PLATFORM_ANDROID || PLATFORM_MACOSX
static void
push_uname(lua_State *L) {
    struct utsname buf;
    lua_newtable(L);
    if (0 == uname(&buf)) {
        if (*buf.sysname) {
            lua_pushstring(L, buf.sysname);
            lua_setfield(L, -2, "sysname");     
        }
        if (*buf.nodename) {
            lua_pushstring(L, buf.nodename);
            lua_setfield(L, -2, "nodename");     
        }
        if (*buf.release) {
            lua_pushstring(L, buf.release);
            lua_setfield(L, -2, "release");     
        }
        if (*buf.version) {
            lua_pushstring(L, buf.version);
            lua_setfield(L, -2, "version");     
        }
        if (*buf.machine) {
            lua_pushstring(L, buf.machine);
            lua_setfield(L, -2, "machine");     
        }
    }
    lua_setfield(L, -2, "uname");
}
#endif

void
RAAT__script_init_misc(lua_State *L) {
    lua_getglobal(L, "raat");                  // push raat table
    lua_newtable(L);                           // push platformtable

#if PLATFORM_IOS 
    lua_pushstring(L, "ios");
    lua_setfield(L, -2, "os");           // platformtable.os = ...
    push_uname(L);
#elif PLATFORM_ANDROID 
    lua_pushstring(L, "android");
    lua_setfield(L, -2, "os");           // platformtable.os = ...
    push_uname(L);
#elif PLATFORM_MACOSX 
    lua_pushstring(L, "osx");
    lua_setfield(L, -2, "os");           // platformtable.os = ...
    push_uname(L);
#elif PLATFORM_LINUX 
    lua_pushstring(L, "linux");
    lua_setfield(L, -2, "os");           // platformtable.os = ...
    push_uname(L);
#elif PLATFORM_WINDOWS 
    lua_pushstring(L, "windows");
    lua_setfield(L, -2, "os");           // platformtable.os = ...
#endif
    lua_setfield(L, -2, "platform");     // raat.platform = platformtable

    lua_pop(L, 1);                       // pop raat table
}
