#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/acl.h>
#include <pwd.h>
#include <grp.h>
#include <wchar.h>
#include <locale.h>
#include "ls.h"

/* ── timestamp helpers ────────────────────────────────────── */

static time_t entry_timestamp(const ls_entry_t *e)
{
    switch (g_opts.time_field) {
    case TIME_ATIME: return e->st.st_atimespec.tv_sec;
    case TIME_CTIME: return e->st.st_ctimespec.tv_sec;
    case TIME_BTIME: return e->st.st_birthtimespec.tv_sec;
    default:         return e->st.st_mtimespec.tv_sec;
    }
}

/* Print timestamp in ls long-format style. */
static void print_timestamp(time_t t)
{
    char buf[64];

    if (g_opts.date_format) {
        struct tm *tm = localtime(&t);
        strftime(buf, sizeof(buf), g_opts.date_format, tm);
        printf("%s ", buf);
        return;
    }

    struct tm *tm = localtime(&t);
    double age = difftime(g_now, t);

    if (g_opts.complete_time) {
        strftime(buf, sizeof(buf), "%b %e %T %Y ", tm);
    } else if (age >= 0 && age < 15778476.0) { /* ~6 months */
        strftime(buf, sizeof(buf), "%b %e %H:%M ", tm);
    } else {
        strftime(buf, sizeof(buf), "%b %e  %Y ", tm);
    }
    fputs(buf, stdout);
}

/* ── name display ─────────────────────────────────────────── */

/* Print one character of a filename, applying -b/-q/-v translation. */
static void print_name_char(unsigned char c)
{
    if (g_opts.raw_output || (c >= 32 && c < 127) || c >= 160) {
        putchar(c);
    } else if (g_opts.print_octal) {
        printf("\\%03o", c);
    } else if (g_opts.print_question) {
        putchar('?');
    } else {
        putchar(c); /* default: raw passthrough */
    }
}

static void print_name(const char *name)
{
    while (*name)
        print_name_char((unsigned char)*name++);
}

/* Append the -F indicator character for a file's type/permissions. */
static void print_indicator(const ls_entry_t *e)
{
    mode_t m = e->st.st_mode;
    if      (S_ISDIR(m))                           putchar('/');
    else if (S_ISLNK(m))                           putchar('@');
    else if (S_ISSOCK(m))                          putchar('=');
    else if (S_ISFIFO(m))                          putchar('|');
    else if (m & (S_IXUSR | S_IXGRP | S_IXOTH))   putchar('*');
    else if (S_ISREG(m) && (m & S_ISVTX))         putchar('%');
}

/* Print name with optional color and indicator — used by short-format outputs. */
void print_name_with_color(ls_entry_t *e)
{
    if (g_opts.colorize) fputs(color_for_entry(e), stdout);
    fputs(e->name, stdout);
    if (g_opts.colorize) color_reset_print();
    if (g_opts.append_indicator)
        print_indicator(e);
    else if (g_opts.append_slash && S_ISDIR(e->st.st_mode))
        putchar('/');
}

/* Compute the terminal display width of an entry's name. */
int entry_display_len(ls_entry_t *e)
{
    int len = 0;
    const char *p = e->name;
    mbstate_t mbs = {0};
    wchar_t wc;
    size_t n;

    while ((n = mbrtowc(&wc, p, MB_LEN_MAX, &mbs)) > 0 && n != (size_t)-1) {
        int w = wcwidth(wc);
        if (w > 0) len += w;
        else if (w < 0) len++; /* non-printable counted as 1 */
        p += n;
    }
    /* Account for indicator character. */
    if (g_opts.append_indicator || g_opts.append_slash)
        len++;
    return len;
}

/* ── size display ─────────────────────────────────────────── */

/* Format bytes into human-readable form: 1.2K, 3.4M, etc. */
static void format_human_size(char *buf, size_t bufsz, off_t size)
{
    static const char units[] = "BKMGTPE";
    double val = (double)size;
    int u = 0;
    while (val >= 1024.0 && u < 6) { val /= 1024.0; u++; }
    if (u == 0)
        snprintf(buf, bufsz, "%lld", (long long)size);
    else if (val < 10.0)
        snprintf(buf, bufsz, "%.1f%c", val, units[u]);
    else
        snprintf(buf, bufsz, "%.0f%c", val, units[u]);
}

