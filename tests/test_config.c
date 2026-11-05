/* test_config.c: tests for the key = value config parser (common/config.c)
 *
 * usage: test_config <path to toydb.conf> [present_only]
 *
 * Temp conf files are made in the current directory with mkstemp (make test
 * runs this from a scratch dir) and removed at the end. The last check loads
 * the real toydb.conf and checks that every documented key is there with its
 * documented default; with "present_only" it only checks that each key is
 * there and parses as its type, so it still passes after the conf is tuned.
 * The last output line is "test_config done!" only if every check passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "config.h"

static int nfail = 0;		/* failed checks */
static int ncheck = 0;		/* all checks */

#define CHECK(cond) do { \
		ncheck++; \
		if (!(cond)) { \
			nfail++; \
			printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

#define MAXTMP	64
static char tmpnames[MAXTMP][32];	/* temp files to remove at the end */
static int ntmp = 0;

static const char *write_conf(const char *text)
/****************************************************************************
SPECIFICATIONS:
	Write "text" to a new temp file in the current directory.

RETURN VALUE:
	The file name (kept until the end of the program), or exit on error.
*****************************************************************************/
{
	FILE *fp;
	int fd;

	if (ntmp >= MAXTMP) {
		printf("test_config: too many temp files\n");
		exit(1);
	}
	strcpy(tmpnames[ntmp], "cfgtestXXXXXX");
	fd = mkstemp(tmpnames[ntmp]);
	if (fd < 0 || (fp = fdopen(fd, "w")) == NULL) {
		perror("test_config: mkstemp");
		exit(1);
	}
	fputs(text, fp);
	fclose(fp);
	return tmpnames[ntmp++];
}

static int load_text(const char *text)
/****************************************************************************
SPECIFICATIONS:
	Write text to a temp file and Config_Load it.

RETURN VALUE:
	The return value of Config_Load.
*****************************************************************************/
{
	return Config_Load(write_conf(text));
}

#define G_INT	0	/* the getter expect_exit calls */
#define G_DBL	1
#define G_ILIST	2
#define G_SLIST	3

static void expect_exit(const char *text, const char *key, int getter,
			const char *want)
/****************************************************************************
SPECIFICATIONS:
	Write "text" to a temp conf, then in a child process load it and
	call the getter "getter" on "key". The value does not parse, so the
	getter must print exactly the line "want" on stderr and exit with
	status 1; if it returns, the child exits 0 and the check fails.
	Counts as one check.
*****************************************************************************/
{
	char msg[512];
	char sv[4][CONFIG_VALLEN];
	const char *name;
	int v[4];
	int fds[2], status, len;
	pid_t pid;
	ssize_t n;

	ncheck++;
	name = write_conf(text);
	fflush(stdout);
	fflush(stderr);
	if (pipe(fds) != 0 || (pid = fork()) < 0) {
		nfail++;
		printf("FAILED expect_exit '%s': pipe or fork failed\n", key);
		return;
	}
	if (pid == 0) {
		close(fds[0]);
		dup2(fds[1], 2);
		Config_Load(name);
		if (getter == G_INT)
			(void)Config_GetInt(key, 0);
		else if (getter == G_DBL)
			(void)Config_GetDouble(key, 0.0);
		else if (getter == G_ILIST)
			(void)Config_GetIntList(key, v, 4, "1");
		else
			(void)Config_GetStrList(key, sv, 4, "x");
		_exit(0);	/* the getter returned: wrong */
	}
	close(fds[1]);
	len = 0;
	while (len < (int)sizeof(msg) - 1 &&
	       (n = read(fds[0], msg + len, sizeof(msg) - 1 - len)) > 0)
		len += (int)n;
	msg[len] = '\0';
	close(fds[0]);
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 1 || strcmp(msg, want) != 0) {
		nfail++;
		printf("FAILED expect_exit '%s': exit %d, stderr \"%s\" (want exit 1, \"%s\")\n",
		       key, WIFEXITED(status) ? WEXITSTATUS(status) : -1, msg, want);
	}
}

static void test_plain(void)
{
	CHECK(load_text("a = 1\nb = hello\nc=3\n") == CONFIG_OK);
	CHECK(Config_GetInt("a", -1) == 1);
	CHECK(strcmp(Config_GetStr("b", "x"), "hello") == 0);
	CHECK(Config_GetInt("c", -1) == 3);
	/* keys are case sensitive */
	CHECK(Config_GetInt("A", -1) == -1);
}

static void test_comments(void)
{
	CHECK(load_text("# full line comment\n"
		"x = 5   # trailing comment\n"
		"   # indented comment\n"
		"y = abc#no space before it\n"
		"#z = 9\n") == CONFIG_OK);
	CHECK(strcmp(Config_GetStr("x", ""), "5") == 0);
	CHECK(Config_GetInt("x", -1) == 5);
	CHECK(strcmp(Config_GetStr("y", ""), "abc") == 0);
	CHECK(Config_GetStr("z", NULL) == NULL);
}

static void test_whitespace(void)
{
	CHECK(load_text("  \t key1 \t=\t  val one  \t\n"
		"\n   \n\t\n"
		"key2=   7\n") == CONFIG_OK);
	CHECK(strcmp(Config_GetStr("key1", ""), "val one") == 0);
	CHECK(Config_GetInt("key2", -1) == 7);
}

static void test_crlf(void)
{
	CHECK(load_text("a = 1\r\nb = two\r\n\r\nc = 2.5 # note\r\n") == CONFIG_OK);
	CHECK(Config_GetInt("a", -1) == 1);
	CHECK(strcmp(Config_GetStr("b", ""), "two") == 0);
	CHECK(Config_GetDouble("c", -1.0) == 2.5);
}

static void test_duplicate(void)
{
	CHECK(load_text("k = 1\nother = 0\nk = 2\n") == CONFIG_OK);
	CHECK(Config_GetInt("k", -1) == 2);
}

static void test_no_equals(void)
{
	/* bad lines are warnings, not errors: the load still returns OK */
	CHECK(load_text("good = 1\n"
		"this line has no equals\n"
		"= 5\n"
		"my key = 3\n"
		"after = 2\n") == CONFIG_OK);
	CHECK(Config_GetInt("good", -1) == 1);
	CHECK(Config_GetInt("after", -1) == 2);
	CHECK(Config_GetStr("this line has no equals", NULL) == NULL);
	CHECK(Config_GetStr("my key", NULL) == NULL);
	CHECK(Config_GetStr("my", NULL) == NULL);
	CHECK(Config_GetStr("", NULL) == NULL);
}

static void test_empty_value(void)
{
	int v[4];

	CHECK(load_text("e =\nf =   # only a comment\n") == CONFIG_OK);
	CHECK(Config_GetStr("e", NULL) != NULL);
	CHECK(strcmp(Config_GetStr("e", "dflt"), "") == 0);
	CHECK(strcmp(Config_GetStr("f", "dflt"), "") == 0);
	/* an empty value is an empty list, but not a number */
	CHECK(Config_GetIntList("e", v, 4, "1,2") == 0);
	expect_exit("e =\n", "e", G_INT, "config: e: '' is not a valid number\n");
	expect_exit("e =   # only a comment\n", "e", G_DBL,
		"config: e: '' is not a valid number\n");
}

static void test_getters(void)
{
	CHECK(load_text("i = 42\nneg = -12\nplus = +3\nd = 0.25\n"
		"dint = 42\nexp = 1e3\ns = some text, with comma\n") == CONFIG_OK);
	CHECK(Config_GetInt("i", 0) == 42);
	CHECK(Config_GetInt("neg", 0) == -12);
	CHECK(Config_GetInt("plus", 0) == 3);
	CHECK(Config_GetDouble("d", 0.0) == 0.25);
	CHECK(Config_GetDouble("dint", 0.0) == 42.0);
	CHECK(Config_GetDouble("exp", 0.0) == 1000.0);
	CHECK(strcmp(Config_GetStr("s", ""), "some text, with comma") == 0);
}

