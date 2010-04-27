#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <limits.h>
#include <langinfo.h>

#include "terminal.h"
#include "window.h"
#include "internal.h"
#include "convert_output.h"

/* The curses header file defines too many symbols that get in the way of our
   own, so we have a separate C file which exports only those functions that
   we actually use. */
#include "curses_interface.h"

/** @file */

/*
TODO list:
- TermError to localized string conversion routine
- line drawing for UTF-8 may require that we use the UTF-8 line drawing
  characters as apparently the linux console does not do alternate character set
  drawing. On the other hand if it does do proper UTF-8 line drawing there is not
  really a problem anyway. Simply the question of which default to use may be
  of interest.
- do proper cleanup on failure, especially on term_init
- km property indicates that 8th bit *may* be alt. Then smm and rmm may help.
  (it seems the smm and rmm properties are not often filled in though).
- we need to do some checking which header files we need
- allow alternative for langinfo.h/nl_langinfo
*/


/** @internal
    @brief Wrapper for strcmp which converts the return value to boolean. */
#define streq(a,b) (strcmp((a), (b)) == 0)

/** @internal
    @brief Add a separator for creating ANSI strings in ::term_set_attrs. */
#define ADD_ANSI_SEP() do { strcat(mode_string, sep); sep = ";"; } while(0)

/** @internal
    @brief Swap two LineData structures. Used in ::term_update. */
#define SWAP_LINES(a, b) do { LineData save; save = (a); (a) = (b); (b) = save; } while (0)

static struct termios saved; /**< Terminal state as saved in ::term_init */
static Bool initialised, /**< Boolean indicating whether the terminal has been initialised. */
	seqs_initialised; /**< Boolean indicating whether the terminal control sequences have been initialised. */
static fd_set inset; /**< File-descriptor set used for select in ::term_get_keychar. */

static char *smcup, /**< Terminal control string: start cursor positioning mode. */
	*rmcup, /**< Terminal control string: stop cursor positioning mode. */
	*cup, /**< Terminal control string: position cursor. */
	*sc, /**< Terminal control string: save cursor position. */
	*rc, /**< Terminal control string: restore cursor position. */
	*clear, /**< Terminal control string: clear terminal. */
	*home, /**< Terminal control string: cursor to home position. */
	*vpa, /**< Terminal control string: set vertical cursor position. */
	*hpa, /**< Terminal control string: set horizontal cursor position. */
	*cud, /**< Terminal control string: move cursor up. */
	*cud1, /**< Terminal control string: move cursor up 1 line. */
	*cuf, /**< Terminal control string: move cursor forward. */
	*cuf1, /**< Terminal control string: move cursor forward one position. */
	*civis, /**< Terminal control string: hide cursor. */
	*cnorm, /**< Terminal control string: show cursor. */
	*sgr, /**< Terminal control string: set graphics rendition. */
	*setaf, /**< Terminal control string: set foreground color (ANSI). */
	*setab, /**< Terminal control string: set background color (ANSI). */
	*op, /**< Terminal control string: reset colors. */
	*smacs, /**< Terminal control string: start alternate character set mode. */
	*rmacs, /**< Terminal control string: stop alternate character set mode. */
	*sgr0, /**< Terminal control string: reset graphics rendition. */
	*smul, /**< Terminal control string: start underline mode. */
	*rmul, /**< Terminal control string: stop underline mode. */
	*rev, /**< Terminal control string: start reverse video. */
	*bold, /**< Terminal control string: start bold. */
	*blink, /**< Terminal control string: start blink. */
	*dim, /**< Terminal control string: start dim. */
	*setf, /**< Terminal control string: set foreground color. */
	*setb, /**< Terminal control string: set background color. */
	*el; /**< Terminal control string: clear to end of line. */
static CharData ncv; /**< Terminal info: Non-color video attributes (encoded in CharData). */
static Bool bce; /**< Terminal info: screen erased with background color. */

static Window *terminal_window; /**< Window struct representing the last drawn terminal state. */
static LineData old_data; /**< LineData struct used in terminal update to save previous line state. */

static int lines, /**< Size of terminal (lines). */
	columns, /**< Size of terminal (columns). */
	cursor_y, /**< Cursor position (y coordinate). */
	cursor_x; /**< Cursor position (x coordinate). */
static Bool show_cursor = True; /**< Boolean indicating whether the cursor is visible currently. */

/** Conversion table between color attributes and ANSI colors. */
static int attr_to_color[10] = { 9, 0, 1, 2, 3, 4, 5, 6, 7, 9 };
/** Conversion table between color attributes and non-ANSI colors. */
static int attr_to_alt_color[10] = { 0, 0, 4, 2, 6, 1, 5, 3, 7, 0 };
static CharData attrs = 0, /**< Last used set of attributes. */
	ansi_attrs = 0, /**< Bit mask indicating which attributes should be drawn as ANSI colors. */
	/** Attributes for which the only way to turn of the attribute is to reset all attributes. */
	reset_required_mask = ATTR_BOLD | ATTR_REVERSE | ATTR_BLINK | ATTR_DIM;
