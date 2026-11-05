/* linkbrarian configuration */
#define VERSION "0.1.0"

/* HTTP limits */
#define HTTP_CONNECT_TIMEOUT 5L
#define HTTP_TOTAL_TIMEOUT 15L
#define HTTP_MAX_REDIRECTS 5L
#define HTTP_MAX_BYTES (1024L * 1024L)

/* Notes */
#define SLUG_MAX_BYTES 80
#define NOTES_SUFFIX_MAX_ATTEMPTS 100

/* Exit codes */
#define EXIT_USAGE 1
#define EXIT_BAD_URL 2
#define EXIT_STORAGE 3
#define EXIT_NOTES 4

/* SQLite schema version */
#define SCHEMA_VERSION 1