static void test_missing_key(void)
{
	int v[4];
	char s[4][CONFIG_VALLEN];

	CHECK(load_text("a = 1\n") == CONFIG_OK);
	CHECK(Config_GetInt("nokey", 99) == 99);
	CHECK(Config_GetDouble("nokey", 0.5) == 0.5);
	CHECK(strcmp(Config_GetStr("nokey", "dflt"), "dflt") == 0);
	CHECK(Config_GetStr("nokey", NULL) == NULL);
	CHECK(Config_GetIntList("nokey", v, 4, "4, 5") == 2);
	CHECK(v[0] == 4 && v[1] == 5);
	CHECK(Config_GetIntList("nokey", v, 4, NULL) == 0);
	CHECK(Config_GetStrList("nokey", s, 4, "p ,q") == 2);
	CHECK(strcmp(s[0], "p") == 0 && strcmp(s[1], "q") == 0);
}

static void test_malformed(void)
{
	/* a value that does not parse stops the program with status 1 */
	expect_exit("n = abc\n", "n", G_INT, "config: n: 'abc' is not a valid number\n");
	expect_exit("n = 12abc\n", "n", G_INT, "config: n: '12abc' is not a valid number\n");
	expect_exit("n = 1.5\n", "n", G_INT, "config: n: '1.5' is not a valid number\n");
	expect_exit("n = 1.5x\n", "n", G_INT, "config: n: '1.5x' is not a valid number\n");
	expect_exit("n = 99999999999\n", "n", G_INT,
		"config: n: '99999999999' is not a valid number\n");
	expect_exit("n = 1 2\n", "n", G_INT, "config: n: '1 2' is not a valid number\n");
	expect_exit("d = abc\n", "d", G_DBL, "config: d: 'abc' is not a valid number\n");
	expect_exit("d = 1.5x\n", "d", G_DBL, "config: d: '1.5x' is not a valid number\n");
	/* the same values can still be read as strings */
	CHECK(load_text("i = 12abc\n") == CONFIG_OK);
	CHECK(strcmp(Config_GetStr("i", ""), "12abc") == 0);
}

static void test_lists(void)
{
	int v[8];
	char s[8][CONFIG_VALLEN];

	CHECK(load_text("l = 20, 50,100\n"
		"one = 7\n"
		"s = uniform , hotcold,sequential\n"
		"long = 1,2,3,4,5\n"
		"slong = a,b,c\n") == CONFIG_OK);

	CHECK(Config_GetIntList("l", v, 8, "0") == 3);
	CHECK(v[0] == 20 && v[1] == 50 && v[2] == 100);
	CHECK(Config_GetIntList("one", v, 8, "0") == 1);
	CHECK(v[0] == 7);

	CHECK(Config_GetStrList("s", s, 8, "x") == 3);
	CHECK(strcmp(s[0], "uniform") == 0);
	CHECK(strcmp(s[1], "hotcold") == 0);
	CHECK(strcmp(s[2], "sequential") == 0);

	/* an empty or non-numeric element stops the program with status 1 */
	expect_exit("l = 100,,abc\n", "l", G_ILIST,
		"config: l: '100,,abc' is not a valid number\n");
	expect_exit("l = 1,x,3\n", "l", G_ILIST, "config: l: '1,x,3' is not a valid number\n");
	expect_exit("l = 1,2,\n", "l", G_ILIST, "config: l: '1,2,' is not a valid number\n");
	expect_exit("l = 1.5x\n", "l", G_ILIST, "config: l: '1.5x' is not a valid number\n");
	expect_exit("s = a, ,c\n", "s", G_SLIST, "config: s: 'a, ,c' is not a valid list\n");

	/* too long: keep the first max values */
	v[3] = -5;
	CHECK(Config_GetIntList("long", v, 3, "0") == 3);
	CHECK(v[0] == 1 && v[1] == 2 && v[2] == 3);
	CHECK(v[3] == -5);
	CHECK(Config_GetStrList("slong", s, 2, "x") == 2);
	CHECK(strcmp(s[0], "a") == 0 && strcmp(s[1], "b") == 0);
	CHECK(Config_GetIntList("l", v, 0, "1") == 0);
}

