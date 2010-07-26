#include <string.h>
#include <errno.h>
#ifdef USE_GETTEXT
#include <libintl.h>
#define _(x) dgettext("LIBT3", (x))
#else
#define _(x) (x)
#endif

#include "terminal.h"

/** Get the value of ::T3_WINDOW_VERSION corresponding to the actually used library.
    @ingroup t3window_other
    @return The value of ::T3_WINDOW_VERSION.

    This function can be useful to determine at runtime what version of the library
    was linked to the program. Although currently there are no known uses for this
    information, future library additions may prompt library users to want to operate
    differently depending on the available features.
*/
long t3_window_get_version(void) {
	return T3_WINDOW_VERSION;
}

/** Get a string description for an error code.
    @param error The error code returned by a function in libt3window.
    @return An internationalized string description for the error code.
*/
const char *t3_window_strerror(int error) {
	switch (error) {
		default:
			t3_window_strerror_base(error);

		case T3_ERR_NOT_A_TTY:
			return _("in/output device is not a terminal");
		case T3_ERR_TIMEOUT:
			return _("timeout");
		case T3_ERR_NO_SIZE_INFO:
			return _("size information for terminal could not be found");
		case T3_ERR_NONPRINT:
			return _("non-printable character passed for display");
	}
}
