#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>

#include "window.h"
#include "internal.h"
//FIXME: implement "hardware" scrolling for optimization
//FIXME: add scrolling, because it can save a lot of repainting

/* FIXME: ATTR_ACS should only be allowed on chars below 128 etc. Otherwise interpretation
   of width info may be weird. */

enum {
	ERR_ILSEQ,
	ERR_INCOMPLETE,
	ERR_NONPRINT,
	ERR_TRUNCATED
};


static int win_sbaddnstr(Window *win, const char *str, size_t n, CharData attr);
static int (*_win_addnstr)(Window *win, const char *str, size_t n, CharData attr) = win_sbaddnstr;

/* Head and tail of depth sorted Window list */
static Window *head, *tail;

static void _win_del(Window *win);
static Bool ensureSpace(LineData *line, size_t n);

Window *win_new(int height, int width, int y, int x, int depth) {
	Window *retval, *ptr;
	int i;

	if (height <= 0 || width <= 0)
		return NULL;

	if ((retval = calloc(1, sizeof(Window))) == NULL)
		return NULL;

	if ((retval->lines = calloc(1, sizeof(LineData) * height)) == NULL) {
		_win_del(retval);
		return NULL;
	}

	for (i = 0; i < height; i++) {
		if ((retval->lines[i].data = malloc(sizeof(CharData) * INITIAL_ALLOC)) == NULL) {
			_win_del(retval);
			return NULL;
		}
		retval->lines[i].allocated = INITIAL_ALLOC;
	}

	retval->x = x;
	retval->y = y;
	retval->paint_x = 0;
	retval->paint_y = 0;
	retval->width = width;
	retval->height = height;
	retval->depth = depth;
	retval->shown = False;
	retval->default_attrs = 0;

	if (head == NULL) {
		tail = head = retval;
		retval->next = retval->prev = NULL;
		return retval;
	}

	ptr = head;
	while (ptr != NULL && ptr->depth < depth)
		ptr = ptr->next;

	if (ptr == NULL) {
		retval->prev = tail;
		retval->next = NULL;
		tail->next = retval;
		tail = retval;
	} else if (ptr->prev == NULL) {
		retval->prev = NULL;
		retval->next = ptr;
		head->prev = retval;
		head = retval;
	} else {
		retval->prev = ptr->prev;
		retval->next = ptr;
		ptr->prev->next = retval;
		ptr->prev = retval;
	}
	return retval;
}

Window *win_new_relative(int height, int width, int y, int x, int depth, Window *parent, int relation) {
	Window *retval;

	if (parent == NULL && GETREL(relation) != REL_ABSOLUTE)
			return NULL;

	if (GETREL(relation) != REL_TOPLEFT && GETREL(relation) != REL_TOPRIGHT &&
			GETREL(relation) != REL_BOTTOMLEFT && GETREL(relation) != REL_BOTTOMRIGHT &&
			GETREL(relation) != REL_ABSOLUTE) {
		return NULL;
	}

	if (GETRELTO(relation) != REL_TOPLEFT && GETRELTO(relation) != REL_TOPRIGHT &&
			GETRELTO(relation) != REL_BOTTOMLEFT && GETRELTO(relation) != REL_BOTTOMRIGHT &&
			GETRELTO(relation) != REL_ABSOLUTE) {
		return NULL;
	}

	retval = win_new(height, width, y, x, depth);
	if (retval == NULL)
		return retval;

	retval->parent = parent;
	retval->relation = relation;
	if (depth == INT_MIN && parent != NULL && parent->depth != INT_MIN)
		retval->depth = parent->depth - 1;
	return retval;
}

static void _win_del(Window *win) {
	int i;
	if (win->lines != NULL) {
		for (i = 0; i < win->height; i++)
			free(win->lines[i].data);
		free(win->lines);
	}
	free(win);
}

void win_set_default_attrs(Window *win, CharData attr) {
	win->default_attrs = attr;
}

