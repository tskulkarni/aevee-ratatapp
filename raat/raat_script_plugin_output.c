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
#include "raat_plugin_output.h"

#include <string.h>
#include <stdio.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define CHECK_ARGUMENT_COUNT(L, n) { if (lua_gettop(L) != n) INVALID_ARGUMENT_COUNT(L); }
#define INVALID_ARGUMENT_COUNT(L) { LUA_ERROR(L, "invalid argument count"); }

// pushes { sample_type: "pcm" | "dsd", sample_rate: 44100, bits_per_sample: 16, channels: 2 }
static void push_format(lua_State *L, RAAT__StreamFormat *format) {
    lua_newtable(L);

    switch (format->sample_type) {
        case RAAT__SAMPLE_TYPE_PCM:    lua_pushstring(L, "pcm"); break;
        case RAAT__SAMPLE_TYPE_DSD:    lua_pushstring(L, "dsd"); break;
        default:                       RC__ASSERT(false);        break;
    }
    lua_setfield(L, -2, "sample_type");

    lua_pushinteger(L, format->sample_rate);
    lua_setfield(L, -2, "sample_rate");

    lua_pushinteger(L, format->bits_per_sample);
    lua_setfield(L, -2, "bits_per_sample");

    lua_pushinteger(L, format->channels);
    lua_setfield(L, -2, "channels");

    switch (format->sample_subtype) {
        case RAAT__SAMPLE_SUBTYPE_NONE:     lua_pushstring(L, "none"); break;
        case RAAT__SAMPLE_SUBTYPE_MQA:      lua_pushstring(L, "mqa"); break;
        case RAAT__SAMPLE_SUBTYPE_MQA_CORE: lua_pushstring(L, "mqa_core"); break;
        default:                            lua_pushstring(L, "none"); break;
    }
    lua_setfield(L, -2, "sample_subtype");

    if (format->mqa_original_sample_rate) {
        lua_pushinteger(L, format->mqa_original_sample_rate);
        lua_setfield(L, -2, "mqa_original_sample_rate");
    }
}

// does not return
static void output_plugin_error(lua_State *L, RC__Status status) {
    lua_pushstring(L, RC__status_to_string(status));
    lua_error(L);
}

typedef struct {
    lua_State *    lua;
    int            token;
    uv_thread_t    thread;
    RAAT__Session *session;
    char          *lost_reason;

    uv_mutex_t     lock;
    int            refcnt;
    bool           is_dead;

    // status for setup callback
    RC__Status  status;
} CallbackState;

typedef struct { 
    lua_State         *lua;
    uv_thread_t        thread;
    RAAT__Session     *session;
} MessageListenerState;

typedef struct {
    lua_State *    lua;
    uv_thread_t    thread;
    RAAT__Session *session;
    char          *message;
} MessageCallbackState;


static void ref_cb_state(CallbackState *state) {
    uv_mutex_lock(&state->lock);
    state->refcnt++;
    uv_mutex_unlock(&state->lock);
}

static void unref_cb_state(CallbackState *state) {
    uv_mutex_lock(&state->lock);
    int rc = --state->refcnt;
    uv_mutex_unlock(&state->lock);
    if (rc == 0) {
        uv_mutex_destroy(&state->lock);
        RC__free(RC__ALLOCATOR_DEFAULT, state);
    }
}

static void output_plugin_message_cb_inthread(RAAT__Session *self, void *userdata) {
    MessageCallbackState *cbstate = userdata;
    lua_State *L = cbstate->lua;

    lua_getglobal(L, "raat");           // push raat table
    lua_getfield(L, -1, "output");
    lua_getfield(L, -1, "message_handlers");
    lua_pushnil(L);  /* first key */
    while (lua_next(L, -2) != 0) {
        lua_pushstring(L, cbstate->message);
        RAAT__session_lua_pcall(cbstate->session, 1, 0);
    }
    lua_pop(L, 3);  // remove state_change_handlers, output table, raat table

    free(cbstate->message);
    RC__free(RC__ALLOCATOR_DEFAULT, cbstate);
}

static void output_plugin_message_cb(void *userdata, json_t *message) {
    MessageListenerState *listenerstate = userdata;

    MessageCallbackState *cbstate = RC__new0(RC__ALLOCATOR_DEFAULT, MessageCallbackState, 1);
    cbstate->lua     = listenerstate->lua;
    cbstate->thread = uv_thread_self();
    cbstate->session = listenerstate->session;
    cbstate->message = json_dumps(message, 0);

    uv_thread_t self = uv_thread_self();
    if (uv_thread_equal(&self, &listenerstate->thread)) {
        // if we have cause our own output to be lost, then call callback directly. 
        output_plugin_message_cb_inthread(listenerstate->session, cbstate);
    } else {
        // otherwise, post is required
        RAAT__session_post(listenerstate->session, output_plugin_message_cb_inthread, cbstate);
    }
}


static int output_plugin_gc(lua_State *L) {
    RAAT__OutputPlugin *output = RAAT__script_get_registry(L, "raat_output_plugin");

    MessageListenerState *listenerstate = RAAT__script_get_registry(L, "raat_output_plugin_message_listener_state");
    RAAT__script_set_registry(L, "raat_output_plugin_message_listener_state", NULL);
    output->remove_message_listener(output, output_plugin_message_cb, listenerstate);
    RC__free(RC__ALLOCATOR_DEFAULT, listenerstate);

    CallbackState *state = RAAT__script_get_registry(L, "raat_output_plugin_setup_token");
    if (state != NULL) {
        output->teardown(output, state->token);
        // clean up callbacks table
        lua_pushlightuserdata(state->lua, state);
        lua_pushnil(state->lua);
        lua_rawset(state->lua, LUA_REGISTRYINDEX);

        RAAT__script_set_registry(state->lua, "raat_output_plugin_setup_token", NULL);
        if (!state->is_dead) {
            state->is_dead = true;
            unref_cb_state(state);      
        }
    }
    return 0;
}

static void output_lost_cb_inthread(RAAT__Session *session, void *userdata) {
    CallbackState *state = userdata;

    // load and invoke lua callback
    lua_pushlightuserdata(state->lua, state);
    lua_rawget(state->lua, LUA_REGISTRYINDEX);
    lua_getfield(state->lua, -1, "on_output_lost");
    if (state->lost_reason) {
        lua_pushstring(state->lua, state->lost_reason);
        free(state->lost_reason);
    } else {
        lua_pushnil(state->lua);
    }
    lua_remove(state->lua, -3); // pop callbacks table
    RAAT__session_lua_pcall(state->session, 1, 0);

    CallbackState *curr_state = RAAT__script_get_registry(state->lua, "raat_output_plugin_setup_token");

    if (curr_state == state) {
        // clean up callbacks table
        lua_pushlightuserdata(state->lua, state);
        lua_pushnil(state->lua);
        lua_rawset(state->lua, LUA_REGISTRYINDEX);
        RAAT__OutputPlugin *output = RAAT__script_get_registry(state->lua, "raat_output_plugin");
        output->teardown(output, state->token);
        RAAT__script_set_registry(state->lua, "raat_output_plugin_setup_token", NULL);
    }

    if (!state->is_dead) {
        state->is_dead = true;
        unref_cb_state(state);      
    }

    unref_cb_state(state);      // for the ref applied before post
}

static void output_lost_cb(void *userdata, json_t *reason) {
    CallbackState *state = userdata;

    if (reason) {
        state->lost_reason = json_dumps(reason, 0);
    }

    ref_cb_state(state);

    RAAT__session_post(state->session, output_lost_cb_inthread, state);
}

