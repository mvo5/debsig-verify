/*
 * debsig-verify - Debian package signature verification tool
 *
 * Copyright © 2000 Ben Collins <bcollins@debian.org>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * processes ar style archives (the format of a .deb package)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ar.h>

#include "debsig.h"

/* borrowed from dpkg */
static unsigned long parseLength(const char *inh, size_t len) {
    char buf[16];
    unsigned long r;
    char *endp;

    if (memchr(inh, 0, len))
	ds_fail_printf(DS_FAIL_INTERNAL, "parseLength: member lenght contains NULL's");

    assert(sizeof(buf) > len);
    memcpy(buf, inh, len);
    buf[len]= ' ';
    *strchr(buf,' ') = 0;
    r = strtoul(buf,&endp,10);

    if (*endp)
	ds_fail_printf(DS_FAIL_INTERNAL, "parseLength: archive is corrupt - bad digit `%c' member length",
		  *endp);

    return r;
}

/* This function takes a member name as an argument. It then goes through
 * the archive trying to find it. If it does, it returns the size of the
 * member's data, and leaves the deb_fs file pointer at the start of that
 * data. Yes, we may have a zero length member in here somewhere, but
 * nothing important is going to be zero length anyway, so we treat it as
 * "non-existant".  */
size_t findMember(const char *name) {
    char magic[SARMAG+1];
    struct ar_hdr arh;
    long mem_len;
    int len = strlen(name);

    if (len > sizeof(arh.ar_name)) {
	ds_printf(DS_LEV_DEBUG, "findMember: `%s' is too long to be an archive member name",
		  name);
	return 0;
    }
    
    /* This shouldn't happen, but... */
    if (deb_fs == NULL)
	ds_fail_printf(DS_FAIL_INTERNAL, "findMember: called while deb_fs == NULL");

    rewind(deb_fs);
    
    if (!fgets(magic,sizeof(magic),deb_fs))
	ds_fail_printf(DS_FAIL_INTERNAL, "findMember: failure to read package (%s)",
		  strerror(errno));

    /* We will fail in main() with this one */
    if (strcmp(magic,ARMAG)) {
	ds_printf(DS_LEV_DEBUG, "findMember: archive has bad magic");
	return 0;
    }

    while(!feof(deb_fs)) {
	if (fread(&arh, 1, sizeof(arh),deb_fs) != sizeof(arh)) {
	    if (ferror(deb_fs))
		ds_fail_printf(DS_FAIL_INTERNAL, "findMember: error while parsing archive header (%s)",
			  strerror(errno));
	    return 0;
	}

	if (memcmp(arh.ar_fmag, ARFMAG, sizeof(arh.ar_fmag)))
	    ds_fail_printf(DS_FAIL_INTERNAL, "findMember: archive appears to be corrupt, fmag incorrect");

	if ((mem_len = parseLength(arh.ar_size, sizeof(arh.ar_size))) < 0)
	    ds_fail_printf(DS_FAIL_INTERNAL,
			   "findMember: archive appears to be corrupt, negative member length");

	/*
	 * If all looks well, then we return the length of the member, and
	 * leave the file pointer where it is (at the start of the data).
	 * The logic here is based on the ar spec. The ar_name field is
	 * padded with spaces to get the full length. The actual name may
	 * also be suffixed with '/' (dpkg-deb creates .deb's without the
	 * trailing '/' in the member names, but binutils ar does, so we
	 * try to be compatible, like dpkg does). We don't support the
	 * "extended naming" scheme that binutils does.
	 */
	if (!strncmp(arh.ar_name, name, len) && (len == sizeof(arh.ar_name) ||
		    arh.ar_name[len] == '/' || arh.ar_name[len] == ' '))
	    return (size_t)mem_len;

	/* fseek to the start of the next member, and try again */
	if (fseek(deb_fs, mem_len + (mem_len & 1), SEEK_CUR) == -1 && ferror(deb_fs))
	    ds_fail_printf(DS_FAIL_INTERNAL,
			   "findMember: error during file seek (%s)", strerror(errno));
    }

    /* well, nothing found, so let's pass on the bad news */
    return 0;
}
