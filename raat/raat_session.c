//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_session.h" 
#include "raat_script.h"
#include <uv.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <luv.h>

#define RAAT__CURRENT_LOG (self->log)

struct RAAT__Session_s {
    RC__Allocator                    *alloc;
    RAAT__Log                        *log;
    RAAT__Info                       *info;
    uv_loop_t                        *loop;
    uv_async_t                        async_stop;
    uv_mutex_t                        lock;
    bool                              is_running;
    uv_thread_t                       tid;
    RAAT__SessionMessageCallback      message_cb;
    void                             *message_userdata;
    RAAT__SessionFailureCallback      failure_cb;
    void                             *failure_userdata;
    lua_State                        *lua;
    struct sockaddr_storage           addr;
    char                              displayaddr[RC__MAX_ADDR_LEN];   // printable client address
    bool                              ran_shutdown_handlers;
    RAAT__Device                     *device;
    char                             *client_type;

    uv_async_t                        async_post;
    RC__List                          post_handlers;
};

RC_API RC__Status
RAAT__session_message_new         (RC__Allocator *alloc, uint32_t message_type, uint8_t* data, uint32_t length, bool take_ownership_of_data, RAAT__SessionMessage **out_message) {
    RAAT__SessionMessage *message;

    RC__ASSERT(out_message);

    alloc = RC__allocator_default(alloc);

    message = RC__new0(alloc, RAAT__SessionMessage, 1);
    if (message == NULL) {
        return RC__STATUS_OUT_OF_MEMORY;
    }

    if (!take_ownership_of_data) {
        uint8_t *data2 = RC__alloc(alloc, length);
        if (!data2) {
            RC__free(alloc, message);
            return RC__STATUS_OUT_OF_MEMORY;
        }
        memcpy(data2, data, length);
        data = data2;
    }

    message->alloc        = alloc;
    message->message_type = message_type;
    message->data         = data;
    message->length       = length;

    *out_message = message;
    return RC__STATUS_SUCCESS;
}

void
RAAT__session_get_remote_addr(RAAT__Session *self, struct sockaddr_storage *out_addr) {
    RC__ASSERT(self != NULL);
    RC__ASSERT(out_addr != NULL);
    *out_addr = self->addr;
}

const char *
RAAT__session_get_display_addr(RAAT__Session *self) {
    RC__ASSERT(self != NULL);
    return self->displayaddr;
}

RC_API void
RAAT__session_message_delete       (RAAT__SessionMessage *message) {
    if (!message) return;
    RC__free(message->alloc, message->data);
    RC__free(message->alloc, message);
}

RC_API RC__Status
RAAT__script_new                  (RC__Allocator *alloc, const char *name, const char *module, const char *data, uint32_t length, bool take_ownership_of_data, RAAT__Script **out_script) {
    RAAT__Script *script;

    RC__ASSERT(out_script != NULL);

    alloc = RC__allocator_default(alloc);

    script = RC__new0(alloc, RAAT__Script, 1);
    if (script == NULL) {
        return RC__STATUS_OUT_OF_MEMORY;
    }

    if (module != NULL) {
        module = RC__allocator_strdup(alloc, module);
    }
    if (name != NULL) {
        name = RC__allocator_strdup(alloc, name);
    }

    if (!take_ownership_of_data) {
        char *data2 = RC__alloc(alloc, length);
        if (!data2) {
            RC__free(alloc, script);
            return RC__STATUS_OUT_OF_MEMORY;
        }
        memcpy(data2, data, length);
        data = data2;
    }

    script->name         = (char*)name;
    script->module       = (char*)module;
    script->alloc        = alloc;
    script->data         = (char*)data;
    script->length       = length;

    *out_script = script;
    return RC__STATUS_SUCCESS;
}

RC_API void
RAAT__script_delete               (RAAT__Script *script) {
    if (!script) return;
    RC__free(script->alloc, script->module);
    RC__free(script->alloc, script->name);
    RC__free(script->alloc, script->data);
    RC__free(script->alloc, script);
}

static void exec_script_nofail(RAAT__Session *self, const char *script) {
    int error;

    //RAAT__TRACE("[session] [%s] [internal] executing lua script", self->displayaddr);
    lua_getglobal(self->lua, "on_script_error");
    error = luaL_loadbuffer(self->lua, script, strlen(script), "line") || lua_pcall(self->lua, 0, 0, -2);
    if (error) {
        lua_pop(self->lua, 1);  /* pop error message from the stack */
        RC__ASSERT(0);
    }
}

typedef struct {
    RAAT__SessionCallback  cb;
    void                  *userdata;
} PostHandler;

