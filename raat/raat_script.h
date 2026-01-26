//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_SCRIPT_H
#define INCLUDED_RAAT_SCRIPT_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"

#include "raat_session.h"
#include "raat_info.h"
#include "raat_log.h"
#include "raat_device.h"
#include "raat_stream.h"

#include <lua.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LUA_ERROR(L, s) { lua_pushliteral(L, s); lua_error(L); return 0; }

void *RAAT__script_get_registry(lua_State *lua, const char *key);
void RAAT__script_set_registry(lua_State *lua, const char *key, void *val);

/* defined in raat_script_*.c */
void RAAT__script_init_session(lua_State *lua, RAAT__Session *session, struct sockaddr_storage *remote_addr);
void RAAT__script_init_log(lua_State *lua, RAAT__Log *log);
void RAAT__script_init_info(lua_State *lua, RAAT__Info *info);
void RAAT__script_init_buffer(lua_State *lua);
void RAAT__script_init_stream(lua_State *lua);
void RAAT__script_init_plugin_output(lua_State *lua, RAAT__Device *device);
void RAAT__script_init_plugin_volume(lua_State *lua, RAAT__Device *device);
void RAAT__script_init_plugin_source_selection(lua_State *lua, RAAT__Device *device);
void RAAT__script_init_plugin_transport(lua_State *lua, RAAT__Device *device);

typedef struct {
    size_t   len;
    size_t   pos;
    size_t   capacity;
    uint8_t *data;
} RAAT__ScriptBuffer;

RAAT__ScriptBuffer *RAAT__check_buffer(lua_State * L, int n);
RAAT__Stream       *RAAT__check_stream(lua_State * L, int n);

void *lua_alloc(lua_State *L, void *ptr, int osize, int nsize);

void RAAT__unpack_stream_format(lua_State *L, int n, RAAT__StreamFormat *out_format);

void RAAT__script_init_misc(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif

