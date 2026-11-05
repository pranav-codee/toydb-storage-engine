/* config.c: reads key = value lines (toydb.conf) into a small static table */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "config.h"

/*
 * The table of loaded keys. Keys are kept in the order they first appeared,
 * so Config_Dump prints them in file order. This is the only global state.
 */
static struct {
	char key[CONFIG_KEYLEN];
	char val[CONFIG_VALLEN];
} cfg_tab[CONFIG_MAXKEYS];
static int cfg_n = 0;		/* number of entries in use in cfg_tab */

/* list element types for cfg_list() */
#define CFG_INT	0
#define CFG_STR	1

/****************** Internal Support Functions *****************************/

static int cfg_isblank(int c)
/****************************************************************************
SPECIFICATIONS:
	True if c is whitespace for our purposes. '\r' and '\n' count, so
	CRLF line endings are trimmed away like any other trailing blank.
*****************************************************************************/
{
	return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static char *cfg_trim(char *s)
/****************************************************************************
SPECIFICATIONS:
	Remove blanks at both ends of the string s, in place.

RETURN VALUE:
	Pointer to the first non-blank char of s (inside s).
*****************************************************************************/
{
	char *end;

	while (*s != '\0' && cfg_isblank((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && cfg_isblank((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return s;
}

static int cfg_split(char *line, char **key, char **val)
/****************************************************************************
SPECIFICATIONS:
	Split one line (modified in place) into key and value. Everything
	from the first '#' on is a comment. The key and value are trimmed.

RETURN VALUE:
	0	blank line (or only a comment), nothing to store
	1	*key and *val are set
	-1	no '=' in the line
	-2	empty key, or a key with blanks inside it
*****************************************************************************/
{
	char *hash, *eq, *k, *p;

	hash = strchr(line, '#');
	if (hash != NULL)
		*hash = '\0';
	line = cfg_trim(line);
	if (*line == '\0')
		return 0;

	eq = strchr(line, '=');
	if (eq == NULL)
		return -1;
	*eq = '\0';
	k = cfg_trim(line);
	if (*k == '\0')
		return -2;
	for (p = k; *p != '\0'; p++)
		if (cfg_isblank((unsigned char)*p))
			return -2;

	*key = k;
	*val = cfg_trim(eq + 1);
	return 1;
}

static int cfg_find(const char *key)
/****************************************************************************
SPECIFICATIONS:
	Find "key" in the table (case sensitive).

RETURN VALUE:
	Index into cfg_tab, or -1 if not there.
*****************************************************************************/
{
	int i;

	if (key == NULL)
		return -1;
	for (i = 0; i < cfg_n; i++)
		if (strcmp(cfg_tab[i].key, key) == 0)
			return i;
	return -1;
}

static int cfg_set(const char *key, const char *val, const char *where,
		int lineno)
/****************************************************************************
SPECIFICATIONS:
	Store key = val, replacing an older value of the same key. "where"
	and "lineno" are only used in error messages (file name and line,
	or "argv" and the argument number).

RETURN VALUE:
	0	stored
	-1	key or value too long, or table full (message printed)
*****************************************************************************/
{
	int i;

	if (strlen(key) >= CONFIG_KEYLEN) {
		fprintf(stderr, "config: %s:%d: key longer than %d chars, skipped\n",
			where, lineno, CONFIG_KEYLEN - 1);
		return -1;
	}
	if (strlen(val) >= CONFIG_VALLEN) {
		fprintf(stderr, "config: %s:%d: value of '%s' longer than %d chars, skipped\n",
			where, lineno, key, CONFIG_VALLEN - 1);
		return -1;
	}

	i = cfg_find(key);
	if (i < 0) {
		if (cfg_n >= CONFIG_MAXKEYS) {
			fprintf(stderr, "config: %s:%d: table full (%d keys), '%s' skipped\n",
				where, lineno, CONFIG_MAXKEYS, key);
			return -1;
		}
		i = cfg_n++;
		strcpy(cfg_tab[i].key, key);
	}
	strcpy(cfg_tab[i].val, val);
	return 0;
}

static int cfg_toint(const char *s, int *out)
/****************************************************************************
SPECIFICATIONS:
	Convert the whole string s to an int.

RETURN VALUE:
	0 and *out set if s is a whole decimal int that fits, -1 otherwise.
*****************************************************************************/
{
	char *end;
	long v;

	if (*s == '\0')
		return -1;
	errno = 0;
	v = strtol(s, &end, 10);
	if (*end != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX)
		return -1;
	*out = (int)v;
	return 0;
}

static void cfg_bad(const char *key, const char *val, const char *what)
/****************************************************************************
SPECIFICATIONS:
	A value from the file or the command line did not parse: print
	"config: <key>: '<value>' is not <what>" on stderr and exit with
	status 1, so a typo in toydb.conf can never run an experiment with
	some other value.
*****************************************************************************/
{
	fprintf(stderr, "config: %s: '%s' is not %s\n", key, val, what);
	exit(1);
}

static int cfg_list(const char *key, const char *src, int isdef, int type,
		int *iout, char sout[][CONFIG_VALLEN], int max)
/****************************************************************************
SPECIFICATIONS:
	Parse the comma separated list "src" into iout (type CFG_INT) or
	sout (type CFG_STR), at most "max" elements. "isdef" is only used
	in messages, to say whether src was the value or the default.
	An empty (or all blank) src is an empty list. A list longer than
	max gives a warning and the first max elements are kept.
	A bad element (empty, or not an int for CFG_INT) in the value of
	the key is fatal: cfg_bad prints the key and value and exits 1.

RETURN VALUE:
	Number of elements stored, or -1 on a bad element of the default
	(warning printed).
*****************************************************************************/
{
	char elem[CONFIG_VALLEN];
	char *e;
	const char *p, *q;
	int n, len, v;

	n = 0;
	p = src;
	while (*p != '\0' && cfg_isblank((unsigned char)*p))
		p++;
	if (*p == '\0')
		return 0;

	for (;;) {
		q = strchr(p, ',');
		len = (q == NULL) ? (int)strlen(p) : (int)(q - p);
		if (n >= max) {
			fprintf(stderr, "config: key '%s': %s has more than %d values, "
				"keeping the first %d\n", key,
				isdef ? "default" : "list", max, max);
			return n;
		}
		if (len >= CONFIG_VALLEN) {
			fprintf(stderr, "config: key '%s': %s element %d too long%s\n",
				key, isdef ? "default" : "list", n + 1,
				isdef ? "" : ", using the default list");
			return -1;
		}
		memcpy(elem, p, len);
		elem[len] = '\0';
		e = cfg_trim(elem);
		if (*e == '\0' || (type == CFG_INT && cfg_toint(e, &v) < 0)) {
			if (!isdef)
				cfg_bad(key, src, type == CFG_INT ?
					"a valid number" : "a valid list");
			fprintf(stderr, "config: key '%s': bad default element %d '%s'\n",
				key, n + 1, e);
			return -1;
		}
		if (type == CFG_INT)
			iout[n] = v;
		else
			strcpy(sout[n], e);
		n++;
		if (q == NULL)
			return n;
		p = q + 1;
	}
}

static int cfg_getlist(const char *key, int type, int *iout,
		char sout[][CONFIG_VALLEN], int max, const char *def_csv)
/****************************************************************************
SPECIFICATIONS:
	Common part of Config_GetIntList and Config_GetStrList: use the
	value of key if present (a bad element there exits, see cfg_list),
	else def_csv.

RETURN VALUE:
	Number of elements stored.
*****************************************************************************/
{
	int i, n;

	if (max <= 0)
		return 0;
	i = cfg_find(key);
	if (i >= 0) {
		n = cfg_list(key, cfg_tab[i].val, 0, type, iout, sout, max);
		if (n >= 0)
			return n;
	}
	if (def_csv == NULL)
		return 0;
	n = cfg_list(key, def_csv, 1, type, iout, sout, max);
	return (n < 0) ? 0 : n;
}

/****************** Interface Functions *****************************/

int Config_Load(const char *path)
/****************************************************************************
SPECIFICATIONS:
	See config.h. Empties the table, then reads the file line by line.
	A line that does not fit in CONFIG_LINELEN is skipped as a whole.

RETURN VALUE:
	CONFIG_OK, CONFIG_ENOFILE or CONFIG_ETOOLONG (see config.h).
*****************************************************************************/
{
	FILE *fp;
	char line[CONFIG_LINELEN];
	char *key, *val;
	int lineno, len, c, rc;

	cfg_n = 0;
	if (path == NULL || (fp = fopen(path, "r")) == NULL)
		return CONFIG_ENOFILE;

	rc = CONFIG_OK;
	lineno = 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
		lineno++;
		len = (int)strlen(line);
		if (len == CONFIG_LINELEN - 1 && line[len - 1] != '\n') {
			/* buffer full: fine only if the line ends right here */
			c = getc(fp);
			if (c != EOF && c != '\n') {
				fprintf(stderr, "config: %s:%d: line longer than %d chars, skipped\n",
					path, lineno, CONFIG_LINELEN - 1);
				while (c != EOF && c != '\n')
					c = getc(fp);
				rc = CONFIG_ETOOLONG;
				continue;
			}
		}

		switch (cfg_split(line, &key, &val)) {
		case 0:
			break;
		case 1:
			if (cfg_set(key, val, path, lineno) < 0)
				rc = CONFIG_ETOOLONG;
			break;
		case -1:
			fprintf(stderr, "config: %s:%d: no '=' in line, skipped\n",
				path, lineno);
			break;
		default:
			fprintf(stderr, "config: %s:%d: empty key or blank inside key, skipped\n",
				path, lineno);
			break;
		}
	}
	fclose(fp);
	return rc;
}

int Config_ParseArgs(int argc, char **argv)
/****************************************************************************
SPECIFICATIONS:
	See config.h. Arguments too long for a line buffer are ignored like
	any other non key=value argument.

RETURN VALUE:
	Number of key=value arguments applied.
*****************************************************************************/
{
	char buf[CONFIG_LINELEN];
	char *key, *val;
	int i, n;

	n = 0;
	for (i = 1; i < argc; i++) {
		if (argv[i] == NULL || strchr(argv[i], '=') == NULL)
			continue;
		if (strlen(argv[i]) >= sizeof(buf))
			continue;
		strcpy(buf, argv[i]);
		if (cfg_split(buf, &key, &val) != 1)
			continue;
		if (cfg_set(key, val, "argv", i) == 0)
			n++;
	}
	return n;
}

int Config_Init(int argc, char **argv)
/****************************************************************************
SPECIFICATIONS:
	See config.h. If "conf=" is given more than once the last one wins,
	the same as for any other key.

RETURN VALUE:
	The return value of Config_Load.
*****************************************************************************/
{
	char buf[CONFIG_LINELEN];
	char path[CONFIG_VALLEN];
	char *key, *val, *env;
	int i, rc;

	path[0] = '\0';
	for (i = 1; i < argc; i++) {
		if (argv[i] == NULL || strlen(argv[i]) >= sizeof(buf))
			continue;
		strcpy(buf, argv[i]);
		if (cfg_split(buf, &key, &val) == 1 && strcmp(key, "conf") == 0
				&& strlen(val) < sizeof(path))
			strcpy(path, val);
	}
	if (path[0] == '\0') {
		env = getenv("TOYDB_CONF");
		if (env != NULL && env[0] != '\0' && strlen(env) < sizeof(path))
			strcpy(path, env);
		else
			strcpy(path, "toydb.conf");
	}

	rc = Config_Load(path);
	Config_ParseArgs(argc, argv);
	return rc;
}

int Config_GetInt(const char *key, int def)
/****************************************************************************
SPECIFICATIONS:
	See config.h.

RETURN VALUE:
	The int value of key, or def if the key is missing (a bad value
	exits, see cfg_bad).
*****************************************************************************/
{
	int i, v;

	i = cfg_find(key);
	if (i < 0)
		return def;
	if (cfg_toint(cfg_tab[i].val, &v) < 0)
		cfg_bad(key, cfg_tab[i].val, "a valid number");
	return v;
}

double Config_GetDouble(const char *key, double def)
/****************************************************************************
SPECIFICATIONS:
	See config.h.

RETURN VALUE:
	The double value of key, or def if the key is missing (a bad value
	exits, see cfg_bad).
*****************************************************************************/
{
	char *end;
	double v;
	int i;

	i = cfg_find(key);
	if (i < 0)
		return def;
	errno = 0;
	v = strtod(cfg_tab[i].val, &end);
	if (cfg_tab[i].val[0] == '\0' || *end != '\0' || errno == ERANGE)
		cfg_bad(key, cfg_tab[i].val, "a valid number");
	return v;
}

const char *Config_GetStr(const char *key, const char *def)
/****************************************************************************
SPECIFICATIONS:
	See config.h.

RETURN VALUE:
	The string value of key, or def.
*****************************************************************************/
{
	int i;

	i = cfg_find(key);
	return (i < 0) ? def : cfg_tab[i].val;
}

int Config_GetIntList(const char *key, int *out, int max, const char *def_csv)
/****************************************************************************
SPECIFICATIONS:
	See config.h.

RETURN VALUE:
	Number of ints written to out.
*****************************************************************************/
{
	return cfg_getlist(key, CFG_INT, out, NULL, max, def_csv);
}

int Config_GetStrList(const char *key, char out[][CONFIG_VALLEN], int max,
		const char *def_csv)
/****************************************************************************
SPECIFICATIONS:
	See config.h.

RETURN VALUE:
	Number of strings written to out.
*****************************************************************************/
{
	return cfg_getlist(key, CFG_STR, NULL, out, max, def_csv);
}

void Config_Dump(FILE *f)
/****************************************************************************
SPECIFICATIONS:
	See config.h.
*****************************************************************************/
{
	int i;

	for (i = 0; i < cfg_n; i++)
		fprintf(f, "%s = %s\n", cfg_tab[i].key, cfg_tab[i].val);
}
