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
 * main routines
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>

#include <dpkg/dpkg.h>

#include "debsig.h"

char *rootdir = "";

#define CTAR(x) "control.tar" # x
#define DTAR(x) "data.tar" # x
char *ver_magic_member = "debian-binary";
char *ver_ctrl_members[] = { CTAR(), CTAR(.gz), CTAR(.xz), 0 };
char *ver_data_members[] = { DTAR(), DTAR(.gz), DTAR(.xz), DTAR(.bz2), DTAR(.lzma), 0 };

static int checkSelRules(struct debsig_ctx *ds_ctx, struct group *grp) {
    int opt_count = 0;
    struct match *mtc;
    int len;

    for (mtc = grp->matches; mtc; mtc = mtc->next) {

        ds_printf(DS_LEV_VER, "      Processing '%s' key...", mtc->name);

        /* If we have an ID for this match, check to make sure it exists, and
         * matches the signature we are about to check.  */
        if (mtc->id) {
            char *m_id = getKeyID(ds_ctx->originID, mtc);
            char *d_id = getSigKeyID(ds_ctx, mtc->name);
            if (m_id == NULL || d_id == NULL || strcmp(m_id, d_id))
                return 0;
        }

	/* XXX: If the match doesn't specify an ID, we need to check to
	 * make sure the ID of the signature exists in the keyring
	 * specified, don't we?
	 */

        len = checkSigExist(ds_ctx, mtc->name);

        /* If the member exists and we reject it, fail now. Also, if it
         * doesn't exist, and we require it, fail as well. */
        if ((!len && mtc->type == REQUIRED_MATCH) ||
                (len && mtc->type == REJECT_MATCH)) {
            return 0;
        }
        /* This would mean this is Optional, so we ignore it for now */
        if (!len) continue;

        /* Kick up the count once for checking later */
        if (mtc->type == OPTIONAL_MATCH)
            opt_count++;
    }

    if (opt_count < grp->min_opt) {
        ds_printf(DS_LEV_DEBUG, "checkSelRules: opt passed - %d, opt required %d",
                  opt_count, grp->min_opt);
        return 0;
    }

    return 1;
}

static off_t
passthrough(FILE *in, FILE *out, off_t len)
{
    char buf[2048];
    int t;

    while (len > 0) {
        t = fread(buf, 1, sizeof(buf), in);
        fwrite(buf, 1, (t > len) ? len : t, out);
        len -= t;
    }

    return len;
}