void win_del(Window *win) {
	if (win->next == NULL)
		tail = win->prev;
	else
		win->next->prev = win->prev;

	if (win->prev == NULL)
		head = win->next;
	else
		win->prev->next = win->next;
	_win_del(win);
}

Bool win_resize(Window *win, int height, int width) {
	int i;
	//FIXME validate parameters
	if (height > win->height) {
		void *result;
		if ((result = realloc(win->lines, height * sizeof(LineData))) == NULL)
			return False;
		win->lines = result;
		memset(win->lines + win->height, 0, sizeof(LineData) * (height - win->height));
		for (i = win->height; i < height; i++) {
			if ((win->lines[i].data = malloc(sizeof(CharData) * INITIAL_ALLOC)) == NULL) {
				for (i = win->height; i < height && win->lines[i].data != NULL; i++)
					free(win->lines[i].data);
				return False;
			}
			win->lines[i].allocated = INITIAL_ALLOC;
		}
	} else if (height < win->height) {
		for (i = height; i < win->height; i++)
			free(win->lines[i].data);
		memset(win->lines + height, 0, sizeof(LineData) * (win->height - height));
	}
//FIXME: we should use the clrtoeol function to do this for us, as it uses pretty much the same code!
	if (width < win->width) {
		/* Chop lines to maximum width */
		//FIXME: should we also try to resize the lines (as in realloc)?
		for (i = 0; i < height; i++) {
			if (win->lines[i].start > width) {
				win->lines[i].length = 0;
				win->lines[i].start = 0;
			} else if (win->lines[i].start + win->lines[i].width > width) {
				int sumwidth = win->lines[i].start, j;
				for (j = 0; j < win->lines[i].length && sumwidth + GET_WIDTH(win->lines[i].data[j]) <= width; j++)
					sumwidth += GET_WIDTH(win->lines[i].data[j]);

				if (sumwidth < width) {
					int spaces = width - sumwidth;
					if (spaces < win->lines[i].length - j ||
							ensureSpace(win->lines + i, spaces - win->lines[i].length + j)) {
						for (; spaces > 0; spaces--)
							win->lines[i].data[j++] = WIDTH_TO_META(1) | ' ' | win->default_attrs;
						sumwidth = width;
					}
				}

				win->lines[i].length = j;
				win->lines[i].width = width - win->lines[i].start;
			}
		}
	}

	win->height = height;
	win->width = width;
	return True;
}

void win_move(Window *win, int y, int x) {
	win->y = y;
	win->x = x;
}

int win_get_width(Window *win) {
	return win->width;
}

int win_get_height(Window *win) {
	return win->height;
}

int win_get_x(Window *win) {
	return win->x;
}

int win_get_y(Window *win) {
	return win->y;
}

int win_get_depth(Window *win) {
	return win->depth;
}

int win_get_relation(Window *win, Window **parent) {
	if (parent != NULL)
		*parent = win->parent;
	return win->relation;
}

int win_get_abs_x(Window *win) {
	int result;
	switch (GETREL(win->relation)) {
		case REL_TOPLEFT:
		case REL_BOTTOMLEFT:
			result = win->x + win_get_abs_x(win->parent);
			break;
		case REL_TOPRIGHT:
		case REL_BOTTOMRIGHT:
			result = win_get_abs_x(win->parent) + win->parent->width + win->x;
			break;
		default:
			result = win->x;
			break;
	}

	switch (GETRELTO(win->relation)) {
		case REL_TOPRIGHT:
		case REL_BOTTOMRIGHT:
			return result - win->width;
		default:
			return result;
	}
}

int win_get_abs_y(Window *win) {
	int result;
	switch (GETREL(win->relation)) {
		case REL_TOPLEFT:
		case REL_TOPRIGHT:
			result = win->y + win_get_abs_y(win->parent);
			break;
		case REL_BOTTOMLEFT:
		case REL_BOTTOMRIGHT:
			result = win_get_abs_y(win->parent) + win->parent->height + win->y;
			break;
		default:
			result = win->y;
			break;
	}

	switch (GETRELTO(win->relation)) {
		case REL_BOTTOMLEFT:
		case REL_BOTTOMRIGHT:
			return result - win->height;
		default:
			return result;
	}
}

