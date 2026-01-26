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
#include "raat_plugin_source_selection.h"

#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define CHECK_ARGUMENT_COUNT(L, n) { if (lua_gettop(L) != n) INVALID_ARGUMENT_COUNT(L); }
#define INVALID_ARGUMENT_COUNT(L) { LUA_ERROR(L, "invalid argument count"); }

typedef struct { 
    lua_State                  *lua;
    RAAT__SourceSelectionState  state;
    uv_thread_t                 thread;
    RAAT__Session              *session;
} CallbackState;

// pushes { status = "selected" | "not_selected" | "indeterminate" | "standby"
static void push_source_selection_state(lua_State *L, RAAT__SourceSelectionState *state) {
    lua_newtable(L);
    switch (state->status) {
        case RAAT__SOURCE_SELECTION_STATUS_SELECTED:         lua_pushstring(L, "selected");      break;
        case RAAT__SOURCE_SELECTION_STATUS_DESELECTED:       lua_pushstring(L, "not_selected");  break;
        case RAAT__SOURCE_SELECTION_STATUS_INDETERMINATE:    lua_pushstring(L, "indeterminate"); break;
        case RAAT__SOURCE_SELECTION_STATUS_STANDBY:          lua_pushstring(L, "standby");       break;
        default: RC__ASSERT(false); break;
    }
    lua_setfield(L, -2, "status");
}

static void source_selection_plugin_state_cb_inthread(RAAT__Session *self, void *userdata) {
    CallbackState *cbstate = userdata;
    lua_State *L = cbstate->lua;

    lua_getglobal(L, "raat");           // push raat table
    lua_getfield(L, -1, "source_selection");
    push_source_selection_state(L, &cbstate->state);
    lua_setfield(L, -2, "state");

    lua_getfield(L, -1, "state_change_handlers");
    lua_pushnil(L);  /* first key */
    while (lua_next(L, -2) != 0) {
        push_source_selection_state(L, &cbstate->state);
        RAAT__session_lua_pcall(cbstate->session, 1, 0);
    }
    lua_pop(L, 3);  // remove state_change_handlers, source_selection table, raat table

}

static void source_selection_plugin_state_cb(void *userdata, RAAT__SourceSelectionState *state) {
    CallbackState *cbstate = userdata;

    cbstate->state = *state;

    uv_thread_t self = uv_thread_self();
    if (uv_thread_equal(&self, &cbstate->thread)) {
        // if we have cause our own output to be lost, then call callback directly. 
        source_selection_plugin_state_cb_inthread(cbstate->session, userdata);
    } else {
        // otherwise, post is required
        RAAT__session_post(cbstate->session, source_selection_plugin_state_cb_inthread, cbstate);
    }
}

typedef struct { 
    lua_State                  *lua;
    RAAT__SourceSelectionState  state;
    uv_thread_t                 thread;
    RAAT__Session              *session;
    RC__Status                  status;
    char                       *info;
} RequestStandbyCallbackState;

static void source_selection_request_standby_cb_inthread(RAAT__Session *session, void *userdata) {
    RequestStandbyCallbackState *state = userdata;

    // load and invoke lua callback
    lua_pushlightuserdata(state->lua, state);
    lua_rawget(state->lua, LUA_REGISTRYINDEX);

    lua_pushboolean(state->lua, RC__STATUS_IS_SUCCESS(state->status));
    if (RC__STATUS_IS_SUCCESS(state->status)) {
        lua_pushnil(state->lua);
    } else {
        lua_pushstring(state->lua, RC__status_to_string(state->status));
    }
    if (state->info) {
        lua_pushstring(state->lua, state->info);
    } else {
        lua_pushnil(state->lua);
    }
    RAAT__session_lua_pcall(state->session, 3, 0);

    free(state->info);
    RC__free(RC__ALLOCATOR_DEFAULT, state);
}

static void source_selection_request_standby_cb(void *userdata, RC__Status status, json_t *info) {
    RequestStandbyCallbackState *state = userdata;
    uv_thread_t self = uv_thread_self();

    if (info) {
        state->info = json_dumps(info, 0);
    }

    state->status = status;

    if (uv_thread_equal(&self, &state->thread)) {
        // if we have cause our own output to be setup, then call callback directly. 
        source_selection_request_standby_cb_inthread(state->session, state);
    } else {
        // otherwise, post is required
        RAAT__session_post(state->session, source_selection_request_standby_cb_inthread, state);
    }
}

// function raat.source_selection.request_standby(function(is_success, errormsg) ... end)
static int source_selection_plugin_request_standby(lua_State *L) {
    RAAT__SourceSelectionPlugin *source_selection = RAAT__script_get_registry(L, "raat_source_selection_plugin");

    RequestStandbyCallbackState *state = RC__new0(RC__ALLOCATOR_DEFAULT, RequestStandbyCallbackState, 1);
    RC__ASSERT(state != NULL);

    state->lua    = L;
    state->thread = uv_thread_self();
    state->session = RAAT__script_get_registry(state->lua, "raat_session");

    // put state->function into the registry. This keeps function alive.
    lua_pushlightuserdata(L, state);  // push key (addr of state)
    lua_pushvalue(L, 1);              // push lua callback function
    lua_rawset(L, LUA_REGISTRYINDEX);

    source_selection->request_standby(source_selection, source_selection_request_standby_cb, state);

    return 0;
}

typedef struct { 
    lua_State                  *lua;
    RAAT__SourceSelectionState  state;
    uv_thread_t                 thread;
    RAAT__Session              *session;
    RC__Status                  status;
    char                       *info;
} RequestSourceCallbackState;