static int verifyGroupRules(struct debsig_ctx *ds_ctx, struct group *grp) {
    FILE *fp;
    char tmp_sig[32] = {'\0'}, tmp_data[32] = {'\0'};
    int opt_count = 0, t, i, fd;
    struct match *mtc;
    off_t len;

    /* If we don't have any matches, we fail. We don't want blank,
     * take-all rules. This actually gets checked while we parse the
     * policy file, but we check again for good measure.  */
    if (grp->matches == NULL)
	return 0;

    /* Go ahead and write out our data to a temp file */
    strncpy(tmp_data, "/tmp/debsig-data.XXXXXX", sizeof(tmp_data));
    if ((fd = mkstemp(tmp_data)) == -1 || (fp = fdopen(fd, "w+")) == NULL) {
	ds_printf(DS_LEV_ERR, "error creating temp file %s: %s\n",
		  tmp_data, strerror(errno));
	if (fd != -1) {
	    close(fd);
	    unlink(tmp_data);
	}
	return 0;
    }

    /* Now, let's find all the members we need to check and cat them into a
     * single temp file. This is what we pass to gpg.  */
    if (!(len = findMember(ds_ctx, ver_magic_member)))
        goto fail_and_close;
    len = passthrough(ds_ctx->deb_fs, fp, len);

    for (i = 0; ver_ctrl_members[i]; i++) {
       if (!(len = findMember(ds_ctx, ver_ctrl_members[i])))
	    continue;
	len = passthrough(ds_ctx->deb_fs, fp, len);
	break;
    }
    if (!ver_ctrl_members[i])
	goto fail_and_close;

    for (i = 0; ver_data_members[i]; i++) {
        if (!(len = findMember(ds_ctx, ver_data_members[i])))
	    continue;
	len = passthrough(ds_ctx->deb_fs, fp, len);
	break;
    }
    if (!ver_data_members[i])
	goto fail_and_close;

    fclose(fp);
    fd = -1;

    for (mtc = grp->matches; mtc; mtc = mtc->next) {

	ds_printf(DS_LEV_VER, "      Processing '%s' key...", mtc->name);

	/* If we have an ID for this match, check to make sure it exists, and
	 * matches the signature we are about to check.  */
	if (mtc->id) {
            char *m_id = getKeyID(ds_ctx->originID, mtc);
	    char *d_id = getSigKeyID(ds_ctx, mtc->name);
	    if (m_id == NULL || d_id == NULL || strcmp(m_id, d_id))
		goto fail_and_close;
	}

	/* This will also position deb_fs to the start of the member */
	len = checkSigExist(ds_ctx, mtc->name);

	/* If the member exists and we reject it, die now. Also, if it
	 * doesn't exist, and we require it, die as well. */
	if ((!len && mtc->type == REQUIRED_MATCH) ||
		(len && mtc->type == REJECT_MATCH)) {
	    goto fail_and_close;
	}

	/* This would mean this is Optional, so we ignore it for now */
	if (!len) continue;

	/* let's get our temp file */
	strncpy(tmp_sig, "/tmp/debsig-sig.XXXXXX", sizeof(tmp_sig));
	if ((fd = mkstemp(tmp_sig)) == -1 || (fp = fdopen(fd, "w+")) == NULL) {
	    ds_printf(DS_LEV_ERR, "error creating temp file %s: %s\n",
		      tmp_sig, strerror(errno));
	    goto fail_and_close;
	}

	len = passthrough(ds_ctx->deb_fs, fp, len);
	fclose(fp);

	/* Now, let's check with gpg on this one */
	t = gpgVerify(ds_ctx->originID, tmp_data, mtc, tmp_sig);

	fd = -1;
	unlink(tmp_sig);

	/* We fail no matter what now. Even if this is an optional match
	 * rule, by now, we know that the sig exists, so we must fail */
	if (!t) {
	    ds_printf(DS_LEV_DEBUG, "verifyGroupRules: failed for %s", mtc->name);
	    goto fail_and_close;
	}

	/* Kick up the count once for checking later */
	if (mtc->type == OPTIONAL_MATCH)
	    opt_count++;
    }

    if (opt_count < grp->min_opt) {
	ds_printf(DS_LEV_DEBUG, "verifyGroupRules: opt passed - %d, opt required %d",
		  opt_count, grp->min_opt);
	goto fail_and_close;
    }

    unlink(tmp_data);
    return 1;

fail_and_close:
    unlink(tmp_data);
    if (fd != -1) {
	close(fd);
	unlink(tmp_sig);
    }
    return 0;
}

static int checkIsDeb(struct debsig_ctx *ds_ctx) {
    int i;
    const char *member;

    if (!findMember(ds_ctx, ver_magic_member)) {
       ds_printf(DS_LEV_VER, "Missing archive magic member %s", ver_magic_member);
       return 0;
    }

    for (i = 0; (member = ver_ctrl_members[i]); i++)
        if (findMember(ds_ctx, member))
            break;
    if (!member) {
        ds_printf(DS_LEV_VER, "Missing archive control member, checked:");
        for (i = 0; (member = ver_ctrl_members[i]); i++)
            ds_printf(DS_LEV_VER, "    %s", member);
        return 0;
    }

    for (i = 0; (member = ver_data_members[i]); i++)
        if (findMember(ds_ctx, member))
            break;
    if (!member) {
        ds_printf(DS_LEV_VER, "Missing archive data member, checked:");
        for (i = 0; (member = ver_data_members[i]); i++)
            ds_printf(DS_LEV_VER, "    %s", member);
        return 0;
    }

    return 1;
}

