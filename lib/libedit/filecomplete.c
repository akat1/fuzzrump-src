/*	$NetBSD: filecomplete.c,v 1.69 2021/09/26 13:45:37 christos Exp $	*/

/*-
 * Copyright (c) 1997 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jaromir Dolecek.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#if !defined(lint) && !defined(SCCSID)
__RCSID("$NetBSD: filecomplete.c,v 1.69 2021/09/26 13:45:37 christos Exp $");
#endif /* not lint && not SCCSID */

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "el.h"
#include "filecomplete.h"

static const wchar_t break_chars[] = L" \t\n\"\\'`@$><=;|&{(";

/********************************/
/* completion functions */

/*
 * does tilde expansion of strings of type ``~user/foo''
 * if ``user'' isn't valid user name or ``txt'' doesn't start
 * w/ '~', returns pointer to strdup()ed copy of ``txt''
 *
 * it's the caller's responsibility to free() the returned string
 */
char *
fn_tilde_expand(const char *txt)
{
#if defined(HAVE_GETPW_R_POSIX) || defined(HAVE_GETPW_R_DRAFT)
	struct passwd pwres;
	char pwbuf[1024];
#endif
	struct passwd *pass;
	const char *pos;
	char *temp;
	size_t len = 0;

	if (txt[0] != '~')
		return strdup(txt);

	pos = strchr(txt + 1, '/');
	if (pos == NULL) {
		temp = strdup(txt + 1);
		if (temp == NULL)
			return NULL;
	} else {
		/* text until string after slash */
		len = (size_t)(pos - txt + 1);
		temp = el_calloc(len, sizeof(*temp));
		if (temp == NULL)
			return NULL;
		(void)strlcpy(temp, txt + 1, len - 1);
	}
	if (temp[0] == 0) {
#ifdef HAVE_GETPW_R_POSIX
		if (getpwuid_r(getuid(), &pwres, pwbuf, sizeof(pwbuf),
		    &pass) != 0)
			pass = NULL;
#elif HAVE_GETPW_R_DRAFT
		pass = getpwuid_r(getuid(), &pwres, pwbuf, sizeof(pwbuf));
#else
		pass = getpwuid(getuid());
#endif
	} else {
#ifdef HAVE_GETPW_R_POSIX
		if (getpwnam_r(temp, &pwres, pwbuf, sizeof(pwbuf), &pass) != 0)
			pass = NULL;
#elif HAVE_GETPW_R_DRAFT
		pass = getpwnam_r(temp, &pwres, pwbuf, sizeof(pwbuf));
#else
		pass = getpwnam(temp);
#endif
	}
	el_free(temp);		/* value no more needed */
	if (pass == NULL)
		return strdup(txt);

	/* update pointer txt to point at string immedially following */
	/* first slash */
	txt += len;

	len = strlen(pass->pw_dir) + 1 + strlen(txt) + 1;
	temp = el_calloc(len, sizeof(*temp));
	if (temp == NULL)
		return NULL;
	(void)snprintf(temp, len, "%s/%s", pass->pw_dir, txt);

	return temp;
}

static int
needs_escaping(char c)
{
	switch (c) {
	case '\'':
	case '"':
	case '(':
	case ')':
	case '\\':
	case '<':
	case '>':
	case '$':
	case '#':
	case ' ':
	case '\n':
	case '\t':
	case '?':
	case ';':
	case '`':
	case '@':
	case '=':
	case '|':
	case '{':
	case '}':
	case '&':
	case '*':
	case '[':
		return 1;
	default:
		return 0;
	}
}

static int
needs_dquote_escaping(char c)
{
	switch (c) {
	case '"':
	case '\\':
	case '`':
	case '$':
		return 1;
	default:
		return 0;
	}
}


