/* -*- c-file-style: "k&r"; indent-tabs-mode: nil; -*-
 *
 * FIXME Copyright update
 *
 *********************************************************************/


#define TIMING 0

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <float.h>

#include "fet.h"
#include "utils.h"
#include "log.h"
/* for source_qual */
#include "sam.h"
#include "samutils.h"

#include "snpcaller.h"

#if TIMING
#include <time.h>
#endif


#ifndef MIN
#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))
#endif
#ifndef MAX
#define MAX(X,Y) ((X) > (Y) ? (X) : (Y))
#endif

#define LOGZERO -1e100 
/* FIXME shouldn't we use something from float.h ? */

/* Four nucleotides, with one consensus, makes three
   non-consensus bases */
#define NUM_NONCONS_BASES 3

#if 0
#define DEBUG
#endif

#if 0
#define TRACE
#endif

#if 0
#define NAIVE
#endif


double log_sum(double log_a, double log_b);
double log_diff(double log_a, double log_b);
double probvec_tailsum(const double *probvec, int tail_startindex,
                       int probvec_len);
double *naive_calc_prob_dist(const double *err_probs, int N, int K);
double *pruned_calc_prob_dist(const double *err_probs, int N, int K, 
                      long long int bonf_factor, double sig_level);




/* Estimate as to how likely it is that this read, given the mapping,
 * comes from this reference genome. P(r not from g|mapping) = 1 - P(r
 * from g). 
 * 
 * Use base-qualities and poisson-binomial dist, similar to core SNV
 * calling, but return prob instead of pvalue (and subtract one
 * mismatch which is the SNV we are checking). Furthermore, keep all
 * qualities as they are, i.e. don't replace mismatches with lower
 * values. Rationale: higher SNV quals, means higher chance SNVs are
 * real, therefore higher prob. read does not come from genome.
 *
 * Assuming independence of errors is okay, because if they are not
 * independent, then the prediction made is conservative.
 *
 * FIXME: should always ignore heterozygous or known SNV pos!
 *
 * Returns -1 on error. otherwise prob to see the observed number of
 * mismatches-1
 *
 */
