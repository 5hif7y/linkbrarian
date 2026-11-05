/*
 * linkbrarian - local-first link catalogue with Markdown notes
 *
 * C99, sqlite3 + libcurl. See README.md for the contract.
 */
#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include <curl/curl.h>
#include <sqlite3.h>

#include "config.h"

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define PATHSEP '\\'
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#else
#include <fcntl.h>
#include <strings.h>
#include <unistd.h>
#define PATHSEP '/'
#endif

struct sbuf {
	char *s;
	size_t len;
	size_t cap;
};

struct link_row {
	long long id;
	char *page_name;
	char *link;
	char *notes_path;
	long long n;
};

struct fetchbuf {
	char *data;
	size_t len;
	size_t cap;
	int overflow;
};

static void die(int code, const char *fmt, ...)
{
	va_list ap;

	fputs("linkbrarian: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(code);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p)
		die(EXIT_STORAGE, "out of memory");
	return p;
}

static void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);

	if (!q)
		die(EXIT_STORAGE, "out of memory");
	return q;
}

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;

	return memcpy(xmalloc(n), s, n);
}

static char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);

	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

static char *xasprintf(const char *fmt, ...)
{
	va_list ap, aq;
	int n;
	char *p;

	va_start(ap, fmt);
	va_copy(aq, ap);
	n = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	if (n < 0) {
		va_end(ap);
		die(EXIT_STORAGE, "formatting error");
	}
	p = xmalloc((size_t)n + 1);
	vsnprintf(p, (size_t)n + 1, fmt, ap);
	va_end(ap);
	return p;
}

/* ---------- paths ---------- */

static char *path_join(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	int sep = la > 0 && a[la - 1] != '/' && a[la - 1] != '\\';
	size_t n = la + (size_t)sep + lb;
	char *p = xmalloc(n + 1);

	memcpy(p, a, la);
	if (sep)
		p[la] = PATHSEP;
	memcpy(p + la + (size_t)sep, b, lb);
	p[n] = '\0';
	return p;
}

static int make_dir(const char *p)
{
#ifdef _WIN32
	return _mkdir(p);
#else
	return mkdir(p, 0700);
#endif
}

static int remove_dir(const char *p)
{
#ifdef _WIN32
	return _rmdir(p);
#else
	return rmdir(p);
#endif
}

static int remove_file(const char *p)
{
#ifdef _WIN32
	return _unlink(p);
#else
	return unlink(p);
#endif
}

static char *absolute_path(const char *p)
{
#ifdef _WIN32
	char *r = _fullpath(NULL, p, 0);

	if (!r)
		return NULL;
	{
		char *d = xstrdup(r);

		free(r);
		return d;
	}
#else
	char *r = realpath(p, NULL);

	if (!r)
		return NULL;
	{
		char *d = xstrdup(r);

		free(r);
		return d;
	}
#endif
}

static char *home_directory(void)
{
	const char *home;

#ifdef _WIN32
	home = getenv("USERPROFILE");
	if (!home || !*home)
		home = getenv("HOME");
#else
	home = getenv("HOME");
#endif
	if (!home || !*home)
		die(EXIT_STORAGE, "cannot determine the user home directory");
	return xstrdup(home);
}

static void ensure_data_directory(const char *path)
{
	struct stat st;

	if (make_dir(path) == 0)
		return;
	if (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode))
		return;
	if (errno == EEXIST)
		die(EXIT_STORAGE, "data directory %s exists but is not a directory", path);
	die(EXIT_STORAGE, "cannot create data directory %s: %s", path, strerror(errno));
}

/* ---------- UTF-8 and strings ---------- */

