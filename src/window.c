/* Copyright (C) 2010 G.P. Halkes
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3, as
   published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file */

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>

#include "window.h"
#include "internal.h"

#include "unicode/unicode.h"

/* TODO list:
- _T3_ATTR_ACS should only be allowed on chars below 128 etc. Otherwise interpretation
  of width info may be weird.
- make t3_win_clrtobol
*/

/** @internal
    @brief The maximum size of a UTF-8 character in bytes. Used in ::t3_win_addnstr.
*/
#define UTF8_MAX_BYTES 4

static t3_window_t *head, /**< Head of depth sorted t3_window_t list. */
	*tail; /**< Tail of depth sorted t3_window_t list. */

static t3_bool ensureSpace(line_data_t *line, size_t n);

/** @addtogroup t3window_win */
/** @{ */

/** Insert a window into the list of known windows.
    @param win The t3_window_t to insert.
*/
static void _insert_window(t3_window_t *win) {
	t3_window_t **head_ptr, **tail_ptr;
	t3_window_t *ptr;

	if (win->parent == NULL) {
		head_ptr = &head;
		tail_ptr = &tail;
	} else {
		head_ptr = &win->parent->head;
		tail_ptr = &win->parent->tail;
	}

	if (*head_ptr == NULL) {
		*tail_ptr = *head_ptr = win;
		win->next = win->prev = NULL;
		return;
	}

	/* Insert new window at the correct place in the sorted list of windows. */
	for (ptr = *head_ptr; ptr != NULL && ptr->depth < win->depth; ptr = ptr->next) {}

	if (ptr == NULL) {
		win->prev = tail;
		win->next = NULL;
		(*tail_ptr)->next = win;
		*tail_ptr = win;
	} else if (ptr->prev == NULL) {
		win->prev = NULL;
		win->next = ptr;
		(*head_ptr)->prev = win;
		*head_ptr = win;
	} else {
		win->prev = ptr->prev;
		win->next = ptr;
		ptr->prev->next = win;
		ptr->prev = win;
	}
}

static void _remove_window(t3_window_t *win) {
	if (win->next == NULL) {
		if (win->parent == NULL)
			tail = win->prev;
		else
			win->parent->tail = win->prev;
	} else {
		win->next->prev = win->prev;
	}

	if (win->prev == NULL) {
		if (win->parent == NULL)
			head = win->next;
		else
			win->parent->head = win->next;
	} else {
		win->prev->next = win->next;
	}
}


/** Create a new t3_window_t.
    @param parent t3_window_t used for clipping and relative positioning.
    @param height The desired height in terminal lines.
    @param width The desired width in terminal columns.
    @param y The vertical location of the window in terminal lines.
    @param x The horizontal location of the window in terminal columns.
    @param depth The depth of the window in the stack of windows.
    @return A pointer to a new t3_window_t struct or @c NULL if not enough
    	memory could be allocated.

    The @p depth parameter determines the z-order of the windows. Windows
    with lower depth will hide windows with higher depths. However, this only
    holds relative to the @p parent window. The position will be relative to
    the top-left corner of the @p parent window, or to the top-left corner of
    the terminal if @p parent is @c NULL.
*/
t3_window_t *t3_win_new(t3_window_t *parent, int height, int width, int y, int x, int depth) {
	t3_window_t *retval;
	int i;

	if ((retval = t3_win_new_unbacked(parent, height, width, y, x, depth)) == NULL)
		return NULL;

	if ((retval->lines = calloc(1, sizeof(line_data_t) * height)) == NULL) {
		t3_win_del(retval);
		return NULL;
	}

	for (i = 0; i < height; i++) {
		retval->lines[i].allocated = width > INITIAL_ALLOC ? INITIAL_ALLOC : width;
		if ((retval->lines[i].data = malloc(sizeof(t3_chardata_t) * retval->lines[i].allocated)) == NULL) {
			t3_win_del(retval);
			return NULL;
		}
	}

	return retval;
}

/** Create a new t3_window_t with relative position without backing store.
    @param parent t3_window_t used for clipping.
    @param height The desired height in terminal lines.
    @param width The desired width in terminal columns.
    @param y The vertical location of the window in terminal lines.
    @param x The horizontal location of the window in terminal columns.
    @param depth The depth of the window in the stack of windows.
    @return A pointer to a new t3_window_t struct or @c NULL if not enough
    	memory could be allocated or an invalid parameter was passed.

    Windows without a backing store can not be used for drawing. These are
    only defined to allow a window for positioning other windows only.

    The @p depth parameter determines the z-order of the windows. Windows
    with lower depth will hide windows with higher depths.
*/
t3_window_t *t3_win_new_unbacked(t3_window_t *parent, int height, int width, int y, int x, int depth) {
	t3_window_t *retval;

	if (height <= 0 || width <= 0)
		return NULL;

	if ((retval = calloc(1, sizeof(t3_window_t))) == NULL)
		return NULL;

	retval->x = x;
	retval->y = y;
	retval->width = width;
	retval->height = height;
	retval->paint_x = 0;
	retval->paint_y = 0;
	retval->shown = t3_false;
	retval->default_attrs = 0;
	retval->parent = parent;
	retval->anchor = parent;
	retval->relation = 0;
	retval->depth = depth;

	_insert_window(retval);
	return retval;
}