int
source_qual(const bam1_t *b, const char *ref)
{
     int op_counts[NUM_OP_CATS];
     int **op_quals = NULL;

     double *probvec = NULL;
     int num_non_matches; /* incl indels */
     int orig_num_non_matches; /* FIXME TMP */
     double *err_probs = NULL; /* error probs (qualities) passed down to snpcaller */
     int num_err_probs; /* #elements in err_probs */

     double unused_pval;
     int src_qual = 255;
     double src_prob; /* prob of this read coming from genome */
     int err_prob_idx;
     int i, j;


     /* alloc op_quals
      */
     if (NULL == (op_quals = malloc(NUM_OP_CATS * sizeof(int *)))) {
          fprintf(stderr, "FATAL: couldn't allocate memory at %s:%s():%d\n",
                  __FILE__, __FUNCTION__, __LINE__);
          exit(1);
     }
     for (i=0; i<NUM_OP_CATS; i++) {
          if (NULL == (op_quals[i] = malloc(MAX_READ_LEN * sizeof(int)))) {
               fprintf(stderr, "FATAL: couldn't allocate memory at %s:%s():%d\n",
                       __FILE__, __FUNCTION__, __LINE__);
               free(op_quals);
               exit(1);
          }
     }
          
     /* count match operations and get qualities for them
      */
     num_err_probs = count_cigar_ops(op_counts, op_quals, b, ref, -1);
     if (-1 == num_err_probs) {
          LOG_WARN("%s\n", "count_cigar_ops failed on read"); /* FIXME print read */
          src_qual = -1;
          goto free_and_exit;
     }
     
     /* alloc and fill err_probs with quals returned per op-cat from
      * count_cigar_ops 
      */
     if (NULL == (err_probs = malloc(num_err_probs * sizeof(double)))) {
          fprintf(stderr, "FATAL: couldn't allocate memory at %s:%s():%d\n",
                  __FILE__, __FUNCTION__, __LINE__);
          exit(1);
     }
     num_non_matches = 0;
     err_prob_idx = 0;
     for (i=0; i<NUM_OP_CATS; i++) {
         if (i != OP_MATCH) {
             num_non_matches += op_counts[i];
         }
         for (j=0; j<op_counts[i]; j++) {
             /* LOG_FIXME("err_probs[%d] = op_quals[i=%d][j=%d]=%d\n", err_prob_idx, i, j, op_quals[i][j]); */
             err_probs[err_prob_idx] = PHREDQUAL_TO_PROB(op_quals[i][j]);
             err_prob_idx += 1;
         }
     }
     assert(err_prob_idx == num_err_probs);

     LOG_FIXME("%s\n", "Why -1? What if num_non_matches=0, what if =1? Also, use -1 for softening?");
#if 1
     /* need prob not pv and also need num_non_matches-1 */
     orig_num_non_matches = num_non_matches;
     if (num_non_matches>0) {
          num_non_matches -= 1;
     }
#endif

#if 1
     if (0 == num_non_matches) {
          /* FIXME no softening then? 
           * is MM=0 same as MM=1? 
           */
          src_qual = PROB_TO_PHREDQUAL(0.0);
          goto free_and_exit;
     }
#endif

     /* src_prob: what's the prob of seeing n_mismatches-1 by chance,
      * given quals? or: how likely is this read from the genome.
      * 1-src_value = prob read is not from genome
      */

     /* sorting in theory should be numerically more stable and also
      * make poissbin faster */
     qsort(err_probs, num_err_probs, sizeof(double), dbl_cmp);
     probvec = poissbin(&unused_pval, err_probs,
                        num_err_probs, num_non_matches, 1.0, 0.05);
     src_prob = exp(probvec[num_non_matches-1]);
     free(probvec);
     src_qual =  PROB_TO_PHREDQUAL(1.0 - src_prob);


#if 0
"
PJ = joined Q
PM = map Q
PG = genome Q
PS = source Q


PJ = PM  +  (1-PM) * PG  +  (1-PM) * (1-PG) * PB
# note: niranjan used PS and meant PB I think
# mapping error
# OR
# no mapping error AND genome error
# OR
# no mapping error AND no genome error AND base-error


PJ = PM + (1-PM) * PB
# mapping error OR no mapping error AND base-error
"
#endif

free_and_exit:
     for (i=0; i<NUM_OP_CATS; i++) {
          free(op_quals[i]);
     } 
     free(op_quals);

     free(err_probs);

     LOG_FIXME("%s\n", "how to implement softening? don't use matches from stats but add all the others up. Also use orig_num_non_matches");
     LOG_FIXME("returning src_qual=%d (orig prob = %g) for cigar=%s num_err_probs=%d num_non_matches=%d(%d) @%d\n", 
               src_qual, src_prob, cigar_str_from_bam(b), num_err_probs, num_non_matches, orig_num_non_matches, b->core.pos);

     return src_qual;
}
/* source_qual() */




/**
 * @brief Computes log(exp(log_a) + exp(log_b))
 *
 * Taken from util.h of FAST source code:
 * http://www.cs.cornell.edu/~keich/FAST/fast.tar.gz
 * and using log1p
 */
double
log_sum(double log_a, double log_b)
{
    if (log_a > log_b) {
        return log_a + log1p(exp(log_b-log_a));
    } else {
        return log_b + log1p(exp(log_a-log_b));
    }
}
/* log_sum() */


/**
 * @brief Computes log(exp(log_a) - exp(log_b))
 *
 * Adapted from log_sum above and scala/breeze/numerics logDiff
 * See also http://stackoverflow.com/questions/778047/we-know-log-add-but-how-to-do-log-subtract
 *
 */
double
log_diff(double log_a, double log_b)
{
    if (log_a >= log_b) {
        return log_a + log1p(- exp(log_b-log_a));
    } else {
        return log_b + log1p(- exp(log_a-log_b));
    }
}
/* log_diff() */



/**
 * @brief Computes sum of probvec values (log space) starting from (including)
 * tail_startindex to (excluding) probvec_len
 *
 */
double
probvec_tailsum(const double *probvec, int tail_startindex, int probvec_len)
{
    double tailsum;
    int i;

    tailsum = probvec[tail_startindex];
    for (i=tail_startindex+1; i<probvec_len; i++) {
        tailsum = log_sum(tailsum, probvec[i]);
    }

    return tailsum;
}
/* probvec_tailsum() */