static size_t utf8_decode(const unsigned char *s, unsigned int *cp)
{
	if (s[0] < 0x80) {
		*cp = s[0];
		return 1;
	}
	if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
		*cp = ((unsigned int)(s[0] & 0x1f) << 6) | (unsigned int)(s[1] & 0x3f);
		return 2;
	}
	if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 &&
	    (s[2] & 0xc0) == 0x80) {
		*cp = ((unsigned int)(s[0] & 0x0f) << 12) |
		      ((unsigned int)(s[1] & 0x3f) << 6) |
		      (unsigned int)(s[2] & 0x3f);
		return 3;
	}
	if ((s[0] & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 &&
	    (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
		*cp = ((unsigned int)(s[0] & 0x07) << 18) |
		      ((unsigned int)(s[1] & 0x3f) << 12) |
		      ((unsigned int)(s[2] & 0x3f) << 6) |
		      (unsigned int)(s[3] & 0x3f);
		return 4;
	}
	*cp = 0xfffd;
	return 1;
}

static int utf8_encode(unsigned int cp, char out[4])
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

static void sb_add(struct sbuf *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 32;

		while (cap < b->len + n + 1)
			cap *= 2;
		b->s = xrealloc(b->s, cap);
		b->cap = cap;
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
}

static void sb_add_cp(struct sbuf *b, unsigned int cp)
{
	char tmp[4];
	int n = utf8_encode(cp, tmp);

	sb_add(b, tmp, (size_t)n);
}

static int wide_space(unsigned int cp)
{
	return cp == ' ' || (cp >= 0x09 && cp <= 0x0d) || cp == 0xa0 ||
	       (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
	       cp == 0x202f || cp == 0x205f || cp == 0x3000;
}

static unsigned int lower_cp(unsigned int cp)
{
	if (cp >= 'A' && cp <= 'Z')
		return cp + 32;
	if (cp >= 0xc0 && cp <= 0xde && cp != 0xd7)
		return cp + 0x20;
	if (cp == 0x178)
		return 0xff;
	return cp;
}

static char *trim_dup(const char *s)
{
	const unsigned char *start = (const unsigned char *)s;
	const unsigned char *end = start + strlen(s);
	const unsigned char *p = start;
	const unsigned char *last;

	while (*p) {
		unsigned int cp;
		size_t len = utf8_decode(p, &cp);

		if (!wide_space(cp))
			break;
		p += len;
	}
	start = p;
	last = p;
	while (p < end) {
		unsigned int cp;
		size_t len = utf8_decode(p, &cp);

		p += len;
		if (!wide_space(cp))
			last = p;
	}
	return xstrndup((const char *)start, (size_t)(last - start));
}

static char *normalize_key(const char *s)
{
	struct sbuf out = {0};
	const unsigned char *p = (const unsigned char *)s;
	int pending_space = 0;

	while (*p) {
		unsigned int cp;
		size_t len = utf8_decode(p, &cp);

		if (wide_space(cp)) {
			if (out.len)
				pending_space = 1;
		} else {
			char tmp[4];
			int n;

			if (pending_space) {
				sb_add(&out, " ", 1);
				pending_space = 0;
			}
			cp = lower_cp(cp);
			n = utf8_encode(cp, tmp);
			sb_add(&out, tmp, (size_t)n);
		}
		p += len;
	}
	if (!out.s)
		return xstrdup("");
	return out.s;
}

static const char *latin_slug(unsigned int cp)
{
	if (cp >= 0xc0 && cp <= 0xde && cp != 0xd7)
		cp += 0x20;
	switch (cp) {
	case 0xc6: return "ae";
	case 0xe6: return "ae";
	case 0xdf: return "ss";
	case 0xf0: return "d";
	case 0xf8: return "o";
	case 0xfe: return "th";
	case 0x153: return "oe";
	}
	if (cp >= 0xe0 && cp <= 0xe5) return "a";
	if (cp == 0xe7) return "c";
	if (cp >= 0xe8 && cp <= 0xeb) return "e";
	if (cp >= 0xec && cp <= 0xef) return "i";
	if (cp == 0xf1) return "n";
	if (cp >= 0xf2 && cp <= 0xf6) return "o";
	if (cp >= 0xf9 && cp <= 0xfc) return "u";
	if (cp == 0xfd || cp == 0xff) return "y";
	return NULL;
}

static int reserved_slug(const char *s)
{
	if (!strcmp(s, "con") || !strcmp(s, "prn") || !strcmp(s, "aux") ||
	    !strcmp(s, "nul"))
		return 1;
	if (strlen(s) == 4 &&
	    ((!strncmp(s, "com", 3) || !strncmp(s, "lpt", 3)) &&
	     s[3] >= '1' && s[3] <= '9'))
		return 1;
	return 0;
}

static char *slugify(const char *name)
{
	struct sbuf out = {0};
	const unsigned char *p = (const unsigned char *)name;
	int last_dash = 0;
	char *slug;

	while (*p) {
		unsigned int cp;
		size_t len = utf8_decode(p, &cp);
		const char *map = NULL;
		char one[2] = {0, 0};

		if (cp < 0x80) {
			if (isalnum((int)cp)) {
				one[0] = (char)tolower((int)cp);
				map = one;
			} else {
				map = "-";
			}
		} else {
			map = latin_slug(cp);
			if (!map)
				map = "-";
		}

		if (map) {
			const char *q = map;

			while (*q) {
				if (*q == '-') {
					if (out.len && !last_dash && out.len < SLUG_MAX_BYTES)
						sb_add(&out, "-", 1);
					last_dash = 1;
				} else {
					if (out.len < SLUG_MAX_BYTES) {
						sb_add(&out, q, 1);
						last_dash = 0;
					}
				}
				q++;
			}
		}
		p += len;
	}
	if (!out.s)
		return xstrdup("link");
	while (out.len && out.s[out.len - 1] == '-') {
		out.len--;
		out.s[out.len] = '\0';
	}
	if (!out.len) {
		free(out.s);
		return xstrdup("link");
	}
	if (reserved_slug(out.s)) {
		slug = xasprintf("link-%s", out.s);
		free(out.s);
		return slug;
	}
	return out.s;
}

/* ---------- HTML entities and title extraction ---------- */

static char *decode_entities(const char *s)
{
	struct sbuf out = {0};
	const char *p = s;

	while (*p) {
		if (*p == '&') {
			const char *semi = strchr(p, ';');
			size_t n = semi ? (size_t)(semi - p - 1) : 0;

			if (semi && n > 0 && n <= 10) {
				const char *name = p + 1;
				unsigned int cp = 0;
				int ok = 0;

				if (name[0] == '#') {
					char *end = NULL;
					unsigned long v;

					errno = 0;
					if (n > 1 && (name[1] == 'x' || name[1] == 'X'))
						v = strtoul(name + 2, &end, 16);
					else
						v = strtoul(name + 1, &end, 10);
					if (!errno && end == semi && v > 0 && v <= 0x10ffff &&
					    !(v >= 0xd800 && v <= 0xdfff)) {
						cp = (unsigned int)v;
						ok = 1;
					}
				} else if (n == 3 && !strncasecmp(name, "amp", 3)) {
					cp = '&'; ok = 1;
				} else if (n == 2 && !strncasecmp(name, "lt", 2)) {
					cp = '<'; ok = 1;
				} else if (n == 2 && !strncasecmp(name, "gt", 2)) {
					cp = '>'; ok = 1;
				} else if (n == 4 && !strncasecmp(name, "quot", 4)) {
					cp = '"'; ok = 1;
				} else if (n == 4 && !strncasecmp(name, "apos", 4)) {
					cp = '\''; ok = 1;
				} else if (n == 4 && !strncasecmp(name, "nbsp", 4)) {
					cp = 0xa0; ok = 1;
				}
				if (ok) {
					sb_add_cp(&out, cp);
					p = semi + 1;
					continue;
				}
			}
		}
		sb_add(&out, p, 1);
		p++;
	}
	if (!out.s)
		return xstrdup("");
	return out.s;
}

static char *strip_tags(const char *s)
{
	struct sbuf out = {0};
	const char *p = s;

	while (*p) {
		if (*p == '<') {
			const char *e = strchr(p, '>');

			if (!e)
				break;
			p = e + 1;
		} else {
			sb_add(&out, p, 1);
			p++;
		}
	}
	if (!out.s)
		return xstrdup("");
	return out.s;
}

static const char *ci_find(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	const char *p;

	if (!n)
		return hay;
	for (p = hay; *p; p++) {
		size_t i;

		if (tolower((unsigned char)*p) != tolower((unsigned char)needle[0]))
			continue;
		for (i = 1; i < n; i++) {
			if (!p[i] ||
			    tolower((unsigned char)p[i]) !=
			    tolower((unsigned char)needle[i]))
				break;
		}
		if (i == n)
			return p;
	}
	return NULL;
}

static void set_attr(char **slot, char *value)
{
	free(*slot);
	*slot = value;
}

static void parse_meta_attrs(const char *s, char **property, char **name, char **content)
{
	while (*s) {
		const char *ks;
		char *key, *value = NULL;

		while (*s && isspace((unsigned char)*s))
			s++;
		if (!*s)
			break;
		ks = s;
		while (*s && !isspace((unsigned char)*s) && *s != '=')
			s++;
		key = xstrndup(ks, (size_t)(s - ks));
		while (*s && isspace((unsigned char)*s))
			s++;
		if (*s == '=') {
			s++;
			while (*s && isspace((unsigned char)*s))
				s++;
			if (*s == '"' || *s == '\'') {
				char quote = *s++;
				const char *vs = s;

				while (*s && *s != quote)
					s++;
				value = xstrndup(vs, (size_t)(s - vs));
				if (*s)
					s++;
			} else {
				const char *vs = s;

				while (*s && !isspace((unsigned char)*s) && *s != '>')
					s++;
				value = xstrndup(vs, (size_t)(s - vs));
			}
		}
		if (!strcasecmp(key, "property"))
			set_attr(property, value), value = NULL;
		else if (!strcasecmp(key, "name"))
			set_attr(name, value), value = NULL;
		else if (!strcasecmp(key, "content"))
			set_attr(content, value), value = NULL;
		free(key);
		free(value);
	}
}

static char *extract_og_title(const char *html)
{
	const char *p = html;

	while ((p = ci_find(p, "<meta")) != NULL) {
		const char *end;
		char *property = NULL, *name = NULL, *content = NULL;
		char *inner, *title = NULL;

		if (p[5] && p[5] != '>' && !isspace((unsigned char)p[5])) {
			p += 5;
			continue;
		}
		end = strchr(p, '>');
		if (!end)
			break;
		inner = xstrndup(p + 5, (size_t)(end - p - 5));
		parse_meta_attrs(inner, &property, &name, &content);
		free(inner);
		if ((property && !strcasecmp(property, "og:title")) ||
		    (name && !strcasecmp(name, "og:title"))) {
			if (content && *content) {
				char *decoded = decode_entities(content);
				char *plain = strip_tags(decoded);
				char *trimmed = trim_dup(plain);

				free(decoded);
				free(plain);
				if (*trimmed)
					title = trimmed;
				else
					free(trimmed);
			}
		}
		free(property);
		free(name);
		free(content);
		if (title)
			return title;
		p = end + 1;
	}
	return NULL;
}

static char *extract_title(const char *html)
{
	const char *p = html;

	while ((p = ci_find(p, "<title")) != NULL) {
		const char *after = p + 6;
		const char *gt, *end;
		char *raw, *decoded, *plain, *title;

		if (*after && *after != '>' && !isspace((unsigned char)*after)) {
			p = after;
			continue;
		}
		gt = strchr(after, '>');
		if (!gt)
			break;
		end = ci_find(gt + 1, "</title");
		if (!end) {
			p = gt + 1;
			continue;
		}
		raw = xstrndup(gt + 1, (size_t)(end - gt - 1));
		decoded = decode_entities(raw);
		plain = strip_tags(decoded);
		title = trim_dup(plain);
		free(raw);
		free(decoded);
		free(plain);
		if (*title)
			return title;
		free(title);
		p = end + 8;
	}
	return extract_og_title(html);
}

/* ---------- HTTP ---------- */

static size_t fetch_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct fetchbuf *b = userdata;
	size_t n = size * nmemb;

	if (n > (size_t)HTTP_MAX_BYTES || b->len > (size_t)HTTP_MAX_BYTES - n) {
		b->overflow = 1;
		return 0;
	}
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 8192;

		while (cap < b->len + n + 1)
			cap *= 2;
		b->data = xrealloc(b->data, cap);
		b->cap = cap;
	}
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	b->data[b->len] = '\0';
	return n;
}

