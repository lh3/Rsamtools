#ifndef TEMPLATE_H
#define TEMPALTE_H

#include <map>
#include <queue>
#include <string>
#include <algorithm>
#include <list>
#include "samtools/sam.h"
#include "scan_bam_data.h"

using namespace std;

class Template {
    typedef list<const bam1_t *> Segments;
    typedef Segments::iterator iterator;
    typedef Segments::const_iterator const_iterator;

    char *rg, *qname;
    Segments inprogress, ambiguous, invalid; 

    // FIXME: check RG retrieval
    const int readgroup_q(const bam1_t *mate) const {
    uint8_t *aux = bam_aux_get(mate, "RG");
    char *rg0 = '\0';
    if (aux != 0)
        rg0 = bam_aux2Z(aux);

        if (rg =='\0' && rg0 == '\0') /* both null ok */
            return 0;
        else
            return strcmp(rg, rg0);
    }

    const int qname_q(const bam1_t *mate) const {
        return strcmp(qname, bam1_qname(mate));
    }

public:

    Template() : rg('\0') {}

    bool empty() const {
        return inprogress.empty() && invalid.empty() &&
            ambiguous.empty();
    }

    // is_valid checks the following bit flags:
    // 1. Bit 0x1 (multiple segments) is 1 
    // 2. Bit 0x4 (segment unmapped) is 0
    // 3. Bit 0x8 (next segment unmapped) is 0
    // 4. 'mpos' != -1 (i.e. PNEXT = 0)
    bool is_valid(const bam1_t *bam) {
        const bool multi_seg = bam->core.flag & BAM_FPAIRED;
        const bool seg_unmapped = bam->core.flag & BAM_FUNMAP;
        const bool mate_unmapped = bam->core.flag & BAM_FMUNMAP;

        return  multi_seg && !seg_unmapped && 
                !mate_unmapped && (bam->core.mpos != -1);
    }

    bool is_template(const bam1_t *mate) const {
        return (readgroup_q(mate) == 0) && (qname_q(mate) == 0);
    }

    // is_mate checks the following bit flags:
    // 1. Bit 0x40 and 0x80: Segments are a pair of first/last OR
    //    neither segment is marked first/last
    // 2. Bit 0x100: Both segments are secondary OR both not secondary
    // 3. Bit 0x2: Both segments properly aligned 
    // 4. Bit 0x10 and 0x20: Segments are on opposite strands
    // 5. mpos match:
    //      segment1 mpos matches segment2 pos AND
    //      segment2 mpos matches segment1 pos
    // 6. tid match
    bool is_mate(const bam1_t *bam, const bam1_t *mate) const {
        const bool bam_read1 = bam->core.flag & BAM_FREAD1;
        const bool mate_read1 = mate->core.flag & BAM_FREAD1;
        const bool bam_read2 = bam->core.flag & BAM_FREAD2;
        const bool mate_read2 = mate->core.flag & BAM_FREAD2;
        const bool bam_secondary = bam->core.flag & BAM_FSECONDARY;
        const bool mate_secondary = mate->core.flag & BAM_FSECONDARY;
        const bool bam_proper = bam->core.flag & BAM_FPROPER_PAIR;
        const bool mate_proper = mate->core.flag & BAM_FPROPER_PAIR;
        const bool bam_rev = bam->core.flag & BAM_FREVERSE;
        const bool mate_rev = mate->core.flag & BAM_FREVERSE;
        const bool bam_mrev = bam->core.flag & BAM_FMREVERSE;
        const bool mate_mrev = mate->core.flag & BAM_FMREVERSE;
        return
            ((bam_read1 ^ bam_read2) && (mate_read1 ^ mate_read2)) &&
            (bam_read1 != mate_read1) &&
            (bam_secondary == mate_secondary) &&
            (bam_proper && mate_proper) &&
            ((bam_rev == mate_mrev) || (bam_mrev == mate_rev)) &&
            (bam->core.pos == mate->core.mpos) && 
            (bam->core.mpos == mate->core.pos) &&
            (bam->core.mtid == mate->core.tid);
    }