/**
 *
 */
double *
naive_calc_prob_dist(const double *err_probs, int N, int K)
{
     double *probvec = NULL;
     double *probvec_prev = NULL;
     double *probvec_swp = NULL;
     
     int n;
     fprintf(stderr, "CRITICAL(%s:%s:%d): Possibly buggy code. Use pruned_calc_prob_dist instead of me\n", 
             __FILE__, __FUNCTION__, __LINE__);
     exit(1);

    if (NULL == (probvec = malloc((N+1) * sizeof(double)))) {
        fprintf(stderr, "FATAL: couldn't allocate memory at %s:%s():%d\n",
                __FILE__, __FUNCTION__, __LINE__);
        return NULL;
    }
    if (NULL == (probvec_prev = malloc((N+1) * sizeof(double)))) {
        fprintf(stderr, "FATAL: couldn't allocate memory at %s:%s():%d\n",
                __FILE__, __FUNCTION__, __LINE__);
        free(probvec);
        return NULL;
    }

    /* init */
    probvec_prev[0] = 0.0; /* 0.0 = log(1.0) */

    for (n=1; n<N+1; n++) {
        int k;
        double log_pn, log_1_pn;
        double pn = err_probs[n-1];

        
        /* if pn=0 log(on) will fail. likewise if pn=1 (Q0) then
         * log1p(-pn) = log(1-1) = log(0) will fail. therefore test */
        if (fabs(pn) < DBL_EPSILON) {             
             log_pn = log(DBL_EPSILON);
        } else {
             log_pn = log(pn);
        }
        if (fabs(pn-1.0) < DBL_EPSILON) {             
             log_1_pn = log1p(-pn+DBL_EPSILON);
        } else {
             log_1_pn = log1p(-pn);
        }

#if 0
        fprintf(stderr, "DEBUG(%s:%s:%d): pn=%g log_pn=%g log_1_pn=%g err_probs[n=%d-1]=%g\n",
                __FILE__, __FUNCTION__, __LINE__, pn, log_pn, log_1_pn, n, err_probs[n-1]);
#endif

        k = 0;
        probvec[k] = probvec_prev[k] + log_1_pn;

        for (k=1; k<K; k++) {
             /* FIXME clang says: The left operand of '+' is a garbage value */
            probvec[k] = log_sum(probvec_prev[k] + log_1_pn,
                                 probvec_prev[k-1] + log_pn);
        }
        k = n;
        probvec[k] = probvec_prev[k-1] + log_pn;


        /* swap */
        probvec_swp = probvec;
        probvec = probvec_prev;
        probvec_prev = probvec_swp;
    }


    free(probvec_prev);    
    return probvec;
}
/* naive_prob_dist */



/**
 * Should really get rid of bonf_factor and sig_level here and
 * upstream as well
 *
 */