/** Callback for ATTR_USER1. */
static TermUserCallback user_callback = NULL;

/** Alternate character set conversion table from TERM_* values to terminal ACS characters. */
static char alternate_chars[256],
/** Alternate character set fall-back characters for when the terminal does not
    provide a proper ACS character. */
	default_alternate_chars[256];

/*FIXME: Should this be a function or should we simply set the default_alternate_chars
	array as it is the only one still being filled this way. */
/** Fill a table with fall-back characters for the alternate character set.
    @param table The table to fill. */
static void set_alternate_chars_defaults(char *table) {
	table['}'] = 'f';
	table['.'] = 'v';
	table[','] = '<';
	table['+'] = '>';
	table['-'] = '^';
	table['h'] = '#';
	table['~'] = 'o';
	table['a'] = ':';
	table['f'] = '\\';
	table['\''] = '+';
	table['z'] = '>';
	table['{'] = '*';
	table['q'] = '-';
	table['i'] = '#';
	table['n'] = '+';
	table['y'] = '<';
	table['m'] = '+';
	table['j'] = '+';
	table['|'] = '!';
	table['g'] = '#';
	table['o'] = '~';
	table['p'] = '-';
	table['r'] = '-';
	table['s'] = '_';
	table['0'] = '#';
	table['w'] = '+';
	table['u'] = '+';
	table['t'] = '+';
	table['v'] = '+';
	table['l'] = '+';
	table['k'] = '+';
	table['x'] = '|';
}

/** Get a terminfo string.
    @param name The name of the requested terminfo string.
    @return The value of the string @p name, or @a NULL if not available.

    Strings returned must be free'd.
*/
static char *get_ti_string(const char *name) {
	char *result = call_tigetstr(name);
	if (result == (char *) 0 || result == (char *) -1)
		return NULL;

	return strdup(result);
}

/** Move cursor to screen position.
    @param line The screen line to move the cursor to.
    @param col The screen column to move the cursor to.

	This function uses the @c cup terminfo string if available, and emulates
    it through other means if necessary.
*/
static void do_cup(int line, int col) {
	if (cup != NULL) {
		call_putp(call_tparm(cup, 2, line, col));
		return;
	}
	if (vpa != NULL) {
		call_putp(call_tparm(vpa, 1, line));
		call_putp(call_tparm(hpa, 1, col));
		return;
	}
	if (home != NULL) {
		int i;

		call_putp(home);
		if (line > 0) {
			if (cud != NULL) {
				call_putp(call_tparm(cud, 1, line));
			} else {
				for (i = 0; i < line; i++)
					call_putp(cud1);
			}
		}
		if (col > 0) {
			if (cuf != NULL) {
				call_putp(call_tparm(cuf, 1, col));
			} else {
				for (i = 0; i < col; i++)
					call_putp(cuf1);
			}
		}
	}
}

/** Start cursor positioning mode.

    If @c smcup is not available for the terminal, a simple @c clear is used instead.
*/
static void do_smcup(void) {
	if (smcup != NULL) {
		call_putp(smcup);
		return;
	}
	if (clear != NULL) {
		call_putp(clear);
		return;
	}
}

/** Stop cursor positioning mode.

    If @c rmcup is not available for the terminal, a @c clear is used instead,
    followed by positioning the cursor in the bottom left corner.
*/
static void do_rmcup(void) {
	if (rmcup != NULL) {
		call_putp(rmcup);
		return;
	}
	if (clear != NULL) {
		call_putp(clear);
		do_cup(lines - 1, 0);
		return;
	}
}