static wchar_t *
unescape_string(const wchar_t *string, size_t length)
{
	size_t i;
	size_t j = 0;
	wchar_t *unescaped = el_calloc(length + 1, sizeof(*string));
	if (unescaped == NULL)
		return NULL;
	for (i = 0; i < length ; i++) {
		if (string[i] == '\\')
			continue;
		unescaped[j++] = string[i];
	}
	unescaped[j] = 0;
	return unescaped;
}

static char *
escape_filename(EditLine * el, const char *filename, int single_match,
		const char *(*app_func)(const char *))
{
	size_t original_len = 0;
	size_t escaped_character_count = 0;
	size_t offset = 0;
	size_t newlen;
	const char *s;
	char c;
	size_t s_quoted = 0;	/* does the input contain a single quote */
	size_t d_quoted = 0;	/* does the input contain a double quote */
	char *escaped_str;
	wchar_t *temp = el->el_line.buffer;
	const char *append_char = NULL;

	if (filename == NULL)
		return NULL;

	while (temp != el->el_line.cursor) {
		/*
		 * If we see a single quote but have not seen a double quote
		 * so far set/unset s_quote, unless it is already quoted
		 */
		if (temp[0] == '\'' && !d_quoted &&
		    (temp == el->el_line.buffer || temp[-1] != '\\'))
			s_quoted = !s_quoted;
		/*
		 * vice versa to the above condition
		 */
		else if (temp[0] == '"' && !s_quoted)
			d_quoted = !d_quoted;
		temp++;
	}

	/* Count number of special characters so that we can calculate
	 * number of extra bytes needed in the new string
	 */
	for (s = filename; *s; s++, original_len++) {
		c = *s;
		/* Inside a single quote only single quotes need escaping */
		if (s_quoted && c == '\'') {
			escaped_character_count += 3;
			continue;
		}
		/* Inside double quotes only ", \, ` and $ need escaping */
		if (d_quoted && needs_dquote_escaping(c)) {
			escaped_character_count++;
			continue;
		}
		if (!s_quoted && !d_quoted && needs_escaping(c))
			escaped_character_count++;
	}

	newlen = original_len + escaped_character_count + 1;
	if (s_quoted || d_quoted)
		newlen++;

	if (single_match && app_func)
		newlen++;

	if ((escaped_str = el_malloc(newlen)) == NULL)
		return NULL;

	for (s = filename; *s; s++) {
		c = *s;
		if (!needs_escaping(c)) {
			/* no escaping is required continue as usual */
			escaped_str[offset++] = c;
			continue;
		}

		/* single quotes inside single quotes require special handling */
		if (c == '\'' && s_quoted) {
			escaped_str[offset++] = '\'';
			escaped_str[offset++] = '\\';
			escaped_str[offset++] = '\'';
			escaped_str[offset++] = '\'';
			continue;
		}

		/* Otherwise no escaping needed inside single quotes */
		if (s_quoted) {
			escaped_str[offset++] = c;
			continue;
		}

		/* No escaping needed inside a double quoted string either
		 * unless we see a '$', '\', '`', or '"' (itself)
		 */
		if (d_quoted && !needs_dquote_escaping(c)) {
			escaped_str[offset++] = c;
			continue;
		}

		/* If we reach here that means escaping is actually needed */
		escaped_str[offset++] = '\\';
		escaped_str[offset++] = c;
	}

	if (single_match && app_func) {
		escaped_str[offset] = 0;
		append_char = app_func(filename);
		/* we want to append space only if we are not inside quotes */
		if (append_char[0] == ' ') {
			if (!s_quoted && !d_quoted)
				escaped_str[offset++] = append_char[0];
		} else
			escaped_str[offset++] = append_char[0];
	}

	/* close the quotes if single match and the match is not a directory */
	if (single_match && (append_char && append_char[0] == ' ')) { 
		if (s_quoted)
			escaped_str[offset++] = '\'';
		else if (d_quoted)
			escaped_str[offset++] = '"';
	}

	escaped_str[offset] = 0;
	return escaped_str;
}

