/*----------------------------------------------------------------------
This source distribution is placed in the public domain by its author,
Ben Buhrow. You may use it for any purpose, free of charge,
without having to notify anyone. I disclaim any responsibility for any
errors.

Optionally, please be nice and tell me if you find this source to be
useful. Again optionally, if you add to the functionality present here
please consider making those additions public too, so that others may 
benefit from your work.	

Some parts of the code (and also this header), included in this 
distribution have been reused from other sources. In particular I 
have benefitted greatly from the work of Jason Papadopoulos's msieve @ 
www.boo.net/~jasonp, Scott Contini's mpqs implementation, and Tom St. 
Denis Tom's Fast Math library.  Many thanks to their kind donation of 
code to the public domain.
       				   --bbuhrow@gmail.com 11/24/09
----------------------------------------------------------------------*/

#include "qs.h"
#include "qs_impl.h"
#include "ytools.h"
#include "common.h"
#include "cofactorize.h"
#ifdef USE_BATCH_FACTOR
#include "batch_factor.h"
#endif

//#define SIQSDEBUG 1

/*
We are given an array of bytes that has been sieved.  The basic trial 
division strategy is as follows:

1) Scan through the array and 'mark' locations that meet criteria 
indicating they may factor completely over the factor base.  

2) 'Filter' the marked locations by trial dividing by small primes
that we did not sieve.  These primes are all less than 256.  If after
removing small primes the location does not meet another set of criteria,
remove it from the 'marked' list (do not subject it to further trial
division).

3) Divide out primes from the factor base between 256 and 2^13 or 2^14, 
depending on the version (2^13 for 32k version, 2^14 for 64k).  

4) Resieve primes between 2^{13|14} and 2^16, max.  

5) Primes larger than 2^16 will have been bucket sieved.  Remove these
by scanning the buckets for sieve hits equal to the current block location.

6) If applicable/appropriate, factor a remaining composite with squfof

this file contains code implementing 6) as well as other auxiliary routines


*/