/** Detect to what extent a terminal description matches the ANSI terminal standard.

    For (partially) ANSI compliant terminals optimization of the output can be done
    such that fewer characters need to be sent to the terminal than by using @c sgr.
*/
static void detect_ansi(void) {
	CharData non_existant = 0;

	if (op != NULL && (streq(op, "\033[39;49m") || streq(op, "\033[49;39m"))) {
		if (setaf != NULL && streq(setaf, "\033[3%p1%dm") &&
				setab != NULL && streq(setab, "\033[4%p1%dm"))
			ansi_attrs |= FG_COLOR_ATTRS |BG_COLOR_ATTRS;
	}
	if (smul != NULL && rmul != NULL && streq(smul, "\033[4m") && streq(rmul, "\033[24m"))
		ansi_attrs |= ATTR_UNDERLINE;
	if (smacs != NULL && rmacs != NULL && streq(smacs, "\033[11m") && streq(rmacs, "\033[10m"))
		ansi_attrs |= ATTR_ACS;

	/* So far, we have been able to check that the "exit mode" operation was ANSI compatible as well.
	   However, for bold, dim, reverse and blink we can't check this, so we will only accept them
	   as attributes if the terminal uses ANSI colors, and they all match in as far as they exist.
	*/
	if ((ansi_attrs & (FG_COLOR_ATTRS | BG_COLOR_ATTRS)) == 0 || (ansi_attrs & (ATTR_UNDERLINE | ATTR_ACS)) == 0)
		return;

	if (rev != NULL) {
		if (streq(rev, "\033[7m"))
			ansi_attrs |= ATTR_REVERSE;
	} else {
		non_existant |= ATTR_REVERSE;
	}

	if (bold != NULL) {
		if (streq(bold, "\033[1m"))
			ansi_attrs |= ATTR_BOLD;
	} else {
		non_existant |= ATTR_BOLD;
	}

	if (dim != NULL) {
		if (streq(dim, "\033[2m"))
			ansi_attrs |= ATTR_DIM;
	} else {
		non_existant |= ATTR_DIM;
	}

	if (blink != NULL) {
		if (streq(blink, "\033[5m"))
			ansi_attrs |= ATTR_BLINK;
	} else {
		non_existant |= ATTR_BLINK;
	}

	/* Only accept as ANSI if all attributes accept ACS are either non specified or ANSI. */
	if (((non_existant | ansi_attrs) & (ATTR_REVERSE | ATTR_BOLD | ATTR_DIM | ATTR_BLINK)) !=
			(ATTR_REVERSE | ATTR_BOLD | ATTR_DIM | ATTR_BLINK))
		ansi_attrs &= ~(ATTR_REVERSE | ATTR_BOLD | ATTR_DIM | ATTR_BLINK);
}

