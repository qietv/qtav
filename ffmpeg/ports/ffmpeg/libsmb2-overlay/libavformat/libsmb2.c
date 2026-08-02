/*
 * libsmb2 SMB2/SMB3 protocol
 *
 * Copyright (c) 2026
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "libsmb2url.h"
#include "url.h"

typedef struct LIBSMB2Context {
    const AVClass *class;
    struct smb2_context *smb2;
    struct smb2fh *fh;
    struct smb2dir *dir;
    FFSMB2URL url;
    int64_t filesize;
    int connected;
    int trunc;
    int timeout;
    char *workgroup;
} LIBSMB2Context;

static int nullable_strcmp(const char *a, const char *b)
{
    if (!a || !b)
        return a != b;
    return strcmp(a, b);
}

static int libsmb2_last_error(LIBSMB2Context *libsmb2, int fallback)
{
    int status;
    int err;

    if (!libsmb2->smb2)
        return AVERROR(fallback);

    status = smb2_get_nterror(libsmb2->smb2);
    if (!status)
        return AVERROR(fallback);

    err = nterror_to_errno((uint32_t)status);
    return AVERROR(err > 0 ? err : fallback);
}

static void libsmb2_log_error(URLContext *h, const char *operation)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    const char *detail = libsmb2->smb2 ? smb2_get_error(libsmb2->smb2) : NULL;

    av_log(h, AV_LOG_ERROR, "%s failed%s%s\n", operation,
           detail && *detail ? ": " : "",
           detail && *detail ? detail : "");
}

static av_cold int libsmb2_close(URLContext *h)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    int ret = 0;
    int status;

    if (libsmb2->dir) {
        smb2_closedir(libsmb2->smb2, libsmb2->dir);
        libsmb2->dir = NULL;
    }

    if (libsmb2->fh) {
        status = smb2_close(libsmb2->smb2, libsmb2->fh);
        if (status < 0)
            ret = status;
        libsmb2->fh = NULL;
    }

    if (libsmb2->connected) {
        status = smb2_disconnect_share(libsmb2->smb2);
        if (status < 0 && ret >= 0)
            ret = status;
        libsmb2->connected = 0;
    }

    smb2_destroy_context(libsmb2->smb2);
    libsmb2->smb2 = NULL;
    ff_smb2_free_url(&libsmb2->url);
    libsmb2->filesize = -1;

    return ret;
}

static av_cold int libsmb2_connect(URLContext *h, const char *url)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    const char *domain;
    int ret;

    ret = ff_smb2_parse_url(url, &libsmb2->url);
    if (ret < 0) {
        av_log(h, AV_LOG_ERROR, "Invalid SMB URL\n");
        return ret;
    }

    libsmb2->smb2 = smb2_init_context();
    if (!libsmb2->smb2) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if (libsmb2->url.user)
        smb2_set_user(libsmb2->smb2, libsmb2->url.user);
    if (libsmb2->url.password)
        smb2_set_password(libsmb2->smb2, libsmb2->url.password);

    domain = libsmb2->url.domain ? libsmb2->url.domain : libsmb2->workgroup;
    if (domain)
        smb2_set_domain(libsmb2->smb2, domain);

    if (libsmb2->timeout > 0)
        smb2_set_timeout(libsmb2->smb2,
                         (int)(((int64_t)libsmb2->timeout + 999) / 1000));

    smb2_set_security_mode(libsmb2->smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);

    ret = smb2_connect_share(libsmb2->smb2, libsmb2->url.server,
                             libsmb2->url.share, libsmb2->url.user);
    if (ret < 0) {
        libsmb2_log_error(h, "SMB share connection");
        goto fail;
    }

    libsmb2->connected = 1;
    return 0;

fail:
    libsmb2_close(h);
    return ret;
}

static av_cold int libsmb2_open(URLContext *h, const char *url, int flags)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    struct smb2_stat_64 st;
    int access;
    int ret;

    libsmb2->filesize = -1;

    ret = libsmb2_connect(h, url);
    if (ret < 0)
        return ret;

    if ((flags & AVIO_FLAG_WRITE) && (flags & AVIO_FLAG_READ)) {
        access = O_CREAT | O_RDWR;
    } else if (flags & AVIO_FLAG_WRITE) {
        access = O_CREAT | O_WRONLY;
    } else {
        access = O_RDONLY;
    }
    if ((flags & AVIO_FLAG_WRITE) && libsmb2->trunc)
        access |= O_TRUNC;

    libsmb2->fh = smb2_open(libsmb2->smb2, libsmb2->url.path, access);
    if (!libsmb2->fh) {
        libsmb2_log_error(h, "SMB file open");
        ret = libsmb2_last_error(libsmb2, EIO);
        goto fail;
    }

    ret = smb2_fstat(libsmb2->smb2, libsmb2->fh, &st);
    if (ret < 0) {
        av_log(h, AV_LOG_WARNING, "Could not stat SMB file: %s\n",
               smb2_get_error(libsmb2->smb2));
    } else if (st.smb2_size <= INT64_MAX) {
        libsmb2->filesize = st.smb2_size;
    }

    return 0;

fail:
    libsmb2_close(h);
    return ret;
}

static int libsmb2_read(URLContext *h, unsigned char *buf, int size)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    int ret = smb2_read(libsmb2->smb2, libsmb2->fh, buf, size);

    if (ret < 0) {
        libsmb2_log_error(h, "SMB read");
        return ret;
    }
    return ret ? ret : AVERROR_EOF;
}

static int libsmb2_write(URLContext *h, const unsigned char *buf, int size)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    int ret = smb2_write(libsmb2->smb2, libsmb2->fh, buf, size);

    if (ret < 0)
        libsmb2_log_error(h, "SMB write");
    return ret;
}

static int64_t libsmb2_seek(URLContext *h, int64_t pos, int whence)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    int64_t ret;

    if (whence == AVSEEK_SIZE)
        return libsmb2->filesize >= 0 ? libsmb2->filesize : AVERROR(EIO);

    whence &= ~AVSEEK_FORCE;
    ret = smb2_lseek(libsmb2->smb2, libsmb2->fh, pos, whence, NULL);
    if (ret < 0)
        libsmb2_log_error(h, "SMB seek");
    return ret;
}

static int libsmb2_open_dir(URLContext *h)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    int ret = libsmb2_connect(h, h->filename);

    if (ret < 0)
        return ret;

    libsmb2->dir = smb2_opendir(libsmb2->smb2, libsmb2->url.path);
    if (!libsmb2->dir) {
        libsmb2_log_error(h, "SMB directory open");
        ret = libsmb2_last_error(libsmb2, EIO);
        libsmb2_close(h);
        return ret;
    }

    return 0;
}

static int libsmb2_read_dir(URLContext *h, AVIODirEntry **next)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    struct smb2dirent *dirent;
    AVIODirEntry *entry;

    do {
        dirent = smb2_readdir(libsmb2->smb2, libsmb2->dir);
        if (!dirent) {
            *next = NULL;
            return 0;
        }
    } while (!strcmp(dirent->name, ".") || !strcmp(dirent->name, ".."));

    entry = ff_alloc_dir_entry();
    if (!entry)
        return AVERROR(ENOMEM);

    entry->name = av_strdup(dirent->name);
    if (!entry->name) {
        av_free(entry);
        return AVERROR(ENOMEM);
    }

    switch (dirent->st.smb2_type) {
    case SMB2_TYPE_DIRECTORY:
        entry->type = AVIO_ENTRY_DIRECTORY;
        break;
    case SMB2_TYPE_FILE:
        entry->type = AVIO_ENTRY_FILE;
        break;
    case SMB2_TYPE_LINK:
        entry->type = AVIO_ENTRY_SYMBOLIC_LINK;
        break;
    default:
        entry->type = AVIO_ENTRY_UNKNOWN;
        break;
    }

    entry->utf8 = 1;
    if (dirent->st.smb2_size <= INT64_MAX)
        entry->size = dirent->st.smb2_size;
    entry->modification_timestamp =
        INT64_C(1000000) * dirent->st.smb2_mtime +
        dirent->st.smb2_mtime_nsec / 1000;
    entry->access_timestamp =
        INT64_C(1000000) * dirent->st.smb2_atime +
        dirent->st.smb2_atime_nsec / 1000;
    entry->status_change_timestamp =
        INT64_C(1000000) * dirent->st.smb2_ctime +
        dirent->st.smb2_ctime_nsec / 1000;

    *next = entry;
    return 0;
}

static int libsmb2_close_dir(URLContext *h)
{
    return libsmb2_close(h);
}

static int libsmb2_delete(URLContext *h)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    struct smb2_stat_64 st;
    int ret;
    int close_ret;

    ret = libsmb2_connect(h, h->filename);
    if (ret < 0)
        return ret;

    ret = smb2_stat(libsmb2->smb2, libsmb2->url.path, &st);
    if (ret < 0) {
        libsmb2_log_error(h, "SMB stat");
        goto cleanup;
    }

    if (st.smb2_type == SMB2_TYPE_DIRECTORY)
        ret = smb2_rmdir(libsmb2->smb2, libsmb2->url.path);
    else
        ret = smb2_unlink(libsmb2->smb2, libsmb2->url.path);
    if (ret < 0)
        libsmb2_log_error(h, "SMB delete");

cleanup:
    close_ret = libsmb2_close(h);
    return ret < 0 ? ret : close_ret;
}

static int libsmb2_move(URLContext *h_src, URLContext *h_dst)
{
    LIBSMB2Context *libsmb2 = h_src->priv_data;
    FFSMB2URL dst = { 0 };
    int ret;
    int close_ret;

    ret = ff_smb2_parse_url(h_dst->filename, &dst);
    if (ret < 0)
        return ret;

    ret = libsmb2_connect(h_src, h_src->filename);
    if (ret < 0)
        goto cleanup_url;

    if (av_strcasecmp(libsmb2->url.server, dst.server) ||
        av_strcasecmp(libsmb2->url.share, dst.share) ||
        nullable_strcmp(libsmb2->url.domain, dst.domain) ||
        nullable_strcmp(libsmb2->url.user, dst.user) ||
        nullable_strcmp(libsmb2->url.password, dst.password)) {
        ret = AVERROR(EXDEV);
        goto cleanup_connection;
    }

    ret = smb2_rename(libsmb2->smb2, libsmb2->url.path, dst.path);
    if (ret < 0)
        libsmb2_log_error(h_src, "SMB rename");

cleanup_connection:
    close_ret = libsmb2_close(h_src);
    if (ret >= 0)
        ret = close_ret;
cleanup_url:
    ff_smb2_free_url(&dst);
    return ret;
}

#define OFFSET(x) offsetof(LIBSMB2Context, x)
#define D AV_OPT_FLAG_DECODING_PARAM
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "timeout",   "set socket I/O timeout in milliseconds",
      OFFSET(timeout), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, INT_MAX, D | E },
    { "truncate",  "truncate existing files on write",
      OFFSET(trunc), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, E },
    { "workgroup", "set the authentication domain/workgroup",
      OFFSET(workgroup), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, D | E },
    { NULL }
};

static const AVClass libsmb2_context_class = {
    .class_name = "libsmb2",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const URLProtocol ff_libsmb2_protocol = {
    .name            = "smb",
    .url_open        = libsmb2_open,
    .url_read        = libsmb2_read,
    .url_write       = libsmb2_write,
    .url_seek        = libsmb2_seek,
    .url_close       = libsmb2_close,
    .url_delete      = libsmb2_delete,
    .url_move        = libsmb2_move,
    .url_open_dir    = libsmb2_open_dir,
    .url_read_dir    = libsmb2_read_dir,
    .url_close_dir   = libsmb2_close_dir,
    .priv_data_size  = sizeof(LIBSMB2Context),
    .priv_data_class = &libsmb2_context_class,
    .flags           = URL_PROTOCOL_FLAG_NETWORK,
};