/* Returns 1 and sets *title on success, 0 to use the host fallback. */
static int fetch_page_title(const char *url, char **title)
{
	CURL *curl;
	CURLcode rc;
	struct fetchbuf body = {0};
	char errbuf[CURL_ERROR_SIZE] = {0};
	long status = 0;
	char *content_type = NULL;

	*title = NULL;
	curl = curl_easy_init();
	if (!curl)
		return 0;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, HTTP_CONNECT_TIMEOUT);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, HTTP_TOTAL_TIMEOUT);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, HTTP_MAX_REDIRECTS);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "linkbrarian/" VERSION);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
			 (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
			 (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

	rc = curl_easy_perform(curl);
	if (rc == CURLE_OK) {
		char *reported_type = NULL;

		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &reported_type);
		if (reported_type)
			content_type = xstrdup(reported_type);
	}
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK || body.overflow || status >= 400 ||
	    (content_type && !ci_find(content_type, "html"))) {
		free(content_type);
		free(body.data);
		return 0;
	}
	free(content_type);
	if (body.data) {
		char *found = extract_title(body.data);

		free(body.data);
		if (found) {
			*title = found;
			return 1;
		}
	}
	return 0;
}

static int valid_url(const char *url, char **host)
{
	const char *sep, *p;
	CURLU *u;
	char *scheme = NULL, *h = NULL;

	*host = NULL;
	if (!url || !*url)
		return 0;
	for (p = url; *p; p++) {
		if (isspace((unsigned char)*p) || iscntrl((unsigned char)*p))
			return 0;
	}
	sep = strstr(url, "://");
	if (!sep || sep == url || !isalpha((unsigned char)url[0]))
		return 0;
	for (p = url; p < sep; p++) {
		if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.')
			return 0;
	}
	u = curl_url();
	if (!u)
		return 0;
	if (curl_url_set(u, CURLUPART_URL, url, 0) != CURLUE_OK) {
		curl_url_cleanup(u);
		return 0;
	}
	if (curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK ||
	    (strcasecmp(scheme, "http") && strcasecmp(scheme, "https")) ||
	    curl_url_get(u, CURLUPART_HOST, &h, 0) != CURLUE_OK || !h || !*h) {
		curl_free(scheme);
		curl_free(h);
		curl_url_cleanup(u);
		return 0;
	}
	*host = xstrdup(h);
	curl_free(scheme);
	curl_free(h);
	curl_url_cleanup(u);
	return 1;
}

