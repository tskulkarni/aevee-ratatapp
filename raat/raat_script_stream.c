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

RAAT__Stream *RAAT__check_stream(lua_State * L, int n) {
    return *(RAAT__Stream **)luaL_checkudata(L, n, "RAAT__Stream");
}

// stream.new(format, nsamples) => stream
static int stream_new(lua_State *L) {
    RAAT__Log *log = RAAT__script_get_registry(L, "raat_log");
    RAAT__Stream *ret;
    RC__Status status;
    RAAT__StreamFormat format;

    RAAT__unpack_stream_format(L, 1, &format);
    int nsamples = (int)luaL_checkinteger(L, 2);

    status = RAAT__stream_new(RC__ALLOCATOR_DEFAULT, log, &format, nsamples, &ret);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        lua_pushstring(L, RC__status_to_string(status));
        lua_error(L);
    }

    RAAT__Stream **streamptr = (RAAT__Stream**)lua_newuserdata(L, sizeof(RAAT__Stream*));
    *streamptr = ret;
    luaL_getmetatable(L, "RAAT__Stream");
    lua_setmetatable(L, -2);

    return 1;
}

static int stream_decref(lua_State *L) {
    RAAT__Stream *stream = RAAT__check_stream(L, 1);
    RAAT__stream_decref(stream);
    return 0;
}

static int stream_clear(lua_State *L) {
    RAAT__Stream *stream = RAAT__check_stream(L, 1);
    RAAT__stream_clear(stream);
    return 0;
}

// stream:write_chmap(streamtime, gain, peak, buffer|string, nsamples, chmap)
static int stream_write_chmap(lua_State *L) {
    RAAT__Stream *stream = RAAT__check_stream(L, 1);
    lua_Integer streamtime = luaL_checkinteger(L, 2);
    double gain = luaL_checknumber(L, 3);
    double peak = luaL_checknumber(L, 4);

    void *data;
    size_t len;

    // accept buffer or string
    RAAT__ScriptBuffer *buffer  = luaL_testudata(L, 5, "RAAT__ScriptBuffer");
    if (buffer != NULL) {
        data = buffer->data;
        len  = buffer->len;
    } else {
        data = (void*)luaL_checklstring(L, 5, &len);
    }

    lua_Integer nsamples = luaL_checkinteger(L, 6);
    lua_Integer chmap    = luaL_checkinteger(L, 7);

    if ((size_t)RAAT__stream_format_compute_buffer_size(RAAT__stream_format(stream), nsamples) < len) {
        lua_pushliteral(L, "nsamples exceeds buffer size");
        lua_error(L);
    }

    RC__Status status = RAAT__stream_write_chmap(stream, streamtime, gain, peak, data, nsamples, chmap);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        lua_pushstring(L, RC__status_to_string(status));
        lua_error(L);
    }

    return 0;
}

// stream:write(streamtime, buffer|string, nsamples)
static int stream_write(lua_State *L) {
    RAAT__Stream *stream = RAAT__check_stream(L, 1);
    lua_Integer streamtime = luaL_checkinteger(L, 2);
    double gain = luaL_checknumber(L, 3);
    double peak = luaL_checknumber(L, 4);

    void *data;
    size_t len;

    // accept buffer or string
    RAAT__ScriptBuffer *buffer  = luaL_testudata(L, 5, "RAAT__ScriptBuffer");
    if (buffer != NULL) {
        data = buffer->data;
        len  = buffer->len;
    } else {
        data = (void*)luaL_checklstring(L, 5, &len);
    }

    lua_Integer nsamples = luaL_checkinteger(L, 6);

    if ((size_t)RAAT__stream_format_compute_buffer_size(RAAT__stream_format(stream), nsamples) < len) {
        lua_pushliteral(L, "nsamples exceeds buffer size");
        lua_error(L);
    }

    RC__Status status = RAAT__stream_write(stream, streamtime, gain, peak, data, nsamples);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        lua_pushstring(L, RC__status_to_string(status));
        lua_error(L);
    }

    return 0;
}

// stream:get_stats()
static int stream_get_stats(lua_State *L) {
    int argc = lua_gettop(L);
    RAAT__Stream *stream = RAAT__check_stream(L, 1);
    RAAT__StreamStats stats;

    memset(&stats, 0, sizeof(stats));

    RAAT__stream_get_stats(stream, &stats);

    if (argc == 2) {
        lua_pushvalue(L, 2);            // if an arg is provided, fill stats into that table then return it. Otherwise create a new table
    } else {
        lua_newtable(L);
    }
    lua_pushinteger(L, stats.write_count);      lua_setfield(L, -2, "write_count");
    lua_pushinteger(L, stats.read_count);       lua_setfield(L, -2, "read_count");
    lua_pushinteger(L, stats.samples_in);       lua_setfield(L, -2, "samples_in");
    lua_pushinteger(L, stats.samples_out);      lua_setfield(L, -2, "samples_out");
    lua_pushinteger(L, stats.dropout_count);    lua_setfield(L, -2, "dropout_count");
    lua_pushinteger(L, stats.samples_dropped);  lua_setfield(L, -2, "samples_dropped");
    lua_pushinteger(L, stats.overrun_count);    lua_setfield(L, -2, "overrun_count");
    lua_pushinteger(L, stats.samples_overrun);  lua_setfield(L, -2, "samples_overrun");
    lua_pushinteger(L, stats.capacity_samples); lua_setfield(L, -2, "capacity_samples");
    lua_pushinteger(L, stats.fill_samples);     lua_setfield(L, -2, "fill_samples");

    return 1;
}

static const luaL_Reg streamlib[] = {
    { "new",                    stream_new                      },
    { "write",                  stream_write                    },
    { "write_chmap",            stream_write_chmap              },
    { "clear",                  stream_clear                    },
    { "get_stats",              stream_get_stats                },
    { "__gc",                   stream_decref                   },
    { NULL,                     NULL                            },
};

static int open_stream(lua_State *L) {
    luaL_newmetatable(L, "RAAT__Stream");
    luaL_setfuncs(L, streamlib, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -1, "__index");
    lua_pushboolean(L, true);
    lua_setfield(L, -2, "has_write_chmap");
    return 1;
}

void RAAT__script_init_stream(lua_State *L) {
    luaL_requiref(L, "stream", open_stream, 1);
}