void trial_divide_Q_siqs(uint32_t report_num,  uint8_t parity, 
						 uint32_t poly_id, uint32_t bnum, 
						 static_conf_t *sconf, dynamic_conf_t *dconf)
{
	//we have flagged this sieve offset as likely to produce a relation
	//nothing left to do now but check and see.
	uint64_t q64, f64;
	int j, i, it;
	uint32_t prime;
	int dlp_done = 0;
	int smooth_num;
	uint32_t *fb_offsets;
	uint32_t polya_factors[20];
    sieve_fb_compressed *fbc = dconf->comp_sieve_p;
	uint32_t offset, block_loc;
	fb_offsets = &dconf->fb_offsets[report_num][0];
	smooth_num = dconf->smooth_num[report_num];
	block_loc = dconf->reports[report_num];
    int VFLAG = sconf->obj->VFLAG;
    int THREADS = sconf->obj->THREADS;

	offset = (bnum << sconf->qs_blockbits) + block_loc;

    //gmp_printf("commencing tdiv on %Zd\n", dconf->Qvals[report_num]);

	// check for additional factors of the a-poly factors
	// make a separate list then merge it with fb_offsets
	it=0;	// max 20 factors allocated for - should be overkill
	for (j = 0; (j < dconf->curr_poly->s) && (it < 20); j++)
	{
        prime = fbc->prime[dconf->curr_poly->qlisort[j]]; // .prime;

		while ((mpz_tdiv_ui(dconf->Qvals[report_num],prime) == 0) && (it < 20))
		{
			mpz_tdiv_q_ui(dconf->Qvals[report_num], dconf->Qvals[report_num], prime);

#ifndef SPARSE_STORE
			polya_factors[it++] = dconf->curr_poly->qlisort[j];
#endif
		}
	}

#if 0 //def SPARSE_STORE
    // edit: removed because med_B seems to be a little too high
    // of a bound for non-stored factors.  Puts quite a bit of 
    // extra work in the filtering code.  Current bound is the
    // small prime bound.

    // only actually store indices of primes larger than med_B.
    // If the relation ultimately proves useful during filtering
    // then we will trial divide it there to recover its divisors,
    // including any factors of the A poly.
    for (j = 0, i = 0; j <= smooth_num; j++)
    {
        if (fb_offsets[j] >= sconf->factor_base->med_B)
        {
            fb_offsets[i++] = fb_offsets[j];
        }
    }
    smooth_num = i - 1;
#endif
    
	// check if it completely factored by looking at the unfactored portion in tmp
	if ((mpz_size(dconf->Qvals[report_num]) == 1) && 
		(mpz_cmp_ui(dconf->Qvals[report_num], sconf->large_prime_max) < 0))
	{
        // save this slp (single large prime)
		uint32_t large_prime[3];
		
		large_prime[0] = (uint32_t)mpz_get_ui(dconf->Qvals[report_num]); //Q->val[0];
		large_prime[1] = 1;
		large_prime[2] = 1;

		if (large_prime[0] == 1)
			dconf->num_full++;
		else
			dconf->num_slp++;

		//add this one
		if (sconf->is_tiny)
		{	
            buffer_relation(offset, large_prime, smooth_num + 1,
                fb_offsets, dconf->curr_poly->index, poly_id,
                parity, dconf, polya_factors, it, 1);
		}
        else
        {
            buffer_relation(offset, large_prime, smooth_num + 1,
                fb_offsets, dconf->curr_poly->index, poly_id, 
                parity, dconf, polya_factors, it, 1);
        }

		return;
	}

    // if we are not considering dlps then we are done.
	if (sconf->use_dlp == 0)
		return;

	// quick check if Q is way too big for DLP (more than 64 bits)	
	if (mpz_sizeinbase(dconf->Qvals[report_num], 2) < 64)
	{
		uint64_t res;

        // not obviously too big.  So now check if we are actually
        // within the defined bounds for DLPs.
		q64 = mpz_get_64(dconf->Qvals[report_num]);

		if ((q64 > sconf->max_fb2) && (q64 < sconf->large_prime_max2))
		{

            if ((sconf->use_dlp == 2) && (dconf->do_batch) && 1)
            {
                int32_t soffset = offset;

                if (parity)
                {
                    soffset *= -1;
                }

                mpz_set_ui(dconf->gmptmp1, 0);

                // use this field to record how many we've batched.
                dconf->attempted_cosiqs++;
                relation_batch_add(dconf->curr_poly->index, poly_id, soffset, fb_offsets, smooth_num + 1,
                    dconf->Qvals[report_num], NULL, 0, dconf->gmptmp1, NULL, &dconf->rb);

                // start processing the relations in the indicated buffer if running
                // single threaded.  otherwise, let the threading dispatcher assign 
                // the processing to a free thread.
                if ((dconf->batch_run_override > 0) ||
                    ((THREADS == 1) && (sconf->rb[0].num_relations > sconf->rb[0].target_relations)))
                {
                    struct timeval start;
                    struct timeval stop;
                    double ttime;
                    relation_batch_t* rb;
                    int i;

                    if (THREADS == 1)
                        rb = &sconf->rb[0];
                    else
                        rb = &sconf->rb[dconf->batch_run_override - 1];
                    
                    if (VFLAG > 1)
                    {
                        printf("now processing %u relations in batch %d in thread %d\n",
                            rb->num_relations, dconf->batch_run_override, dconf->tid);
                    }

                    gettimeofday(&start, NULL);
                    relation_batch_run(rb, &dconf->lcg_state);
                    gettimeofday(&stop, NULL);

                    ttime = ytools_difftime(&start, &stop);
                    if (VFLAG > 1)
                    {
                        printf("relation_batch_run took %1.4f sec producing %u tlp's\n",
                            ttime, rb->num_success);
                    }

                    rb->conversion_ratio =
                        (double)rb->num_success / (double)rb->num_relations;

                    // take our new tlp relations and buffer them to be
                    // saved out to the data file.
                    for (i = 0; i < rb->num_relations; i++)
                    {
                        cofactor_t* c = rb->relations + i;
                        uint32_t* f = rb->factors + c->factor_list_word;

                        if (c->success)
                        {
                            uint8_t parity = c->signed_offset < 0 ? 1 : 0;

                            if (c->success == 3)
                                dconf->tlp_useful++;
                            else if (c->success == 2)
                                dconf->dlp_useful++;
                            else if (c->success == 1)
                                dconf->num_slp++;

                            buffer_relation(abs(c->signed_offset), c->lp_r, c->num_factors_r,
                                f, c->a, c->b, parity, dconf, NULL, 0, 1);
                        }
                    }

                    if (VFLAG > 1)
                    {
                        printf("done processing batch %d in thread %d, found %u relations\n",
                            dconf->batch_run_override, dconf->tid, rb->num_success);
                    }

                    // clear the relation batch now that we are done processing it.
                    rb->num_relations = 0;
                    rb->num_success = 0;
                    rb->num_factors = 0;

                    // signal we are done processing the batch.
                    if (THREADS > 1)
                        dconf->batch_run_override = -1;
                }

                // if batch factoring, we're done now.
                return;
            }


			//quick prime check: compute 2^(residue-1) mod residue.  

#if defined(_MSC_VER) || (BITS_PER_DIGIT == 32)
			mpz_set_64(dconf->gmptmp1, q64);
			mpz_set_64(dconf->gmptmp2, 2);
			//mpz_set_64(dconf->gmptmp3, q64 - 1);

			mpz_powm_ui(dconf->gmptmp1, dconf->gmptmp2, q64 - 1, dconf->gmptmp1);
			res = mpz_get_64(dconf->gmptmp1);
#elif defined (FORCE_GENERIC) && !defined(TARGET_KNC)
			mpz_set_64(dconf->gmptmp1, q64);
			mpz_set_64(dconf->gmptmp2, 2);
			mpz_set_64(dconf->gmptmp3, q64 - 1);

			mpz_powm(dconf->gmptmp1, dconf->gmptmp2, dconf->gmptmp3, dconf->gmptmp1);
			res = mpz_get_64(dconf->gmptmp1);
#else
			// meh, not really any faster, but fun to write...
			res = spPRP2(q64);
            //res = pow2m(q64 - 1, q64);
#endif

			// if equal to 1, assume it is prime.  this may be wrong sometimes, but we don't care.
			// more important to quickly weed out probable primes than to spend more time to be
			// more sure.
			if (res == 1)
			{
				dconf->dlp_prp++;
				return;
			}

			// try to find a double large prime.
            // now with superfast ecm, squfof, rho, etc are obsolete.
            // but we still for now use "attempted_squfof" to indicate
            // that we are attempting to factor the potential dlp 
            // residue.
			dconf->attempted_squfof++;
            f64 = do_uecm(q64);
			
			if ((f64 > 1) && (f64 != q64) && (q64 % f64 == 0))
			{
				uint32_t large_prime[3];

				large_prime[0] = (uint32_t)f64;
				large_prime[1] = (uint32_t)(q64 / f64);
				large_prime[2] = 1;

				if ((large_prime[0] < sconf->large_prime_max) &&
					(large_prime[1] < sconf->large_prime_max))
				{
					//add this one
					dconf->dlp_useful++;
					buffer_relation(offset, large_prime, smooth_num + 1,
						fb_offsets, dconf->curr_poly->index, poly_id, parity,
                        dconf, polya_factors, it, 1);
				}
			}
			else
			{
				dconf->failed_squfof++;
			}

			// whether we found a DLP or not, we are done checking.
			return;
		}
		else
		{
			dconf->dlp_outside_range++;
			
			// too big for DLP, but too small for TLP (if active).
			return;
		}
	}
	else
    {
        dconf->dlp_outside_range++;
    }

    // if we are not considering TLP, then we are done.
	if (sconf->use_dlp < 2)
		return;
    
	// quick check if Q is obviously too big.
	if (mpz_sizeinbase(dconf->Qvals[report_num], 2) < 96)
	{
		uint64_t res;
		uint32_t numit = 128;

		double qfloat = mpz_get_d(dconf->Qvals[report_num]);

//#define OUTPUT_TLP_ATTEMPT_DETAILS

        // not obviously too big, see if it is actually within
        // the defined tlp bounds.
		if ((qfloat > sconf->max_fb3) && (qfloat < sconf->large_prime_max3))
		{
			uint32_t large_prime[3];
			uint32_t r;

#ifdef OUTPUT_TLP_ATTEMPT_DETAILS
			FILE *fid;
			char fname[20];
			sprintf(fname, "tlp_attempts.dat");
#endif

            if (dconf->do_batch)
            {
                int32_t soffset = offset;
                
                if (parity)
                {
                    soffset *= -1;
                }

                mpz_set_ui(dconf->gmptmp1, 0);

                // use this field to record how many we've batched.
                dconf->attempted_cosiqs++;
                relation_batch_add(dconf->curr_poly->index, poly_id, soffset, fb_offsets, smooth_num + 1,
                    dconf->Qvals[report_num], NULL, 0, dconf->gmptmp1, NULL, &dconf->rb);

                // the relation batch persists across polynomials (we save enough
                // info to know which polynomial the relation belongs to).  When we've 
                // reached our target watermark the following batch processing will
                // save valid relations off to a buffer.  That buffer in turn
                // will get merged into the top-level relation structure and
                // eventual TLP filtering.  Reset the batch structure when finished.
                //printf("batched %u of %u target relations\n", 
                //    dconf->rb.num_relations, dconf->rb.target_relations);

                // rough in a flag that we can eventually toggle from calling code,
                // forcing the batch to be processed (for example on abort or after
                // determination that we have enough relations for final filtering).
                // right now this flag isn't used and any relations in the batch
                // buffer are lost on abort or when post-processing starts.

                // one other thing we could do is switch to non-batch processing
                // when post-processing is getting close... or incrementally 
                // lower the target_relations watermark.

                //if ((dconf->rb.num_relations >= sconf->rb.target_relations) &&
                //    (dconf->batch_run_override == 1))

                // start processing the relations in the indicated buffer if running
                // single threaded.  otherwise, let the threading dispatcher assign 
                // the processing to a free thread.
                if ((dconf->batch_run_override > 0) ||
                    ((THREADS == 1) && (sconf->rb[0].num_relations > sconf->rb[0].target_relations)))
                {
                    struct timeval start;
                    struct timeval stop;
                    double ttime;
                    relation_batch_t *rb;
                    int i;

                    if (THREADS == 1)
                        rb = &sconf->rb[0];
                    else
                        rb = &sconf->rb[dconf->batch_run_override - 1];

                    //if (dconf->batch_run_override == 1)
                    //{
                    //    rb = &sconf->rb;
                    //}
                    //else
                    //{
                    //    rb = &sconf->rb2;
                    //}
                    
                    if (VFLAG > 1)
                    {
                        printf("now processing %u relations in batch %d in thread %d\n",
                            rb->num_relations, dconf->batch_run_override, dconf->tid);
                    }

                    //if (VFLAG > 0)
                    //{
                    //    printf("processing batch relations\n");
                    //}

                    //relation_batch_run(&dconf->rb);

                    gettimeofday(&start, NULL);
                    relation_batch_run(rb, &dconf->lcg_state);
                    gettimeofday(&stop, NULL);

                    ttime = ytools_difftime(&start, &stop);
                    if (VFLAG > 1)
                    {
                        printf("relation_batch_run took %1.4f sec producing %u tlp's\n", 
                            ttime, rb->num_success);
                    }

                    //dconf->rb.conversion_ratio = 
                    //    (double)dconf->rb.num_success / (double)dconf->rb.num_relations;
                    rb->conversion_ratio =
                          (double)rb->num_success / (double)rb->num_relations;

                    //if (VFLAG > 0)
                    //{
                    //    printf("found %u new relations in batch of %u\n", 
                    //        dconf->rb.num_success, dconf->rb.num_relations);
                    //}

                    // take our new tlp relations and buffer them to be
                    // saved out to the data file.
                    for (i = 0; i < rb->num_relations; i++)
                    {
                        cofactor_t *c = rb->relations + i;
                        uint32_t *f = rb->factors + c->factor_list_word;

                        if (c->success)
                        {
                            uint8_t parity = c->signed_offset < 0 ? 1 : 0;

                            if (c->success == 3)
                                dconf->tlp_useful++;
                            else if (c->success == 2)
                                dconf->dlp_useful++;
                            else if (c->success == 1)
                                dconf->num_slp++;

                            buffer_relation(abs(c->signed_offset), c->lp_r, c->num_factors_r,
                                f, c->a, c->b, parity, dconf, NULL, 0, 1);
                        }
                    }

                    if (VFLAG > 1)
                    {
                        printf("done processing batch %d in thread %d, found %u relations\n",
                            dconf->batch_run_override, dconf->tid, rb->num_success);
                    }

                    // clear the relation batch now that we are done processing it.
                    rb->num_relations = 0;
                    rb->num_success = 0;
                    rb->num_factors = 0;

                    // signal we are done processing the batch.
                    if (THREADS > 1)
                        dconf->batch_run_override = -1;
                }
                
                // if batch factoring, we're done now.
                return;
            }

			//FILE *tlp;
			//tlp = fopen("tlp_attempts.txt", "a");
			//gmp_fprintf(tlp, "%Zd\n", dconf->Qvals[report_num]);
			//fclose(tlp);

			//mpz_set_ui(dconf->gmptmp1, 2);
			//mpz_set(dconf->gmptmp2, dconf->Qvals[report_num]);
			//mpz_sub_ui(dconf->gmptmp2, dconf->gmptmp2, 1);
			//mpz_powm(dconf->gmptmp3, dconf->gmptmp1, dconf->gmptmp2, dconf->Qvals[report_num]);

            // if we're here, that means we're not batch factoring.  So we
            // process each potential tlp as it comes, just like with slp
            // and dlp relations.  First step is to reject primes.
            // could maybe use a quicker and dirtier method to reject primes...
			res = mpz_probab_prime_p(dconf->Qvals[report_num], 1);

			if (res) //mpz_cmp_ui(dconf->gmptmp3, 1) == 0)
			{
#ifdef OUTPUT_TLP_ATTEMPT_DETAILS
				fid = fopen(fname, "a");
				fprintf(fid, "%1.0lf,0\n", qfloat);
				fclose(fid);
#endif
				dconf->tlp_prp++;
				return;
			}

			// now with superfast ecm, tinyqs is obsolete.  But we
            // still use "attempted_cosiqs" to track how many tlp's
            // we've tried to factor.
			dconf->attempted_cosiqs++;

			mpz_set_ui(dconf->gmptmp1, 0);
			mpz_set_ui(dconf->gmptmp2, 0);

#if defined(__MINGW64__)
            // TODO: get tinyqs working on mingw64
            if (tinyqs(dconf->cosiqs, dconf->Qvals[report_num], dconf->gmptmp1, dconf->gmptmp2))
            {
                // factors from tinyqs are prime
                // large prime bound will never exceed 32 bits
                if (mpz_sizeinbase(dconf->gmptmp1, 2) > 32)
                    return;

                if (mpz_sizeinbase(dconf->gmptmp2, 2) > 32)
                    return;

                large_prime[0] = mpz_get_ui(dconf->gmptmp1);
                large_prime[1] = mpz_get_ui(dconf->gmptmp2);

                // sanity checks
                if ((large_prime[0] == 0) || (large_prime[1] == 0))
                    return;

                r = mpz_tdiv_q_ui(dconf->gmptmp2, dconf->Qvals[report_num], large_prime[0]);
                if (r != 0)
                {
                    printf("tlp problem: r != 0\n");
                    dconf->failed_cosiqs++;
                    return;
                }

                r = mpz_tdiv_q_ui(dconf->gmptmp2, dconf->gmptmp2, large_prime[1]);
                if (r != 0)
                {
                    printf("tlp problem: r != 0\n");
                    dconf->failed_cosiqs++;
                    return;
                }

                // large prime bound will never exceed 32 bits
                if (mpz_sizeinbase(dconf->gmptmp2, 2) > 32)
                    return;

                large_prime[2] = mpz_get_ui(dconf->gmptmp2);

                if ((large_prime[0] < sconf->large_prime_max) &&
                    (large_prime[1] < sconf->large_prime_max) &&
                    (large_prime[2] < sconf->large_prime_max))
                {
#ifdef OUTPUT_TLP_ATTEMPT_DETAILS
                    fid = fopen(fname, "a");
                    fprintf(fid, "%1.0lf,1\n", qfloat);
                    fclose(fid);
#endif

                    //if (large_prime[2] < (uint32_t)sconf->pmax)
                    //	printf("tlp with small 3rd prime: %u,%u,%u\n",
                    //		large_prime[0], large_prime[1], large_prime[2]);

                    //gmp_printf("tlp: %Zd = %u,%u,%u\n",
                    //	dconf->Qvals[report_num], large_prime[0], large_prime[1], large_prime[2]);

                    //add this one
                    dconf->tlp_useful++;
                    buffer_relation(offset, large_prime, smooth_num + 1,
                        fb_offsets, dconf->curr_poly->index, poly_id, parity, dconf,
                        polya_factors, it, 1);
                }

            }
#else
			if (1)
			{
				int B1, B2, curves, bits = mpz_sizeinbase(dconf->Qvals[report_num], 2);
                // successful tlps will have 3 about equal size factors, so
                // we first try to pull out one of them.
				int targetBits = bits / 3 + 1;
				if (targetBits <= 25)
				{
					B1 = 70;
					curves = 16;
				}
				else if (targetBits <= 26)
				{
					B1 = 85;
					curves = 16;
				}
				else if (targetBits <= 29)
				{
					B1 = 125;
					curves = 16;
				}
				else if (targetBits <= 31)
				{
					B1 = 165;
					curves = 24;
				}
				else if (targetBits <= 32)
				{
					B1 = 205;
					curves = 24;
				}
				else
				{
					printf("something's wrong, bits = %u, targetBits = %u\n", bits, targetBits);
				}

				tinyecm(dconf->Qvals[report_num], dconf->gmptmp1, B1, B1 * 25, 
                    curves, &dconf->lcg_state, 0);
				if (mpz_sizeinbase(dconf->gmptmp1, 2) > 32)
				{
					return;
				}

				large_prime[0] = mpz_get_ui(dconf->gmptmp1);
				if ((large_prime[0] > 1) && (large_prime[0] < sconf->large_prime_max))
				{
					// first factor qualifies.  
					// divide out factor with sanity check
					r = mpz_tdiv_q_ui(dconf->gmptmp2, dconf->Qvals[report_num], large_prime[0]);
					if (r != 0)
					{
						gmp_printf("tlp0 problem: r != 0, Q = %Zd, LP = %u\n", 
                            dconf->Qvals[report_num], large_prime[0]);
						dconf->failed_cosiqs++;
						return;
					}

                    // if the remaining residue is obviously too big, we're done.
					if (mpz_sizeinbase(dconf->gmptmp2, 2) > 64)
					{
						return;
					}

					// check if the residue is prime.  could again use
                    // a cheaper method.
					res = mpz_probab_prime_p(dconf->gmptmp2, 1);

					if (res)
					{
#ifdef OUTPUT_TLP_ATTEMPT_DETAILS
						fid = fopen(fname, "a");
						fprintf(fid, "%1.0lf,0\n", qfloat);
						fclose(fid);
#endif
						dconf->tlp_prp++;
						return;
					}

					// ok, so we have extracted one suitable factor, and the 
					// cofactor is not prime.  Do more work to split the cofactor,
					// which is now <= 64 bits in size.
					q64 = mpz_get_ui(dconf->gmptmp2);

                    // todo: target this better based on expected factor size.
                    f64 = do_uecm(q64);
					mpz_set_ui(dconf->gmptmp1, f64);

                    // check if the factor we found is obviously too big.
					if (mpz_sizeinbase(dconf->gmptmp1, 2) > 32)
					{
						return;
					}

                    // this should probably come before the above 
                    // check for a too-large factor.
					large_prime[1] = f64;
					if ((large_prime[1] <= 1) || (f64 == q64))
					{
						dconf->failed_cosiqs++;
						return;
					}

                    // after a lot of use this has never come up, so
                    // can probably abandon this check
					// sanity check:
					r = q64 % large_prime[1];
					if (r != 0)
					{
                        printf("tlp1 problem: r != 0, Q = %lu, LP = %u\n",
                            q64, large_prime[1]);
						dconf->failed_cosiqs++;
						return;
					}

                    // divide out the factor we found and check if what's
                    // left is obviously too big.
					q64 /= large_prime[1];
					mpz_set_ui(dconf->gmptmp1, q64);
					if (mpz_sizeinbase(dconf->gmptmp1, 2) > 32)
					{
						return;
					}

					large_prime[2] = q64;

                    // is this even true?  maybe we've found a dlp somehow?
					if (large_prime[2] <= 1)
					{
						dconf->failed_cosiqs++;
						return;
					}

                    // with factorization done and obvious failures abandoned,
                    // do one last check to make sure this is a valid tlp.
					if ((large_prime[0] < sconf->large_prime_max) &&
						(large_prime[1] < sconf->large_prime_max) &&
						(large_prime[2] < sconf->large_prime_max))
					{
#ifdef OUTPUT_TLP_ATTEMPT_DETAILS
						fid = fopen(fname, "a");
						fprintf(fid, "%1.0lf,1\n", qfloat);
						fclose(fid);
#endif

						// add this one
						dconf->tlp_useful++;
						buffer_relation(offset, large_prime, smooth_num + 1,
							fb_offsets, dconf->curr_poly->index, poly_id, parity, dconf,
                            polya_factors, it, 1);
					}
				}
				else
				{
					return;
				}

			}
            else if (0) //(tinyqs(dconf->cosiqs, dconf->Qvals[report_num], dconf->gmptmp1, dconf->gmptmp2))
            {
                // factors from tinyqs are prime
                // large prime bound will never exceed 32 bits
                if (mpz_sizeinbase(dconf->gmptmp1, 2) > 32)
                    return;

                if (mpz_sizeinbase(dconf->gmptmp2, 2) > 32)
                    return;

                large_prime[0] = mpz_get_ui(dconf->gmptmp1);
                large_prime[1] = mpz_get_ui(dconf->gmptmp2);

                // sanity checks
                if ((large_prime[0] == 0) || (large_prime[1] == 0))
                    return;

                r = mpz_tdiv_q_ui(dconf->gmptmp2, dconf->Qvals[report_num], large_prime[0]);
                if (r != 0)
                {
                    printf("tlp problem: r != 0\n");
                    dconf->failed_cosiqs++;
                    return;
                }

                r = mpz_tdiv_q_ui(dconf->gmptmp2, dconf->gmptmp2, large_prime[1]);
                if (r != 0)
                {
                    printf("tlp problem: r != 0\n");
                    dconf->failed_cosiqs++;
                    return;
                }

                // large prime bound will never exceed 32 bits
                if (mpz_sizeinbase(dconf->gmptmp2, 2) > 32)
                    return;

                large_prime[2] = mpz_get_ui(dconf->gmptmp2);

                if ((large_prime[0] < sconf->large_prime_max) &&
                    (large_prime[1] < sconf->large_prime_max) &&
                    (large_prime[2] < sconf->large_prime_max))
                {
#ifdef OUTPUT_TLP_ATTEMPT_DETAILS
                    fid = fopen(fname, "a");
                    fprintf(fid, "%1.0lf,1\n", qfloat);
                    fclose(fid);
#endif

                    //if (large_prime[2] < (uint32_t)sconf->pmax)
                    //	printf("tlp with small 3rd prime: %u,%u,%u\n",
                    //		large_prime[0], large_prime[1], large_prime[2]);

                    //gmp_printf("tlp: %Zd = %u,%u,%u\n",
                    //	dconf->Qvals[report_num], large_prime[0], large_prime[1], large_prime[2]);

                    //add this one
                    dconf->tlp_useful++;
                    buffer_relation(offset, large_prime, smooth_num + 1,
                        fb_offsets, dconf->curr_poly->index, poly_id, parity, dconf,
                        polya_factors, it, 1);
                }

            }
#endif
			else
			{
				dconf->failed_cosiqs++;
			}
		}
		else
		{
			dconf->tlp_outside_range++;
		}
	}
	else
	{
		dconf->tlp_outside_range++;
	}
	
	return;
}