/** Initialize the terminal.
    @return A TermError inidicating the result.

    This function depends on the correct setting of the @c LC_CTYPE property
    by the @c setlocale function. Therefore, the @c setlocale function should
    be called before this function.
*/
int term_init(void) {
	struct winsize wsz;
	char *enacs;

	if (!initialised) {
		struct termios new_params;
		int ncv_int;

		if (!isatty(STDOUT_FILENO))
			return ERR_NOT_A_TTY;

		if (tcgetattr(STDOUT_FILENO, &saved) < 0)
			return ERR_ERRNO;

		new_params = saved;
		new_params.c_iflag &= ~(IXON | IXOFF | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
		new_params.c_lflag &= ~(ISIG | ICANON | ECHO);
		new_params.c_oflag &= ~OPOST;
		new_params.c_cflag &= ~(CSIZE | PARENB);
		new_params.c_cflag |= CS8;
		new_params.c_cc[VMIN] = 1;

		if (tcsetattr(STDOUT_FILENO, TCSADRAIN, &new_params) < 0)
			return ERR_ERRNO;

		initialised = True;
		FD_ZERO(&inset);
		FD_SET(STDIN_FILENO, &inset);

		if (!seqs_initialised) {
			int error;
			char *acsc;

			if ((error = call_setupterm()) != 0) {
				if (error == 1)
					return ERR_HARDCOPY_TERMINAL;
				else if (error == -1)
					return ERR_TERMINFODB_NOT_FOUND;

				return ERR_UNKNOWN;
			}

			if ((smcup = get_ti_string("smcup")) == NULL || (rmcup = get_ti_string("rmcup")) == NULL) {
				if (smcup != NULL) {
					free(smcup);
					smcup = NULL;
				}
			}
			if ((clear = get_ti_string("clear")) == NULL)
				return ERR_TERMINAL_TOO_LIMITED;
			if ((cup = get_ti_string("cup")) == NULL)
				return ERR_TERMINAL_TOO_LIMITED;

			sgr = get_ti_string("sgr");
			smul = get_ti_string("smul");
			if (smul != NULL && ((rmul = get_ti_string("rmul")) == NULL || streq(rmul, "\033[m")))
				reset_required_mask |= ATTR_UNDERLINE;
			bold = get_ti_string("bold");
			/* FIXME: we could get smso and rmso for the purpose of ANSI detection. On many
			   terminals smso == rev and rmso = exit rev */
			rev = get_ti_string("rev");
			blink = get_ti_string("blink");
			dim = get_ti_string("dim");
			smacs = get_ti_string("smacs");
			if (smacs != NULL && ((rmacs = get_ti_string("rmacs")) == NULL || streq(rmul, "\033[m")))
				reset_required_mask |= ATTR_ACS;

			/* FIXME: use scp if neither setaf/setf is available */
			if ((setaf = get_ti_string("setaf")) == NULL)
				setf = get_ti_string("setf");
			if ((setab = get_ti_string("setab")) == NULL)
				setb = get_ti_string("setb");

			op = get_ti_string("op");

			detect_ansi();

			sgr0 = get_ti_string("sgr0");
			/* If sgr0 and sgr are not defined, don't go into modes in reset_required_mask. */
			if (sgr0 == NULL && sgr == NULL) {
				reset_required_mask = 0;
				rev = NULL;
				bold = NULL;
				blink = NULL;
				dim = NULL;
				if (rmul == NULL) smul = NULL;
				if (rmacs == NULL) smacs = NULL;
			}

			bce = call_tigetflag("bce");
			if ((el = get_ti_string("el")) == NULL)
				bce = True;

			if ((sc = get_ti_string("sc")) != NULL && (rc = get_ti_string("rc")) == NULL)
				sc = NULL;

			civis = get_ti_string("civis");
			cnorm = get_ti_string("cnorm");

			if ((acsc = get_ti_string("acsc")) != NULL) {
				if (sgr != NULL || smacs != NULL) {
					size_t i;
					for (i = 0; i < strlen(acsc); i += 2)
						alternate_chars[(unsigned int) acsc[i]] = acsc[i + 1];
				}
				free(acsc);
			}

			set_alternate_chars_defaults(default_alternate_chars);

			ncv_int = call_tigetnum("ncv");
			if (ncv_int & (1<<1)) ncv |= ATTR_UNDERLINE;
			if (ncv_int & (1<<2)) ncv |= ATTR_REVERSE;
			if (ncv_int & (1<<3)) ncv |= ATTR_BLINK;
			if (ncv_int & (1<<4)) ncv |= ATTR_DIM;
			if (ncv_int & (1<<5)) ncv |= ATTR_BOLD;
			if (ncv_int & (1<<8)) ncv |= ATTR_ACS;

			seqs_initialised = True;
		}

		/* Get terminal size. First try ioctl, then environment, then terminfo. */
		if (ioctl(STDIN_FILENO, TIOCGWINSZ, &wsz) == 0) {
			lines = wsz.ws_row;
			columns = wsz.ws_col;
		} else {
			char *lines_env = getenv("LINES");
			char *columns_env = getenv("COLUMNS");
			if (lines_env == NULL || columns_env == NULL || (lines = atoi(lines_env)) == 0 || (columns = atoi(columns_env)) == 0) {
				if ((lines = call_tigetnum("lines")) < 0 || (columns = call_tigetnum("columns")) < 0)
					return ERR_NO_SIZE_INFO;
			}
		}

		/* Create or resize terminal window */
		if (terminal_window == NULL) {
			/* FIXME: maybe someday we can make the window outside of the window stack. */
			if ((terminal_window = win_new(lines, columns, 0, 0, 0)) == NULL)
				return ERR_ERRNO;
			if ((old_data.data = malloc(sizeof(CharData) * INITIAL_ALLOC)) == NULL)
				return ERR_ERRNO;
			old_data.allocated = INITIAL_ALLOC;
		} else {
			if (!win_resize(terminal_window, lines, columns))
				return ERR_ERRNO;
		}

		/* FIXME: can we find a way to save the current terminal settings (attrs etc)? */
		/* Start cursor positioning mode. */
		do_smcup();
		/* Make sure the cursor is visible */
		call_putp(cnorm);

		/* Enable alternate character set if required by terminal. */
		if ((enacs = get_ti_string("enacs")) != NULL) {
			call_putp(enacs);
			free(enacs);
		}

		/* Set the attributes of the terminal to a known value. */
		term_set_attrs(0);

		init_output_buffer();
		/* FIXME: make sure that the encoding is really set! */
		/* FIXME: nl_langinfo only works when setlocale has been called first. This should
		   therefore be a requirement for calling this function */
		init_output_iconv(nl_langinfo(CODESET));
	}
	return ERR_SUCCESS;
}

/** Restore terminal state (de-initialize). */
void term_restore(void) {
	if (initialised) {
		/* Ensure complete repaint of the terminal on re-init (if required) */
		win_set_paint(terminal_window, 0, 0);
		win_clrtobot(terminal_window);
		if (seqs_initialised) {
			do_rmcup();
			/* Restore cursor to visible state. */
			if (!show_cursor)
				call_putp(cnorm);
			/* Make sure attributes are reset */
			term_set_attrs(0);
			attrs = 0;
			fflush(stdout);
		}
		tcsetattr(STDOUT_FILENO, TCSADRAIN, &saved);
		initialised = False;
	}
}

/** Read a character from @c stdin, continueing after interrupts.
    @return A @c char read from stdin, or KEY_ERROR if an error occurred or on end of file.
*/
static int safe_read_char(void) {
	char c;
	while (1) {
		ssize_t retval = read(STDIN_FILENO, &c, 1);
		if (retval < 0 && errno == EINTR)
			continue;
		else if (retval >= 1)
			return (int) (unsigned char) c;
		else if (retval == 0)
			return ERR_EOF;
		return ERR_ERRNO;
	}
}

static int last_key = -1, /**< Last keychar returned from ::term_get_keychar. Used in ::term_unget_keychar. */
	stored_key = -1; /**< Location for storing "ungot" keys in ::term_unget_keychar. */

/** Get a key @c char from stdin with timeout.
    @param msec The time out in miliseconds, or a value <= 0 for indefinite wait.
    @retval A @c char read from stdin.
    @retval ::KEY_ERROR on error, with @c errno set to the error.
    @retval ::KEY_TIMEOUT if there was no character to read within the specified timeout.
*/
int term_get_keychar(int msec) {
	int retval;
	fd_set _inset;
	struct timeval timeout;

	if (stored_key >= 0) {
		last_key = stored_key;
		stored_key = -1;
		return last_key;
	}

	while (1) {
		_inset = inset;
		if (msec > 0) {
			timeout.tv_sec = msec / 1000;
			timeout.tv_usec = (msec % 1000) * 1000;
		}

		retval = select(STDIN_FILENO + 1, &_inset, NULL, NULL, msec > 0 ? &timeout : NULL);

		if (retval < 0) {
			if (errno == EINTR)
				continue;
			return ERR_ERRNO;
		} else if (retval == 0) {
			return ERR_TIMEOUT;
		} else {
			return last_key = safe_read_char();
		}
	}
}

/** Push a @c char back for later retrieval with ::term_get_keychar.
    @param c The @c char to push back.
    @return The @c char pushed back or ::KEY_ERROR.

    Only a @c char just read from stdin with ::term_get_keychar can be pushed back.
*/
int term_unget_keychar(int c) {
	if (c == last_key) {
		stored_key = c;
		return c;
	}
	return ERR_BAD_ARG;
}

/** Move cursor.
    @param y The terminal line to move the cursor to.
    @param x The terminal column to move the cursor to.

    If the cursor is invisible the new position is recorded, but not actually moved yet.
*/
void term_set_cursor(int y, int x) {
	cursor_y = y;
	cursor_x = x;
	if (show_cursor) {
		do_cup(y, x);
		fflush(stdout);
	}
}

/** Hide the cursor.

    Instructs the terminal to make the cursor invisible. If the terminal does not provide
    the required functionality, the cursor is moved to the bottom right corner.
*/
void term_hide_cursor(void) {
	if (show_cursor) {
		if (civis != NULL) {
			show_cursor = False;
			call_putp(civis);
			fflush(stdout);
		} else {
			/* Put cursor in bottom right corner if it can't be made invisible. */
			do_cup(lines - 1, columns - 1);
		}
	}
}

/** Show the cursor. */
void term_show_cursor(void) {
	if (!show_cursor) {
		show_cursor = True;
		do_cup(cursor_y, cursor_x);
		call_putp(cnorm);
		fflush(stdout);
	}
}

/** Retrieve the terminal size. */
void term_get_size(int *height, int *width) {
	*height = lines;
	*width = columns;
}

/** Handle resizing of the terminal.
    @return A boolean indicating success of the resizing operation, which depends on
		memory allocation success.

    Retrieves the size of the terminal and resizes the backing structures.
	After calling ::term_resize, ::term_get_size can be called to retrieve
    the new terminal size. Should be called after a @c SIGWINCH.
*/
Bool term_resize(void) {
	struct winsize wsz;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &wsz) < 0)
		return True;

	lines = wsz.ws_row;
	columns = wsz.ws_col;
	if (columns > terminal_window->width || lines != terminal_window->height) {
		int i;

		/* Clear the cache of the terminal contents and the actual terminal. This
		   is necessary because shrinking the terminal tends to cause all kinds of
		   weird corruption of the on screen state. */
		for (i = 0; i < terminal_window->height; i++) {
			win_set_paint(terminal_window, i, 0);
			win_clrtoeol(terminal_window);
		}
		call_putp(clear);
	}
	return win_resize(terminal_window, lines, columns);
}