static void test_limits(void)
{
	char text[2048];
	char *p;
	int i;

	/* a line longer than the line buffer is skipped, the rest still loads */
	strcpy(text, "before = 1\nlong = ");
	p = text + strlen(text);
	for (i = 0; i < CONFIG_LINELEN + 50; i++)
		*p++ = 'x';
	strcpy(p, "\nafter = 2\n");
	CHECK(load_text(text) == CONFIG_ETOOLONG);
	CHECK(Config_GetInt("before", -1) == 1);
	CHECK(Config_GetInt("after", -1) == 2);
	CHECK(Config_GetStr("long", NULL) == NULL);

	/* a value longer than the value buffer */
	strcpy(text, "v = ");
	p = text + strlen(text);
	for (i = 0; i < CONFIG_VALLEN; i++)
		*p++ = 'y';
	strcpy(p, "\nw = 3\n");
	CHECK(load_text(text) == CONFIG_ETOOLONG);
	CHECK(Config_GetStr("v", NULL) == NULL);
	CHECK(Config_GetInt("w", -1) == 3);

	/* one key more than the table holds */
	text[0] = '\0';
	for (i = 0; i <= CONFIG_MAXKEYS; i++)
		sprintf(text + strlen(text), "k%d = %d\n", i, i);
	CHECK(load_text(text) == CONFIG_ETOOLONG);
	CHECK(Config_GetInt("k0", -1) == 0);
	CHECK(Config_GetInt("k63", -1) == CONFIG_MAXKEYS - 1);
	CHECK(Config_GetStr("k64", NULL) == NULL);
}

static void test_args(void)
{
	char *argv[9];

	CHECK(load_text("a = 1\nb = 2\n") == CONFIG_OK);
	argv[0] = "prog";
	argv[1] = "a=10";
	argv[2] = "-v";
	argv[3] = "file.txt";
	argv[4] = "new = hi";
	argv[5] = "=5";
	argv[6] = "bad key=1";
	argv[7] = "b=";
	argv[8] = NULL;
	CHECK(Config_ParseArgs(8, argv) == 3);
	CHECK(Config_GetInt("a", -1) == 10);
	CHECK(strcmp(Config_GetStr("new", ""), "hi") == 0);
	CHECK(strcmp(Config_GetStr("b", "x"), "") == 0);
	CHECK(Config_GetStr("-v", NULL) == NULL);
	CHECK(Config_GetStr("file.txt", NULL) == NULL);
	CHECK(Config_ParseArgs(1, argv) == 0);
}

static void test_missing_file(void)
{
	CHECK(load_text("a = 1\n") == CONFIG_OK);
	CHECK(Config_Load("cfgtest_no_such_file.conf") == CONFIG_ENOFILE);
	/* the table was emptied, so defaults apply */
	CHECK(Config_GetInt("a", 5) == 5);
	CHECK(strcmp(Config_GetStr("a", "d"), "d") == 0);
	CHECK(Config_Load(NULL) == CONFIG_ENOFILE);
}

