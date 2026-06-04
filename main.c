#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <signal.h>
#include <errno.h>
#include <err.h>
#include <fts.h>
#include <sys/stat.h>
#include <sys/acl.h>
#include <sys/xattr.h>
#include <pwd.h>
#include <grp.h>
#include "ls.h"

ls_options_t g_opts;
time_t       g_now;
int          g_exit_status = 0;

/* ── entry helpers ────────────────────────────────────────── */

entry_list_t *entry_list_new(void)
{
    entry_list_t *list = calloc(1, sizeof(*list));
    list->capacity = 32;
    list->items    = calloc(list->capacity, sizeof(ls_entry_t *));
    return list;
}

void entry_list_add(entry_list_t *list, ls_entry_t *e)
{
    if (list->count >= list->capacity) {
        list->capacity *= 2;
        list->items = realloc(list->items, list->capacity * sizeof(ls_entry_t *));
        if (!list->items) err(1, "realloc");
    }
    list->items[list->count++] = e;
    /* 512-byte block count stored in st_blocks; convert if needed */
    list->total_blocks += e->st.st_blocks;
}

void entry_list_free(entry_list_t *list)
{
    for (int i = 0; i < list->count; i++)
        free_entry(list->items[i]);
    free(list->items);
    free(list);
}

ls_entry_t *make_entry(const char *name, const char *path, struct stat *st)
{
    ls_entry_t *e = calloc(1, sizeof(*e));
    if (!e) err(1, "calloc");

    strncpy(e->name, name, sizeof(e->name) - 1);
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->st = *st;

    /* Read symlink target. */
    if (S_ISLNK(st->st_mode)) {
        ssize_t len = readlink(path, e->link_target, sizeof(e->link_target) - 1);
        if (len > 0) {
            e->link_target[len] = '\0';
            e->link_ok = (stat(path, &e->link_st) == 0);
        }
    }

    /* Check for extended ACL. */
    acl_t acl = acl_get_link_np(path, ACL_TYPE_EXTENDED);
    if (acl) {
        acl_entry_t ace;
        e->has_acl = (acl_get_entry(acl, ACL_FIRST_ENTRY, &ace) == 0);
        acl_free(acl);
    }

    /* Check for extended attributes. */
    ssize_t xattr_size = listxattr(path, NULL, 0, XATTR_NOFOLLOW);
    if (xattr_size > 0) {
        e->has_xattr   = 1;
        e->xattr_count = 0;
        /* Count the null-separated names. */
        char *xbuf = malloc(xattr_size);
        if (xbuf) {
            listxattr(path, xbuf, xattr_size, XATTR_NOFOLLOW);
            for (ssize_t i = 0; i < xattr_size; i++)
                if (xbuf[i] == '\0') e->xattr_count++;
            free(xbuf);
        }
    }

    return e;
}

void free_entry(ls_entry_t *e)
{
    free(e);
}

/* ── dot-file filter ──────────────────────────────────────── */

static int should_skip(const char *name)
{
    if (g_opts.show_all) return 0;
    if (name[0] != '.') return 0;
    if (g_opts.show_almost_all &&
        (strcmp(name, ".") == 0 || strcmp(name, "..") == 0))
        return 1;
    if (!g_opts.show_almost_all) return 1;
    return 0;
}

/* Add a synthetic entry for "." or ".." using stat on the real path. */
static void add_dot_entry(entry_list_t *list, const char *name,
                          const char *dir_path)
{
    char full[4096];
    snprintf(full, sizeof(full), "%s/%s", dir_path, name);
    struct stat st;
    if (lstat(full, &st) == 0) {
        ls_entry_t *e = make_entry(name, full, &st);
        entry_list_add(list, e);
    }
}

/* ── collect all direct children of an fts directory node ── */

static entry_list_t *collect_dir_children(FTS *fts, FTSENT *dir_node)
{
    entry_list_t *list = entry_list_new();

    /* Prepend . and .. when -a is set (fts doesn't yield them by default). */
    if (g_opts.show_all) {
        add_dot_entry(list, ".",  dir_node->fts_path);
        add_dot_entry(list, "..", dir_node->fts_path);
    }

    FTSENT *child = fts_children(fts, 0);
    if (!child && errno) {
        warn("%s", dir_node->fts_path);
        return list;
    }
    for (FTSENT *cp = child; cp; cp = cp->fts_link) {
        if (should_skip(cp->fts_name)) continue;
        /* fts_children results have fts_path = parent dir, not the full
           child path.  Build the real path from parent + "/" + name. */
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s",
                 dir_node->fts_path, cp->fts_name);
        ls_entry_t *e = make_entry(cp->fts_name, full_path, cp->fts_statp);
        entry_list_add(list, e);
    }
    return list;
}

/* ── main traversal ───────────────────────────────────────── */