double *
pruned_calc_prob_dist(const double *err_probs, int N, int K, 
                      long long int bonf_factor, double sig_level)
{
    double *probvec = NULL;
    double *probvec_prev = NULL;
    double *probvec_swp = NULL;
    int n;

    if (NULL == (probvec = malloc((K+1) * sizeof(double)))) {
        fprintf(stderr, "FATAL: couldn't allocate memory at %s:%s():%d\n",
                __FILE__, __FUNCTION__, __LINE__);
        return NULL;
    }
    if (NULL == (probvec_prev = malloc((K+1) * sizeof(double)))) {
        fprintf(stderr, "FATAL: couldn't allocate memory at %s:%s():%d\n",
                __FILE__, __FUNCTION__, __LINE__);
        free(probvec);
        return NULL;
    }

    for (n=0; n<N; n++) {
         /*LOG_FIXME("err_probs[n=%d]=%g\n", n, err_probs[n]);*/
         assert(err_probs[n] + DBL_EPSILON >= 0.0 && err_probs[n] - DBL_EPSILON <= 1.0);
    }

#ifdef DEBUG
    for (n=0; n<K+1; n++) {
        probvec_prev[n] = probvec[n] = 666.666;
    }
#endif

    /* init */
    probvec_prev[0] = 0.0; /* log(1.0) */

    for (n=1; n<=N; n++) {
        int k;
        double pvalue;
        double pn = err_probs[n-1];
        double log_pn, log_1_pn;


        /* if pn=0 log(on) will fail. likewise if pn=1 (Q0) then
         * log1p(-pn) = log(1-1) = log(0) will fail. therefore test */
        if (fabs(pn) < DBL_EPSILON) {             
             log_pn = log(DBL_EPSILON);
        } else {
             log_pn = log(pn);
        }
        if (fabs(pn-1.0) < DBL_EPSILON) {             
             log_1_pn = log1p(-pn+DBL_EPSILON);
        } else {
             log_1_pn = log1p(-pn);/* 0.0 = log(1.0) */
        }
        
#ifdef TRACE
		fprintf(stderr, "DEBUG(%s:%s:%d): n=%d err_probs[n-1]=%g pn=%g log_pn=%g log_1_pn=%g\n", 
                __FILE__, __FUNCTION__, __LINE__, n, err_probs[n-1], pn, log_pn, log_1_pn);
#endif

        if(n < K) {
            probvec_prev[n] = LOGZERO;
        }

        for (k=MIN(n,K-1); k>=1; k--) {
            assert(probvec_prev[k]<=0.0 && probvec_prev[k-1]<=0.0);
            probvec[k] = log_sum(probvec_prev[k] + log_1_pn,
                                 probvec_prev[k-1] + log_pn);            
        }
        k = 0;
        assert(probvec_prev[k]<=0.0);
        probvec[k] = probvec_prev[k] + log_1_pn;

#ifdef TRACE
        for (k=0; k<=MIN(n, K-1); k++) {
            fprintf(stderr, "DEBUG(%s:%s:%d): probvec[k=%d] = %g\n", 
                    __FILE__, __FUNCTION__, __LINE__, k, probvec[k]);
        }
        for (k=0; k<=MIN(n,K-1); k++) {
            fprintf(stderr, "DEBUG(%s:%s:%d): probvec_prev[k=%d] = %g\n", 
                    __FILE__, __FUNCTION__, __LINE__, k, probvec_prev[k]);
        }
#endif

        if (n==K) {
            probvec[K] = probvec_prev[K-1] + log_pn;
            /* FIXME check here as well */

        } else if (n > K) { 
             /*LOG_FIXME("probvec_prev[K=%d]=%g probvec_prev[K=%d -1]=%g\n", K, probvec_prev[K], K, probvec_prev[K-1]);*/
             assert(probvec_prev[K]-DBL_EPSILON<=0.0 && probvec_prev[K-1]-DBL_EPSILON<=0.0);
             probvec[K] = log_sum(probvec_prev[K], probvec_prev[K-1]+log_pn);
             pvalue = exp(probvec[K]);
             
             if (pvalue * (double)bonf_factor >= sig_level) {
#ifdef DEBUG
                  fprintf(stderr, "DEBUG(%s:%s:%d): early exit at n=%d with pvalue %g\n", 
                          __FILE__, __FUNCTION__, __LINE__, n, pvalue);
#endif
                  goto free_and_exit;
             }
        }

        assert(! isinf(probvec[0])); /* used to happen when first q=0 */

        /* swap */
        probvec_swp = probvec;
        probvec = probvec_prev;
        probvec_prev = probvec_swp;
    }

 free_and_exit:
    free(probvec_prev);    

    return probvec;
}
/* pruned_calc_prob_dist */


#ifdef PSEUDO_BINOMIAL
/* binomial test using poissbin. only good for high n and small prob.
 * returns -1 on error */
int
pseudo_binomial(double *pvalue, 
                int num_success, int num_trials, double succ_prob)
{
     const long long int bonf = 1.0;
     const double sig = 1.0;
     double *probvec = NULL;
     double *probs;
     int i;

     fprintf(stderr, "WARNING(%s): this function only approximates the binomial for high n and small p\n", __FUNCTION__);
     if (NULL == (probs = malloc((num_trials) * sizeof(double)))) {
          fprintf(stderr, "FATAL: couldn't allocate memory at %s:%s():%d\n",
                  __FILE__, __FUNCTION__, __LINE__);
          return -1;
     }

     for (i=0; i<num_trials; i++) {
          probs[i] = succ_prob;
     }     

     probvec = poissbin(pvalue, probs,
                        num_trials, num_success,
                        bonf, sig);
     free(probvec);
     free(probs);

     return 0;
}
#endif