/** Link a t3_window_t's position to the position of another t3_window_t.
    @param win The t3_window_t to set the anchor for.
    @param anchor The t3_window_t to link to.
    @param relation The relation between this window and @p anchor (see ::WinAnchor).
*/
void t3_win_set_anchor(t3_window_t *win, t3_window_t *anchor, int relation) {
	if (T3_GETPARENT(relation) != T3_ANCHOR_TOPLEFT && T3_GETPARENT(relation) != T3_ANCHOR_TOPRIGHT &&
			T3_GETPARENT(relation) != T3_ANCHOR_BOTTOMLEFT && T3_GETPARENT(relation) != T3_ANCHOR_BOTTOMRIGHT) {
		return;
	}

	if (T3_GETCHILD(relation) != T3_ANCHOR_TOPLEFT && T3_GETCHILD(relation) != T3_ANCHOR_TOPRIGHT &&
			T3_GETCHILD(relation) != T3_ANCHOR_BOTTOMLEFT && T3_GETCHILD(relation) != T3_ANCHOR_BOTTOMRIGHT) {
		return;
	}

	if (anchor == NULL && (T3_GETPARENT(relation) != T3_ANCHOR_TOPLEFT || T3_GETCHILD(relation) != T3_ANCHOR_TOPLEFT))
		return;

	win->anchor = anchor;
	win->relation = relation;
}

/** Change the depth of a t3_window_t.
    @param win The t3_window_t to set the depth for.
    @param depth The new depth for the window.
*/
void t3_win_set_depth(t3_window_t *win, int depth) {
	_remove_window(win);
	win->depth = depth;
	_insert_window(win);
}

/** Check whether a window is show, both by the direct setting of the shown flag,
		as well as the parents.
*/
static t3_bool _win_is_shown(t3_window_t *win) {
	do {
		if (!win->shown)
			return t3_false;
		win = win->parent;
	} while (win != NULL);
	return t3_true;
}

/** Set default attributes for a window.
    @param win The t3_window_t to set the default attributes for.
    @param attr The attributes to set.

    This function can be used to set a default background for the entire window, as
    well as any other attributes.
*/
void t3_win_set_default_attrs(t3_window_t *win, t3_attr_t attr) {
	win->default_attrs = attr;
}

/** Discard a t3_window_t.
    @param win The t3_window_t to discard.

    Note that child windows are @em not automatically discarded as well.
*/
void t3_win_del(t3_window_t *win) {
	int i;
	if (win == NULL)
		return;

	_remove_window(win);

	if (win->lines != NULL) {
		for (i = 0; i < win->height; i++)
			free(win->lines[i].data);
		free(win->lines);
	}
	free(win);
}

/** Change a t3_window_t's size.
    @param win The t3_window_t to change the size of.
    @param height The desired new height of the t3_window_t in terminal lines.
    @param width The desired new width of the t3_window_t in terminal columns.
    @return A boolean indicating succes, depending on the validity of the
        parameters and whether reallocation of the internal data
        structures succeeds.
*/
t3_bool t3_win_resize(t3_window_t *win, int height, int width) {
	int i;

	if (height <= 0 || width <= 0)
		return t3_false;

	if (win->lines == NULL) {
		win->height = height;
		win->width = width;
		return t3_true;
	}

	if (height > win->height) {
		void *result;
		if ((result = realloc(win->lines, height * sizeof(line_data_t))) == NULL)
			return t3_false;
		win->lines = result;
		memset(win->lines + win->height, 0, sizeof(line_data_t) * (height - win->height));
		for (i = win->height; i < height; i++) {
			if ((win->lines[i].data = malloc(sizeof(t3_chardata_t) * INITIAL_ALLOC)) == NULL) {
				for (i = win->height; i < height && win->lines[i].data != NULL; i++)
					free(win->lines[i].data);
				return t3_false;
			}
			win->lines[i].allocated = INITIAL_ALLOC;
		}
	} else if (height < win->height) {
		for (i = height; i < win->height; i++)
			free(win->lines[i].data);
		memset(win->lines + height, 0, sizeof(line_data_t) * (win->height - height));
	}

	if (width < win->width) {
		/* Chop lines to maximum width */
		/* FIXME: should we also try to resize the lines (as in realloc)? */
		int paint_x = win->paint_x, paint_y = win->paint_y;

		for (i = 0; i < height; i++) {
			t3_win_set_paint(win, i, width);
			t3_win_clrtoeol(win);
		}

		win->paint_x = paint_x;
		win->paint_y = paint_y;
	}

	win->height = height;
	win->width = width;
	return t3_true;
}

/** Change a t3_window_t's position.
    @param win The t3_window_t to change the position of.
    @param y The desired new vertical position of the t3_window_t in terminal lines.
    @param x The desired new horizontal position of the t3_window_t in terminal lines.

    This function will always succeed as it only updates the internal book keeping.
*/
void t3_win_move(t3_window_t *win, int y, int x) {
	win->y = y;
	win->x = x;
}