    void add_to_complete(const bam1_t *bam, const bam1_t *mate,
                         queue<Segments> &complete) {
        Segments tmp;
        if (bam->core.flag & BAM_FREAD1) {
            tmp.push_back(bam);
            tmp.push_back(mate);
        } else {
            tmp.push_back(mate);
            tmp.push_back(bam);
        } 
        complete.push(tmp);
    }

    // Returns true if segment added was a mate
    bool add_segment(const bam1_t *bam1) {
        bam1_t *bam = bam_dup1(bam1);
        if (!is_valid(bam)) {
            invalid.push_back(bam);
            return false;
        }

        // new template
        if (inprogress.empty()) {
            qname = bam1_qname(bam);
            uint8_t *aux = bam_aux_get(bam, "RG");
            if (aux != 0)
                rg = bam_aux2Z(aux);
        }
        inprogress.push_back(bam);
        return true;
    }

    void mate(queue<Segments> &complete) {
        const int unmated=-1, multiple=-2, processed=-3;
        vector<pair<int, const bam1_t *> >
            status(inprogress.size(), pair<int, const bam1_t *>(unmated, NULL));
        Segments::iterator it0;

        // identify unambiguous and ambigous mates
        it0 = inprogress.begin();
        for (int i = 0; i < inprogress.size(); ++i) {
            status[i].second = *it0;
            Segments::iterator it1 = it0;
            for (int j = i + 1; j < inprogress.size(); ++j) {
                ++it1;
                if (is_mate(*it0, *it1)) {
                    status[i].first = status[i].first == unmated ? j : multiple;
                    status[j].first = status[j].first == unmated ? i : multiple;
                }
            }
            ++it0;
        }

        // process unambiguous and ambigous mates
        for (int i = 0; i < status.size(); ++i) {
            if (status[i].first == unmated)
                continue;
            if (status[i].first >= 0 && status[status[i].first].first >= 0) {
                // unambiguous mates
                add_to_complete(status[i].second,
                                status[status[i].first].second,
                                complete);
                status[status[i].first].first = processed;
                status[i].first = processed;
            } else if (status[i].first != processed) {
                // ambigous mates, added to 'ambigous' queue
                ambiguous.push_back(status[i].second);
                status[i].first = processed;
            }
            ++it0;
        }

        // remove segments that have been assigned to complete or
        // ambiguous queue
        it0 = inprogress.begin();
        for (int i = 0; i != status.size(); ++i) {
            if (status[i].first == processed) {
                it0 = inprogress.erase(it0);
            } else {
                ++it0;
            }
        }
    }

    // (BamRangeIterator only)
    void mate_inprogress_segments(bamFile bfile, const bam_index_t * bindex,
                                  queue<Segments> &complete) {
        Segments found(inprogress);
        bam1_t *bam = bam_init1();

        // search for mate for each 'inprogress' segment
        iterator it = found.begin();
        while (it != found.end()) {
            bool touched = false;
            const bam1_t *curr = *it;
            const int tid = curr->core.mtid;
            const int beg = curr->core.mpos;

            // search all records that overlap the iterator
            bam_iter_t iter = bam_iter_query(bindex, tid, beg, beg + 1);
            while (bam_iter_read(bfile, iter, bam) >= 0) {
                if (beg == -1)
                    break;
                if (is_valid(bam) && is_template(bam) && is_mate(curr, bam))
                    touched = touched || add_segment(bam);
            }
            bam_iter_destroy(iter);

            if (touched)
                mate(complete);

            ++it;
        }

        bam_destroy1(bam);
    }

    // cleanup: move 'ambigous' to ambigous_queue; move 'inprogress'
    // and 'invalid' to iterator 'invalid_queue'
    void cleanup(queue<Segments> &ambiguous_queue,
                 queue<Segments> &invalid_queue) {
        if (!ambiguous.empty())
            ambiguous_queue.push(ambiguous);
        if  (!invalid.empty())
            inprogress.splice(inprogress.end(), invalid);
        if (!inprogress.empty()) {
            invalid_queue.push(inprogress);
            inprogress.clear();
        }
    }

};

#endif