static void test_init(void)
{
	char confarg[64];
	char *argv[4];
	const char *name;

	name = write_conf("a = 1\nb = 2\n");
	sprintf(confarg, "conf=%s", name);
	argv[0] = "prog";
	argv[1] = confarg;
	argv[2] = "a=2";
	argv[3] = NULL;
	CHECK(Config_Init(3, argv) == CONFIG_OK);
	CHECK(Config_GetInt("a", -1) == 2);
	CHECK(Config_GetInt("b", -1) == 2);

	/* no conf= argument: $TOYDB_CONF is used */
	CHECK(setenv("TOYDB_CONF", name, 1) == 0);
	CHECK(Config_Init(1, argv) == CONFIG_OK);
	CHECK(Config_GetInt("a", -1) == 1);

	/* conf= wins over $TOYDB_CONF */
	CHECK(setenv("TOYDB_CONF", "cfgtest_no_such_file.conf", 1) == 0);
	CHECK(Config_Init(2, argv) == CONFIG_OK);
	CHECK(Config_GetInt("a", -1) == 1);
	CHECK(Config_Init(1, argv) == CONFIG_ENOFILE);
	CHECK(Config_GetInt("a", -1) == -1);
	unsetenv("TOYDB_CONF");
}

static void test_dump(void)
{
	char got[256];
	const char *name;
	FILE *fp;
	size_t n;

	CHECK(load_text("b = 2\na = 1 # c\nb = 3\ne =\n") == CONFIG_OK);
	name = write_conf("");
	fp = fopen(name, "w");
	CHECK(fp != NULL);
	if (fp == NULL)
		return;
	Config_Dump(fp);
	fclose(fp);

	fp = fopen(name, "r");
	CHECK(fp != NULL);
	if (fp == NULL)
		return;
	n = fread(got, 1, sizeof(got) - 1, fp);
	got[n] = '\0';
	fclose(fp);
	CHECK(strcmp(got, "b = 3\na = 1\ne = \n") == 0);

	/* the dump loads back to the same table */
	CHECK(Config_Load(name) == CONFIG_OK);
	CHECK(Config_GetInt("b", -1) == 3);
	CHECK(Config_GetInt("a", -1) == 1);
	CHECK(strcmp(Config_GetStr("e", "x"), "") == 0);
}

/* key types in toydb.conf */
#define T_INT	0
#define T_DBL	1
#define T_STR	2
#define T_ILIST	3
#define T_SLIST	4

static struct {
	const char *key;
	int type;
	const char *def;	/* documented default, as written in toydb.conf */
} conf_keys[] = {
	{ "buffer_pool_size",	T_INT,	 "50" },
	{ "default_policy",	T_STR,	 "LRU" },
	{ "data_file",		T_STR,	 "../data/student.txt" },
	{ "work_dir",		T_STR,	 "/tmp/toydb_run" },
	{ "seed",		T_INT,	 "42" },
	{ "buf_file_pages",	T_INT,	 "1000" },
	{ "buf_queries",	T_INT,	 "10000" },
	{ "buf_pool_sizes",	T_ILIST, "20,50,100,900,1200" },
	{ "buf_read_pcts",	T_ILIST, "100,90,75,50,25,10,0" },
	{ "buf_patterns",	T_SLIST, "uniform,hotcold,sequential" },
	{ "hot_fraction",	T_DBL,	 "0.2" },
	{ "hot_prob",		T_DBL,	 "0.8" },
	{ "plot_pool",		T_INT,	 "100" },
	{ "plot_read_pct",	T_INT,	 "50" },
	{ "static_lengths",	T_ILIST, "100,104,110,128,160,200,256" },
	{ "churn_fraction",	T_DBL,	 "0.3" },
	{ "bulk_fill_factor",	T_DBL,	 "1.0" },
	{ "idx_point_queries",	T_INT,	 "1000" },
	{ "idx_missing_queries",T_INT,	 "100" },
	{ "idx_range_queries",	T_INT,	 "200" },
	{ "idx_range_len",	T_INT,	 "100" },
	{ "repeats",		T_INT,	 "5" },
	{ NULL,			0,	 NULL }
};

static int is_int(const char *s)
{
	char *end;

	if (*s == '\0')
		return 0;
	(void)strtol(s, &end, 10);
	return *end == '\0';
}

static int is_double(const char *s)
{
	char *end;

	if (*s == '\0')
		return 0;
	(void)strtod(s, &end);
	return *end == '\0';
}

