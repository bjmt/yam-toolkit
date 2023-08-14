/*
 *   yamshuf: A small super-fast higher-order DNA/RNA sequence shuffler
 *   Copyright (C) 2023  Benjamin Jean-Marie Tremblay
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

/* A complete k-mer table is constructed, meaning an exponential increase in
 * memory usage with increasing k. Since counts are stored using uint64_t,
 * this means for k=10:
 *
 * 8 [bytes] * 5 [# of letters, ACGTN] ^ 10 [k] = 74.51 MB
 *
 * In addition, this is accompanied by a k-1 edge graph:
 *
 * 8 [bytes] * 5 [# of letters, ACGTN] ^ 9 [k - 1] = 14.90 MB
 *
 * Final memory usage for a k=10 scenario: 74.51 + 14.90 = 89.41 MB
 *
 * Other approx. memory footprints:
 *
 * k<5  --> <0.01 MB
 * k=5  -->  0.03 MB
 * k=6  -->  0.14 MB
 * k=7  -->  0.72 MB
 * k=8  -->  3.58 MB
 * k=9  --> 17.88 MB
 * k=10 --> 89.41 MB
 * ...
 * k=12 --> 2.18 GB
 * ...
 * k=16 --> 1.33 TB
 *
 * A more effcient way of storing k-mer counts would be to use hash tables,
 * thus only ever storing existing k-mers in memory. However since sequence
 * shuffling tasks only ever need k=1-3 99.9% of the time, yamshuf just uses
 * dumb complete k-mer tables.
 */

/* Keeping gaps with -g
 * - what about making an array to keep track of location + length of gaps,
 *   then deleting them? afterwhich the modified sequence can be shuffled as
 *   normal, and gaps inserted again when writing. this seems simpler than
 *   embedding if checks everywhere in the code for gap chars.
 * - this would lead to an incorrect number of each k-mer afterwards though..
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <locale.h>
#include <getopt.h>
#include <math.h>
#include <limits.h>
#include <time.h>
#include <stdint.h>
#include <zlib.h>
#include "kseq.h"
#include "krng.h"

KSEQ_INIT(gzFile, gzread)

#define YAMSHUF_VERSION             "1.0"
#define YAMSHUF_YEAR                 2023

// Maybe test which one is faster?
/* Using a complete k-mer table: */
/* #define MAX_K_TABLE                            8 */
/* Using hashes to only represent available k-mers: */
/* #define MAX_K_HASH                            27 */

/* HARD LIMIT FOR 64-BIT: 5^27 */
#define MAX_K                                  9

#define FASTA_LINE_LEN                        60

/* These must be positive integers: */
#define DEFAULT_K                              3
#define DEFAULT_SEED                           4

/* Which characters should be seen as gaps? */
#define GAP_CHARS                           ".-"
/* When writing the gaps, use which character? */
#define FINAL_GAP                            '-'

#define ERASE_ARRAY(ARR, LEN) memset(ARR, 0, sizeof(ARR[0]) * (LEN))

#define LIKELY(COND) __builtin_expect(COND, 1)
#define UNLIKELY(COND) __builtin_expect(COND, 0)

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
long peak_mem(void) {
  return 0;
}
#else
#include <sys/resource.h>
long peak_mem(void) {
  struct rusage r_mem;
  getrusage(RUSAGE_SELF, &r_mem);
#ifdef __linux__
  return r_mem.ru_maxrss * 1024;
#else
  return r_mem.ru_maxrss;
#endif
}
#endif

void print_peak_mb(void) {
  long bytes = peak_mem();
  if (bytes > (1 << 30)) {
    fprintf(stderr, "Approx. peak memory usage: %'.2f GB.\n",
      (((double) bytes / 1024.0) / 1024.0) / 1024.0);
  } else if (bytes > (1 << 20)) {
    fprintf(stderr, "Approx. peak memory usage: %'.2f MB.\n",
      ((double) bytes / 1024.0) / 1024.0);
  } else if (bytes) {
    fprintf(stderr, "Approx. peak memory usage: %'.2f KB.\n",
      (double) bytes / 1024.0);
  }
}