/** Get a t3_window_t's width. */
int t3_win_get_width(t3_window_t *win) {
	return win->width;
}

/** Get a t3_window_t's height. */
int t3_win_get_height(t3_window_t *win) {
	return win->height;
}

/** Get a t3_window_t's horizontal position.

    The retrieved position may be relative to another window. Use ::t3_win_get_abs_x
    to find the absolute position.
*/
int t3_win_get_x(t3_window_t *win) {
	return win->x;
}

/** Get a t3_window_t's vertical position.

    The retrieved position may be relative to another window. Use ::t3_win_get_abs_y
    to find the absolute position.
*/

int t3_win_get_y(t3_window_t *win) {
	return win->y;
}

/** Get a t3_window_t's depth. */
int t3_win_get_depth(t3_window_t *win) {
	return win->depth;
}

/** Get a t3_window_t's relative positioning information.
    @param win The t3_window_t to get the relative positioning information for.
    @param [out] anchor The location to store the pointer to the t3_window_t relative to
    	which the position is specified.
    @return The relative positioning method.

    To retrieve the separate parts of the relative positioning information, use
    ::T3_GETPARENT and ::T3_GETCHILD.
*/
int t3_win_get_relation(t3_window_t *win, t3_window_t **anchor) {
	if (anchor != NULL)
		*anchor = win->anchor;
	return win->relation;
}

/** Get a t3_window_t's absolute horizontal position. */
int t3_win_get_abs_x(t3_window_t *win) {
	int result;

	if (win == NULL)
		return 0;

	switch (T3_GETPARENT(win->relation)) {
		case T3_ANCHOR_TOPLEFT:
		case T3_ANCHOR_BOTTOMLEFT:
			result = win->x + t3_win_get_abs_x(win->anchor);
			break;
		case T3_ANCHOR_TOPRIGHT:
		case T3_ANCHOR_BOTTOMRIGHT:
			/* If anchor == NULL, the relation is always T3_ANCHOR_TOPLEFT. */
			result = t3_win_get_abs_x(win->anchor) + win->anchor->width + win->x;
			break;
		default:
			result = win->x;
			break;
	}

	switch (T3_GETCHILD(win->relation)) {
		case T3_ANCHOR_TOPRIGHT:
		case T3_ANCHOR_BOTTOMRIGHT:
			return result - win->width;
		default:
			return result;
	}
}

/** Get a t3_window_t's absolute vertical position. */
int t3_win_get_abs_y(t3_window_t *win) {
	int result;

	if (win == NULL)
		return 0;

	switch (T3_GETPARENT(win->relation)) {
		case T3_ANCHOR_TOPLEFT:
		case T3_ANCHOR_TOPRIGHT:
			result = win->y + t3_win_get_abs_y(win->anchor);
			break;
		case T3_ANCHOR_BOTTOMLEFT:
		case T3_ANCHOR_BOTTOMRIGHT:
			/* If anchor == NULL, the relation is always T3_ANCHOR_TOPLEFT. */
			result = t3_win_get_abs_y(win->anchor) + win->anchor->height + win->y;
			break;
		default:
			result = win->y;
			break;
	}

	switch (T3_GETCHILD(win->relation)) {
		case T3_ANCHOR_BOTTOMLEFT:
		case T3_ANCHOR_BOTTOMRIGHT:
			return result - win->height;
		default:
			return result;
	}
}

/** Position the cursor relative to a t3_window_t.
    @param win The t3_window_t to position the cursor in.
    @param y The line relative to @p win to position the cursor at.
    @param x The column relative to @p win to position the cursor at.

    The cursor is only moved if the window is currently shown.
*/
void t3_win_set_cursor(t3_window_t *win, int y, int x) {
	if (_win_is_shown(win))
		t3_term_set_cursor(t3_win_get_abs_y(win) + y, t3_win_get_abs_x(win) + x);
}

/** Change the position where characters are written to the t3_window_t. */
void t3_win_set_paint(t3_window_t *win, int y, int x) {
	win->paint_x = x < 0 ? 0 : x;
	win->paint_y = y < 0 ? 0 : y;
}

/** Make a t3_window_t visible. */
void t3_win_show(t3_window_t *win) {
	win->shown = t3_true;
}

/** Make a t3_window_t invisible. */
void t3_win_hide(t3_window_t *win) {
	win->shown = t3_false;
}

/** Ensure that a line_data_t struct has at least a specified number of
        bytes of unused space.
    @param line The line_data_t struct to check.
    @param n The required unused space in bytes.
    @return A boolean indicating whether, after possibly reallocating, the
    	requested number of bytes is available.
*/
static t3_bool ensureSpace(line_data_t *line, size_t n) {
	int newsize;
	t3_chardata_t *resized;

	/* FIXME: ensure that n + line->length will fit in int */
	if (n > INT_MAX)
		return t3_false;

	if ((unsigned) line->allocated > line->length + n)
		return t3_true;

	newsize = line->allocated;

	do {
		newsize *= 2;
		/* Sanity check for overflow of allocated variable. Prevents infinite loops. */
		if (!(newsize > line->length))
			return -1;
	} while ((unsigned) newsize - line->length < n);

	if ((resized = realloc(line->data, sizeof(t3_chardata_t) * newsize)) == NULL)
		return t3_false;
	line->data = resized;
	line->allocated = newsize;
	return t3_true;
}