void buffer_relation(uint32_t offset, uint32_t *large_prime, uint32_t num_factors,
    uint32_t *fb_offsets, uint32_t apoly_id, uint32_t bpoly_id, uint32_t parity,
    dynamic_conf_t *conf, uint32_t *polya_factors,
    uint32_t num_polya_factors, uint64_t unfactored_residue)
{
    // put this relations's info into a temporary buffer which
    // will get merged with other such buffers (if multi-threaded) and
    // dumped to file once the threads are joined.
    siqs_r *rel;
    uint32_t i, j, k;

    // first check that this relation won't overflow the buffer
    if (conf->buffered_rels >= conf->buffered_rel_alloc)
    {
        //printf("reallocating relation buffer\n");
        conf->relation_buf = (siqs_r *)realloc(conf->relation_buf,
            conf->buffered_rel_alloc * 2 * sizeof(siqs_r));

        if (conf->relation_buf == NULL)
        {
            printf("error re-allocating temporary storage of relations\n");
            exit(-1);
        }
        conf->buffered_rel_alloc *= 2;
    }

    // then stick all the info in the buffer
    rel = conf->relation_buf + conf->buffered_rels;

    rel->sieve_offset = offset;
    rel->parity = parity;
    rel->apoly_idx = apoly_id;
    rel->poly_idx = bpoly_id;

    if ((num_polya_factors + num_factors) > MAX_SMOOTH_PRIMES)
    {
        printf("error: too many smooth primes!\n");
        exit(234);
    }

#ifdef SPARSE_STORE
    // extra factors of polya are not added to the list of factors. they will be added
    // during filtering on only the relations that survive singleton removal.
    memcpy(rel->fb_offsets, fb_offsets, sizeof(uint32_t) * num_factors);
    rel->num_factors = num_factors;

#else
    //merge in extra factors of the apoly factors
    i = j = k = 0;
    while (k < num_factors && j < num_polya_factors) {
        if (fb_offsets[k] < polya_factors[j]) {
            rel->fb_offsets[i++] = fb_offsets[k++];
        }
        else if (fb_offsets[k] > polya_factors[j]) {
            rel->fb_offsets[i++] = polya_factors[j++];
        }
        else {
            rel->fb_offsets[i++] = fb_offsets[k++];
            rel->fb_offsets[i++] = polya_factors[j++];
        }
    }
    while (k < num_factors)
        rel->fb_offsets[i++] = fb_offsets[k++];
    while (j < num_polya_factors)
        rel->fb_offsets[i++] = polya_factors[j++];

    rel->num_factors = num_factors + num_polya_factors;
#endif

    

#ifdef USE_VEC_SQUFOF

    if (unfactored_residue > 1)
    {
        if (conf->num_64bit_residue >= 4096)
        {
            printf("uh-oh, too many residues\n"); fflush(stdout);
        }

        //printf("adding %lu to unfactored residue list in position %d, relation %d\n", 
        //    unfactored_residue, conf->num_64bit_residue, conf->buffered_rels);

        conf->unfactored_residue[conf->num_64bit_residue] = unfactored_residue;        

        // use this to signify we need to add factors later.
        rel->large_prime[0] = 0xffffffff;
        conf->num_64bit_residue++;
    }
    else
    {
        rel->large_prime[0] = large_prime[0];
        rel->large_prime[1] = large_prime[1];
        rel->large_prime[2] = large_prime[2];
    }

#else
    
    rel->large_prime[0] = large_prime[0];
	rel->large_prime[1] = large_prime[1];
	rel->large_prime[2] = large_prime[2];

#endif


	conf->buffered_rels++;
	return;
}