/* ---------- SQLite ---------- */

static void db_exec(sqlite3 *db, const char *sql)
{
	char *err = NULL;

	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		char *msg = xstrdup(err ? err : "unknown error");

		sqlite3_free(err);
		die(EXIT_STORAGE, "database error: %s", msg);
	}
}

static sqlite3 *db_open(const char *path)
{
	sqlite3 *db;
	int rc = sqlite3_open_v2(path, &db,
				 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);

	if (rc != SQLITE_OK) {
		const char *msg = db ? sqlite3_errmsg(db) : "cannot open database";

		fprintf(stderr, "linkbrarian: cannot open %s: %s\n", path, msg);
		sqlite3_close(db);
		exit(EXIT_STORAGE);
	}
	sqlite3_busy_timeout(db, 5000);
	return db;
}

static sqlite3_stmt *db_prepare(sqlite3 *db, const char *sql)
{
	sqlite3_stmt *st = NULL;

	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
		die(EXIT_STORAGE, "database error: %s", sqlite3_errmsg(db));
	return st;
}

static int db_user_version(sqlite3 *db)
{
	sqlite3_stmt *st = db_prepare(db, "PRAGMA user_version");
	int version = 0;

	if (sqlite3_step(st) == SQLITE_ROW)
		version = sqlite3_column_int(st, 0);
	else
		die(EXIT_STORAGE, "database error: %s", sqlite3_errmsg(db));
	sqlite3_finalize(st);
	return version;
}