/* main logic. return of probvec (needs to be freed by caller allows
 * to check pvalues for other numbers < (original num_failures), like
 * so: exp(probvec_tailsum(probvec, smaller_numl, orig_num+1)) but
 * only if first pvalue was below limits implied by bonf and sig.
 * default pvalue is DBL_MAX (1 might still be significant).
 *
 *  note: pvalues > sig/bonf are not computed properly
 */       
double *
poissbin(double *pvalue, const double *err_probs,
         const int num_err_probs, const int num_failures, 
         const long long int bonf, const double sig) 
{
    double *probvec = NULL;
#if TIMING
    clock_t start = clock();
    int msec;
#endif
    *pvalue = DBL_MAX;

#if TIMING
    start = clock();
#endif
#ifdef NAIVE
    probvec = naive_prob_dist(err_probs, num_err_probs,
                                    num_failures);
#else
    probvec = pruned_calc_prob_dist(err_probs, num_err_probs,
                                    num_failures, bonf, sig);    
#endif
#if TIMING
    msec = (clock() - start) * 1000 / CLOCKS_PER_SEC;
    fprintf(stderr, "calc_prob_dist() took %d s %d ms\n", msec/1000, msec%1000);
#endif

    *pvalue = exp(probvec[num_failures]); /* no need for tailsum here */
    assert(! isnan(*pvalue));
    return probvec;
}



/**
 * @brief
 * 
 * pvalues computed for each of the NUM_NONCONS_BASES noncons_counts
 * will be written to snp_pvalues in the same order. If pvalue was not
 * computed (always insignificant) its value will be set to DBL_MAX
 * 
 */
int
snpcaller(double *snp_pvalues, 
          const double *err_probs, const int num_err_probs, 
          const int *noncons_counts, 
          const long long int bonf_factor, const double sig_level)
{
    double *probvec = NULL;
    int i;
    int max_noncons_count = 0;
    double pvalue;

#ifdef DEBUG
    fprintf(stderr, "DEBUG(%s:%s():%d): num_err_probs=%d noncons_counts=%d,%d,%d bonf_factor=%lld sig_level=%f\n", 
            __FILE__, __FUNCTION__, __LINE__, 
            num_err_probs, noncons_counts[0], noncons_counts[1], noncons_counts[2],
            bonf_factor, sig_level);
#endif

    /* initialise empty results so that we can return anytime */
    for (i=0; i<NUM_NONCONS_BASES; i++) {
        snp_pvalues[i] = DBL_MAX;
    }
    
    /* determine max non-consensus count */
    for (i=0; i<NUM_NONCONS_BASES; i++) {
        if (noncons_counts[i] > max_noncons_count) {
            max_noncons_count = noncons_counts[i];
        }
    }

    /* no need to do anything if no snp bases */
    if (0==max_noncons_count) {
        goto free_and_exit;
    }

    probvec = poissbin(&pvalue, err_probs, num_err_probs,
                       max_noncons_count, bonf_factor, sig_level);
    
    if (pvalue * (double)bonf_factor >= sig_level) {
#ifdef DEBUG
        fprintf(stderr, "DEBUG(%s:%s():%d): Most frequent SNV candidate already gets not signifcant pvalue of %g * %lld >= %g\n", 
                __FILE__, __FUNCTION__, __LINE__, 
                pvalue, bonf_factor, sig_level);
#endif
        goto free_and_exit;
    }


    /* report p-value for each non-consensus base
     */
#if 0
    for (i=1; i<max_noncons_count+1; i++) {        
        fprintf(stderr, "DEBUG(%s:%s():%d): prob for count %d=%g\n", 
                __FILE__, __FUNCTION__, __LINE__, 
                i, exp(probvec[i]));
    }
#endif
#if 0
    for (i=1; i<max_noncons_count+1; i++) {        
        fprintf(stderr, "DEBUG(%s:%s():%d): pvalue=%g for noncons_counts %d\n", 
                __FILE__, __FUNCTION__, __LINE__, 
                exp(probvec_tailsum(probvec, i, max_noncons_count+1)), i);
    }
#endif

    for (i=0; i<NUM_NONCONS_BASES; i++) { 
        if (0 != noncons_counts[i]) {
            pvalue = exp(probvec_tailsum(probvec, noncons_counts[i], max_noncons_count+1));
            snp_pvalues[i] = pvalue;
#ifdef DEBUG
            fprintf(stderr, "DEBUG(%s:%s():%d): i=%d noncons_counts=%d max_noncons_count=%d pvalue=%g\n", 
                    __FILE__, __FUNCTION__, __LINE__, 
                    i, noncons_counts[i], max_noncons_count, pvalue);                  
#endif
        }
    }

 free_and_exit:
    if (NULL != probvec) {
        free(probvec);
    }

    return 0;
}
/* snpcaller() */


