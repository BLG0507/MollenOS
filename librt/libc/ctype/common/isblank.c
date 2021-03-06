/*
FUNCTION
	<<isblank>>, <<isblank_l>>---blank character predicate

INDEX
	isblank

INDEX
	isblank_l

ANSI_SYNOPSIS
	#include <ctype.h>
	int isblank(int <[c]>);

	#include <ctype.h>
	int isblank_l(int <[c]>, locale_t <[locale]>);

TRAD_SYNOPSIS
	#include <ctype.h>
	int isblank(<[c]>);

DESCRIPTION
<<isblank>> is a function which classifies singlebyte charset values by table
lookup.  It is a predicate returning non-zero for blank characters, and 0
for other characters.  It is defined only if <[c]> is representable as an
unsigned char or if <[c]> is EOF.

<<isblank_l>> is like <<isblank>> but performs the check based on the
locale specified by the locale object locale.  If <[locale]> is
LC_GLOBAL_LOCALE or not a valid locale object, the behaviour is undefined.

RETURNS
<<isblank>>, <<isblank_l>> return non-zero if <[c]> is a blank character.

PORTABILITY
<<isblank>> is C99.
<<isblank_l>> is POSIX-1.2008.

No supporting OS subroutines are required.
*/

/* Includes */
#include <ctype.h>

/* Undefine symbol in case it's been
* inlined as a macro */
#undef isblank

/* <<isblank>> is a function which classifies singlebyte charset values by table
 * lookup.  It is a predicate returning non-zero for blank characters, and 0
 * for other characters.  It is defined only if <[c]> is representable as an
 * unsigned char or if <[c]> is EOF. */
int isblank(int c)
{
	return ((__CTYPE_PTR[c+1] & _CTYPE_B) || (c == '\t'));
}