static void db_init(sqlite3 *db)
{
	int version = db_user_version(db);

	if (version > SCHEMA_VERSION)
		die(EXIT_STORAGE, "database schema %d is newer than supported schema %d",
		    version, SCHEMA_VERSION);
	if (version == 0) {
		/*
		 * BEGIN IMMEDIATE serialises schema creation. The version is read
		 * again after taking the write lock, because another process may
		 * have initialised the database while this one waited.
		 */
		db_exec(db, "BEGIN IMMEDIATE");
		version = db_user_version(db);
		if (version > SCHEMA_VERSION) {
			db_exec(db, "ROLLBACK");
			die(EXIT_STORAGE,
			    "database schema %d is newer than supported schema %d",
			    version, SCHEMA_VERSION);
		}
		if (version == 0) {
			db_exec(db,
				"CREATE TABLE IF NOT EXISTS links ("
				" id INTEGER PRIMARY KEY,"
				" page_name TEXT NOT NULL,"
				" page_name_key TEXT NOT NULL,"
				" link TEXT NOT NULL UNIQUE,"
				" tags TEXT NOT NULL DEFAULT '',"
				" notes_path TEXT,"
				" n INTEGER NOT NULL CHECK (n >= 1),"
				" created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
				" UNIQUE (page_name_key, n))");
			db_exec(db,
				"CREATE INDEX IF NOT EXISTS idx_links_page_name"
				" ON links (page_name_key)");
			db_exec(db, "PRAGMA user_version = 1");
		}
		db_exec(db, "COMMIT");
	}
}