/** Set the non-ANSI terminal drawing attributes.
    @param new_attrs The new attributes that should be used for subsequent character display.

    This function updates ::attrs to reflect any changes made to the terminal attributes.
    Note that this function may reset all attributes if an attribute was previously set for
    which no independent reset is available.
*/
static void set_attrs_non_ansi(CharData new_attrs) {
	CharData attrs_basic_non_ansi = attrs & BASIC_ATTRS & ~ansi_attrs;
	CharData new_attrs_basic_non_ansi = new_attrs & BASIC_ATTRS & ~ansi_attrs;

	if (attrs_basic_non_ansi != new_attrs_basic_non_ansi) {
		CharData changed;
		if (attrs_basic_non_ansi & ~new_attrs & reset_required_mask) {
			if (sgr != NULL) {
				call_putp(call_tparm(sgr, 9,
					/* new_attrs & ATTR_STANDOUT */ 0,
					new_attrs & ATTR_UNDERLINE,
					new_attrs & ATTR_REVERSE,
					new_attrs & ATTR_BLINK,
					new_attrs & ATTR_DIM,
					new_attrs & ATTR_BOLD,
					0,
					0,
					/* FIXME: UTF-8 terminals may need different handling */
					new_attrs & ATTR_ACS));
				attrs = new_attrs & ~(FG_COLOR_ATTRS | BG_COLOR_ATTRS);
				attrs_basic_non_ansi = attrs & ~ansi_attrs;
			} else {
				/* Note that this will not be NULL if it is required because of
				   tests in the initialization. */
				call_putp(sgr0);
				attrs_basic_non_ansi = attrs = 0;
			}
		}

		/* Set any attributes required. If sgr was previously used, the calculation
		   of 'changed' results in 0. */
		changed = attrs_basic_non_ansi ^ new_attrs_basic_non_ansi;
		if (changed) {
			if (changed & ATTR_UNDERLINE)
				call_putp(new_attrs & ATTR_UNDERLINE ? smul : rmul);
			if (changed & ATTR_REVERSE)
				call_putp(rev);
			if (changed & ATTR_BLINK)
				call_putp(blink);
			if (changed & ATTR_DIM)
				call_putp(dim);
			if (changed & ATTR_BOLD)
				call_putp(bold);
			if (changed & ATTR_ACS)
				call_putp(new_attrs & ATTR_ACS ? smacs : rmacs);
		}
	}


	/* If colors are set using ANSI sequences, we are done here. */
	if ((~ansi_attrs & (FG_COLOR_ATTRS | BG_COLOR_ATTRS)) == 0)
		return;

	/* Specifying DEFAULT as color is the same as not specifying anything. However,
	   for ::term_combine_attrs there is a distinction between an explicit and an
	   implicit color. Here we don't care about that distinction so we remove it. */
	if ((new_attrs & FG_COLOR_ATTRS) == ATTR_FG_DEFAULT)
		new_attrs &= ~(FG_COLOR_ATTRS);
	if ((new_attrs & BG_COLOR_ATTRS) == ATTR_BG_DEFAULT)
		new_attrs &= ~(BG_COLOR_ATTRS);

	/* Set default color through op string */
	if (((attrs & FG_COLOR_ATTRS) != (new_attrs & FG_COLOR_ATTRS) && (new_attrs & FG_COLOR_ATTRS) == 0) ||
			((attrs & BG_COLOR_ATTRS) != (new_attrs & BG_COLOR_ATTRS) && (new_attrs & BG_COLOR_ATTRS) == 0)) {
		if (op != NULL) {
			call_putp(op);
			attrs = new_attrs & ~(FG_COLOR_ATTRS | BG_COLOR_ATTRS);
		}
	}

	/* FIXME: for alternatives this may not work! specifically this won't
	   work if only color pairs are supported, rather than random combinations. */
	if ((attrs & FG_COLOR_ATTRS) != (new_attrs & FG_COLOR_ATTRS)) {
		if (setaf != NULL)
			call_putp(call_tparm(setaf, 1, attr_to_color[(new_attrs >> _ATTR_COLOR_SHIFT) & 0xf]));
		else if (setf != NULL)
			call_putp(call_tparm(setf, 1, attr_to_alt_color[(new_attrs >> _ATTR_COLOR_SHIFT) & 0xf]));
	}

	if ((attrs & BG_COLOR_ATTRS) != (new_attrs & BG_COLOR_ATTRS)) {
		if (setab != NULL)
			call_putp(call_tparm(setab, 1, attr_to_color[(new_attrs >> (_ATTR_COLOR_SHIFT + 4)) & 0xf]));
		else if (setb != NULL)
			call_putp(call_tparm(setb, 1, attr_to_alt_color[(new_attrs >> (_ATTR_COLOR_SHIFT + 4)) & 0xf]));
	}
}