static void traverse(char **paths, int n_paths)
{
    int fts_flags = FTS_PHYSICAL;   /* lstat() by default */
    if (g_opts.follow_all_links)
        fts_flags = FTS_LOGICAL;
    else if (g_opts.follow_cmd_links)
        fts_flags |= FTS_COMFOLLOW;

    /* fts uses 0-terminated array. */
    FTS *fts = fts_open(paths, fts_flags,
                        g_opts.sort_by == SORT_NONE ? NULL : fts_name_compare);
    if (!fts) err(1, "fts_open");

    /* Non-directory command-line entries go here and print first. */
    entry_list_t *cmdline_files = entry_list_new();

    int printed = 0;        /* whether we've emitted any output yet  */
    /* Show "dirname:" headers when multiple top-level dirs are given,
       OR when recursing into a SUBDIRECTORY (level > 0).  Never for the
       single root-level directory (that would be redundant noise). */
    int need_label_for_multiple = (n_paths > 1);

    FTSENT *p;
    while ((p = fts_read(fts)) != NULL) {
        switch (p->fts_info) {

        case FTS_D:
            if (p->fts_level == FTS_ROOTLEVEL && g_opts.list_dir_itself) {
                /* -d: show directory itself, don't descend. */
                ls_entry_t *e = make_entry(p->fts_name, p->fts_path,
                                           p->fts_statp);
                entry_list_add(cmdline_files, e);
                fts_set(fts, p, FTS_SKIP);
                break;
            }
            if (p->fts_level == FTS_ROOTLEVEL || g_opts.recursive) {
                /* Print any accumulated non-dir command-line files first. */
                if (cmdline_files->count > 0) {
                    display_list(cmdline_files, NULL, 0);
                    entry_list_free(cmdline_files);
                    cmdline_files = entry_list_new();
                    printed = 1;
                }
                if (printed) putchar('\n');
                printed = 1;

                entry_list_t *dir_list = collect_dir_children(fts, p);
                /* Show label for: (a) multiple top-level args, or
                   (b) any recursed subdirectory (level > 0). */
                int show_label = need_label_for_multiple ||
                                 (g_opts.recursive && p->fts_level > FTS_ROOTLEVEL);
                const char *label = show_label ? p->fts_path : NULL;
                int show_total = (g_opts.format == FMT_LONG || g_opts.show_blocks);
                display_list(dir_list, label, show_total);
                entry_list_free(dir_list);

                if (!g_opts.recursive)
                    fts_set(fts, p, FTS_SKIP);
                /* else: let fts descend for recursive processing */
            }
            break;

        case FTS_F:
        case FTS_SL:
        case FTS_SLNONE:
        case FTS_DEFAULT:
            if (p->fts_level == FTS_ROOTLEVEL) {
                /* Use the full path as given on the command line as the
                   display name (e.g. /usr/bin/python3 not just python3). */
                ls_entry_t *e = make_entry(p->fts_path, p->fts_path,
                                           p->fts_statp);
                entry_list_add(cmdline_files, e);
            }
            break;

        case FTS_DP:
            /* Leaving a directory — nothing to do (already printed on FTS_D). */
            break;

        case FTS_DC:
            warnx("%s: directory causes a cycle", p->fts_path);
            break;

        case FTS_ERR:
        case FTS_NS:
            warnx("%s: %s", p->fts_path, strerror(p->fts_errno));
            g_exit_status = 1;
            break;

        default:
            break;
        }
    }
    if (errno) err(1, "fts_read");
    fts_close(fts);

    /* Print remaining command-line non-directory entries. */
    if (cmdline_files->count > 0) {
        if (printed) putchar('\n');
        display_list(cmdline_files, NULL, 0);
    }
    entry_list_free(cmdline_files);
}

/* ── signal handler for SIGINFO ───────────────────────────── */

static volatile sig_atomic_t g_siginfo = 0;
static void on_siginfo(int sig) { (void)sig; g_siginfo = 1; }

/* ── main ─────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    time(&g_now);

    /* stdout_is_tty must be set before parse_options so --color=auto works. */
    g_opts.stdout_is_tty = isatty(STDOUT_FILENO);
    g_opts.format = g_opts.stdout_is_tty ? FMT_COLUMN : FMT_SINGLE;

    int first_arg;
    parse_options(argc, argv, &first_arg);

    signal(SIGINFO, on_siginfo);

    /* Build the path list for fts_open; use "." if no arguments. */
    char *default_dot[] = { ".", NULL };
    char **paths;
    int n_paths;

    if (first_arg >= argc) {
        paths   = default_dot;
        n_paths = 1;
    } else {
        n_paths = argc - first_arg;
        paths   = argv + first_arg;

        /* When all command-line args are non-directories, fts will call
           them FTS_ROOTLEVEL files — no "dirname:" labels needed. */
    }

    /* fts_open needs a NULL-terminated array. argv is already NULL-terminated
       when we use argv + first_arg, but the default_dot array is too. */
    traverse(paths, n_paths);

    /* Flush stdout and check for write errors. */
    if (fflush(stdout) != 0 || ferror(stdout))
        err(1, "stdout");

    return g_exit_status;
}