/** @internal
    @brief Add character data to a t3_window_t at the current painting position.
    @param win The t3_window_t to add the characters to.
    @param str The characters to add as t3_chardata_t.
    @param n The length of @p str.
    @return A boolean indicating whether insertion succeeded (only fails on memory
        allocation errors).

    This is the most complex and most central function of the library. All character
    drawing eventually ends up here.
*/
static t3_bool _win_add_chardata(t3_window_t *win, t3_chardata_t *str, size_t n) {
	int width = 0;
	int extra_spaces = 0;
	int i, j;
	size_t k;
	t3_bool result = t3_true;
	t3_chardata_t space = ' ' | _t3_term_attr_to_chardata(win->default_attrs);

	if (win->lines == NULL)
		return t3_false;

	if (win->paint_y >= win->height)
		return t3_true;
	if (win->paint_x >= win->width)
		return t3_true;

	for (k = 0; k < n; k++) {
		if (win->paint_x + width + _T3_CHARDATA_TO_WIDTH(str[k]) > win->width)
			break;
		width += _T3_CHARDATA_TO_WIDTH(str[k]);
	}

	if (k < n)
		extra_spaces = win->width - win->paint_x - width;
	n = k;

	if (width == 0) {
		int pos_width;
		/* Combining characters. */

		/* Simply drop characters that don't belong to any other character. */
		if (win->lines[win->paint_y].length == 0 ||
				win->paint_x <= win->lines[win->paint_y].start ||
				win->paint_x > win->lines[win->paint_y].start + win->lines[win->paint_y].width + 1)
			return t3_true;

		if (!ensureSpace(win->lines + win->paint_y, n))
			return t3_false;

		pos_width = win->lines[win->paint_y].start;

		/* Locate the first character that at least partially overlaps the position
		   where this string is supposed to go. */
		for (i = 0; i < win->lines[win->paint_y].length; i++) {
			pos_width += _T3_CHARDATA_TO_WIDTH(win->lines[win->paint_y].data[i]);
			if (pos_width >= win->paint_x)
				break;
		}

		/* Check whether we are being asked to add a zero-width character in the middle
		   of a double-width character. If so, ignore. */
		if (pos_width > win->paint_x)
			return t3_true;

		/* Skip to the next non-zero-width character. */
		if (i < win->lines[win->paint_y].length)
			for (i++; i < win->lines[win->paint_y].length && _T3_CHARDATA_TO_WIDTH(win->lines[win->paint_y].data[i]) == 0; i++) {}

		memmove(win->lines[win->paint_y].data + i + n, win->lines[win->paint_y].data + i, sizeof(t3_chardata_t) * (win->lines[win->paint_y].length - i));
		memcpy(win->lines[win->paint_y].data + i, str, n * sizeof(t3_chardata_t));
		win->lines[win->paint_y].length += n;
	} else if (win->lines[win->paint_y].length == 0) {
		/* Empty line. */
		if (!ensureSpace(win->lines + win->paint_y, n))
			return t3_false;
		win->lines[win->paint_y].start = win->paint_x;
		memcpy(win->lines[win->paint_y].data, str, n * sizeof(t3_chardata_t));
		win->lines[win->paint_y].length += n;
		win->lines[win->paint_y].width = width;
	} else if (win->lines[win->paint_y].start + win->lines[win->paint_y].width <= win->paint_x) {
		/* Add characters after existing characters. */
		int diff = win->paint_x - (win->lines[win->paint_y].start + win->lines[win->paint_y].width);

		if (!ensureSpace(win->lines + win->paint_y, n + diff))
			return t3_false;
		for (i = diff; i > 0; i--)
			win->lines[win->paint_y].data[win->lines[win->paint_y].length++] = WIDTH_TO_META(1) | ' ' | _t3_term_attr_to_chardata(win->default_attrs);
		memcpy(win->lines[win->paint_y].data + win->lines[win->paint_y].length, str, n * sizeof(t3_chardata_t));
		win->lines[win->paint_y].length += n;
		win->lines[win->paint_y].width += width + diff;
	} else if (win->paint_x + width <= win->lines[win->paint_y].start) {
		/* Add characters before existing characters. */
		int diff = win->lines[win->paint_y].start - (win->paint_x + width);

		if (!ensureSpace(win->lines + win->paint_y, n + diff))
			return t3_false;
		memmove(win->lines[win->paint_y].data + n + diff, win->lines[win->paint_y].data, sizeof(t3_chardata_t) * win->lines[win->paint_y].length);
		memcpy(win->lines[win->paint_y].data, str, n * sizeof(t3_chardata_t));
		for (i = diff; i > 0; i--)
			win->lines[win->paint_y].data[n++] = WIDTH_TO_META(1) | ' ' | _t3_term_attr_to_chardata(win->default_attrs);
		win->lines[win->paint_y].length += n;
		win->lines[win->paint_y].width += width + diff;
		win->lines[win->paint_y].start = win->paint_x;
	} else {
		/* Character (partly) overwrite existing chars. */
		int pos_width = win->lines[win->paint_y].start;
		size_t start_replace = 0, start_space_meta, start_spaces, end_replace, end_space_meta, end_spaces;
		int sdiff;

		/* Locate the first character that at least partially overlaps the position
		   where this string is supposed to go. */
		for (i = 0; i < win->lines[win->paint_y].length && pos_width + _T3_CHARDATA_TO_WIDTH(win->lines[win->paint_y].data[i]) <= win->paint_x; i++)
			pos_width += _T3_CHARDATA_TO_WIDTH(win->lines[win->paint_y].data[i]);
		start_replace = i;

		/* If the character only partially overlaps, we replace the first part with
		   spaces with the attributes of the old character. */
		start_space_meta = (win->lines[win->paint_y].data[start_replace] & _T3_ATTR_MASK) | WIDTH_TO_META(1);
		start_spaces = win->paint_x >= win->lines[win->paint_y].start ? win->paint_x - pos_width : 0;

		/* Now we need to find which other character(s) overlap. However, the current
		   string may overlap with a double width character but only for a single
		   position. In that case we will replace the trailing portion of the character
		   with spaces with the old character's attributes. */
		pos_width += _T3_CHARDATA_TO_WIDTH(win->lines[win->paint_y].data[start_replace]);

		i++;

		/* If the character where we start overwriting already fully overlaps with the
		   new string, then we need to only replace this and any spaces that result
		   from replacing the trailing portion need to use the start space attribute */
		if (pos_width >= win->paint_x + width) {
			end_space_meta = start_space_meta;
		} else {
			for (; i < win->lines[win->paint_y].length && pos_width < win->paint_x + width; i++)
				pos_width += _T3_CHARDATA_TO_WIDTH(win->lines[win->paint_y].data[i]);

			end_space_meta = (win->lines[win->paint_y].data[i - 1] & _T3_ATTR_MASK) | WIDTH_TO_META(1);
		}

		/* Skip any zero-width characters. */
		for (; i < win->lines[win->paint_y].length && _T3_CHARDATA_TO_WIDTH(win->lines[win->paint_y].data[i]) == 0; i++) {}
		end_replace = i;

		end_spaces = pos_width > win->paint_x + width ? pos_width - win->paint_x - width : 0;

		for (j = i; j < win->lines[win->paint_y].length && _T3_CHARDATA_TO_WIDTH(win->lines[win->paint_y].data[j]) == 0; j++) {}

		/* Move the existing characters out of the way. */
		sdiff = n + end_spaces + start_spaces - (end_replace - start_replace);
		if (sdiff > 0 && !ensureSpace(win->lines + win->paint_y, sdiff))
			return t3_false;

		memmove(win->lines[win->paint_y].data + end_replace + sdiff, win->lines[win->paint_y].data + end_replace,
			sizeof(t3_chardata_t) * (win->lines[win->paint_y].length - end_replace));

		for (i = start_replace; start_spaces > 0; start_spaces--)
			win->lines[win->paint_y].data[i++] = start_space_meta | ' ';
		memcpy(win->lines[win->paint_y].data + i, str, n * sizeof(t3_chardata_t));
		i += n;
		for (; end_spaces > 0; end_spaces--)
			win->lines[win->paint_y].data[i++] = end_space_meta | ' ';

		win->lines[win->paint_y].length += sdiff;
		if (win->lines[win->paint_y].start + win->lines[win->paint_y].width < width + win->paint_x)
			win->lines[win->paint_y].width = width + win->paint_x - win->lines[win->paint_y].start;
		if (win->lines[win->paint_y].start > win->paint_x) {
			win->lines[win->paint_y].width += win->lines[win->paint_y].start - win->paint_x;
			win->lines[win->paint_y].start = win->paint_x;
		}
	}
	win->paint_x += width;

	for (i = 0; i < extra_spaces; i++)
		result &= _win_add_chardata(win, &space, 1);

	return result;
}