/*
 * return first found file name starting by the ``text'' or NULL if no
 * such file can be found
 * value of ``state'' is ignored
 *
 * it's the caller's responsibility to free the returned string
 */
char *
fn_filename_completion_function(const char *text, int state)
{
	static DIR *dir = NULL;
	static char *filename = NULL, *dirname = NULL, *dirpath = NULL;
	static size_t filename_len = 0;
	struct dirent *entry;
	char *temp;
	const char *pos;
	size_t len;

	if (state == 0 || dir == NULL) {
		pos = strrchr(text, '/');
		if (pos) {
			char *nptr;
			pos++;
			nptr = el_realloc(filename, (strlen(pos) + 1) *
			    sizeof(*nptr));
			if (nptr == NULL) {
				el_free(filename);
				filename = NULL;
				return NULL;
			}
			filename = nptr;
			(void)strcpy(filename, pos);
			len = (size_t)(pos - text);	/* including last slash */

			nptr = el_realloc(dirname, (len + 1) *
			    sizeof(*nptr));
			if (nptr == NULL) {
				el_free(dirname);
				dirname = NULL;
				return NULL;
			}
			dirname = nptr;
			(void)strlcpy(dirname, text, len + 1);
		} else {
			el_free(filename);
			if (*text == 0)
				filename = NULL;
			else {
				filename = strdup(text);
				if (filename == NULL)
					return NULL;
			}
			el_free(dirname);
			dirname = NULL;
		}

		if (dir != NULL) {
			(void)closedir(dir);
			dir = NULL;
		}

		/* support for ``~user'' syntax */

		el_free(dirpath);
		dirpath = NULL;
		if (dirname == NULL) {
			if ((dirname = strdup("")) == NULL)
				return NULL;
			dirpath = strdup("./");
		} else if (*dirname == '~')
			dirpath = fn_tilde_expand(dirname);
		else
			dirpath = strdup(dirname);

		if (dirpath == NULL)
			return NULL;

		dir = opendir(dirpath);
		if (!dir)
			return NULL;	/* cannot open the directory */

		/* will be used in cycle */
		filename_len = filename ? strlen(filename) : 0;
	}

	/* find the match */
	while ((entry = readdir(dir)) != NULL) {
		/* skip . and .. */
		if (entry->d_name[0] == '.' && (!entry->d_name[1]
		    || (entry->d_name[1] == '.' && !entry->d_name[2])))
			continue;
		if (filename_len == 0)
			break;
		/* otherwise, get first entry where first */
		/* filename_len characters are equal	  */
		if (entry->d_name[0] == filename[0]
#if HAVE_STRUCT_DIRENT_D_NAMLEN
		    && entry->d_namlen >= filename_len
#else
		    && strlen(entry->d_name) >= filename_len
#endif
		    && strncmp(entry->d_name, filename,
			filename_len) == 0)
			break;
	}

	if (entry) {		/* match found */

#if HAVE_STRUCT_DIRENT_D_NAMLEN
		len = entry->d_namlen;
#else
		len = strlen(entry->d_name);
#endif

		len = strlen(dirname) + len + 1;
		temp = el_calloc(len, sizeof(*temp));
		if (temp == NULL)
			return NULL;
		(void)snprintf(temp, len, "%s%s", dirname, entry->d_name);
	} else {
		(void)closedir(dir);
		dir = NULL;
		temp = NULL;
	}

	return temp;
}


static const char *
append_char_function(const char *name)
{
	struct stat stbuf;
	char *expname = *name == '~' ? fn_tilde_expand(name) : NULL;
	const char *rs = " ";

	if (stat(expname ? expname : name, &stbuf) == -1)
		goto out;
	if (S_ISDIR(stbuf.st_mode))
		rs = "/";
out:
	if (expname)
		el_free(expname);
	return rs;
}
/*
 * returns list of completions for text given
 * non-static for readline.
 */