#ifdef SNPCALLER_MAIN


/* 
 * gcc -pedantic -Wall -g -std=gnu99 -O2 -DSNPCALLER_MAIN -o snpcaller snpcaller.c utils.c log.c
 * 
 * to test the pvalues (remember: large n and small p when comparing to binomial)
 *
 * >>> [scipy.stats.binom_test(x, 10000, 0.0001) for x in [2, 3, 4]]
 * [0.26424111735042727, 0.080292199242652212, 0.018982025450177534]
 * 
 * ./snpcaller 4 10000 0.0001
 * prob from snpcaller(): (.. 0.264204 .. 0.0802738 ..) 0.0189759
 *
 * 
 * to test probs:

 * >>> print(zip(range(1,11), scipy.stats.binom.pmf(range(1,11), 10000, 0.0001)))
 * [(1, 0.36789783621841865), (2, 0.18394891810853542), (3, 0.061310173792593993), (4, 0.015324477633007457), (5, 0.0030639759659701437), (6, 0.00051045837549934915), (7, 7.2886160113568433e-05), (8, 9.1053030054241777e-06), (9, 1.0109920728573426e-06), (10, 1.0101831983287595e-07)]
 *
 * ./snpcaller 10 10000 0.0001
 * prob for count 1=...
 *
 */
int main(int argc, char *argv[]) {
     int num_success;
     int num_trials;
     double succ_prob;
     verbose = 1;

     if (argc<4) {
          LOG_ERROR("%s\n", "need num_success num_trials and succ_prob as args");
          return -1;
     }
     num_success = atoi(argv[1]);
     num_trials = atoi(argv[2]);
     succ_prob = atof(argv[3]);

     LOG_VERBOSE("num_success=%d num_trials=%d succ_prob=%f\n", num_success, num_trials, succ_prob);



#ifdef PSEUDO_BINOMIAL
     {
          double pvalue;
          if (-1 == pseudo_binomial(&pvalue, 
                                    num_success, num_trials, succ_prob)) {
               LOG_ERROR("%s\n", "pseudo_binomial() failed");
               return -1;
          }
          printf("pseudo_binomial: %g\n", pvalue);
     }
#endif


#if 1
     {
          double snp_pvalues[NUM_NONCONS_BASES];
          int noncons_counts[NUM_NONCONS_BASES];
          double *err_probs;
          int i;

          if (NULL == (err_probs = malloc((num_trials) * sizeof(double)))) {
               fprintf(stderr, "FATAL: couldn't allocate memory at %s:%s():%d\n",
                       __FILE__, __FUNCTION__, __LINE__);
               return -1;
          }
          for (i=0; i<num_trials; i++) {
               err_probs[i] = succ_prob;
          }

          noncons_counts[0] = num_success;
          noncons_counts[1] = num_success-1;
          noncons_counts[2] = num_success-2;

          snpcaller(snp_pvalues, err_probs, num_trials, noncons_counts, 1, 1);

          printf("prob from snpcaller(): (.. -2:%g .. -1:%g ..) %g\n", snp_pvalues[2], snp_pvalues[1], snp_pvalues[0]);
          free(err_probs);
     }
#endif
}
#endif