/** Add a string with explicitly specified size to a t3_window_t with specified attributes.
    @param win The t3_window_t to add the string to.
    @param str The string to add.
    @param n The size of @p str.
    @param attr The attributes to use.
    @retval ::T3_ERR_SUCCESS on succes
    @retval ::T3_ERR_NONPRINT if a control character was encountered.
    @retval ::T3_ERR_ERRNO otherwise.

    The default attributes are combined with the specified attributes, with
    @p attr used as the priority attributes. All other t3_win_add* functions are
    (indirectly) implemented using this function.
*/
int t3_win_addnstr(t3_window_t *win, const char *str, size_t n, t3_attr_t attr) {
	size_t bytes_read, i;
	t3_chardata_t cd_buf[UTF8_MAX_BYTES + 1];
	uint32_t c;
	uint8_t char_info;
	int retval = T3_ERR_SUCCESS;
	int width;

	attr = t3_term_combine_attrs(attr, win->default_attrs);
	attr = _t3_term_attr_to_chardata(attr) & _T3_ATTR_MASK;

	for (; n > 0; n -= bytes_read, str += bytes_read) {
		bytes_read = n;
		c = t3_getuc(str, &bytes_read);

		char_info = t3_get_codepoint_info(c);
		width = T3_INFO_TO_WIDTH(char_info);
		if ((char_info & (T3_GRAPH_BIT | T3_SPACE_BIT)) == 0 || width < 0) {
			retval = T3_ERR_NONPRINT;
			continue;
		}

		cd_buf[0] = attr | WIDTH_TO_META(width) | (unsigned char) str[0];
		for (i = 1; i < bytes_read; i++)
			cd_buf[i] = (unsigned char) str[i];

		if (bytes_read > 1) {
			cd_buf[0] &= ~_T3_ATTR_ACS;
		} else if ((cd_buf[0] & _T3_ATTR_ACS) && !t3_term_acs_available(cd_buf[0] & _T3_CHAR_MASK)) {
			int replacement = _t3_term_get_default_acs(cd_buf[0] & _T3_CHAR_MASK);
			cd_buf[0] &= ~(_T3_ATTR_ACS | _T3_CHAR_MASK);
			cd_buf[0] |= replacement & _T3_CHAR_MASK;
		}
		if (!_win_add_chardata(win, cd_buf, bytes_read))
			return T3_ERR_ERRNO;
	}
	return retval;
}

