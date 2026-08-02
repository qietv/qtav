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

#ifndef AVFORMAT_LIBSMB2URL_H
#define AVFORMAT_LIBSMB2URL_H

typedef struct FFSMB2URL {
    char *domain;
    char *user;
    char *password;
    char *server;
    char *share;
    char *path;
} FFSMB2URL;

/**
 * Parse an smb:// URL for use with libsmb2.
 *
 * User information, share names, and paths are percent-decoded exactly once
 * and validated as UTF-8. A literal '+' is not converted to a space.
 */
int ff_smb2_parse_url(const char *url, FFSMB2URL *parsed);
void ff_smb2_free_url(FFSMB2URL *parsed);

#endif /* AVFORMAT_LIBSMB2URL_H */