static char *column_text_dup(sqlite3_stmt *st, int col)
{
	const unsigned char *p = sqlite3_column_text(st, col);

	return p ? xstrdup((const char *)p) : NULL;
}

static int db_find_link(sqlite3 *db, const char *url, struct link_row *row)
{
	sqlite3_stmt *st;
	int rc, found = 0;

	memset(row, 0, sizeof(*row));
	st = db_prepare(db,
			"SELECT id, page_name, link, notes_path, n"
			" FROM links WHERE link = ?1");
	sqlite3_bind_text(st, 1, url, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(st);
	if (rc == SQLITE_ROW) {
		row->id = (long long)sqlite3_column_int64(st, 0);
		row->page_name = column_text_dup(st, 1);
		row->link = column_text_dup(st, 2);
		row->notes_path = column_text_dup(st, 3);
		row->n = (long long)sqlite3_column_int64(st, 4);
		found = 1;
	} else if (rc != SQLITE_DONE) {
		sqlite3_finalize(st);
		die(EXIT_STORAGE, "database error: %s", sqlite3_errmsg(db));
	}
	sqlite3_finalize(st);
	return found;
}

static int db_find_id(sqlite3 *db, long long id, struct link_row *row)
{
	sqlite3_stmt *st;
	int rc, found = 0;

	memset(row, 0, sizeof(*row));
	st = db_prepare(db,
			"SELECT id, page_name, link, notes_path, n"
			" FROM links WHERE id = ?1");
	sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
	rc = sqlite3_step(st);
	if (rc == SQLITE_ROW) {
		row->id = (long long)sqlite3_column_int64(st, 0);
		row->page_name = column_text_dup(st, 1);
		row->link = column_text_dup(st, 2);
		row->notes_path = column_text_dup(st, 3);
		row->n = (long long)sqlite3_column_int64(st, 4);
		found = 1;
	} else if (rc != SQLITE_DONE) {
		sqlite3_finalize(st);
		die(EXIT_STORAGE, "database error: %s", sqlite3_errmsg(db));
	}
	sqlite3_finalize(st);
	return found;
}

/* 0: inserted, 1: link was inserted concurrently, exits on other errors. */
static int db_insert_link(sqlite3 *db, const char *page_name,
			  const char *page_name_key, const char *url,
			  long long *id, long long *n)
{
	sqlite3_stmt *st;
	int rc;

	db_exec(db, "BEGIN IMMEDIATE");
	st = db_prepare(db,
			"SELECT COALESCE(MAX(n), 0) + 1 FROM links"
			" WHERE page_name_key = ?1");
	sqlite3_bind_text(st, 1, page_name_key, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(st);
	if (rc != SQLITE_ROW) {
		sqlite3_finalize(st);
		db_exec(db, "ROLLBACK");
		die(EXIT_STORAGE, "database error: %s", sqlite3_errmsg(db));
	}
	*n = (long long)sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);

	st = db_prepare(db,
			"INSERT INTO links"
			" (page_name, page_name_key, link, tags, notes_path, n)"
			" VALUES (?1, ?2, ?3, '', NULL, ?4)");
	sqlite3_bind_text(st, 1, page_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, page_name_key, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, url, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 4, (sqlite3_int64)*n);
	rc = sqlite3_step(st);
	if (rc != SQLITE_DONE) {
		int code = sqlite3_errcode(db);
		char *msg = xstrdup(sqlite3_errmsg(db));

		sqlite3_finalize(st);
		db_exec(db, "ROLLBACK");
		if (code == SQLITE_CONSTRAINT) {
			free(msg);
			return 1;
		}
		die(EXIT_STORAGE, "cannot insert link: %s", msg);
	}
	sqlite3_finalize(st);
	*id = (long long)sqlite3_last_insert_rowid(db);
	db_exec(db, "COMMIT");
	return 0;
}

/* 1: updated, 0: another process set notes_path first, -1: error. */
static int db_set_notes(sqlite3 *db, long long id, const char *path)
{
	sqlite3_stmt *st;
	int rc, changes;

	st = db_prepare(db,
			"UPDATE links SET notes_path = ?1"
			" WHERE id = ?2 AND notes_path IS NULL");
	sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 2, (sqlite3_int64)id);
	rc = sqlite3_step(st);
	if (rc != SQLITE_DONE) {
		sqlite3_finalize(st);
		return -1;
	}
	changes = sqlite3_changes(db);
	sqlite3_finalize(st);
	return changes;
}