void print_time(const uint64_t s, const char *what) {
  if (s > 7200) {
    fprintf(stderr, "Needed %'.2f hours to %s.\n", ((double) s / 60.0) / 60.0, what);
  } else if (s > 120) {
    fprintf(stderr, "Needed %'.2f minutes to %s.\n", (double) s / 60.0, what);
  } else if (s > 1) {
    fprintf(stderr, "Needed %'llu seconds to %s.\n", s, what);
  }
}

void usage(void) {
  printf(
    "yamshuf v%s  Copyright (C) %d  Benjamin Jean-Marie Tremblay                \n"
    "                                                                              \n"
    "Usage:  yamshuf [options] -i sequences.fa                                     \n"
    "                                                                              \n"
    " -i <str>   Filename of fast(a|q)-formatted file containing DNA/RNA sequences \n"
    "            to scan. Can be gzipped. Use '-' for stdin.  Non-standard         \n"
    "            characters (i.e. other than ACGTU) will be read but are treated as\n"
    "            the letter N during shuffling (exceptions: when -l is used or when\n"
    "            -k is set to 1). Fastq files will be output as fasta.             \n"
    " -k <int>   Size of shuffled k-mers. Default: %d. When k = 1 a Fisher-Yates    \n"
    "            shuffle is performed. Max k for Euler/Markov methods: %d.        \n"
    " -o <str>   Filename to output results. By default output goes to stdout.     \n"
    " -s <int>   Seed to initialize random number generator. Default: %d.           \n"
    " -m         Use Markov shuffling instead of performing a random Eulerian walk.\n"
    "            Essentially generates random sequences with similar k-mer         \n"
    "            compositions. Generally requires large sequences to be effective. \n"
    " -l         Split up the sequences linearly into k-mers and do a Fisher-Yates \n"
    "            shuffle instead of performing a random Eulerian walk. Very fast.  \n"
    " -r <int>   Repeat shuffling for each sequence any number of times. The repeat\n"
    "            number will be appended to the sequence name. Default: 0.         \n"
    /* " -W <int>   Instead of shuffling the entire sequence, progressively move      \n" */
    /* "            through the sequence and shuffle in windows of any size.          \n" */
    /* " -S <int>   Window step size when -w is set. Default: window size.            \n" */
    " -R         Reset the random number generator every time a new sequence is    \n"
    "            shuffled using the set seed instead of only setting it once.      \n"
    /* " -g         Gap characters [%s] will be left in-place, and not shuffled.      \n" */
    " -n         Output sequence as RNA. By default the sequence is output as DNA, \n"
    "            even if the input is RNA. This flag only applies when k > 1 and -l\n"
    "            is not used, since in such cases the existing sequence letters are\n"
    "            simply being rearranged.                                          \n"
    " -v         Verbose mode.                                                     \n"
    " -w         Very verbose mode.                                                \n"
    " -h         Print this help message.                                          \n"
    , YAMSHUF_VERSION, YAMSHUF_YEAR, DEFAULT_K, MAX_K, DEFAULT_SEED //, GAP_CHARS
  );
}

typedef struct args_t {
  int      k;
  int      seed;
  int      reset_seed : 1;
  int      use_markov : 1;
  int      use_linear : 1;
  int      leave_gaps : 1;
  int      rna_out : 1;
  int      v : 1;
  int      w : 1;
  int      shuf_repeats;
  uint64_t window_step;
  uint64_t window_overlap;
} args_t;

args_t args = {
  .k              = DEFAULT_K,
  .seed           = DEFAULT_SEED,
  .reset_seed     = 0,
  .use_markov     = 0,
  .use_linear     = 0,
  .leave_gaps     = 0,
  .rna_out        = 0,
  .v              = 0,
  .w              = 0,
  .shuf_repeats   = 0,
  .window_step    = 0,
  .window_overlap = 0
};

typedef struct files_t {
  int    s_open : 1;
  int    o_open : 1;
  gzFile s;
  FILE  *o;
} files_t;

files_t files = {
  .s_open = 0,
  .o_open = 0
};

void close_files(void) {
  if (files.s_open) gzclose(files.s);
  if (files.o_open) fclose(files.o);
}

/* aA = 0, cC = 1, gG = 2, tTuU = 3 */
const unsigned char char2index[] = { /* 16 x 16 */
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 0, 4, 1, 4, 4, 4, 2, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 0, 4, 1, 4, 4, 4, 2, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4 
};

