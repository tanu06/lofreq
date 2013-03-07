#!/usr/bin/env python
"""Quality aware SNP caller.
"""


# Copyright (C) 2011, 2012 Genome Institute of Singapore
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.



#--- standard library imports
#
from __future__ import division
import logging

#--- third-party imports
#
# /

#--- project specific imports
#
# /
from lofreq_ext import snpcaller_qual
from lofreq import snp
from lofreq import conf


__author__ = "Andreas Wilm"
__email__ = "wilma@gis.a-star.edu.sg"
__copyright__ = "2011, 2012 Genome Institute of Singapore"
__license__ = "GPL2"


#global logger
# http://docs.python.org/library/logging.html
LOG = logging.getLogger("")
logging.basicConfig(level=logging.WARN,
                    format='%(levelname)s [%(asctime)s]: %(message)s')


def median(alist):
    """Compute median of list. Taken from
    http://stackoverflow.com/questions/7578689/median-code-explanation
    """
    
    srtd = sorted(alist) # returns a sorted copy
    mid = len(alist)//2 # double slash truncates unconditionally
    if len(alist) % 2 == 0:  # take the avg of middle two
        return (srtd[mid-1] + srtd[mid]) / 2.0
    else:
        return srtd[mid]

    
class QualBasedSNPCaller(object):
    """
    Quality/probabilty based SNP caller.
    """

    def __init__(self,
                 noncons_default_qual = conf.NONCONS_DEFAULT_QUAL,
                 noncons_filter_qual = conf.NONCONS_FILTER_QUAL,
                 ign_bases_below_q = conf.DEFAULT_IGN_BASES_BELOW_Q,
                 bonf_factor = 1,
                 sig_thresh = conf.DEFAULT_SIG_THRESH):
        """
        init function

        noncons_default_qual is the assumed quality of non-consensus bases,
        for which it is to be determined if they are an error or a SNP

        noncons_filter_qual is a threshold. non-consensus basepairs
        below this quality will be completely ignored.
        """

        # allowed to be 'median' instead of int
        assert isinstance(noncons_default_qual, int) or noncons_default_qual == 'median'
        assert isinstance(bonf_factor, int)
        assert isinstance(noncons_filter_qual, int)

        self.replace_noncons_quals = True
        self.noncons_default_qual = noncons_default_qual
        self.noncons_filter_qual = noncons_filter_qual
        self.ign_bases_below_q = ign_bases_below_q
        self.bonf_factor = bonf_factor
        self.sig_thresh = sig_thresh
        LOG.debug("New QualBasedSNPCaller: noncons default qual = %s"
                  ", noncons filter qual = %d"
                  ", ign_bases_below_q = %d"
                  ", bonferroni factor = %d"
                  ", sign. level = %f"
                  " and replace_noncons_quals = %s" % (
                self.noncons_default_qual,
                self.noncons_filter_qual,
                self.ign_bases_below_q,
                self.bonf_factor,
                self.sig_thresh,
                self.replace_noncons_quals))



    def call_snp_in_column(self, col_coord, base_qual_hist, ref_base):
        """Call SNP in one pileup colum given it's bases and qualities.
        """

        ret_snp_calls = []

        assert len(base_qual_hist.keys())==4, (
            "Expected exactly four bases as keys for base_qual_hist")
        for base in base_qual_hist.keys():
            assert base in 'ACGT', (
                "Only allowed bases/keys are A, C, G or T, but not %s" % base)
        assert ref_base in 'ACGT', (
            "consensus base must be one of A, C, G or T, but not %s" % ref_base)


        # count non-consensus bases and remove if their quality is
        # below noncons_filter_qual or ign_bases_below_q
        #
        noncons_counts_dict = dict()
        noncons_quals = []# NOTE: temporary only
        noncons_filterq = max(self.noncons_filter_qual, self.ign_bases_below_q)
        for b in base_qual_hist.keys():
            if b == ref_base:
                continue
            noncons_counts_dict[b] = sum(c for (q, c) in base_qual_hist[b].iteritems() 
                                         if q >= noncons_filterq)
            noncons_quals.extend([q for (q, c) in base_qual_hist[b].iteritems() 
                                  if q >= noncons_filterq])

        # return if no non-consbases left after filtering
        if sum(noncons_counts_dict.values()) == 0:
            LOG.debug("Consensus bases only. Early exit at col %d..." % col_coord)
            return []

        # get list of consensus qualities but remove if quality is
        # below ign_bases_below_q
        #
        cons_quals = []
        for (q, c) in base_qual_hist[ref_base].iteritems():
            if q < self.ign_bases_below_q:
                continue
            cons_quals.extend([q]*c)
        cons_count = len(cons_quals)

        # add a default value for each non consensus base
        # (WARNING: reusing cons_quals!)
        base_quals = cons_quals
        if self.noncons_default_qual == 'median':
            m = int(median(cons_quals)) # median might be float
            if False:
                # shouldn't actually use noncons qual as we would be
                # building values into the system that we're trying to
                # test
                m =  int(median((noncons_quals)))
            base_quals.extend(
                [m] * sum(noncons_counts_dict.values()))
        else:
            base_quals.extend(
                [self.noncons_default_qual] * sum(noncons_counts_dict.values()))
      
        
        # need noncons counts and bases in order since only counts are
        # handed down to snpcaller_qual
        noncons_bases = []
        noncons_counts = []
        for (b, c) in noncons_counts_dict.iteritems():
            noncons_bases.append(b)
            noncons_counts.append(c)
        noncons_bases = tuple(noncons_bases)
        noncons_counts = tuple(noncons_counts)

        # using sorted base_quals might in theory be better for speedup and
        # numerical stability. in practice I don't see a difference
        pvalues = snpcaller_qual(sorted(base_quals), noncons_counts,
                                 self.bonf_factor, self.sig_thresh)
        # pvalues only reported if pvalue * bonf < sig

        # setup info dictionary shared between different alleles for
        # this position
        #
        info_dict = dict()

        # note: coverage after filtering!
        coverage = sum(noncons_counts_dict.values()) + cons_count
        info_dict['coverage'] = coverage

        for (k, v) in noncons_counts_dict.iteritems():
            info_dict["basecount-%s" % k] = v
        info_dict["basecount-%s" % ref_base] = cons_count

        # check pvalues for all possible mutations and report if significant
        #
        for (base, count, pvalue) in zip(noncons_bases, noncons_counts, pvalues):
            if pvalue * self.bonf_factor < self.sig_thresh:
                info_dict['pvalue'] = pvalue
                snpcall = snp.ExtSNP(col_coord, ref_base, base,
                                     count/float(coverage), info_dict)
                ret_snp_calls.append(snpcall)


        return ret_snp_calls


        
if __name__ == "__main__":
    pass