/** Add a nul-terminated string to a t3_window_t with specified attributes.
    @param win The t3_window_t to add the string to.
    @param str The nul-terminated string to add.
    @param attr The attributes to use.
    @return See ::t3_win_addnstr.

	See ::t3_win_addnstr for further information.
*/
int t3_win_addstr(t3_window_t *win, const char *str, t3_attr_t attr) { return t3_win_addnstr(win, str, strlen(str), attr); }

/** Add a single character to a t3_window_t with specified attributes.
    @param win The t3_window_t to add the string to.
    @param c The character to add.
    @param attr The attributes to use.
    @return See ::t3_win_addnstr.

	@p c must be an ASCII character. See ::t3_win_addnstr for further information.
*/
int t3_win_addch(t3_window_t *win, char c, t3_attr_t attr) { return t3_win_addnstr(win, &c, 1, attr); }

/** Add a string with explicitly specified size to a t3_window_t with specified attributes and repetition.
    @param win The t3_window_t to add the string to.
    @param str The string to add.
    @param n The size of @p str.
    @param attr The attributes to use.
    @param rep The number of times to repeat @p str.
    @return See ::t3_win_addnstr.

	All other t3_win_add*rep functions are (indirectly) implemented using this
    function. See ::t3_win_addnstr for further information.
*/
int t3_win_addnstrrep(t3_window_t *win, const char *str, size_t n, t3_attr_t attr, int rep) {
	int i, ret;

	for (i = 0; i < rep; i++) {
		ret = t3_win_addnstr(win, str, n, attr);
		if (ret != 0)
			return ret;
	}
	return 0;
}

/** Add a nul-terminated string to a t3_window_t with specified attributes and repetition.
    @param win The t3_window_t to add the string to.
    @param str The nul-terminated string to add.
    @param attr The attributes to use.
    @param rep The number of times to repeat @p str.
    @return See ::t3_win_addnstr.

    See ::t3_win_addnstrrep for further information.
*/
int t3_win_addstrrep(t3_window_t *win, const char *str, t3_attr_t attr, int rep) { return t3_win_addnstrrep(win, str, strlen(str), attr, rep); }

/** Add a character to a t3_window_t with specified attributes and repetition.
    @param win The t3_window_t to add the string to.
    @param c The character to add.
    @param attr The attributes to use.
    @param rep The number of times to repeat @p c.
    @return See ::t3_win_addnstr.

    See ::t3_win_addnstrrep for further information.
*/
int t3_win_addchrep(t3_window_t *win, char c, t3_attr_t attr, int rep) { return t3_win_addnstrrep(win, &c, 1, attr, rep); }

/** Get the starting t3_window_t for drawing, i.e. the deepest child of the last t3_window_t. */
static t3_window_t *_get_last_window(void) {
	t3_window_t *ptr = tail;

	if (ptr != NULL) {
		/* Keep going deeper into the tree and to the last child. */
		while (ptr->tail != NULL)
			ptr = ptr->tail;
	}
	return ptr;
}

/** Get the next t3_window_t, when iterating over the t3_window_t's for drawing.
    @param ptr The last t3_window_t that was handled.
*/
static t3_window_t *_get_previous_window(t3_window_t *ptr) {
	if (ptr->prev != NULL) {
		ptr = ptr->prev;
		while (ptr->tail != NULL)
			ptr = ptr->tail;
		return ptr;
	}
	return ptr->parent;
}