static void outputVersion(void) {
    fprintf(stderr, "\
Debsig Program Version - "VERSION"\n\
  Signature Version - "SIG_VERSION"\n\
  Signature Namespace - "DEBSIG_NAMESPACE"\n\
  Policies Directory - "DEBSIG_POLICIES_DIR"\n\
  Keyrings Directory - "DEBSIG_KEYRINGS_DIR"\n");
    return;
}

static void outputUsage(void) {
        fprintf(stderr, "\
Usage: %s [ options ] <deb>\n\n\
   -q                  Quiet, only output fatal errors\n\
   -v                  Verbose output (mainly debug)\n\
   -d                  Debug output as well\n\
   --version           Output version info, and exit\n\
   --list-policies     Only list policies that can be used to\n\
                       validate this sig. This runs through\n\
                       'Selection' block of the policies only.\n\
   --use-policy <name> Used in conjunction with the above\n\
                       option. This allows you to specify the\n\
                       short name of the policy you wish to try.\n\
   --rootdir <dir>     Use a alternative root directory for policy lookup\n",
	dpkg_get_progname());
        exit(1);
}

void
ds_catch_fatal_error(void)
{
    pop_error_context(ehflag_bombout);
    exit(DS_FAIL_INTERNAL);
}

void
ds_print_fatal_error(const char *emsg, const void *data)
{
    ds_printf(DS_FAIL_INTERNAL, "%s", emsg);
}

