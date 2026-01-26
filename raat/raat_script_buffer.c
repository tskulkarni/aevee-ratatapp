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

#define IS_LITTLE_ENDIAN (((union { unsigned x; unsigned char c; }){1}).c)

#define CHECK_ARGUMENT_COUNT(L, n) { if (lua_gettop(L) != n) INVALID_ARGUMENT_COUNT(L); }
#define INVALID_ARGUMENT_COUNT(L) { LUA_ERROR(L, "invalid argument count"); }

RAAT__ScriptBuffer * RAAT__check_buffer(lua_State * L, int n) {
    return (RAAT__ScriptBuffer *)luaL_checkudata(L, n, "RAAT__ScriptBuffer");
}

static int buffer_delete(lua_State *L) {
    RAAT__ScriptBuffer *buffer = RAAT__check_buffer(L, 1);
    lua_alloc(L, buffer->data, buffer->capacity, 0);
    return 0;
}

static int buffer_new(lua_State *L) {
    int n = lua_gettop(L);
    size_t capacity = 0;
    size_t len = 0;
    size_t source_len = 0;
    const char *source = NULL;
    RAAT__ScriptBuffer *buf_to_copy;

    if (n == 0) {                               // buffer.new()
        capacity = 64;
    } else if (n == 1) {
        if (lua_isinteger(L, 1)) {              // buffer.new(length)
            capacity = len = luaL_checkinteger(L, 1);
        } else if (lua_isstring(L, 1)) {        // buffer.new(string)
            source = lua_tolstring(L, 1, &source_len);
            capacity = len = source_len;
        } else if (NULL != (buf_to_copy = luaL_testudata(L, 1, "RAAT__ScriptBuffer"))) {        // buffer.new(buffer)
            source     = (const char*)buf_to_copy->data;
            source_len = buf_to_copy->len;
            capacity   = buf_to_copy->capacity;
        } else {
            LUA_ERROR(L, "invalid argument to buffer.new");
        }
    } else {
        INVALID_ARGUMENT_COUNT(L);
    }

    RAAT__ScriptBuffer *buffer = lua_newuserdata(L, sizeof(RAAT__ScriptBuffer));
    buffer->data = lua_alloc(L, NULL, 0, capacity);
    if (capacity && !buffer->data) {
        LUA_ERROR(L, "buffer allocation failure");
    }
    if (source && source_len) {
        memcpy(buffer->data, source, source_len);
    }
    buffer->capacity = capacity;
    buffer->len      = len;
    buffer->pos      = 0;
    luaL_getmetatable(L, "RAAT__ScriptBuffer");
    lua_setmetatable(L, -2);

    return 1;
}

static inline void flip(void *p, size_t n) {
    uint8_t *c = (uint8_t*)p;
    size_t i;
    for (i = 0; i < n / 2; i++) {
        uint8_t tmp = c[i];
        c[i]        = c[n-i-1];
        c[n-i-1]    = tmp;
    }
}

static int ensure_capacity(lua_State *L, RAAT__ScriptBuffer *self, size_t required_capacity) {
    size_t capacity = RC__max(self->capacity, 64);
    while (capacity < required_capacity) capacity *= 2;
    if (capacity != self->capacity) {
        self->data = lua_alloc(L, self->data, self->capacity, capacity);
        if (!self->data) {
            LUA_ERROR(L, "buffer allocation failure");
        }
        self->capacity = capacity;
    }
    return 0;
}

static int check_bounds(lua_State *L, RAAT__ScriptBuffer *self, int pos, size_t size) {
    if (pos + size > self->len) {
        LUA_ERROR(L, "buffer index out of bounds");
    }
    return 0;
}

static int buffer_ensure_capacity(lua_State *L) {
    RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);
    lua_Integer lua_capacity = luaL_checkinteger(L, 2);
    size_t capacity;

    CHECK_ARGUMENT_COUNT(L, 2);

    if (lua_capacity < 0) {
        LUA_ERROR(L, "capacity must be non-negative");
    }
    capacity = (size_t)lua_capacity;

    if (capacity != self->capacity) {
        self->data = lua_alloc(L, self->data, self->capacity, capacity);
        self->capacity = capacity;
    }
    self->pos = RC__min(self->capacity, self->pos);
    self->len = RC__min(self->capacity, self->len);
    return 0;
}