/** Set terminal drawing attributes.
    @param new_attrs The new attributes that should be used for subsequent character display.

    The state of ::attrs is updated to reflect the new state.
*/
void term_set_attrs(CharData new_attrs) {
	char mode_string[30]; /* Max is (if I counted correctly) 24. Use 30 for if I miscounted. */
	CharData changed_attrs;
	const char *sep = "[";

	/* Flush any characters accumulated in the output buffer before switching attributes. */
	output_buffer_print();

	/* Just in case the caller forgot */
	new_attrs &= ATTR_MASK;

	if (new_attrs == 0 && (sgr0 != NULL || sgr != NULL)) {
		/* Use sgr instead of sgr0 as this is probably more tested (see rxvt-unicode terminfo bug) */
		if (sgr != NULL)
			call_putp(call_tparm(sgr, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0));
		else
			call_putp(sgr0);
		attrs = 0;
		return;
	}

	changed_attrs = (new_attrs ^ attrs) & ~ansi_attrs;
	if (changed_attrs != 0)
		set_attrs_non_ansi(new_attrs);

	changed_attrs = (new_attrs ^ attrs) & ansi_attrs;
	if (changed_attrs == 0) {
		attrs = new_attrs;
		return;
	}

	mode_string[0] = '\033';
	mode_string[1] = 0;

	if (changed_attrs & ATTR_UNDERLINE) {
		ADD_ANSI_SEP();
		strcat(mode_string, new_attrs & ATTR_UNDERLINE ? "4" : "24");
	}

	if (changed_attrs & (ATTR_BOLD | ATTR_DIM)) {
		ADD_ANSI_SEP();
		strcat(mode_string, new_attrs & ATTR_BOLD ? "1" : (new_attrs & ATTR_DIM ? "2" : "22"));
	}

	if (changed_attrs & ATTR_REVERSE) {
		ADD_ANSI_SEP();
		strcat(mode_string, new_attrs & ATTR_REVERSE ? "7" : "27");
	}

	if (changed_attrs & ATTR_BLINK) {
		ADD_ANSI_SEP();
		strcat(mode_string, new_attrs & ATTR_BLINK ? "5" : "25");
	}

	if (changed_attrs & ATTR_ACS) {
		ADD_ANSI_SEP();
		strcat(mode_string, new_attrs & ATTR_ACS ? "11" : "10");
	}

	if (changed_attrs & FG_COLOR_ATTRS) {
		char color[3];
		color[0] = '3';
		color[1] = '0' + attr_to_color[(new_attrs >> _ATTR_COLOR_SHIFT) & 0xf];
		color[2] = 0;
		ADD_ANSI_SEP();
		strcat(mode_string, color);
	}

	if (changed_attrs & BG_COLOR_ATTRS) {
		char color[3];
		color[0] = '4';
		color[1] = '0' + attr_to_color[(new_attrs >> (_ATTR_COLOR_SHIFT + 4)) & 0xf];
		color[2] = 0;
		ADD_ANSI_SEP();
		strcat(mode_string, color);
	}
	strcat(mode_string, "m");
	call_putp(mode_string);
	attrs = new_attrs;
}

