#ifndef LS_H
#define LS_H

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/acl.h>
#include <fts.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>

/* ── output format ─────────────────────────────────────────── */
typedef enum {
    FMT_COLUMN  = 0,   /* -C  multi-column top-to-bottom (tty default) */
    FMT_LONG    = 1,   /* -l  one line per file with full metadata     */
    FMT_STREAM  = 2,   /* -m  comma-separated single line              */
    FMT_SINGLE  = 3,   /* -1  one file per line                        */
    FMT_ACROSS  = 4,   /* -x  multi-column left-to-right               */
} format_t;

/* ── sort criteria ─────────────────────────────────────────── */
typedef enum {
    SORT_NAME   = 0,
    SORT_SIZE   = 1,   /* -S */
    SORT_TIME   = 2,   /* -t */
    SORT_NONE   = 3,   /* -f */
} sort_t;

/* ── which timestamp to use ────────────────────────────────── */
typedef enum {
    TIME_MTIME  = 0,
    TIME_ATIME  = 1,   /* -u */
    TIME_CTIME  = 2,   /* -c */
    TIME_BTIME  = 3,   /* -U  birth time */
} timefield_t;

/* ── all parsed command-line options ───────────────────────── */
typedef struct {
    /* filtering */
    int         show_all;           /* -a  include dotfiles          */
    int         show_almost_all;    /* -A  all except . and ..       */
    int         list_dir_itself;    /* -d  don't expand directories  */
    int         recursive;          /* -R                            */

    /* symlink resolution */
    int         follow_cmd_links;   /* -H  follow only cmdline links */
    int         follow_all_links;   /* -L  follow all symlinks       */

    /* output format */
    format_t    format;

    /* metadata to show */
    int         numeric_ids;        /* -n  uid/gid numbers           */
    int         human_readable;     /* -h  1K/1M/1G sizes            */
    int         kibibytes;          /* -k  512-byte → 1024-byte blks */
    int         show_inode;         /* -i                            */
    int         show_blocks;        /* -s                            */
    int         show_flags;         /* -O  BSD file flags            */
    int         show_acl;           /* -e  print full ACLs           */
    int         show_xattr;         /* -@  show xattr names          */
    int         no_group;           /* -o  long without group column */
    int         no_owner;           /* -g  long without owner column */
    int         complete_time;      /* -T  full timestamp            */
    char       *date_format;        /* -D  strftime format string    */

    /* appearance */
    int         append_indicator;   /* -F  append / * @ = | % chars */
    int         append_slash;       /* -p  append / to dirs          */
    int         colorize;           /* -G  LSCOLORS coloring         */
    int         comma_separator;    /* -,  thousands separator       */

    /* character display */
    int         print_octal;        /* -b  escape non-printable      */
    int         print_question;     /* -q  replace non-printable '?' */
    int         raw_output;         /* -v/-w  no char translation    */

    /* sorting */
    sort_t      sort_by;
    int         reverse_sort;       /* -r                            */
    timefield_t time_field;

    /* runtime */
    int         terminal_width;
    int         stdout_is_tty;
} ls_options_t;

/* ── a single file entry ready for display ─────────────────── */
typedef struct ls_entry {
    char        name[1024];         /* display name                  */
    char        path[4096];         /* full path (for readlink etc)  */
    struct stat st;                 /* lstat() result                */
    struct stat link_st;            /* stat() of symlink target      */
    int         link_ok;            /* 1 if symlink resolves         */
    char        link_target[4096];  /* symlink destination string    */
    int         has_acl;            /* entry has a non-trivial ACL   */
    int         has_xattr;          /* entry has extended attributes */
    int         xattr_count;
    int         display_len;        /* terminal width for alignment  */
} ls_entry_t;

/* ── dynamic array of entries ──────────────────────────────── */
typedef struct {
    ls_entry_t  **items;
    int           count;
    int           capacity;
    blkcnt_t      total_blocks;
} entry_list_t;

/* ── globals ────────────────────────────────────────────────── */
extern ls_options_t  g_opts;
extern time_t        g_now;
extern int           g_exit_status;

/* ── options.c ─────────────────────────────────────────────── */
void parse_options(int argc, char **argv, int *first_arg_idx);

/* ── main.c helpers ────────────────────────────────────────── */
ls_entry_t  *make_entry(const char *name, const char *path, struct stat *st);
void         free_entry(ls_entry_t *e);
entry_list_t *entry_list_new(void);
void          entry_list_add(entry_list_t *list, ls_entry_t *e);
void          entry_list_free(entry_list_t *list);

/* ── sort.c ─────────────────────────────────────────────────── */
int  fts_name_compare(const FTSENT **a, const FTSENT **b);
void entry_list_sort(entry_list_t *list);

/* ── print.c ────────────────────────────────────────────────── */
void print_total_blocks(blkcnt_t blocks);
void print_long_entry(ls_entry_t *e, int inode_w, int blocks_w,
                      int nlinks_w, int size_w, int owner_w, int group_w);
void print_name_with_color(ls_entry_t *e);   /* name + indicator + color */
int  entry_display_len(ls_entry_t *e);

/* ── output.c ───────────────────────────────────────────────── */
void display_list(entry_list_t *list, const char *label, int show_total);

/* ── color.c ────────────────────────────────────────────────── */
void        color_init(void);
const char *color_for_entry(ls_entry_t *e);
void        color_reset_print(void);

#endif /* LS_H */
