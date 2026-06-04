#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "ls.h"

/*
 * LSCOLORS encoding: 11 pairs of characters, each pair = (fg, bg).
 * Pair order:
 *   0  directory
 *   1  symbolic link
 *   2  socket
 *   3  named pipe (FIFO)
 *   4  executable
 *   5  block device
 *   6  character device
 *   7  executable + setuid
 *   8  executable + setgid
 *   9  directory writable to others + sticky bit
 *  10  directory writable to others, no sticky
 *
 * Character map:  a=black b=red c=green d=brown e=blue f=magenta
 *                 g=cyan  h=light-grey  x=default
 * Uppercase = bold.
 */
#define N_COLOR_SLOTS 11
#define DEFAULT_LSCOLORS "exfxcxdxbxegedabagacadah"

static char g_color_codes[N_COLOR_SLOTS][32]; /* ANSI escape per slot */
static int  g_colors_ready = 0;

/* Convert an LSCOLORS character to ANSI color number (30–37) or -1 for default. */
static int lscolors_char_to_ansi(char c, int is_bg)
{
    int base = is_bg ? 40 : 30;
    switch (c | 0x20) { /* lowercase */
    case 'a': return base + 0; /* black  */
    case 'b': return base + 1; /* red    */
    case 'c': return base + 2; /* green  */
    case 'd': return base + 3; /* brown  */
    case 'e': return base + 4; /* blue   */
    case 'f': return base + 5; /* magenta*/
    case 'g': return base + 6; /* cyan   */
    case 'h': return base + 7; /* grey   */
    default:  return -1;       /* x = default */
    }
}

void color_init(void)
{
    const char *lscolors = getenv("LSCOLORS");
    if (!lscolors || strlen(lscolors) < 22)
        lscolors = DEFAULT_LSCOLORS;

    for (int i = 0; i < N_COLOR_SLOTS; i++) {
        char fg_char = lscolors[i * 2];
        char bg_char = lscolors[i * 2 + 1];
        int  bold    = (fg_char >= 'A' && fg_char <= 'H');
        int  fg      = lscolors_char_to_ansi(fg_char, 0);
        int  bg      = lscolors_char_to_ansi(bg_char, 1);

        char *p = g_color_codes[i];
        p += sprintf(p, "\033[");
        if (bold)              p += sprintf(p, "1;");
        if (fg != -1)          p += sprintf(p, "%d;", fg);
        if (bg != -1)          p += sprintf(p, "%d;", bg);
        /* replace trailing ';' or '[' with 'm' */
        char *end = p - 1;
        *end = 'm';
    }
    g_colors_ready = 1;
}

const char *color_for_entry(ls_entry_t *e)
{
    if (!g_colors_ready)
        return "";

    mode_t m = e->st.st_mode;

    if (S_ISDIR(m)) {
        int writable_other = (m & S_IWOTH);
        int sticky         = (m & S_ISVTX);
        if (writable_other && sticky)  return g_color_codes[9];
        if (writable_other)            return g_color_codes[10];
        return g_color_codes[0];
    }
    if (S_ISLNK(m))  return g_color_codes[1];
    if (S_ISSOCK(m)) return g_color_codes[2];
    if (S_ISFIFO(m)) return g_color_codes[3];
    if (S_ISBLK(m))  return g_color_codes[5];
    if (S_ISCHR(m))  return g_color_codes[6];
    if (S_ISREG(m)) {
        if (m & S_ISUID) return g_color_codes[7];
        if (m & S_ISGID) return g_color_codes[8];
        if (m & (S_IXUSR | S_IXGRP | S_IXOTH)) return g_color_codes[4];
    }
    return "";
}

void color_reset_print(void)
{
    if (g_colors_ready)
        fputs("\033[0m", stdout);
}