void win_set_cursor(Window *win, int y, int x) {
	if (win->shown)
		term_set_cursor(win->y + y, win->x + x);
}

void win_set_paint(Window *win, int y, int x) {
	win->paint_x = x < 0 ? 0 : x;
	win->paint_y = y < 0 ? 0 : y;
}

void win_show(Window *win) {
	win->shown = True;
}

void win_hide(Window *win) {
	win->shown = False;
}

/*
static void copy_mb(CharData *dest, const char *src, size_t n, CharData meta) {
	*dest++ = ((unsigned char) *src++) | meta;
	n--;
	while (n > 0)
		*dest++ = (unsigned char) *src++;
}
*/
static Bool ensureSpace(LineData *line, size_t n) {
	int newsize;
	CharData *resized;

	/* FIXME: ensure that n + line->length will fit in int */
	if (n > INT_MAX)
		return False;

	if ((unsigned) line->allocated > line->length + n)
		return True;

	newsize = line->allocated;

	do {
		newsize *= 2;
		/* Sanity check for overflow of allocated variable. Prevents infinite loops. */
		if (!(newsize > line->length))
			return -1;
	} while ((unsigned) newsize - line->length < n);

	if ((resized = realloc(line->data, sizeof(CharData) * newsize)) == NULL)
		return False;
	line->data = resized;
	line->allocated = newsize;
	return True;
}