const char index2dna[] = "ACGTN";
const char index2rna[] = "ACGUN";

unsigned char gap2bool[256] = { 0 };
const char gapchars[] = GAP_CHARS;

uint64_t char_counts[256];

const uint64_t pow5[] = {
                    1 ,                   5 ,                  25 ,                 125
,                 625 ,                3125 ,               15625 ,               78125
,              390625 ,             1953125 ,             9765625 ,            48828125
,           244140625 ,          1220703125 ,          6103515625 ,         30517578125
,        152587890625 ,        762939453125 ,       3814697265625 ,      19073486328125
,      95367431640625 ,     476837158203125 ,    2384185791015625 ,   11920928955078124
,   59604644775390624 ,  298023223876953152 , 1490116119384765696 , 7450580596923828224
};

krng_t krng;

void badexit(const char *msg) {
  fprintf(stderr, "%s\nRun yamshuf -h to see usage.\n", msg);
  close_files();
  exit(EXIT_FAILURE);
}

static inline int str_to_int(char *str, int *res) {
  char *tmp; errno = 0;
  long int res_long = strtol(str, &tmp, 10); 
  if (res_long > INT_MAX) return 1;
  *res = (int) res_long;
  if (str == tmp || errno != 0 || *tmp != '\0') {
    return 1;
  } else {
    return 0;
  }
}

/* static inline int str_to_uint64_t(char *str, uint64_t *res) { */
/*   char *tmp; errno = 0; */
/*   *res = (uint64_t) strtoull(str, &tmp, 10); */
/*   if (str == tmp || errno != 0 || *tmp != '\0') { */
/*     return 1; */
/*   } else { */
/*     return 0; */
/*   } */
/* } */

static inline void count_bases(const unsigned char *seq, const uint64_t size) {
  for (uint64_t i = 0; i < size; i++) {
    char_counts[seq[i]]++;
  }
}

static inline void swap(unsigned char *seq, const uint64_t i, const uint64_t j) {
  unsigned char tmp = seq[i]; seq[i] = seq[j]; seq[j] = tmp;
}

int shuffle_fisher_yates(unsigned char *seq, const uint64_t len) {
  for (uint64_t i = 0, l = len - 1; i < l; i++) {
    swap(seq, i, i + kr_rand_r(&krng) % (l - i));
  }
  return 0;
}

static inline void swap_k(unsigned char *seq, const uint64_t i, const uint64_t j, const uint64_t k) {
  for (uint64_t a = 0; a < k; a++) {
    swap(seq, i + a, j + a);
  }
}

int shuffle_linear(unsigned char *seq, const uint64_t size, const uint64_t k) {
  for (uint64_t r, i = 0; i < size - 2 * k + 1; i += k) {
    r = kr_rand_r(&krng) % (size - 2 * k + 1 - i);
    swap_k(seq, i, i + k + r - r % k, k);
  }
  return 0;
}

static inline uint64_t chars2kmer(const unsigned char *seq, const uint64_t k, const uint64_t offset) {
  uint64_t kmer = 0;
  for (uint64_t j = 0, i = k - 1; i < -1; j++, i--) {
    kmer += pow5[i] * char2index[seq[offset + j]];
  }
  return kmer;
}

static inline void count_kmers(const unsigned char *seq, const uint64_t size, uint64_t *kmer_tab, const uint64_t k) {
  for (uint64_t i = 0; i < size - k + 1; i++) {
    kmer_tab[chars2kmer(seq, k, i)]++;
  }
}

/*
static inline void index_to_kmer(const uint64_t index, char *kmer, const uint64_t k, const char *index2xna) {
  uint64_t decomposed[MAX_K];
  decomposed[k - 1] = index;
  for (uint64_t i = k - 2; i < -1; i--) {
    decomposed[i] = decomposed[i + 1] / 5;
  }
  kmer[0] = index2xna[decomposed[0]];
  kmer[k] = '\0';
  for (uint64_t i = 1; i < k; i++) {
    kmer[i] = index2xna[5 + (decomposed[i] - (decomposed[i - 1] + 1) * 5)];
  }
}
*/