static int check_key(int k, int present_only)
/****************************************************************************
SPECIFICATIONS:
	Check one key of the real toydb.conf. The expected values are
	written out by hand in conf_keys (not computed by the parser), except
	that lists compare against strtol/strtod of each default element.

RETURN VALUE:
	1 if the key is fine, 0 otherwise.
*****************************************************************************/
{
	const char *key, *val, *def;
	int got[CONFIG_MAXLIST];
	char sgot[CONFIG_MAXLIST][CONFIG_VALLEN];
	char want[CONFIG_VALLEN];
	char *tok;
	int n, i;

	key = conf_keys[k].key;
	def = conf_keys[k].def;
	val = Config_GetStr(key, NULL);
	if (val == NULL)
		return 0;

	switch (conf_keys[k].type) {
	case T_INT:
		if (!is_int(val))
			return 0;
		return present_only || Config_GetInt(key, -1) == atoi(def);
	case T_DBL:
		if (!is_double(val))
			return 0;
		return present_only || Config_GetDouble(key, -1.0) == strtod(def, NULL);
	case T_STR:
		return present_only ? val[0] != '\0' : strcmp(val, def) == 0;
	case T_ILIST:
		n = Config_GetIntList(key, got, CONFIG_MAXLIST, NULL);
		if (n <= 0)
			return 0;
		if (present_only)
			return 1;
		strcpy(want, def);
		i = 0;
		for (tok = strtok(want, ","); tok != NULL; tok = strtok(NULL, ",")) {
			if (i >= n || got[i] != atoi(tok))
				return 0;
			i++;
		}
		return i == n;
	case T_SLIST:
		n = Config_GetStrList(key, sgot, CONFIG_MAXLIST, NULL);
		if (n <= 0)
			return 0;
		if (present_only)
			return 1;
		strcpy(want, def);
		i = 0;
		for (tok = strtok(want, ","); tok != NULL; tok = strtok(NULL, ",")) {
			if (i >= n || strcmp(sgot[i], tok) != 0)
				return 0;
			i++;
		}
		return i == n;
	}
	return 0;
}

static void test_real_conf(const char *path, int present_only)
{
	int k;

	if (path == NULL) {
		printf("FAILED: no toydb.conf path given (usage: test_config <conf> [present_only])\n");
		nfail++;
		return;
	}
	CHECK(Config_Load(path) == CONFIG_OK);
	for (k = 0; conf_keys[k].key != NULL; k++) {
		ncheck++;
		if (!check_key(k, present_only)) {
			nfail++;
			printf("FAILED real conf %s: key '%s' %s (want %s)\n", path,
				conf_keys[k].key,
				Config_GetStr(conf_keys[k].key, NULL) == NULL
					? "missing" : "has a wrong or bad value",
				present_only ? "present and parseable" : conf_keys[k].def);
		}
	}
	/* the real policy value must be one of the two we support */
	CHECK(strcmp(Config_GetStr("default_policy", ""), "LRU") == 0
		|| strcmp(Config_GetStr("default_policy", ""), "MRU") == 0);
}

int main(int argc, char **argv)
{
	int i, present_only;

	present_only = (argc > 2 && strcmp(argv[2], "present_only") == 0);

	test_plain();
	test_comments();
	test_whitespace();
	test_crlf();
	test_duplicate();
	test_no_equals();
	test_empty_value();
	test_getters();
	test_missing_key();
	test_malformed();
	test_lists();
	test_limits();
	test_args();
	test_missing_file();
	test_init();
	test_dump();
	test_real_conf(argc > 1 ? argv[1] : NULL, present_only);

	for (i = 0; i < ntmp; i++)
		unlink(tmpnames[i]);

	printf("test_config: %d checks, %d failed%s\n", ncheck, nfail,
		present_only ? " (present_only)" : "");
	if (nfail > 0)
		return 1;
	printf("test_config done!\n");
	return 0;
}