static Bool _win_add_chardata(Window *win, CharData *str, size_t n) {
	int width = 0;
	int extra_spaces = 0;
	int i, j;
	size_t k;
	Bool result = True;
	CharData space = ' ' | win->default_attrs;

	if (win->paint_y >= win->height)
		return True;
	if (win->paint_x >= win->width)
		return True;

	for (k = 0; k < n; k++) {
		if (win->paint_x + width + GET_WIDTH(str[k]) > win->width)
			break;
		width += GET_WIDTH(str[k]);
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
			return True;

		if (!ensureSpace(win->lines + win->paint_y, n))
			return False;

		pos_width = win->lines[win->paint_y].start;

		/* Locate the first character that at least partially overlaps the position
		   where this string is supposed to go. */
		for (i = 0; i < win->lines[win->paint_y].length; i++) {
			pos_width += GET_WIDTH(win->lines[win->paint_y].data[i]);
			if (pos_width >= win->paint_x)
				break;
		}

		/* Check whether we are being asked to add a zero-width character in the middle
		   of a double-width character. If so, ignore. */
		if (pos_width > win->paint_x)
			return True;

		/* Skip to the next non-zero-width character. */
		if (i < win->lines[win->paint_y].length)
			for (i++; i < win->lines[win->paint_y].length && GET_WIDTH(win->lines[win->paint_y].data[i]) == 0; i++) {}

		memmove(win->lines[win->paint_y].data + i + n, win->lines[win->paint_y].data + i, sizeof(CharData) * (win->lines[win->paint_y].length - i));
		memcpy(win->lines[win->paint_y].data + i, str, n * sizeof(CharData));
		win->lines[win->paint_y].length += n;
	} else if (win->lines[win->paint_y].length == 0) {
		/* Empty line. */
		if (!ensureSpace(win->lines + win->paint_y, n))
			return False;
		win->lines[win->paint_y].start = win->paint_x;
		memcpy(win->lines[win->paint_y].data, str, n * sizeof(CharData));
		win->lines[win->paint_y].length += n;
		win->lines[win->paint_y].width = width;
	} else if (win->lines[win->paint_y].start + win->lines[win->paint_y].width <= win->paint_x) {
		/* Add characters after existing characters. */
		int diff = win->paint_x - (win->lines[win->paint_y].start + win->lines[win->paint_y].width);

		if (!ensureSpace(win->lines + win->paint_y, n + diff))
			return False;
		for (i = diff; i > 0; i--)
			win->lines[win->paint_y].data[win->lines[win->paint_y].length++] = WIDTH_TO_META(1) | ' ' | win->default_attrs;
		memcpy(win->lines[win->paint_y].data + win->lines[win->paint_y].length, str, n * sizeof(CharData));
		win->lines[win->paint_y].length += n;
		win->lines[win->paint_y].width += width + diff;
	} else if (win->paint_x + width <= win->lines[win->paint_y].start) {
		/* Add characters before existing characters. */
		int diff = win->lines[win->paint_y].start - (win->paint_x + width);

		if (!ensureSpace(win->lines + win->paint_y, n + diff))
			return False;
		memmove(win->lines[win->paint_y].data + n + diff, win->lines[win->paint_y].data, sizeof(CharData) * win->lines[win->paint_y].length);
		memcpy(win->lines[win->paint_y].data, str, n * sizeof(CharData));
		for (i = diff; i > 0; i--)
			win->lines[win->paint_y].data[n++] = WIDTH_TO_META(1) | ' ' | win->default_attrs;
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
		for (i = 0; i < win->lines[win->paint_y].length && pos_width + GET_WIDTH(win->lines[win->paint_y].data[i]) <= win->paint_x; i++)
			pos_width += GET_WIDTH(win->lines[win->paint_y].data[i]);
		start_replace = i;

		/* If the character only partially overlaps, we replace the first part with
		   spaces with the attributes of the old character. */
		start_space_meta = (win->lines[win->paint_y].data[start_replace] & ATTR_MASK) | WIDTH_TO_META(1);
		start_spaces = win->paint_x >= win->lines[win->paint_y].start ? win->paint_x - pos_width : 0;

		/* Now we need to find which other character(s) overlap. However, the current
		   string may overlap with a double width character but only for a single
		   position. In that case we will replace the trailing portion of the character
		   with spaces with the old character's attributes. */
		pos_width += GET_WIDTH(win->lines[win->paint_y].data[start_replace]);

		i++;

		/* If the character where we start overwriting already fully overlaps with the
		   new string, then we need to only replace this and any spaces that result
		   from replacing the trailing portion need to use the start space attribute */
		if (pos_width >= win->paint_x + width) {
			end_space_meta = start_space_meta;
		} else {
			for (; i < win->lines[win->paint_y].length && pos_width < win->paint_x + width; i++)
				pos_width += GET_WIDTH(win->lines[win->paint_y].data[i]);

			end_space_meta = (win->lines[win->paint_y].data[i - 1] & ATTR_MASK) | WIDTH_TO_META(1);
		}

		/* Skip any zero-width characters. */
		for (; i < win->lines[win->paint_y].length && GET_WIDTH(win->lines[win->paint_y].data[i]) == 0; i++) {}
		end_replace = i;

		end_spaces = pos_width > win->paint_x + width ? pos_width - win->paint_x - width : 0;

		for (j = i; j < win->lines[win->paint_y].length && GET_WIDTH(win->lines[win->paint_y].data[j]) == 0; j++) {}

		/* Move the existing characters out of the way. */
		sdiff = n + end_spaces + start_spaces - (end_replace - start_replace);
		if (sdiff > 0 && !ensureSpace(win->lines + win->paint_y, sdiff))
			return False;

		memmove(win->lines[win->paint_y].data + end_replace + sdiff, win->lines[win->paint_y].data + end_replace,
			sizeof(CharData) * (win->lines[win->paint_y].length - end_replace));

		for (i = start_replace; start_spaces > 0; start_spaces--)
			win->lines[win->paint_y].data[i++] = start_space_meta | ' ';
		memcpy(win->lines[win->paint_y].data + i, str, n * sizeof(CharData));
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

static int win_mbaddnstr(Window *win, const char *str, size_t n, CharData attr) {
	size_t result, i;
	int width;
	mbstate_t mbstate;
	wchar_t c[2];
	char buf[MB_LEN_MAX + 1];
	CharData cd_buf[MB_LEN_MAX + 1];
	int retval = 0;

	memset(&mbstate, 0, sizeof(mbstate_t));
	attr = term_combine_attrs(attr & ATTR_MASK, win->default_attrs);

	while (n > 0) {
		result = mbrtowc(c, str, n, &mbstate);
		/* Handle error conditions. Because embedded L'\0' characters have a
		   zero result, they cannot be skipped. */
		if (result == 0)
			return ERR_TRUNCATED;
		else if (result == (size_t)(-1))
			return ERR_ILSEQ;
		else if (result == (size_t)(-2))
			return ERR_INCOMPLETE;

		width = wcwidth(c[0]);
		if (width < 0) {
			retval = ERR_NONPRINT;
			str += result;
			n -= result;
			continue;
		}
		c[1] = L'\0';
		n -= result;
		str += result;

		/* Convert the wchar_t back to an mb string. The reason for doing this
		   is that we want to make sure that the encoding is in the initial
		   shift state before and after we print this character. This allows
		   separate printing of the character. */
		result = wcstombs(buf, c, MB_LEN_MAX + 1);
		/*FIXME: should we check for conversion errors? We probably do for
		  16 bit wchar_t's because those may need to be handled differently */
		cd_buf[0] = attr | WIDTH_TO_META(width) | (unsigned char) buf[0];
		for (i = 1; i < result; i++)
			cd_buf[i] = (unsigned char) buf[i];

		if (result > 1)
			cd_buf[0] &= ~ATTR_ACS;
		else if ((cd_buf[0] & ATTR_ACS) && !term_acs_available(cd_buf[0] & CHAR_MASK)) {
			int replacement = term_get_default_acs(cd_buf[0] & CHAR_MASK);
			cd_buf[0] &= ~(ATTR_ACS | CHAR_MASK);
			cd_buf[0] |= replacement & CHAR_MASK;
		}
		_win_add_chardata(win, cd_buf, result);
	}
	return retval;
}

static Bool _win_sbaddnstr(Window *win, const char *str, size_t n, CharData attr) {
	size_t i;
	Bool result = True;

	/* FIXME: it would seem that this can be done more efficiently, especially
	   if no multibyte characters are used at all. */
	for (i = 0; i < n; i++) {
		CharData c = WIDTH_TO_META(1) | attr | (unsigned char) str[i];
		result &= _win_add_chardata(win, &c, 1);
	}

	return result;
}

static int win_sbaddnstr(Window *win, const char *str, size_t n, CharData attr) {
	size_t i, print_from = 0;
	int retval = 0;

	attr &= ATTR_MASK;
	for (i = 0; i < n; i++) {
		if (!isprint(str[i])) {
			retval = ERR_NONPRINT;
			if (print_from < i)
				_win_sbaddnstr(win, str + print_from, i - print_from, attr);
			print_from = i + 1;
		}
	}
	if (print_from < i)
		_win_sbaddnstr(win, str + print_from, i - print_from, attr);
	return retval;
}

int win_addnstr(Window *win, const char *str, size_t n, CharData attr) { return _win_addnstr(win, str, n, attr); }
int win_addstr(Window *win, const char *str, CharData attr) { return _win_addnstr(win, str, strlen(str), attr); }
int win_addch(Window *win, char c, CharData attr) { return _win_addnstr(win, &c, 1, attr); }

int win_addnstrrep(Window *win, const char *str, size_t n, CharData attr, int rep) {
	int i, ret;

	for (i = 0; i < rep; i++) {
		ret = _win_addnstr(win, str, n, attr);
		if (ret != 0)
			return ret;
	}
	return 0;
}

int win_addstrrep(Window *win, const char *str, CharData attr, int rep) { return win_addnstrrep(win, str, strlen(str), attr, rep); }
int win_addchrep(Window *win, char c, CharData attr, int rep) { return win_addnstrrep(win, &c, 1, attr, rep); }


Bool _win_refresh_term_line(struct Window *terminal, int line) {
	LineData *draw;
	Window *ptr;
	int y;

	terminal->paint_y = line;
	terminal->lines[line].width = 0;
	terminal->lines[line].length = 0;
	terminal->lines[line].start = 0;

	for (ptr = tail; ptr != NULL; ptr = ptr->prev) {
		if (!ptr->shown)
			continue;

		y = win_get_abs_y(ptr);
		if (y > line || y + ptr->height <= line)
			continue;

		draw = ptr->lines + line - y;
		terminal->paint_x = win_get_abs_x(ptr);
		if (ptr->default_attrs == 0)
			terminal->paint_x += draw->start;
		else
			win_addchrep(terminal, ' ', ptr->default_attrs, draw->start);
		_win_add_chardata(terminal, draw->data, draw->length);
		if (ptr->default_attrs != 0 && draw->start + draw->width < ptr->width)
			win_addchrep(terminal, ' ', ptr->default_attrs, ptr->width - draw->start - draw->width);
	}

	/* If a line does not start at position 0, just make it do so. This makes the whole repainting
	   bit a lot easier. */
	if (terminal->lines[line].start != 0) {
		CharData space = ' ' | WIDTH_TO_META(1);
		terminal->paint_x = 0;
		_win_add_chardata(terminal, &space, 1);
	}

	return True;
}

//FIXME: should we take background into account, or should we let the app be bothered about
//erasing with proper background color
void win_clrtoeol(Window *win) {
	if (win->paint_y >= win->height)
		return;

	if (win->paint_x <= win->lines[win->paint_y].start) {
		win->lines[win->paint_y].length = 0;
		win->lines[win->paint_y].width = 0;
		win->lines[win->paint_y].start = 0;
	} else if (win->paint_x < win->lines[win->paint_y].start + win->lines[win->paint_y].width) {
		int sumwidth = win->lines[win->paint_y].start, i;
		for (i = 0; i < win->lines[win->paint_y].length && sumwidth + GET_WIDTH(win->lines[win->paint_y].data[i]) <= win->paint_x; i++)
			sumwidth += GET_WIDTH(win->lines[win->paint_y].data[i]);

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
//FIXME: make win_clrtobol

void _win_set_multibyte(void) {
	_win_addnstr = win_mbaddnstr;
}

int win_box(Window *win, int y, int x, int height, int width, CharData attr) {
	int i;

	attr = term_combine_attrs(attr, win->default_attrs);

	if (y >= win->height || y + height > win->height ||
			x >= win->width || x + width > win->width)
		return -1;

	win_set_paint(win, y, x);
	win_addch(win, TERM_ULCORNER, attr | ATTR_ACS);
	win_addchrep(win, TERM_HLINE, attr | ATTR_ACS, width - 2);
	win_addch(win, TERM_URCORNER, attr | ATTR_ACS);
	for (i = 1; i < height - 1; i++) {
		win_set_paint(win, y + i, x);
		win_addch(win, TERM_VLINE, attr | ATTR_ACS);
		win_set_paint(win, y + i, x + width - 1);
		win_addch(win, TERM_VLINE, attr | ATTR_ACS);
	}
	win_set_paint(win, y + height - 1, x);
	win_addch(win, TERM_LLCORNER, attr | ATTR_ACS);
	win_addchrep(win, TERM_HLINE, attr | ATTR_ACS, width - 2);
	win_addch(win, TERM_LRCORNER, attr | ATTR_ACS);
	//FIXME: quit on first unsuccessful addch
	return 0;
}


void win_clrtobot(Window *win) {
	win_clrtoeol(win);
	for (win->paint_y++; win->paint_y < win->height; win->paint_y++) {
		win->lines[win->paint_y].length = 0;
		win->lines[win->paint_y].width = 0;
		win->lines[win->paint_y].start = 0;
	}
}
