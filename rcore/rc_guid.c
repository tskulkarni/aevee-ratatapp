//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "rc_guid.h"
#include "rc_dict.h"

#include <stdlib.h>
#include <string.h>

void
RC__guid_init(RC__Guid *self, int a, short b, short c,
               unsigned char d, unsigned char e, unsigned char f, unsigned char g,
               unsigned char h, unsigned char i, unsigned char j, unsigned char k)
{
    self->a = a;
    self->b = b;
    self->c = c;
    self->d = d;
    self->e = e;
    self->f = f;
    self->g = g;
    self->h = h;
    self->i = i;
    self->j = j;
    self->k = k;
}

void
RC__guid_init_random(RC__Guid *self)
{
    srand((int)RC__now_us());
    self->a = (rand() & 0xffff) | ((rand() & 0xffff) << 16);
    self->b = (rand() & 0xffff);
    self->c = (rand() & 0xffff);
    self->d = (rand() & 0xff);
    self->e = (rand() & 0xff);
    self->f = (rand() & 0xff);
    self->g = (rand() & 0xff);
    self->h = (rand() & 0xff);
    self->i = (rand() & 0xff);
    self->j = (rand() & 0xff);
    self->k = (rand() & 0xff);
}

int
RC__guid_init_string(RC__Guid *self, const char *str)
{
    char buf[10];
    size_t len;

    len = strlen(str);
    switch (len) {
        case 36:
            buf[8] = 0;
            memcpy(buf, str, 8);
            self->a = strtoul(buf, NULL, 16);

            buf[4] = 0;
            memcpy(buf, str+9, 4);
            self->b = (unsigned short)strtoul(buf, NULL, 16);
            memcpy(buf, str+14, 4);
            self->c = (unsigned short)strtoul(buf, NULL, 16);

            buf[2] = 0;
            memcpy(buf, str+19, 2);
            self->d = (unsigned char)strtoul(buf, NULL, 16);
            memcpy(buf, str+21, 2);
            self->e = (unsigned char)strtoul(buf, NULL, 16);

            memcpy(buf, str+24, 2);
            self->f = (unsigned char)strtoul(buf, NULL, 16);
            memcpy(buf, str+26, 2);
            self->g = (unsigned char)strtoul(buf, NULL, 16);
            memcpy(buf, str+28, 2);
            self->h = (unsigned char)strtoul(buf, NULL, 16);
            memcpy(buf, str+30, 2);
            self->i = (unsigned char)strtoul(buf, NULL, 16);
            memcpy(buf, str+32, 2);
            self->j = (unsigned char)strtoul(buf, NULL, 16);
            memcpy(buf, str+34, 2);
            self->k = (unsigned char)strtoul(buf, NULL, 16);
            break;
        case 32:
            buf[8] = 0;
            memcpy(buf, str, 8);
            self->a = strtoul(buf, NULL, 16);

            buf[4] = 0;
            memcpy(buf, str+8, 4);
            self->b = (unsigned short)strtoul(buf, NULL, 16);
            memcpy(buf, str+12, 4);
            self->c = (unsigned short)strtoul(buf, NULL, 16);

            buf[2] = 0;
            memcpy(buf, str+16, 2);
            self->d = (unsigned char)strtoul(buf, NULL, 16);
            memcpy(buf, str+18, 2);
            self->e = (unsigned char)strtoul(buf, NULL, 16);

            memcpy(buf, str+20, 2);
            self->f = (unsigned char)strtoul(buf, NULL, 16);
            memcpy(buf, str+22, 2);
            self->g = (unsigned char)strtoul(buf, NULL, 16);
            memcpy(buf, str+24, 2);
            self->h = (unsigned char)strtoul(buf, NULL, 16);
            memcpy(buf, str+26, 2);
            self->i = (unsigned char)strtoul(buf, NULL, 16);
            memcpy(buf, str+28, 2);
            self->j = (unsigned char)strtoul(buf, NULL, 16);
            memcpy(buf, str+30, 2);
            self->k = (unsigned char)strtoul(buf, NULL, 16);
            break;
        default:
            return -1;
    }
    return 0;
}

static int
parsehex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return 0;
}

