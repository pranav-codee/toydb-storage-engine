/* config.h: key = value configuration file (toydb.conf) for the toyDB tools */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdio.h>

/*
 * Parser limits. These are fixed sizes of the parser's own table, not
 * tunables of the experiments (those live in toydb.conf).
 */
#define CONFIG_MAXKEYS	64	/* max number of distinct keys kept */
#define CONFIG_KEYLEN	64	/* key buffer size, so keys up to 63 chars */
#define CONFIG_VALLEN	256	/* value buffer size, so values up to 255 chars */
#define CONFIG_LINELEN	512	/* line buffer size, so lines up to 511 chars (newline not counted) */
#define CONFIG_MAXLIST	32	/* suggested size of the out[] array for the list getters */

/* return codes of Config_Load (and Config_Init) */
#define CONFIG_OK	0	/* file read, every line accepted */
#define CONFIG_ENOFILE	1	/* file could not be opened (not fatal) */
#define CONFIG_ETOOLONG	2	/* some line/key/value was too long or the table was full */

int Config_Load(const char *path);
/****************************************************************************
SPECIFICATIONS:
	Empty the table, then read "key = value" lines from the file "path".
	'#' starts a comment anywhere on a line (so a value can not hold a
	'#'). Blank lines, whitespace around key, '=' and value, and CRLF
	line endings are ignored. Keys are case sensitive and a later
	duplicate replaces the earlier value. A non-blank line without '='
	(or with an empty key, or a key with blanks inside) gets one warning
	on stderr with file and line number and is skipped.
	A line, key or value that does not fit its buffer, or a new key when
	the table already holds CONFIG_MAXKEYS keys, is reported on stderr
	and skipped; the rest of the file is still read.

RETURN VALUE:
	CONFIG_OK	file read and every line accepted
	CONFIG_ENOFILE	file could not be opened; the table is left empty,
			so every getter returns its default
	CONFIG_ETOOLONG	some lines were rejected as too long / table full
*****************************************************************************/

int Config_ParseArgs(int argc, char **argv);
/****************************************************************************
SPECIFICATIONS:
	Apply command line arguments of the form "key=value" (argv[1] up to
	argv[argc-1]) on top of what is in the table, so they override the
	file. Each argument is parsed like a file line. Arguments without
	'=' or with a bad key are ignored without a warning, so a program
	can have other arguments too.

RETURN VALUE:
	The number of arguments applied.
*****************************************************************************/

int Config_Init(int argc, char **argv);
/****************************************************************************
SPECIFICATIONS:
	Convenience call for the start of main(). The config file is the
	value of an argument "conf=<path>" if one is given, else the
	environment variable TOYDB_CONF if set and non-empty, else
	"toydb.conf" in the current directory. Calls Config_Load on it and
	then Config_ParseArgs(argc, argv). A missing file is fine: every
	key then falls back to the default given at the getter.

RETURN VALUE:
	The return value of Config_Load.
*****************************************************************************/

int Config_GetInt(const char *key, int def);
/****************************************************************************
SPECIFICATIONS:
	Value of "key" as an int (decimal, optional sign).

RETURN VALUE:
	The value, or "def" if the key is missing. If the value is not a
	whole int (or does not fit in one), "config: <key>: '<value>' is
	not a valid number" is printed on stderr and the program exits 1.
*****************************************************************************/

double Config_GetDouble(const char *key, double def);
/****************************************************************************
SPECIFICATIONS:
	Value of "key" as a double (anything strtod takes).

RETURN VALUE:
	The value, or "def" if the key is missing. A malformed value prints
	the same message as Config_GetInt and the program exits 1.
*****************************************************************************/

const char *Config_GetStr(const char *key, const char *def);
/****************************************************************************
SPECIFICATIONS:
	Value of "key" as a string. An empty value ("key =") is returned as
	"", not as the default. The pointer points into the table and stays
	valid until the next Config_Load.

RETURN VALUE:
	The value, or "def" if the key is missing.
*****************************************************************************/

int Config_GetIntList(const char *key, int *out, int max, const char *def_csv);
/****************************************************************************
SPECIFICATIONS:
	Parse the value of "key" as a comma separated list of ints, with
	optional blanks around each element ("20, 50,100"), into out[0..].
	If the key is missing, "def_csv" is parsed instead (NULL means an
	empty list). An empty value gives an empty list.
	A bad element (empty or not an int) in the value prints the message
	of Config_GetInt and the program exits 1. A list longer than "max"
	gives a warning and only the first "max" values are kept.

RETURN VALUE:
	The number of values written to out.
*****************************************************************************/

int Config_GetStrList(const char *key, char out[][CONFIG_VALLEN], int max,
		const char *def_csv);
/****************************************************************************
SPECIFICATIONS:
	Same as Config_GetIntList but each element is kept as a string
	(blanks around it removed). An empty element exits ("is not a valid list").

RETURN VALUE:
	The number of strings written to out.
*****************************************************************************/

void Config_Dump(FILE *f);
/****************************************************************************
SPECIFICATIONS:
	Print every key in the table as a "key = value" line on "f", in the
	order the keys first appeared. The output can be read back with
	Config_Load, so it can be saved next to benchmark results.
*****************************************************************************/

#endif /* CONFIG_H */
