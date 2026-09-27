/*
 * Copyright (C) 2026 the ROTT-PS3 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * =======================================================================
 *
 * What the engine's C library calls turn into on the PS3. ps3/Makefile
 * renames these symbols in the engine's objects (objcopy --redefine-sym),
 * so Taradino's sources don't change:
 *
 *  - stdout and stderr (the engine prints what it's doing, and its
 *    errors) go to the log: on the PS3 nobody reads the TTY.
 *  - Paths: PSL1GHT's file functions only understand absolute paths
 *    (handoff notes 3.50). Relative ones ("." is one of Taradino's data
 *    dirs) are taken from USRDIR, and doubled slashes (the engine joins
 *    "dir/" + "/" + "file") are collapsed.
 *
 * =======================================================================
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ps3_platform.h"

/* ================================================================ */
/* stdout / stderr                                                    */
/* ================================================================ */

static int
IsConsole(FILE *f)
{
	return f == stdout || f == stderr;
}

int
PS3_vfprintf(FILE *f, const char *fmt, va_list ap)
{
	char buf[2048];
	int n;

	if (!IsConsole(f))
	{
		return vfprintf(f, fmt, ap);
	}

	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	PS3_LogRaw(buf);

	return n;
}

int
PS3_vprintf(const char *fmt, va_list ap)
{
	return PS3_vfprintf(stdout, fmt, ap);
}

int
PS3_fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = PS3_vfprintf(f, fmt, ap);
	va_end(ap);

	return n;
}

int
PS3_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = PS3_vfprintf(stdout, fmt, ap);
	va_end(ap);

	return n;
}

int
PS3_puts(const char *s)
{
	PS3_LogRaw(s);
	PS3_LogRaw("\n");
	return 1;
}

int
PS3_putchar(int c)
{
	char s[2] = {(char)c, 0};

	PS3_LogRaw(s);
	return c;
}

int
PS3_fputs(const char *s, FILE *f)
{
	if (!IsConsole(f))
	{
		return fputs(s, f);
	}

	PS3_LogRaw(s);
	return 1;
}

int
PS3_fputc(int c, FILE *f)
{
	if (!IsConsole(f))
	{
		return fputc(c, f);
	}

	return PS3_putchar(c);
}

int
PS3_putc(int c, FILE *f)
{
	return PS3_fputc(c, f);
}

size_t
PS3_fwrite(const void *p, size_t size, size_t n, FILE *f)
{
	char buf[1024];
	size_t total;

	if (!IsConsole(f))
	{
		return fwrite(p, size, n, f);
	}

	total = size * n;

	if (total >= sizeof(buf))
	{
		total = sizeof(buf) - 1;
	}

	memcpy(buf, p, total);
	buf[total] = '\0';
	PS3_LogRaw(buf);

	return n;
}

int
PS3_fflush(FILE *f)
{
	if (f && IsConsole(f))
	{
		return 0;   /* the log flushes every write */
	}

	return fflush(f);
}

void
PS3_perror(const char *s)
{
	PS3_Log("%s%s%s", s ? s : "", s ? ": " : "", strerror(errno));
}

/* ================================================================ */
/* Paths                                                              */
/* ================================================================ */

/* Absolute, without "//", "/./" or a leading "./". */
static const char *
FixPath(const char *path, char *out, size_t size)
{
	char tmp[1024];
	const char *s;
	size_t o = 0;

	if (!path)
	{
		return NULL;
	}

	if (path[0] == '/')
	{
		snprintf(tmp, sizeof(tmp), "%s", path);
	}
	else
	{
		while (path[0] == '.' && path[1] == '/')
		{
			path += 2;
		}

		if (!strcmp(path, "."))
		{
			path = "";
		}

		snprintf(tmp, sizeof(tmp), "%s/%s", PS3_USRDIR, path);
	}

	for (s = tmp; *s && o + 1 < size; s++)
	{
		if (*s == '/' && o > 0 && out[o - 1] == '/')
		{
			continue;   /* "//" */
		}

		if (*s == '.' && o > 0 && out[o - 1] == '/' && (s[1] == '/' || s[1] == '\0'))
		{
			if (s[1] == '/')
			{
				s++;    /* "/./" */
			}

			continue;
		}

		out[o++] = *s;
	}

	/* no trailing slash, except for "/" itself */
	while (o > 1 && out[o - 1] == '/')
	{
		o--;
	}

	out[o] = '\0';

	return out;
}

int
PS3_open(const char *path, int flags, ...)
{
	char p[1024];
	int mode = 0;

	if (flags & O_CREAT)
	{
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}

	return open(FixPath(path, p, sizeof(p)), flags, mode);
}

FILE *
PS3_fopen(const char *path, const char *mode)
{
	char p[1024];

	return fopen(FixPath(path, p, sizeof(p)), mode);
}

int
PS3_stat(const char *path, struct stat *st)
{
	char p[1024];

	return stat(FixPath(path, p, sizeof(p)), st);
}

/* PSL1GHT's newlib has no access(): a stat() is as good for what the
   engine asks (does it exist?). */
int
PS3_access(const char *path, int mode)
{
	struct stat st;

	(void)mode;

	if (PS3_stat(path, &st) != 0)
	{
		errno = ENOENT;
		return -1;
	}

	return 0;
}

int
PS3_mkdir(const char *path, mode_t mode)
{
	char p[1024];

	return mkdir(FixPath(path, p, sizeof(p)), mode);
}

DIR *
PS3_opendir(const char *path)
{
	char p[1024];

	return opendir(FixPath(path, p, sizeof(p)));
}

int
PS3_remove(const char *path)
{
	char p[1024];

	return remove(FixPath(path, p, sizeof(p)));
}

int
PS3_unlink(const char *path)
{
	char p[1024];

	return unlink(FixPath(path, p, sizeof(p)));
}

int
PS3_rename(const char *from, const char *to)
{
	char a[1024], b[1024];

	return rename(FixPath(from, a, sizeof(a)), FixPath(to, b, sizeof(b)));
}

/* There is no working directory on the PS3. The engine only changes to a
   directory to read the levels of a mod (always given with an absolute
   path) and back: succeed when the directory exists. */
int
PS3_chdir(const char *path)
{
	DIR *d = PS3_opendir(path);

	if (!d)
	{
		errno = ENOENT;
		return -1;
	}

	closedir(d);

	return 0;
}

char *
PS3_getcwd(char *buf, size_t size)
{
	if (!buf || size < sizeof(PS3_USRDIR))
	{
		errno = ERANGE;
		return NULL;
	}

	memcpy(buf, PS3_USRDIR, sizeof(PS3_USRDIR));
	return buf;
}