void
RC__guid_init_serial_string(RC__Guid *self, const char *str)
{
    char buf[16];
	int i; 
	size_t len;

    memset(buf, 0, sizeof(buf));

    len = strlen(str);
    if (len == 12) {
        buf[0] = (char)(parsehex(str[0]) << 4 | parsehex(str[1]));
        buf[1] = (char)(parsehex(str[2]) << 4 | parsehex(str[3]));
        buf[2] = (char)(parsehex(str[4]) << 4 | parsehex(str[5]));
        buf[3] = (char)(parsehex(str[6]) << 4 | parsehex(str[7]));
        buf[4] = (char)(parsehex(str[8]) << 4 | parsehex(str[9]));
        buf[5] = (char)(parsehex(str[10]) << 4 | parsehex(str[11]));
        RC__guid_init_bytes(self, buf);
    } else {
        i = 0;
        while (i < 16 && str[i]) { buf[i] = str[i]; i++; }
        while (i < 16) buf[i++] = 0xff;
        RC__guid_init_bytes(self, buf);
    }
}

void
RC__guid_init_bytes(RC__Guid *self, void *buf /* 16 bytes of guid in network byte order */)
{
    unsigned char *b = buf;
    RC__guid_init(self, b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24),
                   b[4] | (b[5] << 8),
                   b[6] | (b[7] << 8),
                   b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);

}

void
RC__guid_tobytes(RC__Guid *self, void *buf /* this is filled in with 16 bytes of guid in network byte order */)
{
    unsigned char *b = buf;
    b[0]  = (self->a      ) & 0xff;
    b[1]  = (self->a >> 8 ) & 0xff;
    b[2]  = (self->a >> 16) & 0xff;
    b[3]  = (self->a >> 24) & 0xff;

    b[4]  = (self->b      ) & 0xff;
    b[5]  = (self->b >> 8 ) & 0xff;

    b[6]  = (self->c      ) & 0xff;
    b[7]  = (self->c >> 8 ) & 0xff;

    b[8]  = self->d;
    b[9]  = self->e;
    b[10] = self->f;
    b[11] = self->g;
    b[12] = self->h;
    b[13] = self->i;
    b[14] = self->j;
    b[15] = self->k;
}

void
RC__guid_tostring_compact(RC__Guid *self, char *buf/*[RC__GUID_COMPACT_STRLEN + 1]*/)
{
    sprintf(buf, "%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
            self->a, self->b, self->c,
            self->d, self->e,
            self->f, self->g, self->h, self->i, self->j, self->k);
}

void
RC__guid_tostring(RC__Guid *self, char *buf/*[RC__GUID_STRLEN + 1]*/)
{
    sprintf(buf, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            self->a, self->b, self->c,
            self->d, self->e,
            self->f, self->g, self->h, self->i, self->j, self->k);
}

void
RC__guid_serial_tostring(RC__Guid *self, char *buf/*[RC__GUID_STRLEN + 1]*/)
{
    if (self->c == 0 && self->d == 0 && self->e == 0 && self->f == 0 && self->g == 0 &&
        self->h == 0 && self->i == 0 && self->j == 0 && self->k == 0)
    {
        sprintf(buf, "%02X%02X%02X%02X%02X%02X",
                (self->a) & 0xff,
                (self->a >> 8) & 0xff,
                (self->a >> 16) & 0xff,
                (self->a >> 24) & 0xff,
                self->b & 0xff,
                (self->b >> 8) & 0xff);
    } else {
        unsigned char bytes[16];
        int i = 0;

        RC__guid_tobytes(self, bytes);

        while (i < 16) {
            if (bytes[i] == 0xff)
                break;
            buf[i] = bytes[i];
            i++;
        }
        buf[i] = 0;
    }
}

bool
RC__guid_equals(RC__Guid *self, RC__Guid *other)
{
    return (self->a == other->a &&
            self->b == other->b &&
            self->c == other->c &&
            self->d == other->d &&
            self->e == other->e &&
            self->f == other->f &&
            self->g == other->g &&
            self->h == other->h &&
            self->i == other->i &&
            self->j == other->j &&
            self->k == other->k);
}