static void source_selection_request_source_cb_inthread(RAAT__Session *session, void *userdata) {
    RequestSourceCallbackState *state = userdata;

    // load and invoke lua callback
    lua_pushlightuserdata(state->lua, state);
    lua_rawget(state->lua, LUA_REGISTRYINDEX);

    lua_pushboolean(state->lua, RC__STATUS_IS_SUCCESS(state->status));
    if (RC__STATUS_IS_SUCCESS(state->status)) {
        lua_pushnil(state->lua);
    } else {
        lua_pushstring(state->lua, RC__status_to_string(state->status));
    }
    if (state->info) {
        lua_pushstring(state->lua, state->info);
    } else {
        lua_pushnil(state->lua);
    }
    RAAT__session_lua_pcall(state->session, 3, 0);

    free(state->info);
    RC__free(RC__ALLOCATOR_DEFAULT, state);
}

static void source_selection_request_source_cb(void *userdata, RC__Status status, json_t *info) {
    RequestSourceCallbackState *state = userdata;
    uv_thread_t self = uv_thread_self();

    if (info) {
        state->info = json_dumps(info, 0);
    }

    state->status = status;

    if (uv_thread_equal(&self, &state->thread)) {
        // if we have cause our own output to be setup, then call callback directly. 
        source_selection_request_source_cb_inthread(state->session, state);
    } else {
        // otherwise, post is required
        RAAT__session_post(state->session, source_selection_request_source_cb_inthread, state);
    }
}

// function raat.source_selection.request_source(function(is_success, errormsg) ... end)
static int source_selection_plugin_request_source(lua_State *L) {
    RAAT__SourceSelectionPlugin *source_selection = RAAT__script_get_registry(L, "raat_source_selection_plugin");

    RequestSourceCallbackState *state = RC__new0(RC__ALLOCATOR_DEFAULT, RequestSourceCallbackState, 1);
    RC__ASSERT(state != NULL);

    state->lua    = L;
    state->thread = uv_thread_self();
    state->session = RAAT__script_get_registry(state->lua, "raat_session");

    // put state->function into the registry. This keeps function alive.
    lua_pushlightuserdata(L, state);  // push key (addr of state)
    lua_pushvalue(L, 1);              // push lua callback function
    lua_rawset(L, LUA_REGISTRYINDEX);

    source_selection->request_source(source_selection, source_selection_request_source_cb, state);

    return 0;
}

static int source_selection_plugin_gc(lua_State *L) {
    RAAT__SourceSelectionPlugin *source_selection = RAAT__script_get_registry(L, "raat_source_selection_plugin");
    CallbackState *cbstate = RAAT__script_get_registry(L, "raat_source_selection_plugin_callback_state");
    RAAT__script_set_registry(L, "raat_source_selection_plugin_callback_state", NULL);
    source_selection->remove_state_listener(source_selection, source_selection_plugin_state_cb, cbstate);
    RC__free(RC__allocator_default(NULL), cbstate);
    return 0;
}

void RAAT__script_init_plugin_source_selection(lua_State *L, RAAT__Device *device) {
    RC__Status status;
    RAAT__SourceSelectionPlugin *source_selection = RAAT__device_get_source_selection_plugin(device);
    if (source_selection != NULL) {
        RAAT__script_set_registry(L, "raat_source_selection_plugin", source_selection);
        lua_getglobal(L, "raat");           // push raat table
        lua_newtable(L);                    // push new sourceselectiontable

        lua_pushboolean(L, true);
        lua_setfield(L, -2, "is_supported");

        lua_pushboolean(L, source_selection->request_standby ? true : false);
        lua_setfield(L, -2, "is_standby_supported");

        lua_newtable(L);
        lua_setfield(L, -2, "state_change_handlers");

        json_t *info;
        status = source_selection->get_info(source_selection, &info);
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

        // raat.source_selection.request_source = function(value) ... end 
        lua_pushcfunction(L, source_selection_plugin_request_source);
        lua_setfield(L, -2, "request_source");

        // raat.source_selection.request_standby = function(value) ... end 
        lua_pushcfunction(L, source_selection_plugin_request_standby);
        lua_setfield(L, -2, "request_standby");

        CallbackState *cbstate = RC__new0(RC__allocator_default(NULL), CallbackState, 1);
        cbstate->lua     = L;
        cbstate->thread = uv_thread_self();
        cbstate->session = RAAT__script_get_registry(L, "raat_session");
        RAAT__script_set_registry(L, "raat_source_selection_plugin_callback_state", cbstate);
        source_selection->add_state_listener(source_selection, source_selection_plugin_state_cb, cbstate);

        // raat.source_selection.state = { ... }
        RAAT__SourceSelectionState state = {0,};
        source_selection->get_state(source_selection, &state);
        push_source_selection_state(L, &state);
        lua_setfield(L, -2, "state");

        // create a metatable for the source_selection table so we can install a __gc metamethod.
        // This is required to unhook from source_selection state change notifications
        lua_newtable(L);
        lua_pushcfunction(L, source_selection_plugin_gc);
        lua_setfield(L, -2, "__gc");
        lua_setmetatable(L, -2);

        lua_setfield(L, -2, "source_selection");      // raat.source_selection = sourceselectiontable
        lua_remove(L, -1);                  // pop raat table
    } else {
        lua_getglobal(L, "raat");           // push raat table
        lua_newtable(L);                    // push new sourceselectiontable
        lua_pushboolean(L, false);
        lua_setfield(L, -2, "is_supported");
        lua_setfield(L, -2, "source_selection");      // raat.source_selection = sourceselectiontable
        lua_remove(L, -1);                  // pop raat table
    }
}

