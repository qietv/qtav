/*
 * libsmb2 SMB URL parsing helpers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"

#include "libsmb2url.h"
#include "urldecode.h"

static int validate_utf8(const char *value, size_t len)
{
    const uint8_t *p   = (const uint8_t *)value;
    const uint8_t *end = p + len;

    while (p < end) {
        int32_t codepoint;
        if (av_utf8_decode(&codepoint, &p, end, 0) < 0)
            return AVERROR_INVALIDDATA;
    }

    return 0;
}

static int decode_component(char **dst, const char *src, size_t len)
{
    char *decoded;
    size_t i;
    int decoded_len;
    int ret;

    for (i = 0; i < len; i++) {
        if (src[i] == '%' &&
            (i + 2 >= len || !av_isxdigit(src[i + 1]) ||
                             !av_isxdigit(src[i + 2])))
            return AVERROR_INVALIDDATA;
    }

    decoded = av_malloc(len + 1);
    if (!decoded)
        return AVERROR(ENOMEM);

    decoded_len = ff_urldecode_len(decoded, len + 1, src, len, 0);
    if (decoded_len < 0) {
        av_free(decoded);
        return decoded_len;
    }

    if (memchr(decoded, '\0', decoded_len)) {
        av_free(decoded);
        return AVERROR_INVALIDDATA;
    }

    ret = validate_utf8(decoded, decoded_len);
    if (ret < 0) {
        av_free(decoded);
        return ret;
    }

    *dst = decoded;
    return 0;
}

static int copy_component(char **dst, const char *src, size_t len)
{
    *dst = av_strndup(src, len);
    return *dst ? 0 : AVERROR(ENOMEM);
}

void ff_smb2_free_url(FFSMB2URL *parsed)
{
    if (!parsed)
        return;

    av_freep(&parsed->domain);
    av_freep(&parsed->user);
    av_freep(&parsed->password);
    av_freep(&parsed->server);
    av_freep(&parsed->share);
    av_freep(&parsed->path);
}

int ff_smb2_parse_url(const char *url, FFSMB2URL *parsed)
{
    const char *authority;
    const char *authority_end;
    const char *userinfo_end = NULL;
    const char *share;
    const char *share_end;
    const char *path;
    const char *path_end;
    const char *p;
    const char *domain_end;
    const char *password;
    int ret;

    if (!url || !parsed)
        return AVERROR(EINVAL);

    memset(parsed, 0, sizeof(*parsed));

    if (av_strncasecmp(url, "smb://", 6))
        return AVERROR_INVALIDDATA;

    authority = url + 6;
    authority_end = authority + strcspn(authority, "/?#");
    if (authority_end == authority || *authority_end != '/')
        return AVERROR_INVALIDDATA;

    for (p = authority; p < authority_end; p++) {
        if (*p == '@')
            userinfo_end = p;
    }

    if (userinfo_end) {
        const char *userinfo = authority;
        const char *userinfo_user = authority;

        domain_end = memchr(userinfo, ';', userinfo_end - userinfo);
        if (domain_end) {
            if (domain_end == userinfo)
                goto invalid;
            if ((ret = decode_component(&parsed->domain, userinfo,
                                        domain_end - userinfo)) < 0)
                goto fail;
            userinfo_user = domain_end + 1;
        }

        password = memchr(userinfo_user, ':', userinfo_end - userinfo_user);
        if (password) {
            if ((ret = decode_component(&parsed->password, password + 1,
                                        userinfo_end - password - 1)) < 0)
                goto fail;
        } else {
            password = userinfo_end;
        }

        if (password == userinfo_user)
            goto invalid;
        if ((ret = decode_component(&parsed->user, userinfo_user,
                                    password - userinfo_user)) < 0)
            goto fail;
        authority = userinfo_end + 1;
    }

    if (authority == authority_end)
        goto invalid;
    if ((ret = copy_component(&parsed->server, authority,
                              authority_end - authority)) < 0)
        goto fail;

    share = authority_end + 1;
    path_end = share + strcspn(share, "?#");
    share_end = memchr(share, '/', path_end - share);
    if (!share_end)
        share_end = path_end;
    if (share_end == share)
        goto invalid;

    if ((ret = decode_component(&parsed->share, share,
                                share_end - share)) < 0)
        goto fail;
    if (strchr(parsed->share, '/') || strchr(parsed->share, '\\'))
        goto invalid;

    path = share_end < path_end ? share_end + 1 : path_end;
    if ((ret = decode_component(&parsed->path, path, path_end - path)) < 0)
        goto fail;

    return 0;

invalid:
    ret = AVERROR_INVALIDDATA;
fail:
    ff_smb2_free_url(parsed);
    return ret;
}