/** @internal
    @brief Redraw a terminal line, based on all visible t3_window_t structs.
    @param terminal The t3_window_t representing the cached terminal contents.
    @param line The line to redraw.
    @return A boolean indicating whether redrawing succeeded without memory errors.
*/
t3_bool _t3_win_refresh_term_line(t3_window_t *terminal, int line) {
	line_data_t *draw;
	t3_window_t *ptr;
	int y, x, parent_y, parent_x, parent_max_y, parent_max_x;
	int data_start, length, paint_x;
	t3_bool result = t3_true;

	/* FIXME: check return value of called functions for memory allocation errors. */
	terminal->paint_y = line;
	terminal->lines[line].width = 0;
	terminal->lines[line].length = 0;
	terminal->lines[line].start = 0;

	for (ptr = _get_last_window(); ptr != NULL; ptr = _get_previous_window(ptr)) {
		if (ptr->lines == NULL)
			continue;

		if (!_win_is_shown(ptr))
			continue;

		if (ptr->parent == NULL) {
			parent_y = 0;
			parent_max_y = INT_MAX;
			parent_x = 0;
			parent_max_x = INT_MAX;
		} else {
			t3_window_t *parent = ptr->parent;
			parent_y = INT_MIN;
			parent_max_y = INT_MAX;
			parent_x = INT_MIN;
			parent_max_x = INT_MAX;

			do {
				int tmp;
				tmp = t3_win_get_abs_y(parent);
				if (tmp > parent_y)
					parent_y = tmp;
				tmp += parent->height;
				if (tmp < parent_max_y)
					parent_max_y = tmp;

				tmp = t3_win_get_abs_x(parent);
				if (tmp > parent_x)
					parent_x = tmp;
				tmp += parent->width;
				if (tmp < parent_max_x)
					parent_max_x = tmp;

				parent = parent->parent;
			} while (parent != NULL);
		}

		y = t3_win_get_abs_y(ptr);

		/* Skip lines outside the visible area, or that are clipped by the parent window. */
		if (y > line || y + ptr->height <= line || line < parent_y || line >= parent_max_y)
			continue;

		draw = ptr->lines + line - y;
		x = t3_win_get_abs_x(ptr);

		/* Skip lines that are fully clipped by the parent window. */
		if (x >= parent_max_x || x + draw->start + draw->width < parent_x)
			continue;

		data_start = 0;
		/* Draw/skip unused leading part of line. */
		if (x + draw->start >= parent_x) {
			int start;
			if (x + draw->start > parent_max_x)
				start = parent_max_x - x;
			else
				start = draw->start;

			if (ptr->default_attrs == 0) {
				terminal->paint_x = x + start;
			} else if (x >= parent_x) {
				terminal->paint_x = x;
				result &= t3_win_addchrep(terminal, ' ', ptr->default_attrs, start) == 0;
			} else {
				terminal->paint_x = parent_x;
				result &= t3_win_addchrep(terminal, ' ', ptr->default_attrs, start - parent_x + x) == 0;
			}
		} else if (x < parent_x) {
			terminal->paint_x = parent_x;
			for (paint_x = x + draw->start;
				paint_x + _T3_CHARDATA_TO_WIDTH(draw->data[data_start]) <= terminal->paint_x && data_start < draw->length;
				paint_x += _T3_CHARDATA_TO_WIDTH(draw->data[data_start]), data_start++) {}
			/* Add a space for the multi-cell character that is crossed by the parent clipping. */
			if (data_start < draw->length && paint_x < terminal->paint_x) {
				/* WARNING: when changing the internal representation of attributes, this must also be changed!!! */
				result &= t3_win_addch(terminal, ' ', (draw->data[data_start] & _T3_ATTR_MASK) >> _T3_ATTR_SHIFT) == 0;
				for (data_start++;
					data_start < draw->length && _T3_CHARDATA_TO_WIDTH(draw->data[data_start]) == 0;
					data_start++) {}
			}
		}

		paint_x = terminal->paint_x;
		for (length = data_start;
			length < draw->length && paint_x + _T3_CHARDATA_TO_WIDTH(draw->data[length]) <= parent_max_x;
			paint_x += _T3_CHARDATA_TO_WIDTH(draw->data[length]), length++) {}

		result &= _win_add_chardata(terminal, draw->data + data_start, length - data_start);

		/* Add a space for the multi-cell character that is crossed by the parent clipping. */
		if (length < draw->length && paint_x == parent_max_x - 1)
			/* WARNING: when changing the internal representation of attributes, this must also be changed!!! */
			result &= t3_win_addch(terminal, ' ', (draw->data[length] & _T3_ATTR_MASK) >> _T3_ATTR_SHIFT) == 0;

		if (ptr->default_attrs != 0 && draw->start + draw->width < ptr->width &&
				x + draw->start + draw->width < parent_max_x)
		{
			if (x + ptr->width <= parent_max_x)
				result &= t3_win_addchrep(terminal, ' ', ptr->default_attrs, ptr->width - draw->start - draw->width) == 0;
			else
				result &= t3_win_addchrep(terminal, ' ', ptr->default_attrs, parent_max_x - x - draw->start - draw->width) == 0;
		}

/*		terminal->paint_x = t3_win_get_abs_x(ptr);
		if (ptr->default_attrs == 0)
			terminal->paint_x += draw->start;
		else
			result &= t3_win_addchrep(terminal, ' ', ptr->default_attrs, draw->start) == 0;
		result &= _win_add_chardata(terminal, draw->data, draw->length);
		if (ptr->default_attrs != 0 && draw->start + draw->width < ptr->width)
			result &= t3_win_addchrep(terminal, ' ', ptr->default_attrs, ptr->width - draw->start - draw->width) == 0;*/
	}

	/* If a line does not start at position 0, just make it do so. This makes the whole repainting
	   bit a lot easier. */
	if (terminal->lines[line].start != 0) {
		t3_chardata_t space = ' ' | WIDTH_TO_META(1);
		terminal->paint_x = 0;
		result &= _win_add_chardata(terminal, &space, 1);
	}

	return result;
}