static void cb_post(uv_async_t *handle) {
    RAAT__Session *self = handle->data;


    while (true) {
        PostHandler *ph = NULL;

        uv_mutex_lock(&self->lock);
        RC__ListIter it = RC__list_begin(&self->post_handlers);
        if (it != RC__list_end(&self->post_handlers)) {
            ph = (PostHandler*)RC__listiter_data(it);
            RC__list_remove(&self->post_handlers, it);
        }
        uv_mutex_unlock(&self->lock);

        if (ph) {
            ph->cb(self, ph->userdata);
            RC__free(self->alloc, ph);
        } else {
            break;
        }
    }
}

RC_API RC__Status
RAAT__session_new                 (RC__Allocator *alloc, RAAT__Device *device, struct sockaddr_storage *addr, RAAT__Session **out_self) {
    RAAT__Session *self;

    RC__ASSERT(out_self != NULL);

    alloc = RC__allocator_default(alloc);

    self = RC__new0(alloc, RAAT__Session, 1);
    if (self == NULL) {
        return RC__STATUS_OUT_OF_MEMORY;
    }

    self->alloc = alloc;
    self->log   = RAAT__device_get_log(device);
    uv_mutex_init(&self->lock);

    self->addr = *addr;
    RC__sockaddr_to_string(&self->addr, self->displayaddr);

    self->device = device;

    self->lua = luaL_newstate();
    RC__ASSERT(self->lua != NULL);
    luaL_openlibs(self->lua);

    /* Set up luv library */
    lua_getglobal(self->lua, "package");
    lua_getfield(self->lua, -1, "preload");
    lua_remove(self->lua, -2); // Remove package

    // Store uv module definition at preload.uv
    lua_pushcfunction(self->lua, luaopen_luv);
    lua_setfield(self->lua, -2, "luv");
    lua_pop(self->lua, 1);

    exec_script_nofail(self, "uv = require('luv')");
    exec_script_nofail(self, "function on_script_error(emsg) raat.log.error(\"script error: \" .. tostring(emsg))\n raat.log.error(debug.traceback()) end");

    self->loop = luv_loop(self->lua);
    RC__ASSERT(self->loop);

    RC__list_init(&self->post_handlers, self->alloc);
    uv_async_init(self->loop, &self->async_post, cb_post);
    self->async_post.data = self;

    lua_newtable(self->lua);
    lua_setglobal(self->lua, "raat");

    RAAT__script_init_misc(self->lua);
    RAAT__script_init_buffer(self->lua);
    RAAT__script_init_stream(self->lua);
    RAAT__script_init_log(self->lua, self->log);
    RAAT__script_init_session(self->lua, self, &self->addr);
    RAAT__script_init_info(self->lua, RAAT__device_get_info(device));
    RAAT__script_init_plugin_output(self->lua, device);
    RAAT__script_init_plugin_volume(self->lua, device);
    RAAT__script_init_plugin_source_selection(self->lua, device);
    RAAT__script_init_plugin_transport(self->lua, device);

    RAAT__TRACE("[session] [%s] created: %p (%p)", self->displayaddr, self, self->loop);

    *out_self = self;
    return RC__STATUS_SUCCESS;
}

RC_API void
RAAT__session_set_client_type(RAAT__Session *self, const char *client_type) {
    if (self->client_type != NULL) {
        RAAT__device_notify_client_type(self->device, self->client_type, false);
        RC__allocator_free(self->alloc, self->client_type);
    }
    if (client_type) {
        self->client_type = RC__allocator_strdup(self->alloc, client_type);
    } else {
        self->client_type = NULL;
    }
    if (self->client_type != NULL) {
        RAAT__device_notify_client_type(self->device, self->client_type, true);
    }
}