/* ---------- notes ---------- */

static int write_exclusive(const char *path, const char *content)
{
#ifdef _WIN32
	int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
		       _S_IREAD | _S_IWRITE);
#else
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
	size_t len = strlen(content), off = 0;

	if (fd < 0)
		return -1;
	while (off < len) {
#ifdef _WIN32
		int w = _write(fd, content + off, (unsigned int)(len - off));
#else
		ssize_t w = write(fd, content + off, len - off);
#endif
		if (w < 0) {
			int saved = errno;

#ifdef _WIN32
			_close(fd);
#else
			close(fd);
#endif
			errno = saved;
			return -1;
		}
		off += (size_t)w;
	}
#ifdef _WIN32
	if (_close(fd) != 0)
		return -1;
#else
	if (close(fd) != 0)
		return -1;
#endif
	return 0;
}

static void now_string(char *out, size_t size)
{
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);

	if (!tm || !strftime(out, size, "%Y-%m-%d %H:%M:%S", tm))
		snprintf(out, size, "unknown");
}

static void remove_notes_directory(const char *dir)
{
	char *readme = path_join(dir, "README.md");

	remove_file(readme);
	free(readme);
	remove_dir(dir);
}

/* Returns an absolute path, or NULL with *err set. Creates a new directory only. */
static char *create_notes_directory(const char *data_dir, const char *page_name,
				    const char *link, long long n, int *err)
{
	char *slug = slugify(page_name);
	char *result = NULL;
	int attempt;

	*err = 0;
	for (attempt = 0; attempt < NOTES_SUFFIX_MAX_ATTEMPTS; attempt++) {
		char *name, *candidate, *actual, *readme, *content;
		char when[64];
		int saved;

		if (attempt == 0)
			name = xasprintf("%s-%lld-notes", slug, n);
		else
			name = xasprintf("%s-%lld-notes-%d", slug, n, attempt + 1);
		candidate = path_join(data_dir, name);
		free(name);

		if (make_dir(candidate) != 0) {
			saved = errno;
			free(candidate);
			if (saved != EEXIST) {
				*err = saved;
				break;
			}
			continue;
		}

		actual = absolute_path(candidate);
		if (!actual) {
			int saved_path = errno ? errno : EIO;

			/* We only just created this path. */
			remove_dir(candidate);
			free(candidate);
			*err = saved_path;
			break;
		}
		free(candidate);
		now_string(when, sizeof(when));
		content = xasprintf("# %s\n\n"
				    "- **URL:** %s\n"
				    "- **Created:** %s\n"
				    "- **Tags:**\n\n"
				    "## Notes\n\n\n",
				    page_name, link, when);
		readme = path_join(actual, "README.md");
		if (write_exclusive(readme, content) != 0) {
			saved = errno;
			free(content);
			remove_file(readme);
			free(readme);
			remove_dir(actual);
			free(actual);
			*err = saved ? saved : EIO;
			break;
		}
		free(content);
		free(readme);
		result = actual;
		break;
	}
	if (!result && !*err)
		*err = EEXIST;
	free(slug);
	return result;
}

/* ---------- CLI ---------- */

static void usage(FILE *out)
{
	fputs("usage: linkbrarian [-hv] URL [note]\n"
	      "       lbr [-hv] URL [note]\n", out);
}

static void help(void)
{
	usage(stdout);
	fputs("\n"
	      "Register a URL in ~/.linkbrarian/linkography.db.\n"
	      "With \"note\", also create a Markdown notes folder.\n\n"
	      "options:\n"
	      "  -h, --help     show this help\n"
	      "  -v, --version  show version\n", stdout);
}

