#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include "ls.h"

static const struct option long_opts[] = {
    { "color", optional_argument, NULL, 0 },
    { NULL,    0,                 NULL, 0 },
};

/* Read COLUMNS env var or query the terminal. */
static int detect_terminal_width(void)
{
    const char *col_env = getenv("COLUMNS");
    if (col_env && *col_env) {
        int w = atoi(col_env);
        if (w > 0) return w;
    }
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

void parse_options(int argc, char **argv, int *first_arg_idx)
{
    int opt;
    int long_idx = 0;

    /* Environment-based defaults. */
    if (getenv("CLICOLOR") || getenv("CLICOLOR_FORCE"))
        g_opts.colorize = 1;

    while ((opt = getopt_long(argc, argv,
            "+@1ABCD:FGHILOPRSTUWXabcdefghiklmnopqrstuvwxy%,",
            long_opts, &long_idx)) != -1)
    {
        switch (opt) {
        /* ── filtering ────────────────────────────────── */
        case 'a': g_opts.show_all          = 1; break;
        case 'A': g_opts.show_almost_all   = 1; break;
        case 'd': g_opts.list_dir_itself   = 1; break;
        case 'R': g_opts.recursive         = 1; break;

        /* ── symlink handling ──────────────────────────── */
        case 'H': g_opts.follow_cmd_links  = 1; break;
        case 'L': g_opts.follow_all_links  = 1; break;
        case 'P':
            g_opts.follow_cmd_links = 0;
            g_opts.follow_all_links = 0;
            break;

        /* ── output format ─────────────────────────────── */
        case 'l': g_opts.format = FMT_LONG;   break;
        case '1': g_opts.format = FMT_SINGLE; break;
        case 'C': g_opts.format = FMT_COLUMN; break;
        case 'm': g_opts.format = FMT_STREAM; break;
        case 'x': g_opts.format = FMT_ACROSS; break;

        case 'o':
            g_opts.format   = FMT_LONG;
            g_opts.no_group = 1;
            break;
        case 'g':
            g_opts.format   = FMT_LONG;
            g_opts.no_owner = 1;
            break;
        case 'n':
            g_opts.format      = FMT_LONG;
            g_opts.numeric_ids = 1;
            break;

        /* ── metadata ──────────────────────────────────── */
        case 'i': g_opts.show_inode  = 1; break;
        case 's': g_opts.show_blocks = 1; break;
        case 'O': g_opts.show_flags  = 1; break;
        case 'e': g_opts.show_acl    = 1; break;
        case '@': g_opts.show_xattr  = 1; break;
        case 'h': g_opts.human_readable = 1; break;
        case 'k': g_opts.kibibytes   = 1; break;
        case 'T': g_opts.complete_time = 1; break;
        case 'D': g_opts.date_format   = optarg; break;

        /* ── appearance ────────────────────────────────── */
        case 'F': g_opts.append_indicator = 1; break;
        case 'p': g_opts.append_slash     = 1; break;
        case 'G': g_opts.colorize         = 1; break;
        case ',': g_opts.comma_separator  = 1; break;

        /* ── character handling ─────────────────────────── */
        case 'b': g_opts.print_octal    = 1; g_opts.print_question = 0; break;
        case 'q': g_opts.print_question = 1; g_opts.print_octal    = 0; break;
        case 'v':
        case 'w': g_opts.raw_output = 1; g_opts.print_question = 0; break;

        /* ── sorting ───────────────────────────────────── */
        case 'S': g_opts.sort_by = SORT_SIZE;  break;
        case 't': g_opts.sort_by = SORT_TIME;  break;
        case 'f': g_opts.sort_by = SORT_NONE;  break;
        case 'r': g_opts.reverse_sort = 1;     break;

        case 'u': g_opts.time_field = TIME_ATIME; break;
        case 'c': g_opts.time_field = TIME_CTIME; break;
        case 'U': g_opts.time_field = TIME_BTIME; break;

        /* ── whiteouts ─────────────────────────────────── */
        case 'W':
        case '%': /* show whiteout entries; fts handles them */
            break;

        /* ── long options ──────────────────────────────── */
        case 0:
            if (strcmp(long_opts[long_idx].name, "color") == 0) {
                const char *val = optarg ? optarg : "always";
                if (strcmp(val, "always") == 0 || strcmp(val, "force") == 0)
                    g_opts.colorize = 1;
                else if (strcmp(val, "auto") == 0 || strcmp(val, "if-tty") == 0)
                    g_opts.colorize = g_opts.stdout_is_tty;
                else if (strcmp(val, "never") == 0 || strcmp(val, "none") == 0)
                    g_opts.colorize = 0;
                else
                    errx(1, "unsupported --color value '%s' "
                         "(must be always, auto, or never)", val);
            }
            break;

        /* ── ignored for compatibility ─────────────────── */
        case 'B': case 'I': case 'X': case 'y':
            break;

        default:
            fprintf(stderr,
                "usage: ls [-@ABCFGHILOPRSTUWXabcdefghiklmnopqrstuvwxy1%%,]"
                " [--color=when] [-D format] [file ...]\n");
            exit(1);
        }
    }

    /* When stdout is not a tty, default to single-column, no color. */
    g_opts.stdout_is_tty = isatty(STDOUT_FILENO);
    if (!g_opts.stdout_is_tty) {
        if (g_opts.format == FMT_COLUMN)
            g_opts.format = FMT_SINGLE;
        if (!getenv("CLICOLOR_FORCE"))
            g_opts.colorize = 0;
    }

    if (g_opts.colorize)
        color_init();

    g_opts.terminal_width = detect_terminal_width();
    *first_arg_idx = optind;
}
