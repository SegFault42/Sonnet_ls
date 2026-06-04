#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "ls.h"

/* Return the relevant timestamp seconds for an entry based on g_opts.time_field. */
static time_t entry_time_sec(const ls_entry_t *e)
{
    switch (g_opts.time_field) {
    case TIME_ATIME: return e->st.st_atimespec.tv_sec;
    case TIME_CTIME: return e->st.st_ctimespec.tv_sec;
    case TIME_BTIME: return e->st.st_birthtimespec.tv_sec;
    default:         return e->st.st_mtimespec.tv_sec;
    }
}

static long entry_time_nsec(const ls_entry_t *e)
{
    switch (g_opts.time_field) {
    case TIME_ATIME: return e->st.st_atimespec.tv_nsec;
    case TIME_CTIME: return e->st.st_ctimespec.tv_nsec;
    case TIME_BTIME: return e->st.st_birthtimespec.tv_nsec;
    default:         return e->st.st_mtimespec.tv_nsec;
    }
}

/* Comparator used by qsort on ls_entry_t pointers. */
static int compare_entries(const void *va, const void *vb)
{
    const ls_entry_t *a = *(const ls_entry_t **)va;
    const ls_entry_t *b = *(const ls_entry_t **)vb;
    int result = 0;

    switch (g_opts.sort_by) {
    case SORT_SIZE:
        if (a->st.st_size > b->st.st_size)       result =  1;
        else if (a->st.st_size < b->st.st_size)  result = -1;
        else result = strcoll(a->name, b->name);
        break;

    case SORT_TIME: {
        time_t ta = entry_time_sec(a), tb = entry_time_sec(b);
        long  na = entry_time_nsec(a), nb = entry_time_nsec(b);
        if      (ta > tb) result =  1;
        else if (ta < tb) result = -1;
        else if (na > nb) result =  1;
        else if (na < nb) result = -1;
        else              result  = strcoll(a->name, b->name);
        break;
    }

    case SORT_NONE:
        result = 0;
        break;

    default: /* SORT_NAME */
        result = strcoll(a->name, b->name);
        break;
    }

    /* Sort by time is descending (newest first) unless -r inverts it. */
    if (g_opts.sort_by == SORT_TIME)
        result = -result;

    return g_opts.reverse_sort ? -result : result;
}

/* Comparator for fts_open — always sorts by name so fts delivers entries
   in a predictable order before we re-sort with entry_list_sort(). */
int fts_name_compare(const FTSENT **a, const FTSENT **b)
{
    return strcoll((*a)->fts_name, (*b)->fts_name);
}

void entry_list_sort(entry_list_t *list)
{
    if (g_opts.sort_by == SORT_NONE || list->count <= 1)
        return;
    qsort(list->items, list->count, sizeof(ls_entry_t *), compare_entries);
}