int main(int argc, char **argv)
{
	int i, want_note = 0, is_new = 0, status = 0;
	const char *arg;
	char *url = NULL, *host = NULL, *page_name = NULL, *page_name_key = NULL;
	char *home = NULL, *data_dir = NULL, *db_path = NULL;
	sqlite3 *db = NULL;
	struct link_row row;

	if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		help();
		return 0;
	}
	if (argc > 1 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
		puts(VERSION);
		return 0;
	}

	for (i = 1; i < argc; i++) {
		arg = argv[i];
		if (arg[0] != '-' || arg[1] == '\0')
			break;
		if (!strcmp(arg, "--")) {
			i++;
			break;
		}
		fprintf(stderr, "linkbrarian: unknown option %s\n", arg);
		usage(stderr);
		return EXIT_USAGE;
	}
	if (i >= argc) {
		usage(stderr);
		return EXIT_USAGE;
	}
	url = trim_dup(argv[i++]);
	if (i < argc) {
		if (!strcmp(argv[i], "note"))
			want_note = 1, i++;
		else {
			fprintf(stderr, "linkbrarian: unexpected argument: %s\n", argv[i]);
			usage(stderr);
			free(url);
			return EXIT_USAGE;
		}
	}
	if (i < argc) {
		fprintf(stderr, "linkbrarian: unexpected argument: %s\n", argv[i]);
		usage(stderr);
		free(url);
		return EXIT_USAGE;
	}

	memset(&row, 0, sizeof(row));
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
		die(EXIT_STORAGE, "cannot initialize HTTP support");
	if (!valid_url(url, &host))
		die(EXIT_BAD_URL, "invalid URL (expected absolute http or https): %s",
		    url);
	if (!fetch_page_title(url, &page_name))
		page_name = xstrdup(host);
	page_name_key = normalize_key(page_name);

	home = home_directory();
	data_dir = path_join(home, ".linkbrarian");
	ensure_data_directory(data_dir);
	db_path = path_join(data_dir, "linkography.db");
	db = db_open(db_path);
	db_init(db);

	if (db_find_link(db, url, &row)) {
		is_new = 0;
	} else {
		long long id = 0, n = 0;
		int rc = db_insert_link(db, page_name, page_name_key, url, &id, &n);

		if (rc == 1) {
			if (!db_find_link(db, url, &row))
				die(EXIT_STORAGE, "link disappeared after insert conflict");
			is_new = 0;
		} else {
			row.id = id;
			row.page_name = xstrdup(page_name);
			row.link = xstrdup(url);
			row.notes_path = NULL;
			row.n = n;
			is_new = 1;
		}
	}

	if (is_new) {
		printf("[+] Link saved: %s\n", row.page_name);
		printf("    URL: %s\n", row.link);
	} else {
		printf("[=] Link already registered: %s\n", row.page_name);
		printf("    URL: %s\n", row.link);
	}

	if (want_note) {
		if (row.notes_path) {
			printf("[=] Notes already exist: %s\n", row.notes_path);
		} else {
			int err = 0;
			char *dir = create_notes_directory(data_dir, row.page_name,
							   row.link, row.n, &err);

			if (!dir) {
				fprintf(stderr,
					"linkbrarian: cannot create notes for \"%s\" in %s: %s\n",
					row.page_name, data_dir,
					strerror(err ? err : EIO));
				status = EXIT_NOTES;
				goto out;
			}
			{
				int changed = db_set_notes(db, row.id, dir);

				if (changed == 1) {
					printf("[+] Notes created: %s\n", dir);
				} else if (changed == 0) {
					struct link_row other;

					remove_notes_directory(dir);
					if (db_find_id(db, row.id, &other) &&
					    other.notes_path) {
						printf("[=] Notes already exist: %s\n",
						       other.notes_path);
					} else {
						fprintf(stderr,
							"linkbrarian: cannot record notes path for \"%s\"\n",
							row.page_name);
						status = EXIT_STORAGE;
					}
					free(other.page_name);
					free(other.link);
					free(other.notes_path);
				} else {
					fprintf(stderr,
						"linkbrarian: cannot record notes path %s: %s\n",
						dir, sqlite3_errmsg(db));
					remove_notes_directory(dir);
					status = EXIT_STORAGE;
				}
			}
			free(dir);
		}
	}

out:
	sqlite3_close(db);
	curl_global_cleanup();
	free(row.page_name);
	free(row.link);
	free(row.notes_path);
	free(home);
	free(data_dir);
	free(db_path);
	free(page_name);
	free(page_name_key);
	free(host);
	free(url);
	return status;
}
