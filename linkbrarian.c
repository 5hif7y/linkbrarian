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
#include <utf8proc.h>

#include "config.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wchar.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define PATHSEP '\\'
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

#ifdef _WIN32
static wchar_t *utf8_to_wide(const char *s)
{
	int n;
	wchar_t *w;

	n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
	if (n <= 0) {
		errno = EILSEQ;
		return NULL;
	}
	w = xmalloc((size_t)n * sizeof(*w));
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0) {
		free(w);
		errno = EILSEQ;
		return NULL;
	}
	return w;
}

static char *wide_to_utf8(const wchar_t *w)
{
	int n;
	char *s;

	n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (n <= 0) {
		errno = EILSEQ;
		return NULL;
	}
	s = xmalloc((size_t)n);
	if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
		free(s);
		errno = EILSEQ;
		return NULL;
	}
	return s;
}

static char *win_env_utf8(const wchar_t *name)
{
	DWORD n = GetEnvironmentVariableW(name, NULL, 0);
	wchar_t *w;
	char *s;

	if (n == 0)
		return NULL;
	w = xmalloc((size_t)n * sizeof(*w));
	if (GetEnvironmentVariableW(name, w, n) == 0) {
		free(w);
		return NULL;
	}
	s = wide_to_utf8(w);
	free(w);
	return s;
}
#endif

static int make_dir(const char *p)
{
#ifdef _WIN32
	wchar_t *w = utf8_to_wide(p);
	int rc;

	if (!w)
		return -1;
	rc = _wmkdir(w);
	free(w);
	return rc;
#else
	return mkdir(p, 0700);
#endif
}

static int remove_dir(const char *p)
{
#ifdef _WIN32
	wchar_t *w = utf8_to_wide(p);
	int rc;

	if (!w)
		return -1;
	rc = _wrmdir(w);
	free(w);
	return rc;
#else
	return rmdir(p);
#endif
}

static int remove_file(const char *p)
{
#ifdef _WIN32
	wchar_t *w = utf8_to_wide(p);
	int rc;

	if (!w)
		return -1;
	rc = _wunlink(w);
	free(w);
	return rc;
#else
	return unlink(p);
#endif
}

static int path_is_dir(const char *p)
{
#ifdef _WIN32
	wchar_t *w = utf8_to_wide(p);
	DWORD attrs;

	if (!w)
		return 0;
	attrs = GetFileAttributesW(w);
	free(w);
	return attrs != INVALID_FILE_ATTRIBUTES &&
	       (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
	struct stat st;

	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static char *absolute_path(const char *p)
{
#ifdef _WIN32
	wchar_t *w = utf8_to_wide(p);
	wchar_t *full;
	char *r;

	if (!w)
		return NULL;
	full = _wfullpath(NULL, w, 0);
	free(w);
	if (!full)
		return NULL;
	r = wide_to_utf8(full);
	free(full);
	return r;
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
#ifdef _WIN32
	char *home = win_env_utf8(L"USERPROFILE");

	if (!home || !*home) {
		free(home);
		home = win_env_utf8(L"HOME");
	}
	if (!home || !*home)
		die(EXIT_STORAGE, "cannot determine the user home directory");
	return home;
#else
	const char *home = getenv("HOME");

	if (!home || !*home)
		die(EXIT_STORAGE, "cannot determine the user home directory");
	return xstrdup(home);
#endif
}

static void ensure_data_directory(const char *path)
{
	if (make_dir(path) == 0)
		return;
	if (errno == EEXIST && path_is_dir(path))
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
	utf8proc_uint8_t *folded =
		utf8proc_NFKC_Casefold((const utf8proc_uint8_t *)s);
	const unsigned char *p = folded ? (const unsigned char *)folded :
					  (const unsigned char *)s;
	struct sbuf out = {0};
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
			cp = (unsigned int)utf8proc_tolower((utf8proc_int32_t)cp);
			n = utf8_encode(cp, tmp);
			sb_add(&out, tmp, (size_t)n);
		}
		p += len;
	}
	free(folded);
	if (!out.s)
		return xstrdup("");
	return out.s;
}