static inline uint64_t cumsum_and_pick_next_letter(const uint64_t *kmers) {
  const uint64_t k0 =      kmers[0];
  const uint64_t k1 = k0 + kmers[1];
  const uint64_t k2 = k1 + kmers[2];
  const uint64_t k3 = k2 + kmers[3];
  const uint64_t k4 = k3 + kmers[4];
  const uint64_t r = kr_rand_r(&krng) % k4; 
       if (r < k0) return 0;
  else if (r < k1) return 1;
  else if (r < k2) return 2;
  else if (r < k3) return 3;
  else             return 4;
}

static inline uint64_t pick_next_letter(const uint64_t *kmers) {
  if (UNLIKELY(!kmers[4])) {
    return (uint64_t) kr_rand_r(&krng) % 4;
  } else {
    const uint64_t r = kr_rand_r(&krng) % kmers[4]; 
         if (r < kmers[0]) return 0;
    else if (r < kmers[1]) return 1;
    else if (r < kmers[2]) return 2;
    else if (r < kmers[3]) return 3;
    else                   return 4;
  }
}

int shuffle_markov(unsigned char *seq, const uint64_t size, const uint64_t k, uint64_t *kmer_tab, const int is_dna) {
  const char *index2xna = is_dna ? index2dna : index2rna;
  for (uint64_t i = 0; i < pow5[k]; i += 5) {
    kmer_tab[i + 1] += kmer_tab[i];
    kmer_tab[i + 2] += kmer_tab[i + 1];
    kmer_tab[i + 3] += kmer_tab[i + 2];
    kmer_tab[i + 4] += kmer_tab[i + 3];
  }
  for (uint64_t i = 0; i < k - 1; i++) {
    seq[i] = index2xna[char2index[seq[i]]];
  }
  for (uint64_t previous = 0, i = k - 1; i < size; previous = 0, i++) {
    for (uint64_t j = k - 1; j > 0; j--) {
      previous += char2index[seq[i - j]] * pow5[j];
    }
    seq[i] = index2xna[pick_next_letter(kmer_tab + previous)];
  }
  return 0;
}

#define COUNT_EDGES(OFFSET, TABLE) (TABLE[OFFSET]+TABLE[OFFSET+1]+TABLE[OFFSET+2]+TABLE[OFFSET+3]+TABLE[OFFSET+4])

int shuffle_euler(unsigned char *seq, const uint64_t size, const uint64_t k, uint64_t *kmer_tab, const int is_dna, unsigned char *invalid_vertex, uint64_t *euler_path, uint64_t *next_index) {

  const char *index2xna = is_dna ? index2dna : index2rna;

  // Initialize new sequence with starting vertex and final edge.

  for (uint64_t i = 0; i < k - 1; i++) {
    seq[i] = index2xna[char2index[seq[i]]];
  }
  seq[size - 1] = index2xna[char2index[seq[size - 1]]];

  // Remove the final edge from the edge pool.

  uint64_t last_edge = chars2kmer(seq, k, size - k);
  kmer_tab[last_edge]--;

  // Initialize unavailable vertices.

  for (uint64_t i = 0, j = 0; i < pow5[k - 1]; i++, j+= 5) {
    if (!(COUNT_EDGES(j, kmer_tab))) invalid_vertex[i] = 1;
  }

  // Reserve the final vertex.

  invalid_vertex[chars2kmer(seq, k - 1, size - k + 1)] = 1;

  // For k > 2, prefill an array containing the indices to the next vertex from an edge.
  // (For k = 2, it is just 0 as the edge is already the correct index.)

  if (k > 2) {
    for (uint64_t i = 0, j = 0, j_max = pow5[k - 2]; i < pow5[k - 1]; i++, j++) {
      if (j == j_max) j = 0;
      next_index[i] = j * 5;
    }
  }

  // Find a random Eulerian path through all available vertices.

  for (uint64_t u, i = 0; i < pow5[k - 1]; i++) {
    u = i;
    while (!invalid_vertex[u]) {
      euler_path[u] = cumsum_and_pick_next_letter(kmer_tab + u * 5);
      u = euler_path[u] + next_index[u];
    }
    u = i;
    while (!invalid_vertex[u]) {
      invalid_vertex[u] = 1;
      u = euler_path[u] + next_index[u];
    }
  }

  // Remove reserved edges from pool.

  for (uint64_t i = 0, edge; i < pow5[k - 1]; i++) {
    edge = i * 5 + euler_path[i];
    if (edge != last_edge && kmer_tab[edge]) kmer_tab[edge]--;
  }

  // Walk through Eulerian path, using up all available edges.
  // - Use up all edges per vertex, then once all edges = 0 use the final
  //   euler_path[u] edge to exit to next vertex. Start from first (k-1)mer
  //   in the sequence.

  for (uint64_t current_vertex, next_edge, kmer_index, i = k - 2; i < size - 2; i++) {
    current_vertex = chars2kmer(seq, k - 1, i - k + 2);
    kmer_index = current_vertex * 5;
    if (LIKELY(COUNT_EDGES(kmer_index, kmer_tab))) {
      // Use up all available edges per vertex.
      next_edge = cumsum_and_pick_next_letter(kmer_tab + kmer_index);
      kmer_tab[next_edge + kmer_index]--;
    } else {
      // Once exhausted, walk the Eulerian path to the next vertex.
      next_edge = euler_path[current_vertex];
    }
    seq[i + 1] = index2xna[next_edge];
  }

  return 0;

}

