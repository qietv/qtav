/*
 * libsmb2 SMB URL parser test
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <string.h>

#include "libavformat/libsmb2url.h"

typedef struct URLTest {
    const char *name;
    const char *url;
    const char *domain;
    const char *user;
    const char *password;
    const char *server;
    const char *share;
    const char *path;
    int valid;
} URLTest;

static int same(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return !strcmp(a, b);
}

int main(void)
{
    static const URLTest tests[] = {
        {
            "percent-encoded UTF-8 path",
            "smb://DOMAIN;user:p%40ss@server/share/"
            "%E4%B8%AD%E6%96%87%20%E8%A7%86%E9%A2%91.mp4",
            "DOMAIN", "user", "p@ss", "server", "share",
            "中文 视频.mp4", 1
        },
        {
            "raw UTF-8",
            "smb://用户:密码@server/共享/目录/影片.mp4",
            NULL, "用户", "密码", "server", "共享", "目录/影片.mp4", 1
        },
        {
            "decode exactly once",
            "smb://server/share/100%2520real+a%23b.mkv",
            NULL, NULL, NULL, "server", "share", "100%20real+a#b.mkv", 1
        },
        {
            "IPv6 and query",
            "smb://guest@[fe80::1]:1445/share/movie.mkv?ignored=yes#fragment",
            NULL, "guest", NULL, "[fe80::1]:1445", "share", "movie.mkv", 1
        },
        {
            "share root",
            "SMB://server/share/",
            NULL, NULL, NULL, "server", "share", "", 1
        },
        { "invalid percent", "smb://server/share/a%ZZ", .valid = 0 },
        { "encoded NUL", "smb://server/share/a%00b", .valid = 0 },
        { "invalid UTF-8", "smb://server/share/%E4%B8", .valid = 0 },
        { "encoded share separator", "smb://server/a%2Fb/file", .valid = 0 },
        { "missing share", "smb://server", .valid = 0 },
        { "empty username", "smb://:password@server/share/file", .valid = 0 },
    };
    int failures = 0;
    unsigned int i;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        FFSMB2URL parsed;
        int ret = ff_smb2_parse_url(tests[i].url, &parsed);
        int ok = tests[i].valid ? ret >= 0 : ret < 0;

        if (ret >= 0 && tests[i].valid) {
            ok = same(parsed.domain, tests[i].domain) &&
                 same(parsed.user, tests[i].user) &&
                 same(parsed.password, tests[i].password) &&
                 same(parsed.server, tests[i].server) &&
                 same(parsed.share, tests[i].share) &&
                 same(parsed.path, tests[i].path);
        }

        printf("%s: %s\n", tests[i].name, ok ? "ok" : "FAIL");
        failures += !ok;
        if (ret >= 0)
            ff_smb2_free_url(&parsed);
    }

    return failures;
}