static void output_setup_cb_inthread(RAAT__Session *session, void *userdata) {
    CallbackState *state = userdata;

    // load and invoke lua callback
    lua_pushlightuserdata(state->lua, state);
    lua_rawget(state->lua, LUA_REGISTRYINDEX);
    lua_getfield(state->lua, -1, "on_setup");
    lua_remove(state->lua, -2); // pop callbacks table

    lua_pushboolean(state->lua, RC__STATUS_IS_SUCCESS(state->status));
    if (RC__STATUS_IS_SUCCESS(state->status)) {
        lua_pushinteger(state->lua, state->token);
    } else {
        lua_pushstring(state->lua, RC__status_to_string(state->status));
    }
    RAAT__session_lua_pcall(state->session, 2, 0);

    // if setup failed, clean up the state object now. Otherwise it persists until teardown, or cb_lost
    if (!RC__STATUS_IS_SUCCESS(state->status)) {
        lua_pushlightuserdata(state->lua, state);
        lua_pushnil(state->lua);
        lua_rawset(state->lua, LUA_REGISTRYINDEX);
        RAAT__script_set_registry(state->lua, "raat_output_plugin_setup_token", NULL);
        if (!state->is_dead) {
            state->is_dead = true;
            unref_cb_state(state);      
        }
    }

    unref_cb_state(state);      // for the ref applied before post()
}

static void output_setup_cb(void *userdata, RC__Status status, int token) {
    CallbackState *state = userdata;
    uv_thread_t self = uv_thread_self();

    ref_cb_state(state);

    state->status = status;
    state->token  = token;

    RAAT__session_post(state->session, output_setup_cb_inthread, state);
}

// function raat.output.setup(format, { on_output_setup = function(is_success, token|errormsg) ... end,
//                                      on_output_lost  = function(reason) ... end })
static int output_plugin_setup(lua_State *L) {
    RAAT__StreamFormat format = {0,};

    RAAT__OutputPlugin *output = RAAT__script_get_registry(L, "raat_output_plugin");

    RAAT__unpack_stream_format(L, 1, &format);

    CallbackState *state = RC__new0(RC__ALLOCATOR_DEFAULT, CallbackState, 1);
    RC__ASSERT(state != NULL);

    state->lua    = L;
    uv_mutex_init(&state->lock);
    ref_cb_state(state);
    state->thread   = uv_thread_self();
    state->token    = RAAT__OUTPUT_TOKEN_INVALID;
    state->session  = RAAT__script_get_registry(state->lua, "raat_session");

    // put state->function into the registry. This keeps function alive.
    lua_pushlightuserdata(L, state);  // push key (addr of state)
    lua_pushvalue(L, 2);              // push lua callbacks table
    lua_rawset(L, LUA_REGISTRYINDEX);

    output->setup(output, &format, output_setup_cb, state, output_lost_cb, state);
    RAAT__script_set_registry(L, "raat_output_plugin_setup_token", state);              // set this after calling setup in case output was lost as a result of this setup call
    return 0;
}

// function raat.output.send_message(json_str)
static int output_plugin_send_message(lua_State *L) {
    RAAT__OutputPlugin *output = RAAT__script_get_registry(L, "raat_output_plugin");

    CHECK_ARGUMENT_COUNT(L, 1);

    if (!lua_isstring(L, 1)) {
        LUA_ERROR(L, "String argument expected");
    }

    if (!output->send_message) {
        return 0;
    }

    size_t len;
    const char *json_str = lua_tolstring(L, 1, &len);

    json_error_t error;
    json_t *request  = json_loadb(json_str, len, 0, &error);
    if (request) {
        output->send_message(output, request);
    } else {
        LUA_ERROR(L, "Invalid JSON");
    }

    json_decref(request);

    return 0;
}

// function raat.output.start(token, time, streamtime, stream)
static int output_plugin_start(lua_State *L) {
    RAAT__OutputPlugin *output = RAAT__script_get_registry(L, "raat_output_plugin");
    int token = (int)luaL_checkinteger(L, 1);
    lua_Integer walltime = luaL_checkinteger(L, 2);
    lua_Integer streamtime = luaL_checkinteger(L, 3);

    RAAT__Stream *stream = RAAT__check_stream(L, 4);
    RC__Status status = output->start(output, token, walltime, streamtime, stream);
    if (!RC__STATUS_IS_SUCCESS(status)) output_plugin_error(L, status);
    return 0;
}