static int buffer_capacity(lua_State *L) {
    RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);
    int n = lua_gettop(L);
    if (n == 1) {
        lua_pushinteger(L, self->capacity);
        return 1;
    } else if (n == 2) {
        lua_Integer lua_capacity = luaL_checkinteger(L, 2);
        size_t capacity;
        if (lua_capacity < 0) {
            LUA_ERROR(L, "capacity must be non-negative");
        }
        capacity = lua_capacity;
        if (capacity != self->capacity) {
            self->data = lua_alloc(L, self->data, self->capacity, capacity);
            self->capacity = capacity;
        }
        self->pos = RC__min(self->capacity, self->pos);
        self->len = RC__min(self->capacity, self->len);
        return 0;
    } else {
        INVALID_ARGUMENT_COUNT(L);
    }
    return 0;
}

static int buffer_length(lua_State *L) {
    RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);
    int n = lua_gettop(L);
    if (n == 1) {
        lua_pushinteger(L, self->len);
        return 1;
    } else {
        lua_Integer lua_len = luaL_checkinteger(L, 2);
        size_t len = lua_len;

        if (lua_len < 0) LUA_ERROR(L, "length must be non-negative");

        if (len < self->len) {
            self->len = len;
        } else {
            ensure_capacity(L, self, self->len);
            self->len = len;
        }
        return 0;
    }
}

static int buffer_to_string(lua_State *L) {
    RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);
    lua_pushlstring(L, (const char*)self->data, self->len);
    return 1;
}

static int buffer_position(lua_State *L) {
    RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);
    int n = lua_gettop(L);
    if (n == 1) {
        lua_pushinteger(L, self->pos);
        return 1;
    } else if (n == 2) {
        lua_Integer lua_pos = luaL_checkinteger(L, 2);
        size_t pos = lua_pos;
        if (lua_pos < 0 || pos > self->len) {
            LUA_ERROR(L, "position out of bounds");
        }
        self->pos = luaL_checkinteger(L, 1);
        return 0;
    } else {
        INVALID_ARGUMENT_COUNT(L);
    }
}

static inline void null_endian(void *p, size_t n) { }

static inline void big_endian(void *p, size_t n) {
    if (IS_LITTLE_ENDIAN) flip(p, n);
}
static inline void little_endian(void *p, size_t n) {
    if (!IS_LITTLE_ENDIAN) flip(p, n);
}

static int buffer_get_string(lua_State *L) {
   // buf:get_string(offset, n)
   RAAT__ScriptBuffer *self; 
   lua_Integer offset;
   lua_Integer n;
   CHECK_ARGUMENT_COUNT(L, 3);

   self   = RAAT__check_buffer(L, 1);                              
   offset = luaL_checkinteger(L, 2);
   n      = luaL_checkinteger(L, 3);

   check_bounds(L, self, offset, n);

   lua_pushlstring(L, (const char*)(self->data + offset), n);

   return 1;
}

static int buffer_set_string(lua_State *L) {
    // buf:set_string(dst_offset, str) 
    // buf:set_string(dst_offset, str, count) 
    // buf:set_string(dst_offset, str, src_offset, count) 
    int argc = lua_gettop(L);
    RAAT__ScriptBuffer *self;
    const char *s;
    size_t count;
    size_t src_offset = 0, dst_offset;

    self       = RAAT__check_buffer(L, 1);                              
    dst_offset = luaL_checkinteger(L, 2);

    if (lua_isstring(L, 3)) {
        s    = lua_tolstring(L, 3, &count);
    } else {
        s    = lua_touserdata(L, 3);
        count = 0;              // in the userdata case, count must be supplied externally
    }

    if (argc == 3) {
        // nothing to do
    } else if (argc == 4) {
        count      = (size_t)lua_tointeger(L, 4);
    } else if (argc == 5) {
        src_offset = (size_t)lua_tointeger(L, 4);
        count      = (size_t)lua_tointeger(L, 5);
    } else {
        INVALID_ARGUMENT_COUNT(L);
        return 0;
    }
    memcpy(self->data + dst_offset, s + src_offset, count);
    return 0;
}