static void print_size(const ls_entry_t *e, int width)
{
    if (S_ISCHR(e->st.st_mode) || S_ISBLK(e->st.st_mode)) {
        printf("%*s %3d, %3d ", width, "",
               major(e->st.st_rdev), minor(e->st.st_rdev));
        return;
    }

    if (g_opts.human_readable) {
        char hbuf[16];
        format_human_size(hbuf, sizeof(hbuf), e->st.st_size);
        printf("%*s ", width, hbuf);
    } else if (g_opts.comma_separator) {
        printf("%'*lld ", width, (long long)e->st.st_size);
    } else {
        printf("%*lld ", width, (long long)e->st.st_size);
    }
}

/* ── block count ──────────────────────────────────────────── */

void print_total_blocks(blkcnt_t blocks)
{
    blkcnt_t scaled = g_opts.kibibytes ? (blocks / 2) : blocks;
    printf("total %llu\n", (unsigned long long)scaled);
}

/* ── xattr / ACL suffix characters ───────────────────────── */

static char acl_suffix(const ls_entry_t *e)
{
    if (e->has_xattr) return '@';  /* always shown when xattrs present */
    if (e->has_acl)   return '+';
    return ' ';
}

/* ── main long-format printer ─────────────────────────────── */

void print_long_entry(ls_entry_t *e,
                      int inode_w, int blocks_w, int nlinks_w,
                      int size_w,  int owner_w,  int group_w)
{
    /* Inode number */
    if (g_opts.show_inode)
        printf("%*ju ", inode_w, (uintmax_t)e->st.st_ino);

    /* 512-byte blocks used */
    if (g_opts.show_blocks) {
        blkcnt_t b = g_opts.kibibytes ? e->st.st_blocks / 2 : e->st.st_blocks;
        printf("%*lld ", blocks_w, (long long)b);
    }

    /* Permission string: strmode returns 11 chars; last char is the special
       flag (S/T/etc).  We overwrite it with the ACL/xattr marker so the
       output matches the system ls column alignment exactly. */
    char mode_str[12];
    strmode(e->st.st_mode, mode_str);
    mode_str[10] = acl_suffix(e);  /* replace trailing flag with our marker */
    mode_str[11] = '\0';
    fputs(mode_str, stdout);

    /* Hard link count */
    printf(" %*ju ", nlinks_w, (uintmax_t)e->st.st_nlink);

    /* Owner */
    if (!g_opts.no_owner) {
        if (g_opts.numeric_ids) {
            printf("%-*u  ", owner_w, e->st.st_uid);
        } else {
            const char *uname = user_from_uid(e->st.st_uid, 0);
            printf("%-*s  ", owner_w, uname ? uname : "");
        }
    }

    /* Group */
    if (!g_opts.no_group) {
        if (g_opts.numeric_ids) {
            printf("%-*u  ", group_w, e->st.st_gid);
        } else {
            const char *gname = group_from_gid(e->st.st_gid, 0);
            printf("%-*s  ", group_w, gname ? gname : "");
        }
    }

    /* BSD file flags (-O) */
    if (g_opts.show_flags) {
        char *flags = fflagstostr(e->st.st_flags);
        printf("%-8s ", flags ? flags : "-");
        free(flags);
    }

    /* File size */
    print_size(e, size_w);

    /* Timestamp */
    print_timestamp(entry_timestamp(e));

    /* Filename */
    if (g_opts.colorize) {
        fputs(color_for_entry(e), stdout);
        print_name(e->name);
        color_reset_print();
    } else {
        print_name(e->name);
    }

    if (g_opts.append_indicator)
        print_indicator(e);
    else if (g_opts.append_slash && S_ISDIR(e->st.st_mode))
        putchar('/');

    /* Symlink target */
    if (S_ISLNK(e->st.st_mode) && e->link_target[0]) {
        printf(" -> ");
        if (g_opts.colorize && e->link_ok) {
            fputs(color_for_entry(e), stdout); /* reuse entry color for simplicity */
            color_reset_print();
        }
        print_name(e->link_target);
    }

    putchar('\n');

    /* Extended ACL entries */
    if (g_opts.show_acl && e->has_acl) {
        acl_t acl = acl_get_link_np(e->path, ACL_TYPE_EXTENDED);
        if (!acl) acl = acl_get_link_np(e->path, ACL_TYPE_ACCESS);
        if (acl) {
            acl_entry_t ace;
            int idx = ACL_FIRST_ENTRY;
            while (acl_get_entry(acl, idx, &ace) == 0) {
                idx = ACL_NEXT_ENTRY;
                /* Print raw ACL text — simplified */
                char *text = acl_to_text(acl, NULL);
                if (text) { fputs(text, stdout); acl_free(text); }
                break;
            }
            acl_free(acl);
        }
    }
}