void write_seq(const unsigned char *seq, const uint64_t size, const char *name, const char *comment, const uint64_t comment_l, const uint64_t n) {
  if (comment_l && n) {
    fprintf(files.o, ">%s %s-%llu\n", name, comment, n);
  } else if (comment_l) {
    fprintf(files.o, ">%s %s\n", name, comment);
  } else if (n) {
    fprintf(files.o, ">%s-%llu\n", name, n);
  } else {
    fprintf(files.o, ">%s\n", name);
  }
  for (uint64_t i = 0; i < size; i += FASTA_LINE_LEN) {
    fprintf(files.o, "%.*s\n", FASTA_LINE_LEN, seq + i);
  }
}

int main(int argc, char **argv) {

  kseq_t *kseq;
  int opt;
  int use_stdout = 1;

  while ((opt = getopt(argc, argv, "i:k:o:s:mlr:Rgnvwh")) != -1) {
    switch (opt) {
      case 'i':
        if (optarg[0] == '-' && optarg[1] == '\0') {
          files.s = gzdopen(fileno(stdin), "r");
        } else {
          files.s = gzopen(optarg, "r");
          if (files.s == NULL) {
            fprintf(stderr, "Error: Failed to open sequence file \"%s\" [%s]",
              optarg, strerror(errno));
            badexit("");
          }
        }
        files.s_open = 1;
        break;
      case 'o':
        use_stdout = 0;
        files.o = fopen(optarg, "w");
        if (files.o == NULL) {
          fprintf(stderr, "Error: Failed to create output file \"%s\" [%s]",
            optarg, strerror(errno));
          badexit("");
        }
        files.o_open = 1;
        break;
      case 'k':
        if (str_to_int(optarg, &args.k)) {
          badexit("Error: Failed to parse -k value.");
        }
        if (!args.k) {
          badexit("Error: -k must be a positive integer.");
        }
        break;
      case 'm':
        args.use_markov = 1;
        break;
      case 'l':
        args.use_linear = 1;
        break;
      case 's':
        if (str_to_int(optarg, &args.seed)) {
          badexit("Error: Failed to parse -s value.");
        }
        if (!args.seed) {
          badexit("Error: -s must be a positive integer.");
        }
        break;
      case 'r':
        if (str_to_int(optarg, &args.shuf_repeats)) {
          badexit("Error: Failed to parse -r value.");
        }
        if (!args.shuf_repeats) {
          badexit("Error: -r must be a positive integer.");
        }
        break;
      /* case 'W': */
      /*   if (str_to_uint64_t(optarg, &args.window_step)) { */
      /*     badexit("Error: Failed to parse -w value."); */
      /*   } */
      /*   if (!args.window_step) { */
      /*     badexit("Error: -w must be a positive integer."); */
      /*   } */
      /*   break; */
      /* case 'S': */
      /*   if (str_to_uint64_t(optarg, &args.window_overlap)) { */
      /*     badexit("Error: Failed to parse -S value."); */
      /*   } */
      /*   if (!args.window_overlap) { */
      /*     badexit("Error: -S must be a positive integer."); */
      /*   } */
      /*   break; */
      case 'R':
        args.reset_seed = 1;
        break;
      case 'g':
        args.leave_gaps = 1;
        break;
      case 'n':
        args.rna_out = 1;
        break;
      case 'w':
        args.w = 1;
      case 'v':
        args.v = 1;
        break;
      case 'h':
        usage();
        return EXIT_SUCCESS;
      default:
        return EXIT_FAILURE;
    }
  }

  if (args.use_linear && args.use_markov) {
    badexit("Error: Cannot use both -m and -l.");
  }
  if (!args.use_linear && args.k > MAX_K) {
    fprintf(stderr, "Error: -k%d exceeds allowed max for Euler/Markov [MAX_K=%d]", args.k, MAX_K);
    badexit("");
  }

  if (setlocale(LC_NUMERIC, "en_US") == NULL && args.v) {
    fprintf(stderr, "Warning: setlocale(LC_NUMERIC, \"en_US\") failed.\n");
  }

  if (args.v && args.rna_out && (args.k == 1 || args.use_linear)) {
    fprintf(stderr, "Warning: The -n flag is ignored when -k is 1 or -l is used.\n");
  }

  if (use_stdout) {
    files.o = stdout;
    files.o_open = 1;
  }

  if (args.window_step && !args.window_overlap) {
    args.window_overlap = args.window_step;
  }

  if (args.leave_gaps) {
    const unsigned char s_len = (unsigned char) strlen(gapchars);
    for (unsigned char i = 0; i < s_len; i++) {
      gap2bool[(unsigned char) gapchars[i]] = 1;
    }
  }

  time_t time1 = time(NULL);
  unsigned char *seq;
  uint64_t size, gaps, unknowns, seq_comment_l;
  uint64_t n_seqs = 0;
  uint64_t *kmer_tab, *euler_path, *next_index;
  unsigned char *invalid_vertex;
  char *seq_name, *seq_comment;
  double gc_pct;
  const int total_reps = args.shuf_repeats + 1, is_dna = 1 - args.rna_out;
  const uint64_t k = (uint64_t) args.k;
  int ret_val, rep_counter;
  kseq = kseq_init(files.s);
  kr_srand_r(&krng, args.seed);

  if (k > 1 && !args.use_linear) {
    kmer_tab = malloc(sizeof(uint64_t) * pow5[k]);
    if (kmer_tab == NULL) {
      badexit("Failed to allocate memory for k-mer table.");
    }
  }

  if (k > 1 && !args.use_linear && !args.use_markov) {
    invalid_vertex = malloc(sizeof(unsigned char) * pow5[k - 1]);
    if (invalid_vertex == NULL) {
      badexit("Failed to allocate memory for vertex table.");
    }
    euler_path = malloc(sizeof(uint64_t) * pow5[k - 1]);
    if (euler_path == NULL) {
      badexit("Failed to allocate memory for Eulerian path vector.");
    }
    next_index = malloc(sizeof(uint64_t) * pow5[k - 1]);
    if (next_index == NULL) {
      badexit("Failed to allocate memory for Eulerian index vector.");
    }
  }

  int markov_warning_has_been_emitted = 0;

  while ((ret_val = kseq_read(kseq)) >= 0) {

    n_seqs++;
    rep_counter = total_reps;
    seq = (unsigned char *) kseq->seq.s;
    size = kseq->seq.l;
    seq_name = kseq->name.s;
    seq_comment = kseq->comment.s;
    seq_comment_l = kseq->comment.l;

    if (args.v) {
      if (seq_comment_l) {
        fprintf(stderr, "Shuffling sequence #%'llu: %s %s\n", n_seqs, seq_name, seq_comment);
      } else {
        fprintf(stderr, "Shuffling sequence #%'llu: %s\n", n_seqs, seq_name);
      }
      if (args.w) {
        ERASE_ARRAY(char_counts, 256);
        count_bases(seq, size);
        // TODO: what about custom gap chars
        gaps = char_counts['.'] + char_counts['-'];
        unknowns = size - gaps - char_counts['A'] - char_counts['a'] -
        char_counts['C'] - char_counts['c'] - char_counts['G'] - char_counts['g'] -
        char_counts['T'] - char_counts['t'] - char_counts['U'] - char_counts['u'];
        gc_pct = (double) (char_counts['G'] + char_counts['C'] + char_counts['g'] + char_counts['c']);
        gc_pct /= (double) (size - unknowns - gaps);
        gc_pct *= 100.0;
        fprintf(stderr, "  Sequence size: %'llu (%.2f%% non-standard)\n", size,
          100.0 * (double) unknowns / (double) size);
        fprintf(stderr, "  GC content: %.2f%%\n", gc_pct); 
      }
    }

    if (args.reset_seed) {
      kr_srand_r(&krng, args.seed);
    }

    /*
    char kmer[MAX_K + 1];
    for (uint64_t i = 0; i < pow5[k]; i++) {
      index_to_kmer(i, kmer, k, index2dna);
      fprintf(stderr, "[%zu] %s: %zu\n", i, kmer, kmer_tab[i]);
    }
    */

    if (size < k * 2) {
      if (args.v) {
        fprintf(stderr, "! Warning: Sequence too short to shuffle (size = %'llu, k = %llu)\n",
          size, k);
      }
    } else if (args.k == 1) {
      while (rep_counter) {
        if (shuffle_fisher_yates(seq, size)) goto error_shuffle;
        write_seq(seq, size, seq_name, seq_comment, seq_comment_l, total_reps - rep_counter);
        rep_counter--;
      }
    } else if (!args.use_linear && !args.use_markov) {
      while (rep_counter) {
        ERASE_ARRAY(invalid_vertex, pow5[k - 1]);
        ERASE_ARRAY(euler_path, pow5[k - 1]);
        ERASE_ARRAY(next_index, pow5[k - 1]);
        ERASE_ARRAY(kmer_tab, pow5[k]);
        count_kmers(seq, size, kmer_tab, k);
        if (shuffle_euler(seq, size, k, kmer_tab, is_dna, invalid_vertex, euler_path, next_index)) goto error_shuffle;
        write_seq(seq, size, seq_name, seq_comment, seq_comment_l, total_reps - rep_counter);
        rep_counter--;
      }
    } else if (args.use_linear) {
      while (rep_counter) {
        if (shuffle_linear(seq, size, k)) goto error_shuffle;
        write_seq(seq, size, seq_name, seq_comment, seq_comment_l, total_reps - rep_counter);
        rep_counter--;
      }
    } else if (args.use_markov) {
      if (size < 100 && args.v && !markov_warning_has_been_emitted) {
        fprintf(stderr, "! Warning: Markov shuffling of small sequences may generate homopolymer repeats\n");
        markov_warning_has_been_emitted = 1;
      }
      while (rep_counter) {
        ERASE_ARRAY(kmer_tab, pow5[k]);
        count_kmers(seq, size, kmer_tab, k);
        if (shuffle_markov(seq, size, k, kmer_tab, is_dna)) goto error_shuffle;
        write_seq(seq, size, seq_name, seq_comment, seq_comment_l, total_reps - rep_counter);
        rep_counter--;
      }
    }

    // TODO: send to gap-aware handler or not
    // - for markov: just skip over gaps
    // - for linear: don't swap chars when a gap is present
    // - for euler: shuffle between gaps (only way to preserve kmers)
    // TODO: send to window-aware handler or not

  }

  if (k > 1 && !args.use_linear) free(kmer_tab);
  if (k > 1 && !args.use_linear && !args.use_markov) {
    free(invalid_vertex);
    free(euler_path);
    free(next_index);
  }
  kseq_destroy(kseq);
  if (ret_val == -2) {
    badexit("Error: Failed to parse FASTQ qualities.");
  } else if (ret_val < -2) {
    badexit("Error: Failed to read input.");
  } else if (!n_seqs) {
    badexit("Error: Failed to read any sequences from input.");
  }

  close_files();

  time_t time2 = time(NULL);
  if (args.v) {
    fprintf(stderr, "Done.\n");
    time_t time3 = difftime(time2, time1);
    print_time((uint64_t) time3, "shuffle"); 
    print_peak_mb();
  }

  return EXIT_SUCCESS;

error_shuffle:
  if (k > 1 && !args.use_linear) free(kmer_tab);
  if (k > 1 && !args.use_linear && !args.use_markov) {
    free(invalid_vertex);
    free(euler_path);
    free(next_index);
  }
  kseq_destroy(kseq);
  badexit("");

}
