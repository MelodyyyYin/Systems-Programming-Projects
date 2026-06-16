/* ---------- Milestone A: trace parsing ---------- */
/*
 * csim.c - Cache simulator (Milestone A: argument parsing + help/errors)
 *
 * Supports the same command-line interface as csim-ref:
 *   ./csim [-v] -s <s> -E <E> -b <b> -t <trace>
 *   ./csim -h
 *
 * Exit codes (per handout):
 *   0 on success (including -h)
 *   1 on any error (bad args, missing args, etc.)
 */

#include "cachelab.h"

#include <ctype.h> // for isspace
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h> // for uint64_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_OK 0
#define EXIT_ERR 1

static void print_usage(FILE *out, const char *prog) {
    fprintf(out,
            "Usage: %s [-v] -s <s> -E <E> -b <b> -t <trace>\n"
            "       %s -h\n"
            "  -h         Print this help message and exit\n"
            "  -v         Verbose mode (optional)\n"
            "  -s <s>     Number of set index bits (sets = 2^s)\n"
            "  -E <E>     Number of lines per set (associativity)\n"
            "  -b <b>     Number of block bits (block size = 2^b)\n"
            "  -t <trace> Trace file to process\n",
            prog, prog);
}

/* Parse a non-negative integer (0,1,2,...) */
static bool parse_nonneg_int(const char *s, unsigned long *out) {
    if (s == NULL || *s == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0) {
        return false;
    }
    if (end == NULL || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

/* Parse a positive integer (1,2,3,...) */
static bool parse_pos_int(const char *s, unsigned long *out) {
    unsigned long v;
    if (!parse_nonneg_int(s, &v)) {
        return false;
    }
    if (v == 0) {
        return false;
    }
    *out = v;
    return true;
}

typedef struct {
    bool verbose;
    unsigned long s;
    unsigned long E;
    unsigned long b;
    const char *trace_path;
    bool have_s;
    bool have_E;
    bool have_b;
    bool have_t;
} sim_args_t;

/* Returns:
 *   EXIT_OK  if -h was provided (help printed)
 *   EXIT_ERR on any error
 *   -1       if args are valid and we should proceed
 */
static int parse_args(int argc, char **argv, sim_args_t *args) {
    memset(args, 0, sizeof(*args));

    int opt;
    while ((opt = getopt(argc, argv, "hvs:E:b:t:")) != -1) {
        switch (opt) {
        case 'h':
            print_usage(stdout, argv[0]);
            return EXIT_OK;

        case 'v':
            args->verbose = true;
            break;

        case 's': {
            unsigned long v;
            if (!parse_nonneg_int(optarg, &v)) {
                fprintf(stderr, "Invalid -s value: %s\n",
                        optarg ? optarg : "(null)");
                return EXIT_ERR;
            }
            args->s = v;
            args->have_s = true;
            break;
        }

        case 'E': {
            unsigned long v;
            if (!parse_pos_int(optarg, &v)) {
                fprintf(stderr, "Invalid -E value: %s\n",
                        optarg ? optarg : "(null)");
                return EXIT_ERR;
            }
            args->E = v;
            args->have_E = true;
            break;
        }

        case 'b': {
            unsigned long v;
            if (!parse_nonneg_int(optarg, &v)) {
                fprintf(stderr, "Invalid -b value: %s\n",
                        optarg ? optarg : "(null)");
                return EXIT_ERR;
            }
            args->b = v;
            args->have_b = true;
            break;
        }

        case 't':
            if (optarg == NULL || *optarg == '\0') {
                fprintf(stderr, "Missing -t value\n");
                return EXIT_ERR;
            }
            args->trace_path = optarg;
            args->have_t = true;
            break;

        default:
            print_usage(stderr, argv[0]);
            return EXIT_ERR;
        }
    }

    /* No extra positional arguments allowed. */
    if (optind < argc) {
        fprintf(stderr, "Unexpected extra argument: %s\n", argv[optind]);
        return EXIT_ERR;
    }

    /* Must supply -s -E -b -t. */
    if (!(args->have_s && args->have_E && args->have_b && args->have_t)) {
        fprintf(stderr, "Mandatory arguments missing.\n");
        print_usage(stderr, argv[0]);
        return EXIT_ERR;
    }

    /* Sanity: addresses are 64-bit; require s+b <= 64. */
    if (args->s + args->b > 64UL) {
        fprintf(stderr,
                "Invalid cache geometry: s+b must be <= 64 (got %lu+%lu)\n",
                args->s, args->b);
        return EXIT_ERR;
    }

    return -1; /* proceed */
}

/* ---------- Milestone B: trace parsing ---------- */

#define LINE_MAXLEN 256

/* Skip leading whitespace; return pointer to first non-space char. */
static const char *skip_ws(const char *p) {
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

/* Parse one trace line of the form:
 *   L 4f6b868,4
 *   S 7ff005c8,8
 *
 * On success, writes *op, *addr, *size and returns true.
 * Returns false if the line is malformed.
 *
 * Note: We intentionally keep parsing strict and detect trailing junk.
 */
static bool parse_trace_line(const char *line, char *op, uint64_t *addr,
                             unsigned long *size) {
    const char *p = skip_ws(line);

    /* Empty line or just whitespace: reject (trace format should be
     * line-based). */
    if (*p == '\0' || *p == '\n') {
        return false;
    }

    /* Op */
    if (*p != 'L' && *p != 'S') {
        return false;
    }
    *op = *p;
    p++;

    /* Require at least one whitespace after op (matches the handout format). */
    if (!isspace((unsigned char)*p)) {
        return false;
    }
    p = skip_ws(p);

    /* Addr: 64-bit hex without 0x */
    errno = 0;
    char *end = NULL;
    unsigned long long a = strtoull(p, &end, 16);
    if (errno != 0 || end == p) {
        return false;
    }
    p = end;

    /* Must have comma immediately after address (no spaces around comma). */
    if (*p != ',') {
        return false;
    }
    p++; /* skip comma */

    /* Size: small positive decimal */
    errno = 0;
    end = NULL;
    unsigned long sz = strtoul(p, &end, 10);
    if (errno != 0 || end == p || sz == 0) {
        return false;
    }
    p = end;

    /* After size, only allow whitespace/newline until end. */
    p = skip_ws(p);
    if (*p != '\0' && *p != '\n') {
        return false;
    }

    *addr = (uint64_t)a;
    *size = sz;
    return true;
}
/* ---------- Milestone C: cache simulation ---------- */

typedef struct {
    bool valid;
    uint64_t tag;
    bool dirty;
    uint64_t lru_ts; /* larger = more recently used */
} cache_line_t;

typedef struct {
    cache_line_t *lines; /* length E */
} cache_set_t;

typedef struct {
    cache_set_t *sets; /* length S = 2^s */
    unsigned long s;
    unsigned long E;
    unsigned long b;
    unsigned long S; /* number of sets */
    unsigned long B; /* block size in bytes */
    uint64_t tick;   /* global timestamp for LRU */
} cache_t;

static bool cache_init(cache_t *c, unsigned long s, unsigned long E,
                       unsigned long b) {
    memset(c, 0, sizeof(*c));
    c->s = s;
    c->E = E;
    c->b = b;
    c->S = 1UL << s;
    c->B = 1UL << b;
    c->tick = 1;

    c->sets = (cache_set_t *)calloc(c->S, sizeof(cache_set_t));
    if (!c->sets) {
        return false;
    }

    for (unsigned long si = 0; si < c->S; si++) {
        c->sets[si].lines = (cache_line_t *)calloc(E, sizeof(cache_line_t));
        if (!c->sets[si].lines) {
            /* cleanup partial allocations */
            for (unsigned long j = 0; j < si; j++) {
                free(c->sets[j].lines);
            }
            free(c->sets);
            c->sets = NULL;
            return false;
        }
    }
    return true;
}

static void cache_free(cache_t *c) {
    if (!c || !c->sets) {
        return;
    }
    for (unsigned long si = 0; si < c->S; si++) {
        free(c->sets[si].lines);
    }
    free(c->sets);
    c->sets = NULL;
}

/* Return set index and tag for an address */
static inline unsigned long addr_set_index(const cache_t *c, uint64_t addr) {
    unsigned long mask = (c->S - 1UL); /* since S is power of 2 */
    return (unsigned long)((addr >> c->b) & mask);
}

static inline uint64_t addr_tag(const cache_t *c, uint64_t addr) {
    return addr >> (c->b + c->s);
}

/* Access cache with operation op ('L' or 'S'). Updates stats. */
static void cache_access(cache_t *c, char op, uint64_t addr,
                         csim_stats_t *stats, bool verbose) {
    unsigned long si = addr_set_index(c, addr);
    uint64_t tag = addr_tag(c, addr);
    cache_set_t *set = &c->sets[si];

    /* 1) Look for hit */
    for (unsigned long i = 0; i < c->E; i++) {
        cache_line_t *line = &set->lines[i];
        if (line->valid && line->tag == tag) {
            stats->hits++;
            line->lru_ts = c->tick++;
            if (op == 'S') {
                line->dirty = true;
            }
            if (verbose) {
                printf("%c %llx hit\n", op, (unsigned long long)addr);
            }
            return;
        }
    }

    /* 2) Miss */
    stats->misses++;

    /* 2a) Find an empty line if possible */
    cache_line_t *victim = NULL;
    for (unsigned long i = 0; i < c->E; i++) {
        if (!set->lines[i].valid) {
            victim = &set->lines[i];
            break;
        }
    }

    bool eviction = false;

    /* 2b) If no empty, choose LRU victim */
    if (victim == NULL) {
        eviction = true;
        stats->evictions++;

        victim = &set->lines[0];
        for (unsigned long i = 1; i < c->E; i++) {
            if (set->lines[i].lru_ts < victim->lru_ts) {
                victim = &set->lines[i];
            }
        }

        /* write-back: if dirty, count bytes written to memory */
        if (victim->valid && victim->dirty) {
            stats->dirty_evictions += c->B;
        }
    }

    /* write-allocate: fill the line on miss (both L and S) */
    victim->valid = true;
    victim->tag = tag;
    victim->lru_ts = c->tick++;
    victim->dirty = (op == 'S');

    if (verbose) {
        if (eviction) {
            printf("%c %llx miss eviction\n", op, (unsigned long long)addr);
        } else {
            printf("%c %llx miss\n", op, (unsigned long long)addr);
        }
    }
}

/* Compute dirty bytes remaining in cache at end */
static unsigned long cache_dirty_bytes_in_cache(const cache_t *c) {
    unsigned long dirty_lines = 0;
    for (unsigned long si = 0; si < c->S; si++) {
        const cache_set_t *set = &c->sets[si];
        for (unsigned long i = 0; i < c->E; i++) {
            const cache_line_t *line = &set->lines[i];
            if (line->valid && line->dirty) {
                dirty_lines++;
            }
        }
    }
    return dirty_lines * c->B;
}
/* Process the trace file and (for now) just parse lines successfully.
 * Returns 0 on success, 1 on any error (open failure or parse errors). */
static int process_trace_file(const char *trace_path, bool verbose,
                              cache_t *cache, csim_stats_t *stats) {
    FILE *tfp = fopen(trace_path, "rt");
    if (!tfp) {
        fprintf(stderr, "Error opening '%s': %s\n", trace_path,
                strerror(errno));
        return EXIT_ERR;
    }

    char linebuf[LINE_MAXLEN];
    int parse_error = 0;
    unsigned long lineno = 0;

    while (fgets(linebuf, (int)sizeof(linebuf), tfp)) {
        lineno++;

        /* Detect overly long line (no '\n' means it was truncated). */
        size_t len = strlen(linebuf);
        if (len == sizeof(linebuf) - 1 && linebuf[len - 1] != '\n') {
            fprintf(stderr, "Parse error: line %lu too long\n", lineno);
            parse_error = 1;
            break;
        }

        char op;
        uint64_t addr;
        unsigned long size;

        if (!parse_trace_line(linebuf, &op, &addr, &size)) {
            fprintf(stderr, "Parse error on line %lu: %s", lineno, linebuf);
            parse_error = 1;
            break;
        }

        if (verbose) {
            /* Print what we parsed (useful debugging; OK to print in verbose).
             */
            printf("%c %llx,%lu\n", op, (unsigned long long)addr, size);
        }

        /* Milestone C will do: cache_access(op, addr); */
        cache_access(cache, op, addr, stats, verbose);
    }

    fclose(tfp);
    return parse_error ? EXIT_ERR : EXIT_OK;
}

int main(int argc, char **argv) {
    sim_args_t args;
    int r = parse_args(argc, argv, &args);
    if (r == EXIT_OK) {
        return EXIT_OK; // -h
    }
    if (r == EXIT_ERR) {
        return EXIT_ERR; // bad args
    }

    if (args.verbose) {
        fprintf(stderr, "Parsed args: s=%lu E=%lu b=%lu t=%s\n", args.s, args.E,
                args.b, args.trace_path);
    }

    csim_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    cache_t cache;
    if (!cache_init(&cache, args.s, args.E, args.b)) {
        fprintf(stderr, "Out of memory initializing cache\n");
        return EXIT_ERR;
    }

    int tr = process_trace_file(args.trace_path, args.verbose, &cache, &stats);
    if (tr != EXIT_OK) {
        cache_free(&cache);
        return EXIT_ERR;
    }

    stats.dirty_bytes = cache_dirty_bytes_in_cache(&cache);

    printSummary(&stats);

    cache_free(&cache);
    return EXIT_OK;
}