static int buffer_write_string(lua_State *L) {
    // buf:write_string(str)
    // buf:write_string(str, src_offset) 
    // buf:write_string(str, src_offset, count) 
    int argc = lua_gettop(L);
    RAAT__ScriptBuffer *self;
    const char *s;
    size_t count;
    size_t src_offset = 0;

    self = RAAT__check_buffer(L, 1);                              
    s    = lua_tolstring(L, 2, &count);

    if (argc == 2) {
        /* nothing special */
    } else if (argc == 3) {
        src_offset = (size_t)lua_tointeger(L, 3);
    } else if (argc == 4) {
        src_offset = (size_t)lua_tointeger(L, 3);
        count      = (size_t)lua_tointeger(L, 4);
    } else {
        INVALID_ARGUMENT_COUNT(L);
        return 0;
    }
    ensure_capacity(L, self, self->pos + count);
    memcpy(self->data + self->pos, s + src_offset, count);
    self->pos += count;
    return 0;
}

static int buffer_read_string(lua_State *L) {
    // buf:read_string(len)
    RAAT__ScriptBuffer *self;
    size_t count;

    CHECK_ARGUMENT_COUNT(L, 2);
    self  = RAAT__check_buffer(L, 1);                              
    count = luaL_checkinteger(L, 2);

   check_bounds(L, self, self->pos, count);

   lua_pushlstring(L, (const char*)(self->data + self->pos), count);
   self->pos += count;

   return 1;
}

static int buffer_copy(lua_State *L) {
    // buffer.copy(src_buf, src_offset, dst_buf, dst_offset, count)
    RAAT__ScriptBuffer *src_buf;
    RAAT__ScriptBuffer *dst_buf;
    size_t src_offset, dst_offset, count;

    CHECK_ARGUMENT_COUNT(L, 5);
    src_buf    = RAAT__check_buffer(L, 1);
    src_offset = luaL_checkinteger(L, 2);
    dst_buf    = RAAT__check_buffer(L, 3);
    dst_offset = luaL_checkinteger(L, 4);
    count      = luaL_checkinteger(L, 5);

    check_bounds(L, src_buf, src_offset, count);
    check_bounds(L, dst_buf, dst_offset, count);

    memmove(dst_buf->data + dst_offset, src_buf->data + src_offset, count);

    return 0;
}

static int buffer_transform_copy(lua_State *L) {
    // buffer.transform_copy(src_buf, src_offset, src_stride, dst_buf, dst_offset, dst_stride, itemcount, { src_byte => dst_byte, ... })
    RAAT__ScriptBuffer *src_buf;
    RAAT__ScriptBuffer *dst_buf;
    lua_Integer src_offset, dst_offset, count;
    lua_Integer src_stride;
    lua_Integer dst_stride;
    int *dst_stride_buf;
    int i,j;
    uint8_t *src_ptr;
    uint8_t *dst_ptr;

    CHECK_ARGUMENT_COUNT(L, 8);
    src_buf    =    RAAT__check_buffer(L, 1);
    src_offset =    luaL_checkinteger(L, 2);
    src_stride =    luaL_checkinteger(L, 3);
    dst_buf    =    RAAT__check_buffer(L, 4);
    dst_offset =    luaL_checkinteger(L, 5);
    dst_stride =    luaL_checkinteger(L, 6);
    count      =    luaL_checkinteger(L, 7);
    /*mapping  = */ luaL_checkany(L, 8);

    if (dst_stride < 0 || dst_stride > 4096) LUA_ERROR(L, "dst_stride must be >= 0, <= 4096");
    if (src_stride < 0 || src_stride > 4096) LUA_ERROR(L, "src_stride must be >= 0, <= 4096");
    
    // maps from index in dst_stride to index in src_stride or -1 (to zero fill)
    dst_stride_buf = alloca(sizeof(int)*dst_stride);

    for (j = 0; j < dst_stride; j++) {
        int tmp;
        int isnum;
        lua_geti(L, -2, j);
        tmp = (int)lua_tointegerx(L,-1,&isnum);
        lua_pop(L, 1);
        if (isnum) {
            if (tmp < 0 || tmp > src_stride) {
                LUA_ERROR(L, "invalid mapping (src offset exceeds src stride)");
            }
            dst_stride_buf[j] = tmp;
        } else {
            dst_stride_buf[j] = -1;
        }
    }

    check_bounds(L, src_buf, src_offset, count * src_stride);
    check_bounds(L, dst_buf, dst_offset, count * dst_stride);

    src_ptr = src_buf->data + src_offset;
    dst_ptr = dst_buf->data + dst_offset;

    for (i = 0; i < count; i++) {
        for (j = 0; j < dst_stride; j++) {
            if (dst_stride_buf[j] == -1) {
                dst_ptr[j] = 0;
            } else {
                dst_ptr[j] = src_ptr[dst_stride_buf[j]];
            }
        }
        src_ptr += src_stride;
        dst_ptr += dst_stride;
    }

    return 0;
}

