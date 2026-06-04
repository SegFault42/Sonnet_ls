#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include "ls.h"

/* ── long-format column width calculation ─────────────────── */

typedef struct {
    int inode_w;
    int blocks_w;
    int nlinks_w;
    int owner_w;
    int group_w;
    int size_w;
} long_widths_t;

static int digits(uintmax_t n)
{
    int d = 1;
    while (n >= 10) { n /= 10; d++; }
    return d;
}

static long_widths_t calc_long_widths(ls_entry_t **items, int count)
{
    long_widths_t w = { 1, 1, 1, 1, 1, 1 };

    for (int i = 0; i < count; i++) {
        ls_entry_t *e = items[i];

        if (g_opts.show_inode) {
            int d = digits((uintmax_t)e->st.st_ino);
            if (d > w.inode_w) w.inode_w = d;
        }
        if (g_opts.show_blocks) {
            blkcnt_t b = g_opts.kibibytes ? e->st.st_blocks / 2 : e->st.st_blocks;
            int d = digits((uintmax_t)(b < 0 ? 0 : b));
            if (d > w.blocks_w) w.blocks_w = d;
        }
        {
            int d = digits((uintmax_t)e->st.st_nlink);
            if (d > w.nlinks_w) w.nlinks_w = d;
        }
        if (!g_opts.no_owner) {
            if (g_opts.numeric_ids) {
                int d = digits((uintmax_t)e->st.st_uid);
                if (d > w.owner_w) w.owner_w = d;
            } else {
                const char *u = user_from_uid(e->st.st_uid, 0);
                int d = u ? (int)strlen(u) : 1;
                if (d > w.owner_w) w.owner_w = d;
            }
        }
        if (!g_opts.no_group) {
            if (g_opts.numeric_ids) {
                int d = digits((uintmax_t)e->st.st_gid);
                if (d > w.group_w) w.group_w = d;
            } else {
                const char *g = group_from_gid(e->st.st_gid, 0);
                int d = g ? (int)strlen(g) : 1;
                if (d > w.group_w) w.group_w = d;
            }
        }
        {
            int d;
            if (g_opts.human_readable) {
                d = 4; /* "1.2M" */
            } else {
                d = digits((uintmax_t)(e->st.st_size < 0 ? 0 : e->st.st_size));
            }
            if (d > w.size_w) w.size_w = d;
        }
    }
    return w;
}

/* ── column layout helpers ────────────────────────────────── */

/* Figure out how many columns fit in terminal_width given max entry width. */
static int columns_that_fit(ls_entry_t **items, int count, int *out_col_width)
{
    int max_len = 1;
    for (int i = 0; i < count; i++) {
        if (items[i]->display_len > max_len)
            max_len = items[i]->display_len;
    }
    int col_width = max_len + 1; /* +1 for padding space */
    int ncols = g_opts.terminal_width / col_width;
    if (ncols < 1) ncols = 1;
    if (out_col_width) *out_col_width = col_width;
    return ncols;
}

/* ── output_stream (-m) ───────────────────────────────────── */

static void output_stream(ls_entry_t **items, int count)
{
    int col = 0;
    for (int i = 0; i < count; i++) {
        ls_entry_t *e = items[i];
        int name_len = (int)strlen(e->name);

        /* Wrap if this entry would exceed terminal width. */
        if (col > 0) {
            if (col + 2 + name_len > g_opts.terminal_width) {
                fputs(",\n", stdout);
                col = 0;
            } else {
                fputs(", ", stdout);
                col += 2;
            }
        }
        print_name_with_color(e);
        col += name_len;
    }
    if (count > 0) putchar('\n');
}

/* ── output_single (-1) ───────────────────────────────────── */

static void output_single(ls_entry_t **items, int count)
{
    for (int i = 0; i < count; i++) {
        ls_entry_t *e = items[i];
        if (g_opts.show_inode)
            printf("%ju ", (uintmax_t)e->st.st_ino);
        if (g_opts.show_blocks) {
            blkcnt_t b = g_opts.kibibytes ? e->st.st_blocks / 2 : e->st.st_blocks;
            printf("%lld ", (long long)b);
        }
        print_name_with_color(e);
        putchar('\n');
    }
}

/* ── output_column (-C, top-to-bottom) ───────────────────── */

static void output_column(ls_entry_t **items, int count)
{
    int col_width;
    int ncols = columns_that_fit(items, count, &col_width);
    int nrows = (count + ncols - 1) / ncols;

    for (int row = 0; row < nrows; row++) {
        for (int col = 0; col < ncols; col++) {
            int idx = col * nrows + row;
            if (idx >= count) break;
            ls_entry_t *e = items[idx];

            print_name_with_color(e);

            /* Pad to column width unless it's the last column. */
            if (col < ncols - 1 && (col + 1) * nrows + row < count) {
                int pad = col_width - e->display_len;
                for (int p = 0; p < pad; p++) putchar(' ');
            }
        }
        putchar('\n');
    }
}

/* ── output_across (-x, left-to-right) ───────────────────── */

static void output_across(ls_entry_t **items, int count)
{
    int col_width;
    int ncols = columns_that_fit(items, count, &col_width);

    for (int i = 0; i < count; i++) {
        ls_entry_t *e = items[i];
        print_name_with_color(e);

        if ((i + 1) % ncols == 0 || i == count - 1) {
            putchar('\n');
        } else {
            int pad = col_width - e->display_len;
            for (int p = 0; p < pad; p++) putchar(' ');
        }
    }
}

/* ── output_long (-l) ─────────────────────────────────────── */

static void output_long(ls_entry_t **items, int count, int show_total,
                        blkcnt_t total_blocks)
{
    if (show_total)
        print_total_blocks(total_blocks);

    long_widths_t w = calc_long_widths(items, count);

    for (int i = 0; i < count; i++) {
        print_long_entry(items[i],
                         w.inode_w, w.blocks_w, w.nlinks_w,
                         w.size_w, w.owner_w, w.group_w);
    }
}

/* ── public entry point ───────────────────────────────────── */

void display_list(entry_list_t *list, const char *label, int show_total)
{
    if (list->count == 0) return;

    /* Compute display widths before sorting so we can reuse them. */
    for (int i = 0; i < list->count; i++)
        list->items[i]->display_len = entry_display_len(list->items[i]);

    entry_list_sort(list);

    /* Print "dirname:" header when showing multiple directories. */
    if (label)
        printf("%s:\n", label);

    switch (g_opts.format) {
    case FMT_LONG:
        output_long(list->items, list->count, show_total, list->total_blocks);
        break;
    case FMT_STREAM:
        if (show_total) print_total_blocks(list->total_blocks);
        output_stream(list->items, list->count);
        break;
    case FMT_SINGLE:
        if (show_total) print_total_blocks(list->total_blocks);
        output_single(list->items, list->count);
        break;
    case FMT_ACROSS:
        if (show_total) print_total_blocks(list->total_blocks);
        output_across(list->items, list->count);
        break;
    default: /* FMT_COLUMN */
        if (show_total) print_total_blocks(list->total_blocks);
        output_column(list->items, list->count);
        break;
    }
}