static int slug_allowed(utf8proc_int32_t cp)
{
	switch (utf8proc_category(cp)) {
	case UTF8PROC_CATEGORY_LU:
	case UTF8PROC_CATEGORY_LL:
	case UTF8PROC_CATEGORY_LT:
	case UTF8PROC_CATEGORY_LM:
	case UTF8PROC_CATEGORY_LO:
	case UTF8PROC_CATEGORY_ND:
	case UTF8PROC_CATEGORY_NL:
	case UTF8PROC_CATEGORY_NO:
	case UTF8PROC_CATEGORY_MN:
	case UTF8PROC_CATEGORY_MC:
	case UTF8PROC_CATEGORY_ME:
		return 1;
	default:
		return 0;
	}
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
	utf8proc_uint8_t *folded =
		utf8proc_NFKC_Casefold((const utf8proc_uint8_t *)name);
	const unsigned char *p = folded ? (const unsigned char *)folded :
					  (const unsigned char *)name;
	struct sbuf out = {0};
	int last_dash = 0;
	char *slug;

	while (*p) {
		unsigned int cp;
		size_t len = utf8_decode(p, &cp);

		if (slug_allowed((utf8proc_int32_t)cp)) {
			char tmp[4];
			int n;

			cp = (unsigned int)utf8proc_tolower((utf8proc_int32_t)cp);
			n = utf8_encode(cp, tmp);
			if (out.len + (size_t)n > SLUG_MAX_BYTES)
				break;
			sb_add(&out, tmp, (size_t)n);
			last_dash = 0;
		} else if (out.len && !last_dash && out.len < SLUG_MAX_BYTES) {
			sb_add(&out, "-", 1);
			last_dash = 1;
		}
		p += len;
	}
	free(folded);
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

/* ---------- URL identity ---------- */

static int hex_digit(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static char upper_hex(int v)
{
	return (char)(v < 10 ? '0' + v : 'A' + v - 10);
}

static int url_unreserved(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
	       c == '~';
}

static void lowercase_ascii(char *s)
{
	while (*s) {
		if (*s >= 'A' && *s <= 'Z')
			*s += 32;
		s++;
	}
}

static char *normalize_path_escapes(const char *path)
{
	struct sbuf out = {0};
	size_t i = 0;

	while (path[i]) {
		if (path[i] == '%') {
			int hi = hex_digit((unsigned char)path[i + 1]);
			int lo = hi >= 0 ? hex_digit((unsigned char)path[i + 2]) : -1;

			if (hi >= 0 && lo >= 0) {
				unsigned char c = (unsigned char)((hi << 4) | lo);

				if (url_unreserved(c)) {
					sb_add(&out, (const char *)&c, 1);
				} else {
					char esc[3] = { '%', upper_hex(hi), upper_hex(lo) };

					sb_add(&out, esc, 3);
				}
				i += 3;
				continue;
			}
		}
		sb_add(&out, path + i, 1);
		i++;
	}
	return out.s ? out.s : xstrdup("");
}

static void drop_last_segment(struct sbuf *out)
{
	while (out->len && out->s[out->len - 1] != '/')
		out->len--;
	if (out->len)
		out->len--;
	if (out->s)
		out->s[out->len] = '\0';
}

static char *remove_dot_segments(const char *path)
{
	struct sbuf out = {0};
	const char *in = path;

	while (*in) {
		if (!strncmp(in, "../", 3)) {
			in += 3;
		} else if (!strncmp(in, "./", 2)) {
			in += 2;
		} else if (!strncmp(in, "/./", 3)) {
			in += 2;
		} else if (!strcmp(in, "/.")) {
			in += 2;
			sb_add(&out, "/", 1);
		} else if (!strncmp(in, "/../", 4)) {
			in += 3;
			drop_last_segment(&out);
		} else if (!strcmp(in, "/..")) {
			in += 3;
			drop_last_segment(&out);
			sb_add(&out, "/", 1);
		} else if (!strcmp(in, ".") || !strcmp(in, "..")) {
			in += strlen(in);
		} else {
			const char *start = in;

			if (*in == '/')
				in++;
			while (*in && *in != '/')
				in++;
			sb_add(&out, start, (size_t)(in - start));
		}
	}
	return out.s ? out.s : xstrdup("");
}

/*
 * Conservative RFC 3986-style identity key:
 * - lower-case scheme and host
 * - remove the default port
 * - upper-case percent escapes, decode only unreserved characters
 * - remove dot segments from the path
 * Query, fragment, path case and trailing slashes are preserved.
 */
static char *canonical_url(const char *url)
{
	const char *sep = strstr(url, "://");
	const char *authority, *auth_end, *p, *last_at = NULL;
	const char *host_start, *host_end, *port = NULL;
	const char *path_end, *frag, *query_start = NULL, *query_end = NULL;
	size_t port_len = 0;
	char *scheme, *host, *raw_path, *escaped_path, *path;
	struct sbuf out = {0};
	int drop_port = 0;

	if (!sep)
		return xstrdup(url);
	authority = sep + 3;
	auth_end = authority;
	while (*auth_end && *auth_end != '/' && *auth_end != '?' &&
	       *auth_end != '#')
		auth_end++;
	for (p = authority; p < auth_end; p++) {
		if (*p == '@')
			last_at = p;
	}
	host_start = last_at ? last_at + 1 : authority;
	host_end = auth_end;
	if (host_start < auth_end && *host_start == '[') {
		const char *close = memchr(host_start, ']',
					   (size_t)(auth_end - host_start));

		if (close) {
			host_end = close + 1;
			if (host_end < auth_end && *host_end == ':') {
				port = host_end + 1;
				port_len = (size_t)(auth_end - host_end - 1);
			}
		}
	} else {
		for (p = host_start; p < auth_end; p++) {
			if (*p == ':') {
				host_end = p;
				port = p + 1;
				port_len = (size_t)(auth_end - p - 1);
			}
		}
	}

	scheme = xstrndup(url, (size_t)(sep - url));
	lowercase_ascii(scheme);
	host = xstrndup(host_start, (size_t)(host_end - host_start));
	lowercase_ascii(host);

	if (port) {
		if ((!strcmp(scheme, "http") && port_len == 2 &&
		     !memcmp(port, "80", 2)) ||
		    (!strcmp(scheme, "https") && port_len == 3 &&
		     !memcmp(port, "443", 3)))
			drop_port = 1;
	}

	path_end = auth_end;
	while (*path_end && *path_end != '?' && *path_end != '#')
		path_end++;
	frag = strchr(path_end, '#');
	if (*path_end == '?') {
		query_start = path_end;
		query_end = frag ? frag : path_end + strlen(path_end);
	}

	raw_path = xstrndup(auth_end, (size_t)(path_end - auth_end));
	escaped_path = normalize_path_escapes(raw_path);
	path = remove_dot_segments(escaped_path);

	sb_add(&out, scheme, strlen(scheme));
	sb_add(&out, "://", 3);
	if (last_at)
		sb_add(&out, authority, (size_t)(host_start - authority));
	sb_add(&out, host, strlen(host));
	if (port && !drop_port) {
		sb_add(&out, ":", 1);
		sb_add(&out, port, port_len);
	}
	sb_add(&out, path, strlen(path));
	if (query_start)
		sb_add(&out, query_start, (size_t)(query_end - query_start));
	if (frag)
		sb_add(&out, frag, strlen(frag));

	free(scheme);
	free(host);
	free(raw_path);
	free(escaped_path);
	free(path);
	return out.s ? out.s : xstrdup("");
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

static const char links_columns[] =
	" ("
	" id INTEGER PRIMARY KEY,"
	" page_name TEXT NOT NULL,"
	" page_name_key TEXT NOT NULL,"
	" link TEXT NOT NULL UNIQUE,"
	" tags TEXT NOT NULL DEFAULT '',"
	" notes_path TEXT,"
	" n INTEGER NOT NULL CHECK (n >= 1),"
	" created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
	" UNIQUE (page_name_key, n))";

static void db_create_links_table(sqlite3 *db, const char *table, int if_not_exists)
{
	char *sql = xasprintf("CREATE TABLE %s%s%s",
			      if_not_exists ? "IF NOT EXISTS " : "",
			      table, links_columns);

	db_exec(db, sql);
	free(sql);
}

static void bind_text_or_null(sqlite3_stmt *dst, int dst_col,
			      sqlite3_stmt *src, int src_col)
{
	if (sqlite3_column_type(src, src_col) == SQLITE_NULL)
		sqlite3_bind_null(dst, dst_col);
	else
		sqlite3_bind_text(dst, dst_col,
				  (const char *)sqlite3_column_text(src, src_col),
				  -1, SQLITE_TRANSIENT);
}

static void db_migrate_v1_to_v2(sqlite3 *db)
{
	sqlite3_stmt *select, *insert, *next;
	int rc;

	db_create_links_table(db, "links_v2", 0);
	select = db_prepare(db,
			    "SELECT id, page_name, link, tags, notes_path, n,"
			    " created_at FROM links ORDER BY id");
	insert = db_prepare(db,
			    "INSERT INTO links_v2"
			    " (id, page_name, page_name_key, link, tags,"
			    "  notes_path, n, created_at)"
			    " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
	next = db_prepare(db,
			  "SELECT COALESCE(MAX(n), 0) + 1 FROM links_v2"
			  " WHERE page_name_key = ?1");

	while ((rc = sqlite3_step(select)) == SQLITE_ROW) {
		const char *page_name =
			(const char *)sqlite3_column_text(select, 1);
		char *key = normalize_key(page_name ? page_name : "");
		long long n;

		sqlite3_reset(next);
		sqlite3_clear_bindings(next);
		sqlite3_bind_text(next, 1, key, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(next) != SQLITE_ROW) {
			free(key);
			sqlite3_finalize(select);
			sqlite3_finalize(insert);
			sqlite3_finalize(next);
			die(EXIT_STORAGE, "database migration failed: %s",
			    sqlite3_errmsg(db));
		}
		n = (long long)sqlite3_column_int64(next, 0);

		sqlite3_reset(insert);
		sqlite3_clear_bindings(insert);
		sqlite3_bind_int64(insert, 1, sqlite3_column_int64(select, 0));
		sqlite3_bind_text(insert, 2, page_name ? page_name : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(insert, 3, key, -1, SQLITE_TRANSIENT);
		bind_text_or_null(insert, 4, select, 2);
		bind_text_or_null(insert, 5, select, 3);
		bind_text_or_null(insert, 6, select, 4);
		sqlite3_bind_int64(insert, 7, (sqlite3_int64)n);
		bind_text_or_null(insert, 8, select, 6);
		free(key);

		if (sqlite3_step(insert) != SQLITE_DONE) {
			sqlite3_finalize(select);
			sqlite3_finalize(insert);
			sqlite3_finalize(next);
			die(EXIT_STORAGE, "database migration failed: %s",
			    sqlite3_errmsg(db));
		}
	}
	if (rc != SQLITE_DONE) {
		sqlite3_finalize(select);
		sqlite3_finalize(insert);
		sqlite3_finalize(next);
		die(EXIT_STORAGE, "database migration failed: %s",
		    sqlite3_errmsg(db));
	}
	sqlite3_finalize(select);
	sqlite3_finalize(insert);
	sqlite3_finalize(next);

	db_exec(db, "DROP TABLE links");
	db_exec(db, "ALTER TABLE links_v2 RENAME TO links");
	db_exec(db,
		"CREATE INDEX IF NOT EXISTS idx_links_page_name"
		" ON links (page_name_key)");
	db_exec(db, "PRAGMA user_version = 2");
}

static int db_migrate_v2_to_v3(sqlite3 *db)
{
	sqlite3_stmt *select, *exists, *update;
	int rc, conflicts = 0;

	db_exec(db, "ALTER TABLE links ADD COLUMN link_key TEXT");
	select = db_prepare(db, "SELECT id, link FROM links ORDER BY id");
	exists = db_prepare(db,
			    "SELECT 1 FROM links WHERE link_key = ?1 LIMIT 1");
	update = db_prepare(db,
			    "UPDATE links SET link_key = ?1 WHERE id = ?2");

	while ((rc = sqlite3_step(select)) == SQLITE_ROW) {
		const char *link = (const char *)sqlite3_column_text(select, 1);
		char *key = canonical_url(link ? link : "");

		sqlite3_reset(exists);
		sqlite3_clear_bindings(exists);
		sqlite3_bind_text(exists, 1, key, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(exists);
		if (rc == SQLITE_ROW) {
			/*
			 * Older exact-URL rows may canonicalize to the same key.
			 * Keep both; the oldest keeps the key, later ones stay NULL.
			 */
			conflicts++;
			free(key);
			continue;
		}
		if (rc != SQLITE_DONE) {
			free(key);
			sqlite3_finalize(select);
			sqlite3_finalize(exists);
			sqlite3_finalize(update);
			die(EXIT_STORAGE, "database migration failed: %s",
			    sqlite3_errmsg(db));
		}
		sqlite3_reset(update);
		sqlite3_clear_bindings(update);
		sqlite3_bind_text(update, 1, key, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(update, 2, sqlite3_column_int64(select, 0));
		free(key);
		if (sqlite3_step(update) != SQLITE_DONE) {
			sqlite3_finalize(select);
			sqlite3_finalize(exists);
			sqlite3_finalize(update);
			die(EXIT_STORAGE, "database migration failed: %s",
			    sqlite3_errmsg(db));
		}
	}
	if (rc != SQLITE_DONE) {
		sqlite3_finalize(select);
		sqlite3_finalize(exists);
		sqlite3_finalize(update);
		die(EXIT_STORAGE, "database migration failed: %s",
		    sqlite3_errmsg(db));
	}
	sqlite3_finalize(select);
	sqlite3_finalize(exists);
	sqlite3_finalize(update);

	db_exec(db,
		"CREATE UNIQUE INDEX IF NOT EXISTS idx_links_link_key"
		" ON links (link_key) WHERE link_key IS NOT NULL");
	db_exec(db, "PRAGMA user_version = 3");
	return conflicts;
}

static void db_init(sqlite3 *db)
{
	int version = db_user_version(db);
	int conflicts = 0;

	if (version > SCHEMA_VERSION)
		die(EXIT_STORAGE, "database schema %d is newer than supported schema %d",
		    version, SCHEMA_VERSION);
	if (version == 0 || version == 1 || version == 2) {
		/*
		 * BEGIN IMMEDIATE serialises schema creation and migration. The
		 * version is read again after taking the write lock, because another
		 * process may have initialised the database while this one waited.
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
			db_create_links_table(db, "links", 1);
			db_exec(db,
				"CREATE INDEX IF NOT EXISTS idx_links_page_name"
				" ON links (page_name_key)");
			conflicts = db_migrate_v2_to_v3(db);
		} else {
			if (version == 1)
				db_migrate_v1_to_v2(db);
			if (version <= 2)
				conflicts = db_migrate_v2_to_v3(db);
		}
		db_exec(db, "COMMIT");
	}
	if (conflicts)
		fprintf(stderr,
			"linkbrarian: warning: %d existing links share a canonical URL; kept with link_key=NULL\n",
			conflicts);
}

static char *column_text_dup(sqlite3_stmt *st, int col)
{
	const unsigned char *p = sqlite3_column_text(st, col);

	return p ? xstrdup((const char *)p) : NULL;
}

static int db_find_link(sqlite3 *db, const char *url, const char *url_key,
			struct link_row *row)
{
	sqlite3_stmt *st;
	int rc, found = 0;

	memset(row, 0, sizeof(*row));
	st = db_prepare(db,
			"SELECT id, page_name, link, notes_path, n"
			" FROM links WHERE link_key = ?1 OR link = ?2"
			" ORDER BY (link_key = ?1) DESC, id LIMIT 1");
	sqlite3_bind_text(st, 1, url_key, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, url, -1, SQLITE_TRANSIENT);
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
			  const char *url_key, long long *id, long long *n)
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
			" (page_name, page_name_key, link, link_key, tags,"
			"  notes_path, n)"
			" VALUES (?1, ?2, ?3, ?4, '', NULL, ?5)");
	sqlite3_bind_text(st, 1, page_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, page_name_key, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, url, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, url_key, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 5, (sqlite3_int64)*n);
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
	wchar_t *wpath = utf8_to_wide(path);
	int fd;

	if (!wpath)
		return -1;
	fd = _wopen(wpath, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
		    _S_IREAD | _S_IWRITE);
	free(wpath);
#else
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
	size_t len = strlen(content), off = 0;

	if (fd < 0)
		return -1;
	while (off < len) {
#ifdef _WIN32
		int written = _write(fd, content + off,
				     (unsigned int)(len - off));
#else
		ssize_t written = write(fd, content + off, len - off);
#endif
		if (written < 0) {
			int saved = errno;

#ifdef _WIN32
			_close(fd);
#else
			close(fd);
#endif
			errno = saved;
			return -1;
		}
		off += (size_t)written;
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

/* ---------- commands ---------- */

struct catalog {
	char *home;
	char *data_dir;
	char *db_path;
	sqlite3 *db;
};

static void catalog_open(struct catalog *c)
{
	memset(c, 0, sizeof(*c));
	c->home = home_directory();
	c->data_dir = path_join(c->home, ".linkbrarian");
	ensure_data_directory(c->data_dir);
	c->db_path = path_join(c->data_dir, "linkography.db");
	c->db = db_open(c->db_path);
	db_init(c->db);
}

static void catalog_close(struct catalog *c)
{
	sqlite3_close(c->db);
	free(c->home);
	free(c->data_dir);
	free(c->db_path);
	memset(c, 0, sizeof(*c));
}

struct strlist {
	char **v;
	size_t n;
	size_t cap;
};

static void strlist_push(struct strlist *l, char *s)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 8;
		l->v = xrealloc(l->v, l->cap * sizeof(*l->v));
	}
	l->v[l->n++] = s;
}

static void strlist_free(struct strlist *l)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		free(l->v[i]);
	free(l->v);
	memset(l, 0, sizeof(*l));
}

static struct strlist split_words(const char *s)
{
	struct strlist l = {0};
	const char *p = s;

	while (*p) {
		const char *start;

		while (*p && isspace((unsigned char)*p))
			p++;
		start = p;
		while (*p && !isspace((unsigned char)*p))
			p++;
		if (p > start)
			strlist_push(&l, xstrndup(start, (size_t)(p - start)));
	}
	return l;
}

static char *join_args(int argc, char **argv)
{
	struct sbuf out = {0};
	int i;

	for (i = 0; i < argc; i++) {
		if (i)
			sb_add(&out, " ", 1);
		sb_add(&out, argv[i], strlen(argv[i]));
	}
	return out.s ? out.s : xstrdup("");
}

static int cmd_search(int argc, char **argv)
{
	struct catalog c;
	struct strlist terms;
	struct sbuf sql = {0};
	sqlite3_stmt *st;
	char *query = join_args(argc, argv);
	unsigned long long count = 0;
	size_t i;
	int rc;

	terms = split_words(query);
	sb_add(&sql, "SELECT id, page_name, link, tags, notes_path, n"
		     " FROM links",
	       strlen("SELECT id, page_name, link, tags, notes_path, n"
		      " FROM links"));
	for (i = 0; i < terms.n; i++) {
		char *frag = xasprintf(
			" %s (instr(page_name_key, ?%d) > 0"
			" OR instr(lower(link), lower(?%d)) > 0"
			" OR instr(lower(tags), lower(?%d)) > 0)",
			i ? "AND" : "WHERE", (int)(2 * i + 1),
			(int)(2 * i + 2), (int)(2 * i + 2));

		sb_add(&sql, frag, strlen(frag));
		free(frag);
	}
	sb_add(&sql, " ORDER BY id", strlen(" ORDER BY id"));

	catalog_open(&c);
	st = db_prepare(c.db, sql.s);
	for (i = 0; i < terms.n; i++) {
		char *key = normalize_key(terms.v[i]);

		sqlite3_bind_text(st, (int)(2 * i + 1), key, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, (int)(2 * i + 2), terms.v[i], -1,
				  SQLITE_TRANSIENT);
		free(key);
	}
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(st, 1);
		const char *link = (const char *)sqlite3_column_text(st, 2);
		const char *tags = (const char *)sqlite3_column_text(st, 3);
		const char *notes = (const char *)sqlite3_column_text(st, 4);

		count++;
		printf("%llu. %s\n", count, name ? name : "");
		printf("   URL: %s\n", link ? link : "");
		printf("   Tags: %s\n", tags && *tags ? tags : "-");
		printf("   Notes: %s\n", notes ? notes : "-");
	}
	if (rc != SQLITE_DONE) {
		char *msg = xstrdup(sqlite3_errmsg(c.db));

		sqlite3_finalize(st);
		catalog_close(&c);
		free(query);
		free(sql.s);
		strlist_free(&terms);
		die(EXIT_STORAGE, "database error: %s", msg);
	}
	sqlite3_finalize(st);
	if (!count)
		puts("No links found.");
	else
		printf("\n%llu link(s)\n", count);

	free(query);
	free(sql.s);
	strlist_free(&terms);
	catalog_close(&c);
	return 0;
}

/* ---------- CLI ---------- */

static void usage(FILE *out)
{
	fputs("usage: linkbrarian [-hv] URL [note]\n"
	      "       linkbrarian search [QUERY]\n"
	      "       lbr [-hv] URL [note]\n"
	      "       lbr search [QUERY]\n", out);
}

static void help(void)
{
	usage(stdout);
	fputs("\n"
	      "Register a URL in ~/.linkbrarian/linkography.db.\n"
	      "With \"note\", also create a Markdown notes folder.\n"
	      "With \"search\", find links by title, URL or tags.\n\n"
	      "options:\n"
	      "  -h, --help     show this help\n"
	      "  -v, --version  show version\n", stdout);
}

int main(int argc, char **argv)
{
	int i, want_note = 0, is_new = 0, status = 0;
	const char *arg;
	char *url = NULL, *host = NULL, *page_name = NULL, *page_name_key = NULL;
	char *url_key = NULL;
	char *home = NULL, *data_dir = NULL, *db_path = NULL;
	sqlite3 *db = NULL;
	struct link_row row;

#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif

	i = 1;
	while (i < argc) {
		arg = argv[i];
		if (arg[0] != '-' || arg[1] == '\0')
			break;
		if (!strcmp(arg, "--")) {
			i++;
			break;
		}
		if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			help();
			return 0;
		}
		if (!strcmp(arg, "-v") || !strcmp(arg, "--version")) {
			puts(VERSION);
			return 0;
		}
		fprintf(stderr, "linkbrarian: unknown option %s\n", arg);
		usage(stderr);
		return EXIT_USAGE;
	}
	if (i >= argc) {
		usage(stderr);
		return EXIT_USAGE;
	}
	if (!strcmp(argv[i], "search"))
		return cmd_search(argc - i - 1, argv + i + 1);
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
	url_key = canonical_url(url);

	home = home_directory();
	data_dir = path_join(home, ".linkbrarian");
	ensure_data_directory(data_dir);
	db_path = path_join(data_dir, "linkography.db");
	db = db_open(db_path);
	db_init(db);

	if (db_find_link(db, url, url_key, &row)) {
		is_new = 0;
	} else {
		long long id = 0, n = 0;
		int rc;

		/* Fetch metadata only for new links; existing records keep their title. */
		if (!fetch_page_title(url, &page_name))
			page_name = xstrdup(host);
		page_name_key = normalize_key(page_name);
		rc = db_insert_link(db, page_name, page_name_key, url, url_key,
				    &id, &n);

		if (rc == 1) {
			if (!db_find_link(db, url, url_key, &row))
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
	free(url_key);
	free(host);
	free(url);
	return status;
}