// function raat.output.teardown(token) => nil
static int output_plugin_teardown(lua_State *L) {
    RAAT__OutputPlugin *output = RAAT__script_get_registry(L, "raat_output_plugin");
    int token = (int)luaL_checkinteger(L, 1);
    RC__Status status = output->teardown(output, token);
    if (!RC__STATUS_IS_SUCCESS(status)) output_plugin_error(L, status);

    CallbackState *state = RAAT__script_get_registry(L, "raat_output_plugin_setup_token");

    // if the last requested state matches this token, clean it up
    if (state != NULL && state->token == token) {
        lua_pushlightuserdata(state->lua, state);
        lua_pushnil(state->lua);
        lua_rawset(state->lua, LUA_REGISTRYINDEX);
        RAAT__script_set_registry(state->lua, "raat_output_plugin_setup_token", NULL);
        if (!state->is_dead) {
            state->is_dead = true;
            unref_cb_state(state);      
        }
    }

    return 0;
}

// function raat.output.force_teardown(reason_as_json_str_or_nil) => nil
static int output_plugin_force_teardown(lua_State *L) {
    RAAT__OutputPlugin *output = RAAT__script_get_registry(L, "raat_output_plugin");

    json_t *reason = NULL;

    if (lua_isstring(L, 1)) {
        json_error_t error;
        size_t len;
        const char *json_str = lua_tolstring(L, 1, &len);
        reason = json_loadb(json_str, len, 0, &error);
        if (!reason) {
            LUA_ERROR(L, "Invalid JSON");
        }
    }

    RC__Status status = output->force_teardown(output, reason);
    if (!RC__STATUS_IS_SUCCESS(status)) output_plugin_error(L, status);
    return 0;
}

// function raat.output.stop(token) => nil
static int output_plugin_stop(lua_State *L) {
    RAAT__OutputPlugin *output = RAAT__script_get_registry(L, "raat_output_plugin");
    int token = (int)luaL_checkinteger(L, 1);
    RC__Status status = output->stop(output, token);
    if (!RC__STATUS_IS_SUCCESS(status)) output_plugin_error(L, status);
    return 0;
}