#define IMPS_NUM(suffix, type_t, fix_endian, lua_type_t, lua_checkfunction, lua_pushfunction, lua_coerce) \
    static int buffer_get_ ## suffix(lua_State *L) {                    \
        RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);                        \
        int pos = luaL_checkinteger(L, 2);                              \
        type_t t;                                                       \
        CHECK_ARGUMENT_COUNT(L, 2);                                     \
        check_bounds(L, self, pos, sizeof(type_t));                     \
        memcpy(&t, self->data+pos, sizeof(t));                          \
        fix_endian(&t, sizeof(t));                                      \
        lua_pushfunction(L, (lua_type_t)t);                             \
        return 1;                                                       \
    }                                                                   \
    static int buffer_set_ ## suffix(lua_State *L) {                    \
        RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);                        \
        int pos = luaL_checkinteger(L, 2);                              \
        type_t t = (type_t)lua_checkfunction(L, 3);                     \
        CHECK_ARGUMENT_COUNT(L, 3);                                     \
        check_bounds(L, self, pos, sizeof(type_t));                     \
        fix_endian(&t, sizeof(t));                                      \
        memcpy(self->data+pos, &t, sizeof(t));                          \
        return 0;                                                       \
    }                                                                   \
    static int buffer_read_ ## suffix(lua_State *L) {                   \
        RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);                        \
        type_t t;                                                       \
        CHECK_ARGUMENT_COUNT(L, 1);                                     \
        check_bounds(L, self, self->pos, sizeof(type_t));               \
        memcpy(&t, self->data+self->pos, sizeof(t));                    \
        self->pos += sizeof(t);                                         \
        fix_endian(&t, sizeof(t));                                      \
        lua_pushfunction(L, (lua_type_t)t);                             \
        return 1;                                                       \
    }                                                                   \
    static int buffer_write_ ## suffix(lua_State *L) {                  \
        RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);                        \
        type_t t = (type_t)lua_checkfunction(L, 2);                         \
        CHECK_ARGUMENT_COUNT(L, 2);                                     \
        ensure_capacity(L, self, self->pos + sizeof(type_t));           \
        fix_endian(&t, sizeof(t));                                      \
        memcpy(self->data+self->pos, &t, sizeof(t));                    \
        self->pos += sizeof(t);                                         \
        self->len = RC__max(self->pos, self->len);                      \
        return 0;                                                       \
    }                                                                   \
    static int buffer_get_ ## suffix ## _array(lua_State *L) {          \
        /* table = buffer_get_XX_array(pos, n) */                       \
        RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);                        \
        int pos = luaL_checkinteger(L, 2);                              \
        int n   = luaL_checkinteger(L, 3);                              \
        int i;                                                          \
        CHECK_ARGUMENT_COUNT(L, 4);                                     \
        check_bounds(L, self, pos, n * sizeof(type_t));                 \
        lua_newtable(L);                                                \
        for (i = 0; i < n; i++) {                                       \
            type_t t;                                                   \
            memcpy(&t, self->data+pos, sizeof(t));                      \
            fix_endian(&t, sizeof(t));                                  \
            lua_pushfunction(L, (lua_type_t)t);                         \
            lua_rawseti(L, -2, i+1);                                    \
            pos += sizeof(t);                                           \
        }                                                               \
        return 1;                                                       \
    }                                                                   \
    static int buffer_set_ ## suffix ## _array(lua_State *L) {          \
        /* buffer_set_XX_array(pos, arrayval) */                        \
        RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);                        \
        int pos = luaL_checkinteger(L, 2);                              \
        lua_Integer n,i;                                                \
        CHECK_ARGUMENT_COUNT(L, 3);                                     \
        luaL_checkany(L, 3);                                            \
        lua_len(L, -1);         /* push table length onto stack */      \
        n = lua_tointeger(L,-1);  /* get table length */                \
        lua_pop(L,1);           /* pop table length */                  \
        check_bounds(L, self, pos, sizeof(type_t) * n);                 \
        for (i = 0; i < n; i++) {                                       \
            lua_type_t lval;                                            \
            type_t t;                                                   \
            lua_rawgeti(L, -1, i);  /* get value from table[i] */       \
            lval = lua_coerce(L, -1);  /* get value as lua type */      \
            lua_pop(L,1);           /* pop value */                     \
            t = (type_t)lval;                                           \
            fix_endian(&t, sizeof(t));                                  \
            memcpy(self->data + pos, &t, sizeof(t));                    \
            pos += sizeof(t);                                           \
        }                                                               \
        return 0;                                                       \
    }                                                                   \
    static int buffer_read_ ## suffix ## _array(lua_State *L) {         \
        /* table = buffer_read_XX_array(n) */                           \
        RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);                        \
        int n   = luaL_checkinteger(L, 2);                              \
        int i;                                                          \
        CHECK_ARGUMENT_COUNT(L, 2);                                     \
        check_bounds(L, self, self->pos, n * sizeof(type_t));           \
        lua_newtable(L);                                                \
        for (i = 0; i < n; i++) {                                       \
            type_t t;                                                   \
            memcpy(&t, self->data+self->pos, sizeof(t));                \
            fix_endian(&t, sizeof(t));                                  \
            lua_pushfunction(L, (lua_type_t)t);                         \
            lua_rawseti(L, -2, i+1);                                    \
            self->pos += sizeof(t);                                     \
        }                                                               \
        return 1;                                                       \
    }                                                                   \
    static int buffer_write_ ## suffix ## _array(lua_State *L) {        \
        /* buffer_write_XX_array(arrayval) */                           \
        RAAT__ScriptBuffer *self = RAAT__check_buffer(L, 1);                        \
        lua_Integer n,i;                                                \
        CHECK_ARGUMENT_COUNT(L, 2);                                     \
        luaL_checkany(L, 3);                                            \
        lua_len(L, -1);         /* push table length onto stack */      \
        n = lua_tointeger(L,-1);  /* get table length */                \
        lua_pop(L,1);           /* pop table length */                  \
        ensure_capacity(L, self, self->pos + sizeof(type_t) * n);       \
        for (i = 0; i < n; i++) {                                       \
            lua_type_t lval;                                            \
            type_t t;                                                   \
            lua_rawgeti(L, -1, i);  /* get value from table[i] */       \
            lval = lua_coerce(L, -1);  /* get value as lua type */      \
            lua_pop(L,1);           /* pop value */                     \
            t = (type_t)lval;                                           \
            fix_endian(&t, sizeof(t));                                  \
            memcpy(self->data + self->pos, &t, sizeof(t));              \
            self->pos += sizeof(t);                                     \
        }                                                               \
        return 0;                                                       \
    }                                                                   \