char ** completion_matches(const char *, char *(*)(const char *, int));
char **
completion_matches(const char *text, char *(*genfunc)(const char *, int))
{
	char **match_list = NULL, *retstr, *prevstr;
	size_t match_list_len, max_equal, which, i;
	size_t matches;

	matches = 0;
	match_list_len = 1;
	while ((retstr = (*genfunc) (text, (int)matches)) != NULL) {
		/* allow for list terminator here */
		if (matches + 3 >= match_list_len) {
			char **nmatch_list;
			while (matches + 3 >= match_list_len)
				match_list_len <<= 1;
			nmatch_list = el_realloc(match_list,
			    match_list_len * sizeof(*nmatch_list));
			if (nmatch_list == NULL) {
				el_free(match_list);
				return NULL;
			}
			match_list = nmatch_list;

		}
		match_list[++matches] = retstr;
	}

	if (!match_list)
		return NULL;	/* nothing found */

	/* find least denominator and insert it to match_list[0] */
	which = 2;
	prevstr = match_list[1];
	max_equal = strlen(prevstr);
	for (; which <= matches; which++) {
		for (i = 0; i < max_equal &&
		    prevstr[i] == match_list[which][i]; i++)
			continue;
		max_equal = i;
	}

	retstr = el_calloc(max_equal + 1, sizeof(*retstr));
	if (retstr == NULL) {
		el_free(match_list);
		return NULL;
	}
	(void)strlcpy(retstr, match_list[1], max_equal + 1);
	match_list[0] = retstr;

	/* add NULL as last pointer to the array */
	match_list[matches + 1] = NULL;

	return match_list;
}

/*
 * Sort function for qsort(). Just wrapper around strcasecmp().
 */
static int
_fn_qsort_string_compare(const void *i1, const void *i2)
{
	const char *s1 = ((const char * const *)i1)[0];
	const char *s2 = ((const char * const *)i2)[0];

	return strcasecmp(s1, s2);
}

/*
 * Display list of strings in columnar format on readline's output stream.
 * 'matches' is list of strings, 'num' is number of strings in 'matches',
 * 'width' is maximum length of string in 'matches'.
 *
 * matches[0] is not one of the match strings, but it is counted in
 * num, so the strings are matches[1] *through* matches[num-1].
 */
void
fn_display_match_list(EditLine * el, char **matches, size_t num, size_t width,
    const char *(*app_func) (const char *))
{
	size_t line, lines, col, cols, thisguy;
	int screenwidth = el->el_terminal.t_size.h;
	if (app_func == NULL)
		app_func = append_char_function;

	/* Ignore matches[0]. Avoid 1-based array logic below. */
	matches++;
	num--;

	/*
	 * Find out how many entries can be put on one line; count
	 * with one space between strings the same way it's printed.
	 */
	cols = (size_t)screenwidth / (width + 2);
	if (cols == 0)
		cols = 1;

	/* how many lines of output, rounded up */
	lines = (num + cols - 1) / cols;

	/* Sort the items. */
	qsort(matches, num, sizeof(char *), _fn_qsort_string_compare);

	/*
	 * On the ith line print elements i, i+lines, i+lines*2, etc.
	 */
	for (line = 0; line < lines; line++) {
		for (col = 0; col < cols; col++) {
			thisguy = line + col * lines;
			if (thisguy >= num)
				break;
			(void)fprintf(el->el_outfile, "%s%s%s",
			    col == 0 ? "" : " ", matches[thisguy],
				(*app_func)(matches[thisguy]));
			(void)fprintf(el->el_outfile, "%-*s",
				(int) (width - strlen(matches[thisguy])), "");
		}
		(void)fprintf(el->el_outfile, "\n");
	}
}