/** Clear current t3_window_t painting line to end. */
void t3_win_clrtoeol(t3_window_t *win) {
	if (win->paint_y >= win->height || win->lines == NULL)
		return;

	if (win->paint_x <= win->lines[win->paint_y].start) {
		win->lines[win->paint_y].length = 0;
		win->lines[win->paint_y].width = 0;
		win->lines[win->paint_y].start = 0;
	} else if (win->paint_x < win->lines[win->paint_y].start + win->lines[win->paint_y].width) {
		int sumwidth = win->lines[win->paint_y].start, i;
		for (i = 0; i < win->lines[win->paint_y].length && sumwidth + _T3_CHARDATA_TO_WIDTH(win->lines[win->paint_y].data[i]) <= win->paint_x; i++)
			sumwidth += _T3_CHARDATA_TO_WIDTH(win->lines[win->paint_y].data[i]);

		if (sumwidth < win->paint_x) {
			int spaces = win->paint_x - sumwidth;
			if (spaces < win->lines[win->paint_y].length - i ||
					ensureSpace(win->lines + win->paint_y, spaces - win->lines[win->paint_y].length + i)) {
				for (; spaces > 0; spaces--)
					win->lines[win->paint_y].data[i++] = WIDTH_TO_META(1) | ' ';
				sumwidth = win->paint_x;
			}
		}

		win->lines[win->paint_y].length = i;
		win->lines[win->paint_y].width = win->paint_x - win->lines[win->paint_y].start;
	}
}

#define ABORT_ON_FAIL(x) do { int retval; if ((retval = (x)) != 0) return retval; } while (0)

/** Draw a box on a t3_window_t.
    @param win The t3_window_t to draw on.
    @param y The line of the t3_window_t to start drawing on.
    @param x The column of the t3_window_t to start drawing on.
    @param height The height of the box to draw.
    @param width The width of the box to draw.
    @param attr The attributes to use for drawing.
    @return See ::t3_win_addnstr.
*/
int t3_win_box(t3_window_t *win, int y, int x, int height, int width, t3_chardata_t attr) {
	int i;

	attr = t3_term_combine_attrs(attr, win->default_attrs);

	if (y >= win->height || y + height > win->height ||
			x >= win->width || x + width > win->width || win->lines == NULL)
		return -1;

	t3_win_set_paint(win, y, x);
	ABORT_ON_FAIL(t3_win_addch(win, T3_ACS_ULCORNER, attr | T3_ATTR_ACS));
	ABORT_ON_FAIL(t3_win_addchrep(win, T3_ACS_HLINE, attr | T3_ATTR_ACS, width - 2));
	ABORT_ON_FAIL(t3_win_addch(win, T3_ACS_URCORNER, attr | T3_ATTR_ACS));
	for (i = 1; i < height - 1; i++) {
		t3_win_set_paint(win, y + i, x);
		ABORT_ON_FAIL(t3_win_addch(win, T3_ACS_VLINE, attr | T3_ATTR_ACS));
		t3_win_set_paint(win, y + i, x + width - 1);
		ABORT_ON_FAIL(t3_win_addch(win, T3_ACS_VLINE, attr | T3_ATTR_ACS));
	}
	t3_win_set_paint(win, y + height - 1, x);
	ABORT_ON_FAIL(t3_win_addch(win, T3_ACS_LLCORNER, attr | T3_ATTR_ACS));
	ABORT_ON_FAIL(t3_win_addchrep(win, T3_ACS_HLINE, attr | T3_ATTR_ACS, width - 2));
	ABORT_ON_FAIL(t3_win_addch(win, T3_ACS_LRCORNER, attr | T3_ATTR_ACS));
	return T3_ERR_SUCCESS;
}

/** Clear current t3_window_t painting line to end and all subsequent lines fully. */
void t3_win_clrtobot(t3_window_t *win) {
	if (win->lines == NULL)
		return;

	t3_win_clrtoeol(win);
	for (win->paint_y++; win->paint_y < win->height; win->paint_y++) {
		win->lines[win->paint_y].length = 0;
		win->lines[win->paint_y].width = 0;
		win->lines[win->paint_y].start = 0;
	}
}

/** @} */
