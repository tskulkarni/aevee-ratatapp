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
#include "raat_plugin_transport.h"

#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define CHECK_ARGUMENT_COUNT(L, n) { if (lua_gettop(L) != n) INVALID_ARGUMENT_COUNT(L); }
#define INVALID_ARGUMENT_COUNT(L) { LUA_ERROR(L, "invalid argument count"); }

typedef struct { 
    lua_State         *lua;
    uv_thread_t        thread;
    RAAT__Session     *session;
} CallbackState;

typedef struct { 
    CallbackState *cbstate;
    char          *json;
} ControlState;

// does not return
static void transport_plugin_error(lua_State *L, RC__Status status) {
    lua_pushstring(L, RC__status_to_string(status));
    lua_error(L);
}

static void transport_plugin_control_cb_inthread(RAAT__Session *self, void *userdata) {
    ControlState  *controlstate = userdata;
    CallbackState *cbstate      = controlstate->cbstate;

    lua_State *L = cbstate->lua;

    lua_getglobal(L, "raat");           // push raat table
    lua_getfield(L, -1, "transport");
    lua_getfield(L, -1, "control_handlers");
    lua_pushnil(L);  /* first key */
    while (lua_next(L, -2) != 0) {
        lua_pushstring(L, controlstate->json);
        RAAT__session_lua_pcall(cbstate->session, 1, 0);
    }
    lua_pop(L, 3);  // remove control_handlers, transport table, raat table

    free(controlstate->json);
    RC__free(RC__ALLOCATOR_DEFAULT, controlstate);
}

static void transport_plugin_control_cb(void *userdata, json_t *control) {
    CallbackState *cbstate = userdata;

    ControlState *controlstate = RC__new0(RC__ALLOCATOR_DEFAULT, ControlState, 1);
    controlstate->cbstate = cbstate;
    controlstate->json = json_dumps(control, 0);

    uv_thread_t self = uv_thread_self();
    if (uv_thread_equal(&self, &cbstate->thread)) {
        // if we have cause our own output to be lost, then call callback directly. 
        transport_plugin_control_cb_inthread(cbstate->session, controlstate);
    } else {
        // otherwise, post is required
        RAAT__session_post(cbstate->session, transport_plugin_control_cb_inthread, controlstate);
    }
}

// raat.transport.update_artwork(mimetype, luastring)
static int transport_plugin_update_artwork(lua_State *L) {
    RAAT__TransportPlugin *transport = RAAT__script_get_registry(L, "raat_transport_plugin");
    RC__Status status;

    if (!transport->update_artwork) return 0;

    if (lua_isnil(L, 1)) {
        status = transport->update_artwork(transport, NULL, NULL, 0);
        if (!RC__STATUS_IS_SUCCESS(status)) transport_plugin_error(L, status);
        return 0;
    }

    CHECK_ARGUMENT_COUNT(L, 2);

    if (!lua_isstring(L, 1)) LUA_ERROR(L, "invalid argument to update_artwork");
    if (!lua_isstring(L, 2)) LUA_ERROR(L, "invalid argument to update_artwork");

    size_t len;
    const char *mimetype    = lua_tostring(L, 1);
    const char *data        = lua_tolstring(L, 2, &len);

    status = transport->update_artwork(transport, mimetype, (void*)data, len);
    if (!RC__STATUS_IS_SUCCESS(status)) transport_plugin_error(L, status);
    return 0;
}

// raat.transport.update_nowplaying(luastring)
static int transport_plugin_update_status(lua_State *L) {
    RAAT__TransportPlugin *transport = RAAT__script_get_registry(L, "raat_transport_plugin");
    if (!transport->update_status) return 0;

    CHECK_ARGUMENT_COUNT(L, 1);

    if (!lua_isstring(L, 1)) LUA_ERROR(L, "invalid argument to update_status");

    size_t len;
    const char *json_str = lua_tolstring(L, 1, &len);

    json_error_t error;
    json_t *json = json_loadb((const char*)json_str, (size_t)len, 0, &error);

    if (json == NULL) {
        LUA_ERROR(L, "invalid JSON");
    }

    RC__Status status = transport->update_status(transport, json);
    json_decref(json);
    if (!RC__STATUS_IS_SUCCESS(status)) transport_plugin_error(L, status);
    return 0;
}

static int transport_plugin_gc(lua_State *L) {
    RAAT__TransportPlugin *transport = RAAT__script_get_registry(L, "raat_transport_plugin");
    CallbackState *cbstate = RAAT__script_get_registry(L, "raat_transport_plugin_callback_state");
    RAAT__script_set_registry(L, "raat_transport_plugin_callback_state", NULL);
    transport->remove_control_listener(transport, transport_plugin_control_cb, cbstate);
    RC__free(RC__ALLOCATOR_DEFAULT, cbstate);
    return 0;
}

void RAAT__script_init_plugin_transport(lua_State *L, RAAT__Device *device) {
    RC__Status status;
    RAAT__TransportPlugin *transport = RAAT__device_get_transport_plugin(device);
    if (transport != NULL) {
        RAAT__script_set_registry(L, "raat_transport_plugin", transport);
        lua_getglobal(L, "raat");           // push raat table
        lua_newtable(L);                    // push new transporttable

        lua_pushboolean(L, true);
        lua_setfield(L, -2, "is_supported");

        lua_pushboolean(L, transport->update_status != NULL);
        lua_setfield(L, -2, "is_update_status_supported");

        lua_pushboolean(L, transport->update_artwork != NULL);
        lua_setfield(L, -2, "is_update_artwork_supported");

        lua_newtable(L);
        lua_setfield(L, -2, "control_handlers");

        json_t *info;
        status = transport->get_info(transport, &info);
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

        // raat.transport.set_mute = function(value) ... end 
        lua_pushcfunction(L, transport_plugin_update_status);
        lua_setfield(L, -2, "update_status");

        // raat.transport.set_mute = function(mute) ... end
        lua_pushcfunction(L, transport_plugin_update_artwork);
        lua_setfield(L, -2, "update_artwork");

        CallbackState *cbstate = RC__new0(RC__ALLOCATOR_DEFAULT, CallbackState, 1);
        cbstate->lua     = L;
        cbstate->thread = uv_thread_self();
        cbstate->session = RAAT__script_get_registry(L, "raat_session");
        RAAT__script_set_registry(L, "raat_transport_plugin_callback_state", cbstate);
        transport->add_control_listener(transport, transport_plugin_control_cb, cbstate);

        // create a metatable for the transport table so we can install a __gc metamethod.
        // This is required to unhook from transport state change notifications
        lua_newtable(L);
        lua_pushcfunction(L, transport_plugin_gc);
        lua_setfield(L, -2, "__gc");
        lua_setmetatable(L, -2);

        lua_setfield(L, -2, "transport");      // raat.transport = transporttable
        lua_remove(L, -1);                  // pop raat table
    } else {
        lua_getglobal(L, "raat");           // push raat table
        lua_newtable(L);                    // push new transporttable
        lua_pushboolean(L, false);
        lua_setfield(L, -2, "is_supported");
        lua_setfield(L, -2, "transport");      // raat.transport = transporttable
        lua_remove(L, -1);                  // pop raat table
    }
}