static wchar_t *
find_word_to_complete(const wchar_t * cursor, const wchar_t * buffer,
    const wchar_t * word_break, const wchar_t * special_prefixes, size_t * length,
	int do_unescape)
{
	/* We now look backwards for the start of a filename/variable word */
	const wchar_t *ctemp = cursor;
	wchar_t *temp;
	size_t len;

	/* if the cursor is placed at a slash or a quote, we need to find the
	 * word before it
	 */
	if (ctemp > buffer) {
		switch (ctemp[-1]) {
		case '\\':
		case '\'':
		case '"':
			ctemp--;
			break;
		default:
			break;
		}
	}

	for (;;) {
		if (ctemp <= buffer)
			break;
		if (wcschr(word_break, ctemp[-1])) {
			if (ctemp - buffer >= 2 && ctemp[-2] == '\\') {
				ctemp -= 2;
				continue;
			}
			break;
		}
		if (special_prefixes && wcschr(special_prefixes, ctemp[-1]))
			break;
		ctemp--;
	}

	len = (size_t) (cursor - ctemp);
	if (len == 1 && (ctemp[0] == '\'' || ctemp[0] == '"')) {
		len = 0;
		ctemp++;
	}
	*length = len;
	if (do_unescape) {
		wchar_t *unescaped_word = unescape_string(ctemp, len);
		if (unescaped_word == NULL)
			return NULL;
		return unescaped_word;
	}
	temp = el_malloc((len + 1) * sizeof(*temp));
	(void) wcsncpy(temp, ctemp, len);
	temp[len] = '\0';
	return temp;
}

/*
 * Complete the word at or before point,
 * 'what_to_do' says what to do with the completion.
 * \t   means do standard completion.
 * `?' means list the possible completions.
 * `*' means insert all of the possible completions.
 * `!' means to do standard completion, and list all possible completions if
 * there is more than one.
 *
 * Note: '*' support is not implemented
 *       '!' could never be invoked
 */