/** Set callback for drawing characters with ::ATTR_USER1 attribute.
    @param callback The function to call for drawing.
*/
void term_set_user_callback(TermUserCallback callback) {
	user_callback = callback;
}

/** Update the terminal, drawing all changes since last refresh.

    After changing window contents, this function should be called to make those
    changes visible on the terminal. The refresh is not done automatically to allow
    programs to bunch many separate updates. Generally this is called right before
    ::term_get_keychar.
*/
void term_update(void) {
	int i, j;
	CharData new_attrs;

	if (show_cursor) {
		call_putp(sc);
		call_putp(civis);
	}

	/* There may be another optimization possibility here: if the new line is
	   shorter than the old line, we could detect that the end of the new line
	   matches the old line. In that case we could skip printing the end of the
	   new line. Of course the question is how often this will actually happen.
	   It also brings with it several issues with the clearing of the end of
	   the line. */
	for (i = 0; i < lines; i++) {
		int new_idx, old_idx = terminal_window->lines[i].length, width = 0;
		SWAP_LINES(old_data, terminal_window->lines[i]);
		_win_refresh_term_line(terminal_window, i);

		new_idx = terminal_window->lines[i].length;

		/* Find the last character that is different. */
		if (old_data.width == terminal_window->lines[i].width) {
			for (new_idx--, old_idx--; new_idx >= 0 &&
					old_idx >= 0 && terminal_window->lines[i].data[new_idx] == old_data.data[old_idx];
					new_idx--, old_idx--)
			{}
			if (new_idx == -1) {
				assert(old_idx == -1);
				goto done;
			}
			assert(old_idx >= 0);
			for (new_idx++; new_idx < terminal_window->lines[i].length && GET_WIDTH(terminal_window->lines[i].data[new_idx]) == 0; new_idx++) {}
			for (old_idx++; old_idx < old_data.length &&
				GET_WIDTH(old_data.data[old_idx]) == 0; old_idx++) {}
		}

		/* Find the first character that is different */
		for (j = 0; j < new_idx && j < old_idx && terminal_window->lines[i].data[j] == old_data.data[j]; j++)
			width += GET_WIDTH(terminal_window->lines[i].data[j]);

		/* Go back to the last non-zero-width character, because that is the one we want to print first. */
		if ((j < new_idx && GET_WIDTH(terminal_window->lines[i].data[j]) == 0) || (j < old_idx && GET_WIDTH(old_data.data[j]) == 0)) {
			for (; j > 0 && (GET_WIDTH(terminal_window->lines[i].data[j]) == 0 || GET_WIDTH(old_data.data[j]) == 0); j--) {}
			width -= GET_WIDTH(terminal_window->lines[i].data[j]);
		}

		/* Position the cursor */
		do_cup(i, width);
		for (; j < new_idx; j++) {
			if (GET_WIDTH(terminal_window->lines[i].data[j]) > 0) {
				if (width + GET_WIDTH(terminal_window->lines[i].data[j]) > terminal_window->width)
					break;

				new_attrs = terminal_window->lines[i].data[j] & ATTR_MASK;

				width += GET_WIDTH(terminal_window->lines[i].data[j]);
				if (user_callback != NULL && new_attrs & ATTR_USER_MASK) {
					/* Let the user draw this character because they want funky attributes */
					int start = j;
					for (j++; j < new_idx && GET_WIDTH(terminal_window->lines[i].data[j]) == 0; j++) {}
					user_callback(terminal_window->lines[i].data + start, j - start);
					if (j < new_idx)
						j--;
					continue;
				} else if (new_attrs != attrs) {
					term_set_attrs(new_attrs);
				}
			}
			if (attrs & ATTR_ACS)
				output_buffer_add(alternate_chars[terminal_window->lines[i].data[j] & CHAR_MASK]);
			else
				output_buffer_add(terminal_window->lines[i].data[j] & CHAR_MASK);
		}

		/* Clear the terminal line if the new line is shorter than the old one. */
		if ((terminal_window->lines[i].width < old_data.width || j < new_idx) && width < terminal_window->width) {
			if (bce && (attrs & ~FG_COLOR_ATTRS) != 0)
				term_set_attrs(0);

			if (el != NULL) {
				call_putp(el);
			} else {
				int max = old_data.width < terminal_window->width ? old_data.width : terminal_window->width;
				for (; width < max; width++)
					output_buffer_add(' ');
			}
		}
		output_buffer_print();

done: /* Add empty statement to shut up compilers */ ;
	}

	term_set_attrs(0);

	if (show_cursor) {
		if (rc != NULL)
			call_putp(rc);
		else
			do_cup(cursor_y, cursor_x);
		call_putp(cnorm);
	}

	fflush(stdout);
}

