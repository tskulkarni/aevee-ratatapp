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
#include "raat_plugin_volume.h"

#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define CHECK_ARGUMENT_COUNT(L, n) { if (lua_gettop(L) != n) INVALID_ARGUMENT_COUNT(L); }
#define INVALID_ARGUMENT_COUNT(L) { LUA_ERROR(L, "invalid argument count"); }

// pushes { type = "none" | "number | "db" | "incremental", min_volume = 0, max_volume = 100, volume = 23, is_muted = false }
static void push_volume_state(lua_State *L, RAAT__VolumeState *state) {
    lua_newtable(L);

    switch (state->volume_type) {
        case RAAT__VOLUME_TYPE_NONE:          lua_pushstring(L, "none");          break;
        case RAAT__VOLUME_TYPE_NUMBER:        lua_pushstring(L, "number");        break;
        case RAAT__VOLUME_TYPE_DB:            lua_pushstring(L, "db");            break;
        case RAAT__VOLUME_TYPE_INCREMENTAL:   lua_pushstring(L, "incremental");   break;
        case RAAT__VOLUME_TYPE_HIDDEN_NUMBER: lua_pushstring(L, "hidden_number"); break;
        default: RC__ASSERT(false); break;
    }
    lua_setfield(L, -2, "type");

    lua_pushnumber(L, state->min_volume);
    lua_setfield(L, -2, "min");
    
    lua_pushnumber(L, state->max_volume);
    lua_setfield(L, -2, "max");

    lua_pushnumber(L, state->volume_value);
    lua_setfield(L, -2, "value");

    lua_pushnumber(L, state->db_min_volume);
    lua_setfield(L, -2, "db_min");

    lua_pushnumber(L, state->db_max_volume);
    lua_setfield(L, -2, "db_max");

    lua_pushnumber(L, state->volume_step);
    lua_setfield(L, -2, "step");

    lua_pushboolean(L, state->mute_value != 0); lua_setfield(L, -2, "mute");
}

typedef struct { 
    lua_State         *lua;
    RAAT__VolumeState  state;
    uv_thread_t        thread;
    RAAT__Session     *session;
} CallbackState;

static void volume_plugin_state_cb_inthread(RAAT__Session *self, void *userdata) {
    CallbackState *cbstate = userdata;
    lua_State *L = cbstate->lua;

    lua_getglobal(L, "raat");           // push raat table
    lua_getfield(L, -1, "volume");
    push_volume_state(L, &cbstate->state);
    lua_setfield(L, -2, "state");

    lua_getfield(L, -1, "state_change_handlers");
    lua_pushnil(L);  /* first key */
    while (lua_next(L, -2) != 0) {
        push_volume_state(L, &cbstate->state);
        RAAT__session_lua_pcall(cbstate->session, 1, 0);
    }
    lua_pop(L, 3);  // remove state_change_handlers, volume table, raat table
}

static void volume_plugin_state_cb(void *userdata, RAAT__VolumeState *state) {
    CallbackState *cbstate = userdata;

    cbstate->state = *state;

    uv_thread_t self = uv_thread_self();
    if (self && uv_thread_equal(&self, &cbstate->thread)) {
        // if we have cause our own volume change, update directly
        volume_plugin_state_cb_inthread(cbstate->session, userdata);
    } else {
        // otherwise, post is required
        RAAT__session_post(cbstate->session, volume_plugin_state_cb_inthread, cbstate);
    }
}

static int volume_plugin_set_volume(lua_State *L) {
    RAAT__VolumePlugin *volume = RAAT__script_get_registry(L, "raat_volume_plugin");
    CHECK_ARGUMENT_COUNT(L, 1);
    double value = luaL_checknumber(L, 1);
    if (!volume->set_volume) LUA_ERROR(L, "set_volume is not implemented");
    volume->set_volume(volume, value);
    return 0;
}

static int volume_plugin_toggle_mute(lua_State *L) {
    RAAT__VolumePlugin *volume = RAAT__script_get_registry(L, "raat_volume_plugin");
    CHECK_ARGUMENT_COUNT(L, 0);
    if (!volume->toggle_mute) LUA_ERROR(L, "toggle_mute is not implemented");
    volume->toggle_mute(volume);
    return 0;
}