int main(int argc, char *argv[]) {
    struct policy *pol = NULL;
    char buf[8192], pol_file[8192], *tmpID, *force_file = NULL;
    DIR *pd = NULL;
    struct dirent *pd_ent;
    struct group *grp;
    int i, list_only = 0;

    dpkg_set_progname(argv[0]);

    push_error_context_func(ds_catch_fatal_error, ds_print_fatal_error, NULL);

    if (argc < 2)
	outputUsage();

    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
	if (!strcmp(argv[i], "-q"))
	    ds_debug_level = DS_LEV_ERR;
	else if (!strcmp(argv[i], "-v"))
	    ds_debug_level = DS_LEV_VER;
	else if (!strcmp(argv[i], "-d"))
	    ds_debug_level = DS_LEV_DEBUG;
	else if (!strcmp(argv[i], "--version")) {
	    outputVersion();
	    /* Make sure we exit non-zero if there are any more args. This
	     * makes sure someone doesn't do something stupid like pass
	     * --version and a .deb, and expect it to return a validation
	     * exit status.  */
	    if (argc > 2)
		exit(1);
	    else
		exit(0);
	} else if (!strcmp(argv[i], "--list-policies")) {
	    /* Just create a list of policies we can use */
	    list_only = 1;
	    ds_printf(DS_LEV_ALWAYS, "Listing usable policies");
	} else if (!strcmp(argv[i], "--use-policy")) {
	    /* We take one arg */
	    force_file = argv[++i];
	    if (i == argc || force_file[0] == '-') {
		ds_printf(DS_LEV_ERR, "--use-policy requires an argument");
		outputUsage();
	    }
	} else if (!strcmp(argv[i], "--rootdir")) {
	    rootdir = argv[++i];
	    if (i == argc || rootdir[0] == '-') {
		ds_printf(DS_LEV_ERR, "--rootdir requires an argument");
		outputUsage();
	    }
	} else
	    outputUsage();
    }

    if (i + 1 != argc) /* There should only be one arg left */
	outputUsage();

    struct debsig_ctx ds_ctx;
    ds_ctx.deb = argv[i];

    if ((ds_ctx.deb_fs = fopen(ds_ctx.deb, "r")) == NULL)
	ds_fail_printf(DS_FAIL_INTERNAL, "could not open %s (%s)", ds_ctx.deb, strerror(errno));

    if (!list_only)
	ds_printf(DS_LEV_VER, "Starting verification for: %s", ds_ctx.deb);

    if (!checkIsDeb(&ds_ctx))
	ds_fail_printf(DS_FAIL_INTERNAL, "%s does not appear to be a deb format package", ds_ctx.deb);

    if ((tmpID = getSigKeyID(&ds_ctx, "origin")) == NULL)
	ds_fail_printf(DS_FAIL_NOSIGS, "Origin Signature check failed. This deb might not be signed.\n");

    strncpy(ds_ctx.originID, tmpID, sizeof(ds_ctx.originID));

    /* Now we have an ID, let's check the policy to use */
    snprintf(buf, sizeof(buf) - 1, DEBSIG_POLICIES_DIR_FMT, rootdir, ds_ctx.originID);
    if ((pd = opendir(buf)) == NULL)
	ds_fail_printf(DS_FAIL_UNKNOWN_ORIGIN,
		       "Could not open Origin dir %s: %s\n", buf, strerror(errno));

    ds_printf(DS_LEV_VER, "Using policy directory: %s", buf);

    if (list_only)
	ds_printf(DS_LEV_ALWAYS, "  Policies in: %s", buf);

    while ((pd_ent = readdir(pd)) != NULL && (pol == NULL || list_only)) {
	char *ext = strstr(pd_ent->d_name, ".pol");
	/* Make sure we have the right name format */
	if (ext == NULL || 
            (size_t)(ext - pd_ent->d_name) + 4 != strlen(pd_ent->d_name))
	    continue;

	if (force_file != NULL && strcmp(pd_ent->d_name, force_file))
	    continue;

	/* Now try to parse the file */
	snprintf(pol_file, sizeof(pol_file) - 1, "%s/%s", buf, pd_ent->d_name);
	ds_printf(DS_LEV_VER, "  Parsing policy file: %s", pol_file);
	pol = parsePolicyFile(pol_file);

	if (pol == NULL) continue;

	/* Now let's see if this policy's selection is useful for this .deb  */
	ds_printf(DS_LEV_VER, "    Checking Selection group(s).");
	for (grp = pol->sels; grp != NULL; grp = grp->next) {
            if (!checkSelRules(&ds_ctx, grp)) {
		clear_policy();
		ds_printf(DS_LEV_VER, "    Selection group failed checks.");
		pol = NULL;
		break;
	    }
	}

	if (pol && list_only) {
	    ds_printf(DS_LEV_ALWAYS, "    Usable: %s", pd_ent->d_name);
	    list_only++;
	} else if (pol)
	    ds_printf(DS_LEV_VER, "    Selection group(s) passed, policy is usable.");
    }
    closedir(pd);

    if ((pol == NULL && !list_only) || list_only == 1) /* Damn, can't verify this one */
	ds_fail_printf(DS_FAIL_NOPOLICIES, "No applicable policy found.");

    if (list_only)
	exit(0); /* our job is done */

    ds_printf(DS_LEV_VER, "Using policy file: %s", pol_file);

    /* This should actually be caught in the xml-parsing. */
    if (pol->vers == NULL)
	ds_fail_printf(DS_FAIL_NOPOLICIES, "Failed, no Verification groups in policy.");

    /* Now the final test */
    ds_printf(DS_LEV_VER, "    Checking Verification group(s).");

    for (grp = pol->vers; grp; grp = grp->next) {
        if (!verifyGroupRules(&ds_ctx, grp)) {
	    ds_printf(DS_LEV_VER, "    Verification group failed checks.");
	    ds_fail_printf(DS_FAIL_BADSIG, "Failed verification for %s.", ds_ctx.deb);
	}
    }

    ds_printf(DS_LEV_VER, "    Verification group(s) passed, deb is validated.");

    ds_printf(DS_LEV_INFO, "Verified package from '%s' (%s)",
	      pol->description, pol->name);

    pop_error_context(ehflag_normaltidy);

    /* If we get here, then things passed just fine */
    exit(DS_SUCCESS);
}