int
fn_complete2(EditLine *el,
    char *(*complete_func)(const char *, int),
    char **(*attempted_completion_function)(const char *, int, int),
    const wchar_t *word_break, const wchar_t *special_prefixes,
    const char *(*app_func)(const char *), size_t query_items,
    int *completion_type, int *over, int *point, int *end,
    unsigned int flags)
{
	const LineInfoW *li;
	wchar_t *temp;
	char **matches;
	char *completion;
	size_t len;
	int what_to_do = '\t';
	int retval = CC_NORM;
	int do_unescape = flags & FN_QUOTE_MATCH;

	if (el->el_state.lastcmd == el->el_state.thiscmd)
		what_to_do = '?';

	/* readline's rl_complete() has to be told what we did... */
	if (completion_type != NULL)
		*completion_type = what_to_do;

	if (!complete_func)
		complete_func = fn_filename_completion_function;
	if (!app_func)
		app_func = append_char_function;

	li = el_wline(el);
	temp = find_word_to_complete(li->cursor,
	    li->buffer, word_break, special_prefixes, &len, do_unescape);
	if (temp == NULL)
		goto out;

	/* these can be used by function called in completion_matches() */
	/* or (*attempted_completion_function)() */
	if (point != NULL)
		*point = (int)(li->cursor - li->buffer);
	if (end != NULL)
		*end = (int)(li->lastchar - li->buffer);

	if (attempted_completion_function) {
		int cur_off = (int)(li->cursor - li->buffer);
		matches = (*attempted_completion_function)(
		    ct_encode_string(temp, &el->el_scratch),
		    cur_off - (int)len, cur_off);
	} else
		matches = NULL;
	if (!attempted_completion_function ||
	    (over != NULL && !*over && !matches))
		matches = completion_matches(
		    ct_encode_string(temp, &el->el_scratch), complete_func);

	if (over != NULL)
		*over = 0;

	if (matches == NULL) {
		goto out;
	}
	int i;
	size_t matches_num, maxlen, match_len, match_display=1;
	int single_match = matches[2] == NULL &&
		(matches[1] == NULL || strcmp(matches[0], matches[1]) == 0);

	retval = CC_REFRESH;

	if (matches[0][0] != '\0') {
		el_deletestr(el, (int)len);
		if (flags & FN_QUOTE_MATCH)
			completion = escape_filename(el, matches[0],
			    single_match, app_func);
		else
			completion = strdup(matches[0]);
		if (completion == NULL)
			goto out2;

		/*
		 * Replace the completed string with the common part of
		 * all possible matches if there is a possible completion.
		 */
		el_winsertstr(el,
		    ct_decode_string(completion, &el->el_scratch));

		if (single_match && attempted_completion_function &&
		    !(flags & FN_QUOTE_MATCH))
		{
			/*
			 * We found an exact match. Add a space after
			 * it, unless we do filename completion and the
			 * object is a directory. Also do necessary
			 * escape quoting
			 */
			el_winsertstr(el, ct_decode_string(
			    (*app_func)(completion), &el->el_scratch));
		}
		free(completion);
	}


	if (!single_match && (what_to_do == '!' || what_to_do == '?')) {
		/*
		 * More than one match and requested to list possible
		 * matches.
		 */

		for(i = 1, maxlen = 0; matches[i]; i++) {
			match_len = strlen(matches[i]);
			if (match_len > maxlen)
				maxlen = match_len;
		}
		/* matches[1] through matches[i-1] are available */
		matches_num = (size_t)(i - 1);

		/* newline to get on next line from command line */
		(void)fprintf(el->el_outfile, "\n");

		/*
		 * If there are too many items, ask user for display
		 * confirmation.
		 */
		if (matches_num > query_items) {
			(void)fprintf(el->el_outfile,
			    "Display all %zu possibilities? (y or n) ",
			    matches_num);
			(void)fflush(el->el_outfile);
			if (getc(stdin) != 'y')
				match_display = 0;
			(void)fprintf(el->el_outfile, "\n");
		}

		if (match_display) {
			/*
			 * Interface of this function requires the
			 * strings be matches[1..num-1] for compat.
			 * We have matches_num strings not counting
			 * the prefix in matches[0], so we need to
			 * add 1 to matches_num for the call.
			 */
			fn_display_match_list(el, matches,
			    matches_num+1, maxlen, app_func);
		}
		retval = CC_REDISPLAY;
	} else if (matches[0][0]) {
		/*
		 * There was some common match, but the name was
		 * not complete enough. Next tab will print possible
		 * completions.
		 */
		el_beep(el);
	} else {
		/* lcd is not a valid object - further specification */
		/* is needed */
		el_beep(el);
		retval = CC_NORM;
	}

	/* free elements of array and the array itself */
out2:
	for (i = 0; matches[i]; i++)
		el_free(matches[i]);
	el_free(matches);
	matches = NULL;

out:
	el_free(temp);
	return retval;
}

int
fn_complete(EditLine *el,
    char *(*complete_func)(const char *, int),
    char **(*attempted_completion_function)(const char *, int, int),
    const wchar_t *word_break, const wchar_t *special_prefixes,
    const char *(*app_func)(const char *), size_t query_items,
    int *completion_type, int *over, int *point, int *end)
{
	return fn_complete2(el, complete_func, attempted_completion_function,
	    word_break, special_prefixes, app_func, query_items,
	    completion_type, over, point, end,
	    attempted_completion_function ? 0 : FN_QUOTE_MATCH);
}

/*
 * el-compatible wrapper around rl_complete; needed for key binding
 */
/* ARGSUSED */
unsigned char
_el_fn_complete(EditLine *el, int ch __attribute__((__unused__)))
{
	return (unsigned char)fn_complete(el, NULL, NULL,
	    break_chars, NULL, NULL, (size_t)100,
	    NULL, NULL, NULL, NULL);
}