/** Redraw the entire terminal from scratch. */
void term_redraw(void) {
	term_set_attrs(0);
	call_putp(clear);
	win_set_paint(terminal_window, 0, 0);
	win_clrtobot(terminal_window);
}

/** Send a terminal control string to the terminal, with correct padding. */
void term_putp(const char *str) {
	output_buffer_print();
	call_putp(str);
}

/* FIXME: this isn't exactly the cleanest way to do this, but for now it works and
   avoids code duplication */
/** Calculate the cell width of a string.
    @param str The string to calculate the width of.
    @return The width of the string in character cells.

    Using @c strlen on a string will not give one the correct width of a UTF-8 string
    on the terminal screen. This function is provided to calculate that value.
*/
int term_strwidth(const char *str) {
	Window *win = win_new(1, INT_MAX, 0, 0, 0);
	int result;

	win_set_paint(win, 0, 0);
	win_addstr(win, str, 0);
	result = win->paint_x;
	win_del(win);

	return result;
}

/** Check if a character is available in the alternate character set (internal use mostly).
    @param idx The character to check.
    @return ::True if the character is available in the alternate character set.

    Characters for which an alternate character is generally available are documented in
    terminfo(5), but most are encoded in TermAcsConstants.
*/
Bool term_acs_available(int idx) {
	if (idx < 0 || idx > 255)
		return False;
	return alternate_chars[idx] != 0;
}

/** @internal
    @brief Get fall-back character for alternate character set character (internal use only).
    @param idx The character to retrieve the fall-back character for.
    @return The fall-back character.
*/
int _term_get_default_acs(int idx) {
	if (idx < 0 || idx > 255)
		return ' ';
	return default_alternate_chars[idx] != 0 ? default_alternate_chars[idx] : ' ';
}

/** Combine attributes, with priority.
    @param a The first set of attributes to combine (priority).
    @param b The second set of attributes to combine (no priority).
    @return The combined attributes.

    This function combines @p a and @p b, with the color attributes from @p a overriding
	the color attributes from @p b if both specify colors.
*/
CharData term_combine_attrs(CharData a, CharData b) {
	/* FIXME: take ncv into account */
	CharData result = b | (a & ~(FG_COLOR_ATTRS | BG_COLOR_ATTRS));
	if ((a & FG_COLOR_ATTRS) != 0)
		result = (result & ~(FG_COLOR_ATTRS)) | (a & FG_COLOR_ATTRS);
	if ((a & BG_COLOR_ATTRS) != 0)
		result = (result & ~(BG_COLOR_ATTRS)) | (a & BG_COLOR_ATTRS);
	return result;
}

/** Get the set of non-color video attributes. */
CharData term_get_ncv(void) {
	return ncv;
}
