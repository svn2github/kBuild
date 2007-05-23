/* Interface to `ar' archives for GNU Make.
Copyright (C) 1988, 1989, 1990, 1991, 1992, 1993, 1994, 1995, 1996, 1997,
1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006 Free Software
Foundation, Inc.
This file is part of GNU Make.

GNU Make is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 2, or (at your option) any later version.

GNU Make is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
GNU Make; see the file COPYING.  If not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.  */

#include "make.h"

#ifndef	NO_ARCHIVES

#include "filedef.h"
#include "dep.h"
#include <fnmatch.h>

/* Return nonzero if NAME is an archive-member reference, zero if not.
   An archive-member reference is a name like `lib(member)'.
   If a name like `lib((entry))' is used, a fatal error is signaled at
   the attempt to use this unsupported feature.  */

int
ar_name (const char *name)
{
  const char *p = strchr (name, '(');
  const char *end;

  if (p == 0 || p == name)
    return 0;

  end = p + strlen (p) - 1;
  if (*end != ')')
    return 0;

  if (p[1] == '(' && end[-1] == ')')
    fatal (NILF, _("attempt to use unsupported feature: `%s'"), name);

  return 1;
}


/* Parse the archive-member reference NAME into the archive and member names.
   Creates one allocated string containing both names, pointed to by ARNAME_P.
   MEMNAME_P points to the member.  */

void
ar_parse_name (const char *name, char **arname_p, char **memname_p)
{
  char *p;

  *arname_p = xstrdup (name);
  p = strchr (*arname_p, '(');
  *(p++) = '\0';
  p[strlen(p) - 1] = '\0';
  *memname_p = p;
}


/* This function is called by `ar_scan' to find which member to look at.  */

/* ARGSUSED */
static long int
ar_member_date_1 (int desc UNUSED, const char *mem, int truncated,
		  long int hdrpos UNUSED, long int datapos UNUSED,
                  long int size UNUSED, long int date,
                  int uid UNUSED, int gid UNUSED, int mode UNUSED,
		  const void *name)
{
  return ar_name_equal (name, mem, truncated) ? date : 0;
}

/* Return the modtime of NAME.  */

time_t
ar_member_date (const char *name)
{
  char *arname;
  char *memname;
  long int val;

  ar_parse_name (name, &arname, &memname);

  /* Make sure we know the modtime of the archive itself because we are
     likely to be called just before commands to remake a member are run,
     and they will change the archive itself.

     But we must be careful not to enter_file the archive itself if it does
     not exist, because pattern_search assumes that files found in the data
     base exist or can be made.  */
  {
    struct file *arfile;
    arfile = lookup_file (arname);
    if (arfile == 0 && file_exists_p (arname))
      arfile = enter_file (strcache_add (arname));

    if (arfile != 0)
      (void) f_mtime (arfile, 0);
  }

  val = ar_scan (arname, ar_member_date_1, memname);

  free (arname);

  return (val <= 0 ? (time_t) -1 : (time_t) val);
}

/* Set the archive-member NAME's modtime to now.  */

#ifdef VMS
int
ar_touch (const char *name)
{
  error (NILF, _("touch archive member is not available on VMS"));
  return -1;
}
#else
int
ar_touch (const char *name)
{
  char *arname, *memname;
  int val;

  ar_parse_name (name, &arname, &memname);

  /* Make sure we know the modtime of the archive itself before we
     touch the member, since this will change the archive modtime.  */
  {
    struct file *arfile;
    arfile = enter_file (strcache_add (arname));
    f_mtime (arfile, 0);
  }

  val = 1;
  switch (ar_member_touch (arname, memname))
    {
    case -1:
      error (NILF, _("touch: Archive `%s' does not exist"), arname);
      break;
    case -2:
      error (NILF, _("touch: `%s' is not a valid archive"), arname);
      break;
    case -3:
      perror_with_name ("touch: ", arname);
      break;
    case 1:
      error (NILF,
             _("touch: Member `%s' does not exist in `%s'"), memname, arname);
      break;
    case 0:
      val = 0;
      break;
    default:
      error (NILF,
             _("touch: Bad return code from ar_member_touch on `%s'"), name);
    }

  free (arname);

  return val;
}
#endif /* !VMS */

/* State of an `ar_glob' run, passed to `ar_glob_match'.  */

struct ar_glob_state
  {
    const char *arname;
    const char *pattern;
    unsigned int size;
    struct nameseq *chain;
    unsigned int n;
  };

/* This function is called by `ar_scan' to match one archive
   element against the pattern in STATE.  */

static long int
ar_glob_match (int desc UNUSED, const char *mem, int truncated UNUSED,
	       long int hdrpos UNUSED, long int datapos UNUSED,
               long int size UNUSED, long int date UNUSED, int uid UNUSED,
               int gid UNUSED, int mode UNUSED, const void *arg)
{
  struct ar_glob_state *state = (struct ar_glob_state *)arg;

  if (fnmatch (state->pattern, mem, FNM_PATHNAME|FNM_PERIOD) == 0)
    {
      /* We have a match.  Add it to the chain.  */
      struct nameseq *new = xmalloc (state->size);
      new->name = strcache_add (concat (state->arname, mem, ")"));
      new->next = state->chain;
      state->chain = new;
      ++state->n;
    }

  return 0L;
}

/* Return nonzero if PATTERN contains any metacharacters.
   Metacharacters can be quoted with backslashes if QUOTE is nonzero.  */
static int
glob_pattern_p (const char *pattern, int quote)
{
  const char *p;
  int open = 0;

  for (p = pattern; *p != '\0'; ++p)
    switch (*p)
      {
      case '?':
      case '*':
	return 1;

      case '\\':
	if (quote)
	  ++p;
	break;

      case '[':
	open = 1;
	break;

      case ']':
	if (open)
	  return 1;
	break;
      }

  return 0;
}

/* Glob for MEMBER_PATTERN in archive ARNAME.
   Return a malloc'd chain of matching elements (or nil if none).  */

struct nameseq *
ar_glob (const char *arname, const char *member_pattern, unsigned int size)
{
  struct ar_glob_state state;
  struct nameseq *n;
  const char **names;
  char *name;
  unsigned int i;

  if (! glob_pattern_p (member_pattern, 1))
    return 0;

  /* Scan the archive for matches.
     ar_glob_match will accumulate them in STATE.chain.  */
  i = strlen (arname);
  name = alloca (i + 2);
  memcpy (name, arname, i);
  name[i] = '(';
  name[i + 1] = '\0';
  state.arname = name;
  state.pattern = member_pattern;
  state.size = size;
  state.chain = 0;
  state.n = 0;
  ar_scan (arname, ar_glob_match, &state);

  if (state.chain == 0)
    return 0;

  /* Now put the names into a vector for sorting.  */
  names = alloca (state.n * sizeof (const char *));
  i = 0;
  for (n = state.chain; n != 0; n = n->next)
    names[i++] = n->name;

  /* Sort them alphabetically.  */
  qsort (names, i, sizeof (*names), alpha_compare);

  /* Put them back into the chain in the sorted order.  */
  i = 0;
  for (n = state.chain; n != 0; n = n->next)
    n->name = names[i++];

  return state.chain;
}

#endif	/* Not NO_ARCHIVES.  */