RC_API RC__Status
RAAT__session_set_failure_callback(RAAT__Session *self, RAAT__SessionFailureCallback cb, void *userdata) {
    uv_mutex_lock(&self->lock);
    if (self->is_running) {
        uv_mutex_unlock(&self->lock);
        return RAAT__SESSION_STATUS_ALREADY_RUNNING;
    }

    self->failure_cb       = cb;
    self->failure_userdata = userdata;

    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

RC_API RC__Status
RAAT__session_set_message_callback(RAAT__Session *self, RAAT__SessionMessageCallback cb, void *userdata) {
    uv_mutex_lock(&self->lock);
    if (self->is_running) {
        uv_mutex_unlock(&self->lock);
        return RAAT__SESSION_STATUS_ALREADY_RUNNING;
    }

    self->message_cb       = cb;
    self->message_userdata = userdata;

    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

static void stackDump (RAAT__Session *self, const char *desc) {
    int i=lua_gettop(self->lua);
    RAAT__TRACE( " ----------------  Stack Dump %s ----------------",desc );
    while(  i   ) {
        int t = lua_type(self->lua, i);
        switch (t) {
            case LUA_TSTRING:
                RAAT__TRACE( "%d:`%s'", i, lua_tostring(self->lua, i));
                break;
            case LUA_TBOOLEAN:
                RAAT__TRACE( "%d: %s",i,lua_toboolean(self->lua, i) ? "true" : "false");
                break;
            case LUA_TNUMBER:
                RAAT__TRACE( "%d: %g",  i, lua_tonumber(self->lua, i));
                break;
            default: RAAT__TRACE( "%d: %s@%p", i, lua_typename(self->lua, t), lua_topointer(self->lua, i)); break;
        }
        i--;
    }
    RAAT__TRACE( "--------------- Stack Dump Finished ---------------" );
}

void RAAT__session_send_message(RAAT__Session *self, RAAT__SessionMessage *message) {
    self->message_cb(self, message, self->message_userdata);
}

static void process_message(RAAT__Session *self, RAAT__SessionMessage *message) {
    lua_getglobal(self->lua, "raat");                  // push raat table
    lua_getfield(self->lua, -1, "session");
    lua_remove(self->lua, -2); // Remove raat
    lua_getfield(self->lua, -1, "message_handlers");
    lua_remove(self->lua, -2); // Remove session
    lua_pushnil(self->lua);  /* first key */
    while (lua_next(self->lua, -2) != 0) {
        lua_pushinteger(self->lua, message->message_type); 
        lua_pushlstring(self->lua, (const char*)message->data, message->length);
        RAAT__session_lua_pcall(self, 2, 0);
    }
    lua_remove(self->lua, -1); // remove message_handlers
}

static void run_script(RAAT__Session *self, RAAT__Script *script, RAAT__SessionRunScriptCallback cb, void *userdata) {
    int error;

    RC__ASSERT(self);
    RC__ASSERT(script);
    RC__ASSERT(cb);

    if (script->module) {
        RAAT__TRACE("[session] [%s] pre-loading lua module %s", self->displayaddr, script->module);

        lua_getglobal(self->lua, "package");
        lua_getfield(self->lua, -1, "preload");
        lua_remove(self->lua, -2); // Remove package

        error = luaL_loadbuffer(self->lua, script->data, script->length, script->name);
        if (error) {
            cb(self, RAAT__SESSION_STATUS_INVALID_SCRIPT, lua_tostring(self->lua, -1), userdata);
            lua_pop(self->lua, 1);  /* pop error message from the stack */
        } else {
            lua_setfield(self->lua, -2, script->module);
            lua_pop(self->lua, -1); // remove preload
            cb(self, RC__STATUS_SUCCESS, NULL, userdata);
        }

    } else {
        RAAT__TRACE("[session] [%s] executing lua script", self->displayaddr);
        lua_getglobal(self->lua, "on_script_error");
        error = luaL_loadbuffer(self->lua, script->data, script->length, script->name) || lua_pcall(self->lua, 0, 0, -2);
        if (error) {
            cb(self, RAAT__SESSION_STATUS_INVALID_SCRIPT, lua_tostring(self->lua, -1), userdata);
            lua_pop(self->lua, 1);  /* pop error message from the stack */
        } else {
            cb(self, RC__STATUS_SUCCESS, NULL, userdata);
        }
    }
}

typedef struct {
    RAAT__SessionRunScriptCallback cb;
    void                           *userdata;
    RAAT__Script                   *script;
} RunScriptState;

static void cb_run_script(RAAT__Session *self, void *userdata) {
    RunScriptState *state  = userdata;
    run_script(self, state->script, state->cb, state->userdata);
    RAAT__script_delete(state->script);
    RC__free(self->alloc, state);
}

RC_API RC__Status
RAAT__session_run_script          (RAAT__Session *self, RAAT__Script *script, RAAT__SessionRunScriptCallback cb, void *userdata) {
    RunScriptState *state = RC__new0(self->alloc, RunScriptState, 1);
    state->script     = script;
    state->cb         = cb;
    state->userdata   = userdata;
    RAAT__session_post(self, cb_run_script, state);
    return RC__STATUS_SUCCESS;
}

static void cb_session_message(RAAT__Session *self, void *userdata) {
    RAAT__SessionMessage *message = userdata;
    process_message(self, message);
    RAAT__session_message_delete(message);
}

RC_API RC__Status
RAAT__session_process_message     (RAAT__Session *self, RAAT__SessionMessage *message) {
    RAAT__session_post(self, cb_session_message, message);
    return RC__STATUS_SUCCESS;
}

RC_API RC__Status
RAAT__session_post                (RAAT__Session *self, RAAT__SessionCallback cb, void *userdata) {
    PostHandler *ph  = RC__new0(self->alloc, PostHandler, 1);
    ph->cb       = cb;
    ph->userdata = userdata;
    uv_mutex_lock(&self->lock);
    RC__list_push(&self->post_handlers, ph);
    uv_mutex_unlock(&self->lock);
    uv_async_send(&self->async_post);
    return RC__STATUS_SUCCESS;
}

static void RAAT__session_fail(RAAT__Session *self) {
    RC__ASSERT(self != NULL);
    self->failure_cb(self, self->failure_userdata);
}

bool RAAT__session_lua_pcall(RAAT__Session *self, int nargs, int nreturns) {
    int            error;

    lua_getglobal(self->lua, "on_script_error");
    lua_insert(self->lua, -nargs - 2);
    error = lua_pcall(self->lua, nargs, nreturns, -nargs - 2);
    if (error) {
        lua_pop(self->lua, 2);  /* pop error message + error func from the stack */
        self->failure_cb(self, self->failure_userdata);
        return false;
    }
    lua_remove(self->lua, -nreturns - 1);
    return true;
}

static void run_shutdown_handlers(RAAT__Session *self) {
    if (!self->ran_shutdown_handlers) {
        self->ran_shutdown_handlers = true;
        // call shutdown handlers
        lua_getglobal(self->lua, "raat");                  // push raat table
        lua_getfield(self->lua, -1, "session");
        lua_remove(self->lua, -2); // Remove raat
        lua_getfield(self->lua, -1, "shutdown_handlers");
        lua_remove(self->lua, -2); // Remove session
        lua_pushnil(self->lua);  /* first key */
        while (lua_next(self->lua, -2) != 0) {
            RAAT__session_lua_pcall(self, 0, 0);
        }
        lua_remove(self->lua, -1); // remove shutdown_handlers
    }
}

static void session_thread(void *arg) {
    RAAT__Session *self         = arg;
    int            error;
    const char    *script       = "uv.run()";

    lua_getglobal(self->lua, "on_script_error");
    error = luaL_loadbuffer(self->lua, script, strlen(script), "line") || lua_pcall(self->lua, 0, 0, -2);
    if (error) {
        lua_pop(self->lua, 1);  /* pop error message from the stack */
        self->failure_cb(self, self->failure_userdata);
    }

    run_shutdown_handlers(self);
}

static void async_stop_cb(uv_async_t *handle) {
    RAAT__Session *self = handle->data;
    uv_stop(self->loop);
    run_shutdown_handlers(self);
    uv_close((uv_handle_t*)handle, NULL);
}

RC_API void 
RAAT__session_delete             (RAAT__Session   *self) {
    bool was_running;

    if (!self) return;
    RAAT__TRACE("[session] [%s] destroying session", self->displayaddr);

    if (self->client_type) {
        RAAT__device_notify_client_type(self->device, self->client_type, false);
        RC__allocator_free(self->alloc, self->client_type);
    }

    uv_mutex_lock(&self->lock);
    was_running = self->is_running;
    self->is_running = false;
    uv_mutex_unlock(&self->lock);

    uv_close((uv_handle_t*)&self->async_post, NULL);

    if (was_running) {
        self->async_stop.data = self;
        uv_async_send(&self->async_stop);
        uv_thread_join(&self->tid);
    }

    uv_mutex_destroy(&self->lock);
    lua_close(self->lua);
    RC__free(self->alloc, self);
}

RC_API RC__Status
RAAT__session_start               (RAAT__Session *self) {
    int rc;

    RC__ASSERT(self != NULL);

    uv_mutex_lock(&self->lock);
    if (self->message_cb == NULL) {
        uv_mutex_unlock(&self->lock);
        return RAAT__SESSION_STATUS_PRECONDITION_NOT_MET;
    }
    if (self->is_running) {
        uv_mutex_unlock(&self->lock);
        return RAAT__SESSION_STATUS_ALREADY_RUNNING;
    }
    self->is_running = true;
    uv_mutex_unlock(&self->lock);

    uv_async_init(self->loop, &self->async_stop, async_stop_cb);

    rc = uv_thread_create(&self->tid, session_thread, self);
    if (rc != 0) {
        RAAT__ERROR("[session] [%s] error creating thread: %s", self->displayaddr, uv_strerror(rc));
        return RC__STATUS_UNEXPECTED_ERROR;
    }
    return RC__STATUS_SUCCESS;
}

const char * RAAT__session_status_to_string(RC__Status status) {
    RC__ASSERT(status >= RAAT__SESSION_STATUS_BASE && status <= RAAT__SESSION_STATUS_MAX);

    switch (status) {
        case RAAT__SESSION_STATUS_INVALID_SCRIPT:        return "RAAT__SESSION_STATUS_INVALID_SCRIPT";
        case RAAT__SESSION_STATUS_ALREADY_RUNNING:       return "RAAT__SESSION_STATUS_ALREADY_RUNNING";
        case RAAT__SESSION_STATUS_PRECONDITION_NOT_MET:  return "RAAT__SESSION_STATUS_PRECONDITION_NOT_MET";
        default: RC__ASSERT(0); return NULL;
    }
    return NULL;
}