// function raat.output.get_output_delay(token) => delay || nil if not implemented
static int output_plugin_get_output_delay(lua_State *L) {
    RAAT__OutputPlugin *output = RAAT__script_get_registry(L, "raat_output_plugin");
    int token = (int)luaL_checkinteger(L, 1);
    int64_t delay;
    if (output->get_output_delay) {
        RC__Status status = output->get_output_delay(output, token, &delay);
        if (!RC__STATUS_IS_SUCCESS(status)) output_plugin_error(L, status);
        lua_pushinteger(L, delay);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// function raat.output.get_local_time(token) => time
static int output_plugin_get_local_time(lua_State *L) {
    RAAT__OutputPlugin *output = RAAT__script_get_registry(L, "raat_output_plugin");
    int token = (int)luaL_checkinteger(L, 1);
    int64_t time;
    RC__Status status = output->get_local_time(output, token, &time);
    if (!RC__STATUS_IS_SUCCESS(status)) output_plugin_error(L, status);
    lua_pushinteger(L, time);
    return 1;
}

// function raat.output.set_remote_time(token, time) => nil
static int output_plugin_set_remote_time(lua_State *L) {
    RAAT__OutputPlugin *output = RAAT__script_get_registry(L, "raat_output_plugin");
    int token = (int)luaL_checkinteger(L, 1);
    lua_Integer offset = (lua_Integer)luaL_checknumber(L, 2);
    bool new_source = lua_toboolean(L, 3);

    RC__Status status = output->set_remote_time(output, token, offset, new_source);
    if (!RC__STATUS_IS_SUCCESS(status)) output_plugin_error(L, status);
    return 0;
}

// function raat.output.refresh_supported_formats()
static int output_plugin_refresh_supported_formats(lua_State *L) {
    RAAT__OutputPlugin *output = RAAT__script_get_registry(L, "raat_output_plugin");

    // populate supported formats list
    size_t nformats;
    RC__Status status;
    RAAT__StreamFormat *formats = NULL;
    status = output->get_supported_formats(output, RC__ALLOCATOR_DEFAULT, &nformats, &formats);
    RC__ASSERT(RC__STATUS_IS_SUCCESS(status));

    lua_getglobal(L, "raat");           // push raat table
    lua_getfield(L, -1, "output");
    lua_newtable(L);        // push [] for formats
    size_t i;
    for (i = 0; i < nformats; i++) {
        push_format(L, &formats[i]);
        lua_seti(L, -2, i+1);
    }
    lua_setfield(L, -2, "supported_formats");
    lua_pop(L, 2);  // remove output table, raat table

    RC__free(RC__ALLOCATOR_DEFAULT, formats);
    return 0;
}

void RAAT__script_init_plugin_output(lua_State *L, RAAT__Device *device) {
    RAAT__OutputPlugin *output = RAAT__device_get_output_plugin(device);
    if (output != NULL) {
        RAAT__script_set_registry(L, "raat_output_plugin", output);
        lua_getglobal(L, "raat");           // push raat table
        lua_newtable(L);                    // push new outputtable

        lua_pushboolean(L, true);
        lua_setfield(L, -2, "is_supported");

        lua_pushboolean(L, true);
        lua_setfield(L, -2, "is_refresh_supported_formats_supported");

        // message handlers list
        lua_newtable(L);
        lua_setfield(L, -2, "message_handlers");

        // populate supported formats list
        size_t nformats;
        RC__Status status;
        RAAT__StreamFormat *formats = NULL;
        status = output->get_supported_formats(output, RC__ALLOCATOR_DEFAULT, &nformats, &formats);
        RC__ASSERT(RC__STATUS_IS_SUCCESS(status));

        if (output->add_message_listener != NULL) {
            MessageListenerState *cbstate = RC__new0(RC__ALLOCATOR_DEFAULT, MessageListenerState, 1);
            cbstate->lua     = L;
            cbstate->thread = uv_thread_self();
            cbstate->session = RAAT__script_get_registry(L, "raat_session");
            RAAT__script_set_registry(L, "raat_output_plugin_message_listener_state", cbstate);
            output->add_message_listener(output, output_plugin_message_cb, cbstate);
        }

        json_t *info;
        status = output->get_info(output, &info);
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

        lua_newtable(L);        // push [] for formats
        size_t i;
        for (i = 0; i < nformats; i++) {
            push_format(L, &formats[i]);
            lua_seti(L, -2, i+1);
        }
        lua_setfield(L, -2, "supported_formats");
        RC__free(RC__ALLOCATOR_DEFAULT, formats);

        // raat.output.set_mute = function(mute) ... end
        lua_pushcfunction(L, output_plugin_setup);
        lua_setfield(L, -2, "setup");

        lua_pushcfunction(L, output_plugin_teardown);
        lua_setfield(L, -2, "teardown");

        lua_pushcfunction(L, output_plugin_force_teardown);
        lua_setfield(L, -2, "force_teardown");

        lua_pushcfunction(L, output_plugin_start);
        lua_setfield(L, -2, "start");

        lua_pushcfunction(L, output_plugin_send_message);
        lua_setfield(L, -2, "send_message");

        lua_pushcfunction(L, output_plugin_get_local_time);
        lua_setfield(L, -2, "get_local_time");

        lua_pushboolean(L, true);
        lua_setfield(L, -2, "is_get_output_delay_supported");

        lua_pushcfunction(L, output_plugin_get_output_delay);
        lua_setfield(L, -2, "get_output_delay");

        lua_pushcfunction(L, output_plugin_set_remote_time);
        lua_setfield(L, -2, "set_remote_time");

        lua_pushcfunction(L, output_plugin_stop);
        lua_setfield(L, -2, "stop");

        lua_pushcfunction(L, output_plugin_refresh_supported_formats);
        lua_setfield(L, -2, "refresh_supported_formats");

        // create a metatable for the output table so we can install a __gc metamethod.
        // This is required to unhook from output state change notifications
        lua_newtable(L);
        lua_pushcfunction(L, output_plugin_gc);
        lua_setfield(L, -2, "__gc");
        lua_setmetatable(L, -2);

        lua_setfield(L, -2, "output");      // raat.output = outputtable
        lua_pop(L, 1);                      // pop raat table

    } else {
        lua_getglobal(L, "raat");           // push raat table
        lua_newtable(L);                    // push new outputtable
        lua_pushboolean(L, false);
        lua_setfield(L, -2, "is_supported");
        lua_setfield(L, -2, "output");      // raat.output = outputtable
        lua_pop(L, 1);                      // pop raat table
    }
}