/*       Method suffix  C type        byte order           lua type             lua check function      lua push function       lua coerce function
 *       ------------------------------------------------------------------------------------------------------------------------------------------*/
IMPS_NUM(int8,          int8_t,       null_endian,         lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(uint8,         uint8_t,      null_endian,         lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(int16_be,      int16_t,      big_endian,          lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(int32_be,      int32_t,      big_endian,          lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(intptr_be,     intptr_t,     big_endian,          lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(int64_be,      int64_t,      big_endian,          lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(uint16_be,     uint16_t,     big_endian,          lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(uint32_be,     uint32_t,     big_endian,          lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(uint64_be,     uint64_t,     big_endian,          lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(uintptr_be,    uintptr_t,    big_endian,          lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(int16_le,      int16_t,      little_endian,       lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(int32_le,      int32_t,      little_endian,       lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(intptr_le,     intptr_t,     little_endian,       lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(int64_le,      int64_t,      little_endian,       lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(uint16_le,     uint16_t,     little_endian,       lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(uint32_le,     uint32_t,     little_endian,       lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(uint64_le,     uint64_t,     little_endian,       lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(uintptr_le,    uintptr_t,    little_endian,       lua_Integer,         luaL_checkinteger,      lua_pushinteger,        lua_tointeger)
IMPS_NUM(float32_le,    float,        little_endian,       lua_Number,          luaL_checknumber,       lua_pushnumber,         lua_tonumber)
IMPS_NUM(float64_le,    double,       little_endian,       lua_Number,          luaL_checknumber,       lua_pushnumber,         lua_tonumber)
IMPS_NUM(float32_be,    float,        big_endian,          lua_Number,          luaL_checknumber,       lua_pushnumber,         lua_tonumber)
IMPS_NUM(float64_be,    double,       big_endian,          lua_Number,          luaL_checknumber,       lua_pushnumber,         lua_tonumber)

#define DEFNS(x)  \
    { "get_" #x,   buffer_get_ ## x },  \
    { "set_" #x,   buffer_set_ ## x },  \
    { "read_" #x,  buffer_read_ ## x }, \
    { "write_" #x, buffer_write_ ## x }, \
    { "get_" #x "_array",   buffer_get_ ## x ## _array },  \
    { "set_" #x "_array",   buffer_set_ ## x ## _array },  \
    { "read_" #x "_array",  buffer_read_ ## x ## _array }, \
    { "write_" #x "_array", buffer_write_ ## x ## _array } \

static const luaL_Reg bufferlib[] = {
    { "new",              buffer_new             },
    { "position",         buffer_position        },
    { "length",           buffer_length          },
    { "capacity",         buffer_capacity        },
    { "ensure_capacity",  buffer_ensure_capacity },
    DEFNS(int8),      DEFNS(uint8),   
    DEFNS(int16_le),  DEFNS(uint16_le),  DEFNS(int16_be),  DEFNS(uint16_be),
    DEFNS(int32_le),  DEFNS(uint32_le),  DEFNS(int32_be),  DEFNS(uint32_be),
    DEFNS(int64_le),  DEFNS(uint64_le),  DEFNS(int64_be),  DEFNS(uint64_be),
    DEFNS(intptr_le), DEFNS(uintptr_le), DEFNS(intptr_be), DEFNS(uintptr_be),
    DEFNS(float32_le), DEFNS(float32_be),   
    DEFNS(float64_le), DEFNS(float64_be),   
    { "get_string",             buffer_get_string               },
    { "set_string",             buffer_set_string               },
    { "read_string",            buffer_read_string              },
    { "write_string",           buffer_write_string             },
    { "to_string",              buffer_to_string                },
    { "copy",                   buffer_copy                     },
    { "transform_copy",         buffer_transform_copy           },
    { "__len",                  buffer_length                   },
    { "__tostring",             buffer_to_string                },
    { "__gc",                   buffer_delete                   },
    { NULL,                     NULL                            }
};

static int open_buffer(lua_State *L) {
    luaL_newmetatable(L, "RAAT__ScriptBuffer");
    luaL_setfuncs(L, bufferlib, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -1, "__index");

    lua_pushboolean(L, IS_LITTLE_ENDIAN);
    lua_setfield(L, -2, "is_little_endian");

    lua_pushboolean(L, !IS_LITTLE_ENDIAN);
    lua_setfield(L, -2, "is_big_endian");

    lua_pushinteger(L, sizeof(intptr_t));
    lua_setfield(L, -2, "pointer_size");

    lua_pushboolean(L, true);
    lua_setfield(L, -2, "has_working_copy_method");

    return 1;
}

void RAAT__script_init_buffer(lua_State *L) {
    luaL_requiref(L, "buffer", open_buffer, 1);
}