static int volume_plugin_increment_volume(lua_State *L) {
    RAAT__VolumePlugin *volume = RAAT__script_get_registry(L, "raat_volume_plugin");
    CHECK_ARGUMENT_COUNT(L, 1);

    size_t len;
    const char *howstr = lua_tolstring(L, 1, &len);
    if (!volume->increment_volume) LUA_ERROR(L, "increment_volume is not implemented");
    if (!strcmp(howstr, "up")) {
        volume->increment_volume(volume, RAAT__VOLUME_INCREMENT_UP);
    } else if (!strcmp(howstr, "down")) {
        volume->increment_volume(volume, RAAT__VOLUME_INCREMENT_DOWN);
    } else {
        LUA_ERROR(L, "invalid value for 'how'");
    }
    return 0;
}

static int volume_plugin_set_mute(lua_State *L) {
    RAAT__VolumePlugin *volume = RAAT__script_get_registry(L, "raat_volume_plugin");
    CHECK_ARGUMENT_COUNT(L, 1);
    bool value = lua_toboolean(L, 1) != 0;
    if (!volume->set_mute) LUA_ERROR(L, "set_mute is not implemented");
    volume->set_mute(volume, value);
    return 0;
}

static int volume_plugin_gc(lua_State *L) {
    RAAT__VolumePlugin *volume = RAAT__script_get_registry(L, "raat_volume_plugin");
    CallbackState *cbstate = RAAT__script_get_registry(L, "raat_volume_plugin_callback_state");
    RAAT__script_set_registry(L, "raat_volume_plugin_callback_state", NULL);
    volume->remove_state_listener(volume, volume_plugin_state_cb, cbstate);
    RC__free(RC__allocator_default(NULL), cbstate);
    return 0;
}

void RAAT__script_init_plugin_volume(lua_State *L, RAAT__Device *device) {
    RC__Status status;
    RAAT__VolumePlugin *volume = RAAT__device_get_volume_plugin(device);
    if (volume != NULL) {
        RAAT__script_set_registry(L, "raat_volume_plugin", volume);
        lua_getglobal(L, "raat");           // push raat table
        lua_newtable(L);                    // push new volumetable

        lua_pushboolean(L, true);
        lua_setfield(L, -2, "is_supported");

        lua_pushboolean(L, true);
        lua_setfield(L, -2, "is_double_volume_supported");

        lua_newtable(L);
        lua_setfield(L, -2, "state_change_handlers");

        json_t *info;
        status = volume->get_info(volume, &info);
        RC__ASSERT(RC__STATUS_IS_SUCCESS(status));

        if (info) {
            char *s = json_dumps(info, 0);
            lua_pushstring(L, s);
            lua_setfield(L, -2, "info");
            free(s);
            json_decref(info);
        } else {
            lua_pushnil(L);
            lua_setfield(L, -2, "info");
        }

        // raat.volume.set_volume = function(value) ... end 
        lua_pushcfunction(L, volume_plugin_set_volume);
        lua_setfield(L, -2, "set_volume");

        // raat.volume.set_mute = function(mute) ... end
        lua_pushcfunction(L, volume_plugin_set_mute);
        lua_setfield(L, -2, "set_mute");

        // raat.volume.increment_volume = function("up" | "down") ... end 
        lua_pushcfunction(L, volume_plugin_increment_volume);
        lua_setfield(L, -2, "increment_volume");

        // raat.volume.toggle_mute = function() ... end 
        lua_pushcfunction(L, volume_plugin_toggle_mute);
        lua_setfield(L, -2, "toggle_mute");

        CallbackState *cbstate = RC__new0(RC__allocator_default(NULL), CallbackState, 1);
        cbstate->lua     = L;
        cbstate->thread = uv_thread_self();
        cbstate->session = RAAT__script_get_registry(L, "raat_session");
        RAAT__script_set_registry(L, "raat_volume_plugin_callback_state", cbstate);
        volume->add_state_listener(volume, volume_plugin_state_cb, cbstate);

        // raat.volume.state = { ... }
        RAAT__VolumeState state = {0,};
        volume->get_state(volume, &state);
        push_volume_state(L, &state);
        lua_setfield(L, -2, "state");

        // create a metatable for the volume table so we can install a __gc metamethod.
        // This is required to unhook from volume state change notifications
        lua_newtable(L);
        lua_pushcfunction(L, volume_plugin_gc);
        lua_setfield(L, -2, "__gc");
        lua_setmetatable(L, -2);

        lua_setfield(L, -2, "volume");      // raat.volume = volumetable
        lua_remove(L, -1);                  // pop raat table
    } else {
        lua_getglobal(L, "raat");           // push raat table
        lua_newtable(L);                    // push new volumetable
        lua_pushboolean(L, false);
        lua_setfield(L, -2, "is_supported");
        lua_setfield(L, -2, "volume");      // raat.volume = volumetable
        lua_remove(L, -1);                  // pop raat table
    }
}