void save_relation_siqs(uint32_t offset, uint32_t *large_prime, uint32_t num_factors, 
    uint32_t *fb_offsets, uint32_t poly_id, uint32_t apoly_id, uint32_t parity,
	static_conf_t *conf)
{
	char buf[1024];
	fact_obj_t *obj = conf->obj;
	uint32_t i, k;
	uint32_t lp[3];

	if (conf->in_mem)
	{
		// copy info to sconf relation structure
		siqs_r *r;

		//first check that this relation won't overflow the buffer
		if (conf->buffered_rels >= conf->buffered_rel_alloc)
		{
			//printf("reallocating in-mem relation storage...\n");
			conf->in_mem_relations = (siqs_r *)realloc(conf->in_mem_relations, 
				conf->buffered_rel_alloc * 2 * sizeof(siqs_r));
			if (conf->in_mem_relations == NULL)
			{
				printf("error re-allocating in-memory storage of relations\n");
				exit(-1);
			}
			conf->buffered_rel_alloc *= 2;
		}

		r = conf->in_mem_relations + conf->buffered_rels++;

        if (conf->use_dlp == 2)
        {
            // store them sorted in ascending order
            if (large_prime[0] < large_prime[1])
            {
                if (large_prime[1] < large_prime[2])
                {
                    r->large_prime[0] = large_prime[0];
                    r->large_prime[1] = large_prime[1];
                    r->large_prime[2] = large_prime[2];
                }
                else
                {
                    if (large_prime[2] < large_prime[0])
                    {
                        r->large_prime[0] = large_prime[2];
                        r->large_prime[1] = large_prime[0];
                        r->large_prime[2] = large_prime[1];
                    }
                    else
                    {
                        r->large_prime[0] = large_prime[0];
                        r->large_prime[1] = large_prime[2];
                        r->large_prime[2] = large_prime[1];
                    }
                }
            }
            else
            {
                if (large_prime[0] < large_prime[2])
                {
                    r->large_prime[0] = large_prime[1];
                    r->large_prime[1] = large_prime[0];
                    r->large_prime[2] = large_prime[2];
                }
                else
                {
                    if (large_prime[2] < large_prime[1])
                    {
                        r->large_prime[0] = large_prime[2];
                        r->large_prime[1] = large_prime[1];
                        r->large_prime[2] = large_prime[0];
                    }
                    else
                    {
                        r->large_prime[0] = large_prime[1];
                        r->large_prime[1] = large_prime[2];
                        r->large_prime[2] = large_prime[0];
                    }
                }
            }
        }
        else
        {
            if (large_prime[0] < large_prime[1])
            {
                r->large_prime[0] = large_prime[0];
                r->large_prime[1] = large_prime[1];
            }
            else
            {
                r->large_prime[1] = large_prime[0];
                r->large_prime[0] = large_prime[1];
            }
            r->large_prime[2] = large_prime[2];
        }

		r->num_factors = num_factors;
        r->apoly_idx = apoly_id;
		r->poly_idx = poly_id;
		r->parity = parity;
		r->sieve_offset = offset;
        if (num_factors > MAX_SMOOTH_PRIMES)
        {
            printf("error: num_factors (%u) exceeds maximum (%u)!\n",
                num_factors, MAX_SMOOTH_PRIMES);
        }

		for (i=0; i < MIN(num_factors, MAX_SMOOTH_PRIMES); i++)
			r->fb_offsets[i] = fb_offsets[i];

	}
	else
	{
		//store to file
		i = sprintf(buf, "R ");

		if (parity)
			i += sprintf(buf + i, "-%x ", offset);
		else
			i += sprintf(buf + i, "%x ", offset);
	
		i += sprintf(buf + i, "%x ", poly_id);
	
		k = 0;
		while (k < num_factors)
			i += sprintf(buf + i, "%x ", fb_offsets[k++]);

		
		if (conf->use_dlp == 2)
		{
            // store them sorted in ascending order
			if (large_prime[0] < large_prime[1])
			{
				if (large_prime[1] < large_prime[2])
				{
					lp[0] = large_prime[0];
					lp[1] = large_prime[1];
					lp[2] = large_prime[2];
				}
				else
				{
					if (large_prime[2] < large_prime[0])
					{
						lp[0] = large_prime[2];
						lp[1] = large_prime[0];
						lp[2] = large_prime[1];
					}
					else
					{
						lp[0] = large_prime[0];
						lp[1] = large_prime[2];
						lp[2] = large_prime[1];
					}
				}
			}
			else
			{
				if (large_prime[0] < large_prime[2])
				{
					lp[0] = large_prime[1];
					lp[1] = large_prime[0];
					lp[2] = large_prime[2];
				}
				else
				{
					if (large_prime[2] < large_prime[1])
					{
						lp[0] = large_prime[2];
						lp[1] = large_prime[1];
						lp[2] = large_prime[0];
					}
					else
					{
						lp[0] = large_prime[1];
						lp[1] = large_prime[2];
						lp[2] = large_prime[0];
					}
				}
			}

			i += sprintf(buf + i, "L %x %x %x\n", lp[0], lp[1], lp[2]);
		}
		else
		{
			if (large_prime[0] < large_prime[1])
				i += sprintf(buf + i, "L %x %x\n", large_prime[0], large_prime[1]);
			else
				i += sprintf(buf + i, "L %x %x\n", large_prime[1], large_prime[0]);
		}

		qs_savefile_write_line(&obj->qs_obj.savefile, buf);		
	}

	/* for partial relations, also update the bookeeping for
		   tracking the number of fundamental cycles */

	if (conf->use_dlp == 2)
	{
        // when using TLP, cycle counting is a lot more complicated.
        // hand it off here.
		yafu_add_to_cycles3(conf, 0, lp);
	}
	else
	{
		if (large_prime[0] != large_prime[1]) {
			yafu_add_to_cycles(conf, obj->flags, large_prime[0], large_prime[1]);
			conf->num_cycles++;
		}
		else {
			conf->num_relations++;
		}
	}

	return;
}

