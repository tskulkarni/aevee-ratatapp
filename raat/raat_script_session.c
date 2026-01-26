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

// send_message(message_type, buffer)
static int script_session_send_message(lua_State *L) {
    RAAT__Session *session = RAAT__script_get_registry(L, "raat_session");
    uint32_t message_type = (uint32_t)luaL_checkinteger(L, 1);
    void *data;
    size_t len;

    // accept buffer or string
    RAAT__ScriptBuffer *buffer  = luaL_testudata(L, 2, "RAAT__ScriptBuffer");
    if (buffer != NULL) {
        data = buffer->data;
        len  = buffer->len;
    } else {
        data = (void*)luaL_checklstring(L, 2, &len);
    }

    RC__Allocator *alloc = RC__allocator_default(NULL);         // XXX: is this right? 
    RAAT__SessionMessage *msg;
    RC__Status status;
    status = RAAT__session_message_new(alloc, message_type, data, len, false, &msg);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        LUA_ERROR(L, "Failed to instantiate message");
    }
    RAAT__session_send_message(session, msg);
    return 0;
}

// set_client_type(type)
static int script_session_set_client_type(lua_State *L) {
    RAAT__Session *session = RAAT__script_get_registry(L, "raat_session");
    const char *str = luaL_checkstring(L, 1);
    RAAT__session_set_client_type(session, str);
    return 0;
}

static const luaL_Reg sessionlib[] = {
    { "send_message",    script_session_send_message    },
    { "set_client_type", script_session_set_client_type },
    { NULL , NULL }
};

// pushes a sockaddr onto the lua stack as a table: { family: "inet", port: 12345, ip: "192.168.1.1" }
static void push_remote_addr(lua_State* L, struct sockaddr_storage* address, int addrlen) {
    char ip[1024];
    int port = 0;

    if (address->ss_family == AF_INET) {
        struct sockaddr_in* addrin = (struct sockaddr_in*)address;
        uv_inet_ntop(AF_INET, &(addrin->sin_addr), ip, addrlen);
        port = ntohs(addrin->sin_port);

        lua_newtable(L);
        lua_pushstring(L, "inet");
        lua_setfield(L, -2, "family");
        lua_pushinteger(L, port);
        lua_setfield(L, -2, "port");
        lua_pushstring(L, ip);
        lua_setfield(L, -2, "ip");
    } else if (address->ss_family == AF_INET6) {
        struct sockaddr_in6* addrin6 = (struct sockaddr_in6*)address;
        uv_inet_ntop(AF_INET6, &(addrin6->sin6_addr), ip, addrlen);
        port = ntohs(addrin6->sin6_port);

        lua_newtable(L);
        lua_pushstring(L, "inet6");
        lua_setfield(L, -2, "family");
        lua_pushinteger(L, port);
        lua_setfield(L, -2, "port");
        lua_pushstring(L, ip);
        lua_setfield(L, -2, "ip");
    } else {
        RC__ASSERT(false);
    }
}

/** Initialize the session API bindings */
void
RAAT__script_init_session(lua_State *L, RAAT__Session *session, struct sockaddr_storage *remote_addr) {
    RAAT__script_set_registry(L, "raat_session", session);
    lua_getglobal(L, "raat");                  // push raat table
    luaL_newlib(L, sessionlib);                // push table for session functions

    lua_newtable(L);                           // push { }
    lua_setfield(L, -2, "message_handlers");   // session.message_handlers = { }

    lua_newtable(L);                           // push { }
    lua_setfield(L, -2, "shutdown_handlers");  // session.shutdown_handlers = { }

    push_remote_addr(L, remote_addr, sizeof(struct sockaddr_storage));
    lua_setfield(L, -2, "remote_address");     // raat.session = sessiontable

    lua_setfield(L, -2, "session");            // raat.session = sessiontable
    lua_pop(L, 1);                             // pop raat table
}

