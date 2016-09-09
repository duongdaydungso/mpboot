/*
 * sprparsimony.cpp
 *
 *  Created on: Nov 6, 2014
 *      Author: diep
 */
#include "sprparsimony.h"

/**
 * PLL (version 1.0.0) a software library for phylogenetic inference
 * Copyright (C) 2013 Tomas Flouri and Alexandros Stamatakis
 *
 * Derived from
 * RAxML-HPC, a program for sequential and parallel estimation of phylogenetic
 * trees by Alexandros Stamatakis
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * For any other enquiries send an Email to Tomas Flouri
 * Tomas.Flouri@h-its.org
 *
 * When publishing work that uses PLL please cite PLL
 *
 * @file fastDNAparsimony.c
 */

#if defined(__MIC_NATIVE)

#include <immintrin.h>

#define INTS_PER_VECTOR 16
#define LONG_INTS_PER_VECTOR 8
//#define LONG_INTS_PER_VECTOR (64/sizeof(long))
#define INT_TYPE __m512i
#define CAST double*
#define SET_ALL_BITS_ONE _mm512_set1_epi32(0xFFFFFFFF)
#define SET_ALL_BITS_ZERO _mm512_setzero_epi32()
#define VECTOR_LOAD _mm512_load_epi32
#define VECTOR_STORE  _mm512_store_epi32
#define VECTOR_BIT_AND _mm512_and_epi32
#define VECTOR_BIT_OR  _mm512_or_epi32
#define VECTOR_AND_NOT _mm512_andnot_epi32

#elif defined(__AVX)

#include <xmmintrin.h>
#include <immintrin.h>
#include <pmmintrin.h>

#define ULINT_SIZE 64
#define INTS_PER_VECTOR 8
#define LONG_INTS_PER_VECTOR 4
//#define LONG_INTS_PER_VECTOR (32/sizeof(long))
#define INT_TYPE __m256d
#define CAST double*
#define SET_ALL_BITS_ONE (__m256d)_mm256_set_epi32(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)
#define SET_ALL_BITS_ZERO (__m256d)_mm256_set_epi32(0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000)
#define VECTOR_LOAD _mm256_load_pd
#define VECTOR_BIT_AND _mm256_and_pd
#define VECTOR_BIT_OR  _mm256_or_pd
#define VECTOR_STORE  _mm256_store_pd
#define VECTOR_AND_NOT _mm256_andnot_pd

#elif (defined(__SSE3))

#include <xmmintrin.h>
#include <pmmintrin.h>

#define INTS_PER_VECTOR 4
#ifdef __i386__
#define ULINT_SIZE 32
#define LONG_INTS_PER_VECTOR 4
//#define LONG_INTS_PER_VECTOR (16/sizeof(long))
#else
#define ULINT_SIZE 64
#define LONG_INTS_PER_VECTOR 2
//#define LONG_INTS_PER_VECTOR (16/sizeof(long))
#endif
#define INT_TYPE __m128i
#define CAST __m128i*
#define SET_ALL_BITS_ONE _mm_set_epi32(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)
#define SET_ALL_BITS_ZERO _mm_set_epi32(0x00000000, 0x00000000, 0x00000000, 0x00000000)
#define VECTOR_LOAD _mm_load_si128
#define VECTOR_BIT_AND _mm_and_si128
#define VECTOR_BIT_OR  _mm_or_si128
#define VECTOR_STORE  _mm_store_si128
#define VECTOR_AND_NOT _mm_andnot_si128

#endif

#include "pllrepo/src/pll.h"
#include "pllrepo/src/pllInternal.h"


static pllBoolean tipHomogeneityCheckerPars(pllInstance *tr, nodeptr p, int grouping);

extern const unsigned int mask32[32];
/* vector-specific stuff */


extern double masterTime;

/* program options */
extern Params *globalParam;
IQTree * iqtree = NULL;
unsigned long bestTreeScoreHits; // to count hits to bestParsimony

///************************************************ pop count stuff ***********************************************/
//
// unsigned int bitcount_32_bit(unsigned int i)
//{
//  return ((unsigned int) __builtin_popcount(i));
//}

///* bit count for 64 bit integers */
//
//inline unsigned int bitcount_64_bit(unsigned long i)
//{
//  return ((unsigned int) __builtin_popcountl(i));
//}

/* bit count for 128 bit SSE3 and 256 bit AVX registers */

#if (defined(__SSE3) || defined(__AVX))
static inline unsigned int vectorPopcount(INT_TYPE v)
{
  unsigned long
    counts[LONG_INTS_PER_VECTOR] __attribute__ ((aligned (PLL_BYTE_ALIGNMENT)));

  int
    i,
    sum = 0;

  VECTOR_STORE((CAST)counts, v);

  for(i = 0; i < LONG_INTS_PER_VECTOR; i++)
    sum += __builtin_popcountl(counts[i]);

  return ((unsigned int)sum);
}
#endif



/********************************DNA FUNCTIONS *****************************************************************/



// Diep:
// store per site score to nodeNumber
static inline void storePerSiteNodeScores (partitionList * pr, int model, INT_TYPE v, unsigned int offset , int nodeNumber)
{
	unsigned long
		counts[LONG_INTS_PER_VECTOR] __attribute__ ((aligned (PLL_BYTE_ALIGNMENT)));
		parsimonyNumber * buf;

	int
		i,
		j;

	VECTOR_STORE((CAST)counts, v);

	int partialParsLength = pr->partitionData[model]->parsimonyLength * PLL_PCF;
	int nodeStart = partialParsLength * nodeNumber;
	for (i = 0; i < LONG_INTS_PER_VECTOR; ++i){
		buf = &(pr->partitionData[model]->perSitePartialPars[nodeStart + offset * PLL_PCF + i * ULINT_SIZE]);
//		buf = &(pr->partitionData[model]->perSitePartialPars[nodeStart + offset * PLL_PCF + i]);
		for (j = 0; j < ULINT_SIZE; ++ j)
			buf[j] += ((counts[i] >> j) & 1);
	}
}

// Diep:
// Add site scores in q and r to p
// q and r are children of p
void addPerSiteSubtreeScores(partitionList *pr, int pNumber, int qNumber, int rNumber){
	parsimonyNumber * pBuf, * qBuf, *rBuf;
	for(int i = 0; i < pr->numberOfPartitions; i++){
		int partialParsLength = pr->partitionData[i]->parsimonyLength * PLL_PCF;
		pBuf = &(pr->partitionData[i]->perSitePartialPars[partialParsLength * pNumber]);
		qBuf = &(pr->partitionData[i]->perSitePartialPars[partialParsLength * qNumber]);
		rBuf = &(pr->partitionData[i]->perSitePartialPars[partialParsLength * rNumber]);
		for(int k = 0; k < partialParsLength; k++)
			pBuf[k] += qBuf[k] + rBuf[k];
	}
}

// Diep:
// Reset site scores of p
void resetPerSiteNodeScores(partitionList *pr, int pNumber){
	parsimonyNumber * pBuf;
	for(int i = 0; i < pr->numberOfPartitions; i++){
		int partialParsLength = pr->partitionData[i]->parsimonyLength * PLL_PCF;
		pBuf = &(pr->partitionData[i]->perSitePartialPars[partialParsLength * pNumber]);
		memset(pBuf, 0, partialParsLength * sizeof(parsimonyNumber));
	}
}


static int checkerPars(pllInstance *tr, nodeptr p)
{
  int group = tr->constraintVector[p->number];

  if(isTip(p->number, tr->mxtips))
    {
      group = tr->constraintVector[p->number];
      return group;
    }
  else
    {
      if(group != -9)
        return group;

      group = checkerPars(tr, p->next->back);
      if(group != -9)
        return group;

      group = checkerPars(tr, p->next->next->back);
      if(group != -9)
        return group;

      return -9;
    }
}

static pllBoolean tipHomogeneityCheckerPars(pllInstance *tr, nodeptr p, int grouping)
{
  if(isTip(p->number, tr->mxtips))
    {
      if(tr->constraintVector[p->number] != grouping)
        return PLL_FALSE;
      else
        return PLL_TRUE;
    }
  else
    {
      return  (tipHomogeneityCheckerPars(tr, p->next->back, grouping) && tipHomogeneityCheckerPars(tr, p->next->next->back,grouping));
    }
}

static void getxnodeLocal (nodeptr p)
{
  nodeptr  s;

  if((s = p->next)->xPars || (s = s->next)->xPars)
    {
      p->xPars = s->xPars;
      s->xPars = 0;
    }

  assert(p->next->xPars || p->next->next->xPars || p->xPars);

}

static void computeTraversalInfoParsimony(nodeptr p, int *ti, int *counter, int maxTips, pllBoolean full, int perSiteScores)
{
	if(perSiteScores){
		resetPerSiteNodeScores(iqtree->pllPartitions, p->number);
	}
	nodeptr
		q = p->next->back,
		r = p->next->next->back;

	if(! p->xPars)
		getxnodeLocal(p);

	if(full){
		if(q->number > maxTips)
			computeTraversalInfoParsimony(q, ti, counter, maxTips, full, perSiteScores);

		if(r->number > maxTips)
			computeTraversalInfoParsimony(r, ti, counter, maxTips, full, perSiteScores);
	}else{
		if(q->number > maxTips && !q->xPars)
			computeTraversalInfoParsimony(q, ti, counter, maxTips, full, perSiteScores);

		if(r->number > maxTips && !r->xPars)
			computeTraversalInfoParsimony(r, ti, counter, maxTips, full, perSiteScores);
	}

	ti[*counter]     = p->number;
	ti[*counter + 1] = q->number;
	ti[*counter + 2] = r->number;
	*counter = *counter + 4;
}


#if (defined(__SSE3) || defined(__AVX))

static void newviewParsimonyIterativeFast(pllInstance *tr, partitionList *pr, int perSiteScores)
{
  INT_TYPE
    allOne = SET_ALL_BITS_ONE;

  int
    model,
    *ti = tr->ti,
    count = ti[0],
    index;

  for(index = 4; index < count; index += 4)
    {
      unsigned int
        totalScore = 0;

      size_t
        pNumber = (size_t)ti[index],
        qNumber = (size_t)ti[index + 1],
        rNumber = (size_t)ti[index + 2];

      if(perSiteScores){
		  if(qNumber <= tr->mxtips) resetPerSiteNodeScores(pr, qNumber);
		  if(rNumber <= tr->mxtips) resetPerSiteNodeScores(pr, rNumber);
      }

      for(model = 0; model < pr->numberOfPartitions; model++)
        {
          size_t
            k,
            states = pr->partitionData[model]->states,
            width = pr->partitionData[model]->parsimonyLength;

          unsigned int
            i;

          switch(states)
            {
            case 2:
              {
                parsimonyNumber
                  *left[2],
                  *right[2],
                  *cur[2];

                for(k = 0; k < 2; k++)
                  {
                    left[k]  = &(pr->partitionData[model]->parsVect[(width * 2 * qNumber) + width * k]);
                    right[k] = &(pr->partitionData[model]->parsVect[(width * 2 * rNumber) + width * k]);
                    cur[k]  = &(pr->partitionData[model]->parsVect[(width * 2 * pNumber) + width * k]);
                  }

                for(i = 0; i < width; i += INTS_PER_VECTOR)
                  {
                    INT_TYPE
                      s_r, s_l, v_N,
                      l_A, l_C,
                      v_A, v_C;

                    s_l = VECTOR_LOAD((CAST)(&left[0][i]));
                    s_r = VECTOR_LOAD((CAST)(&right[0][i]));
                    l_A = VECTOR_BIT_AND(s_l, s_r);
                    v_A = VECTOR_BIT_OR(s_l, s_r);

                    s_l = VECTOR_LOAD((CAST)(&left[1][i]));
                    s_r = VECTOR_LOAD((CAST)(&right[1][i]));
                    l_C = VECTOR_BIT_AND(s_l, s_r);
                    v_C = VECTOR_BIT_OR(s_l, s_r);

                    v_N = VECTOR_BIT_OR(l_A, l_C);

                    VECTOR_STORE((CAST)(&cur[0][i]), VECTOR_BIT_OR(l_A, VECTOR_AND_NOT(v_N, v_A)));
                    VECTOR_STORE((CAST)(&cur[1][i]), VECTOR_BIT_OR(l_C, VECTOR_AND_NOT(v_N, v_C)));

                    v_N = VECTOR_AND_NOT(v_N, allOne);

                    totalScore += vectorPopcount(v_N);
                    if (perSiteScores)
                       storePerSiteNodeScores(pr, model, v_N, i, pNumber);
                  }
              }
              break;
            case 4:
              {
                parsimonyNumber
                  *left[4],
                  *right[4],
                  *cur[4];

                for(k = 0; k < 4; k++)
                  {
                    left[k]  = &(pr->partitionData[model]->parsVect[(width * 4 * qNumber) + width * k]);
                    right[k] = &(pr->partitionData[model]->parsVect[(width * 4 * rNumber) + width * k]);
                    cur[k]  = &(pr->partitionData[model]->parsVect[(width * 4 * pNumber) + width * k]);
                  }

                for(i = 0; i < width; i += INTS_PER_VECTOR)
                  {
                    INT_TYPE
                      s_r, s_l, v_N,
                      l_A, l_C, l_G, l_T,
                      v_A, v_C, v_G, v_T;

                    s_l = VECTOR_LOAD((CAST)(&left[0][i]));
                    s_r = VECTOR_LOAD((CAST)(&right[0][i]));
                    l_A = VECTOR_BIT_AND(s_l, s_r);
                    v_A = VECTOR_BIT_OR(s_l, s_r);

                    s_l = VECTOR_LOAD((CAST)(&left[1][i]));
                    s_r = VECTOR_LOAD((CAST)(&right[1][i]));
                    l_C = VECTOR_BIT_AND(s_l, s_r);
                    v_C = VECTOR_BIT_OR(s_l, s_r);

                    s_l = VECTOR_LOAD((CAST)(&left[2][i]));
                    s_r = VECTOR_LOAD((CAST)(&right[2][i]));
                    l_G = VECTOR_BIT_AND(s_l, s_r);
                    v_G = VECTOR_BIT_OR(s_l, s_r);

                    s_l = VECTOR_LOAD((CAST)(&left[3][i]));
                    s_r = VECTOR_LOAD((CAST)(&right[3][i]));
                    l_T = VECTOR_BIT_AND(s_l, s_r);
                    v_T = VECTOR_BIT_OR(s_l, s_r);

                    v_N = VECTOR_BIT_OR(VECTOR_BIT_OR(l_A, l_C), VECTOR_BIT_OR(l_G, l_T));

                    VECTOR_STORE((CAST)(&cur[0][i]), VECTOR_BIT_OR(l_A, VECTOR_AND_NOT(v_N, v_A)));
                    VECTOR_STORE((CAST)(&cur[1][i]), VECTOR_BIT_OR(l_C, VECTOR_AND_NOT(v_N, v_C)));
                    VECTOR_STORE((CAST)(&cur[2][i]), VECTOR_BIT_OR(l_G, VECTOR_AND_NOT(v_N, v_G)));
                    VECTOR_STORE((CAST)(&cur[3][i]), VECTOR_BIT_OR(l_T, VECTOR_AND_NOT(v_N, v_T)));

                    v_N = VECTOR_AND_NOT(v_N, allOne);

                    totalScore += vectorPopcount(v_N);
                    if (perSiteScores)
                       storePerSiteNodeScores(pr, model, v_N, i, pNumber);
                  }
              }
              break;
            case 20:
              {
                parsimonyNumber
                  *left[20],
                  *right[20],
                  *cur[20];

                for(k = 0; k < 20; k++)
                  {
                    left[k]  = &(pr->partitionData[model]->parsVect[(width * 20 * qNumber) + width * k]);
                    right[k] = &(pr->partitionData[model]->parsVect[(width * 20 * rNumber) + width * k]);
                    cur[k]  = &(pr->partitionData[model]->parsVect[(width * 20 * pNumber) + width * k]);
                  }

                for(i = 0; i < width; i += INTS_PER_VECTOR)
                  {
                    size_t j;

                    INT_TYPE
                      s_r, s_l,
                      v_N = SET_ALL_BITS_ZERO,
                      l_A[20],
                      v_A[20];

                    for(j = 0; j < 20; j++)
                      {
                        s_l = VECTOR_LOAD((CAST)(&left[j][i]));
                        s_r = VECTOR_LOAD((CAST)(&right[j][i]));
                        l_A[j] = VECTOR_BIT_AND(s_l, s_r);
                        v_A[j] = VECTOR_BIT_OR(s_l, s_r);

                        v_N = VECTOR_BIT_OR(v_N, l_A[j]);
                      }

                    for(j = 0; j < 20; j++)
                      VECTOR_STORE((CAST)(&cur[j][i]), VECTOR_BIT_OR(l_A[j], VECTOR_AND_NOT(v_N, v_A[j])));

                    v_N = VECTOR_AND_NOT(v_N, allOne);

                    totalScore += vectorPopcount(v_N);
                    if (perSiteScores)
                       storePerSiteNodeScores(pr, model, v_N, i, pNumber);
                  }
              }
              break;
            default:

              {
                parsimonyNumber
                  *left[32],
                  *right[32],
                  *cur[32];

                assert(states <= 32);

                for(k = 0; k < states; k++)
                  {
                    left[k]  = &(pr->partitionData[model]->parsVect[(width * states * qNumber) + width * k]);
                    right[k] = &(pr->partitionData[model]->parsVect[(width * states * rNumber) + width * k]);
                    cur[k]  = &(pr->partitionData[model]->parsVect[(width * states * pNumber) + width * k]);
                  }

                for(i = 0; i < width; i += INTS_PER_VECTOR)
                  {
                    size_t j;

                    INT_TYPE
                      s_r, s_l,
                      v_N = SET_ALL_BITS_ZERO,
                      l_A[32],
                      v_A[32];

                    for(j = 0; j < states; j++)
                      {
                        s_l = VECTOR_LOAD((CAST)(&left[j][i]));
                        s_r = VECTOR_LOAD((CAST)(&right[j][i]));
                        l_A[j] = VECTOR_BIT_AND(s_l, s_r);
                        v_A[j] = VECTOR_BIT_OR(s_l, s_r);

                        v_N = VECTOR_BIT_OR(v_N, l_A[j]);
                      }

                    for(j = 0; j < states; j++)
                      VECTOR_STORE((CAST)(&cur[j][i]), VECTOR_BIT_OR(l_A[j], VECTOR_AND_NOT(v_N, v_A[j])));

                    v_N = VECTOR_AND_NOT(v_N, allOne);

                    totalScore += vectorPopcount(v_N);
                    if (perSiteScores)
                       storePerSiteNodeScores(pr, model, v_N, i, pNumber);
                  }
              }
            }
        }

      tr->parsimonyScore[pNumber] = totalScore + tr->parsimonyScore[rNumber] + tr->parsimonyScore[qNumber];
      if (perSiteScores)
    	  addPerSiteSubtreeScores(pr, pNumber, qNumber, rNumber); // Diep: add rNumber and qNumber to pNumber
    }
}



static unsigned int evaluateParsimonyIterativeFast(pllInstance *tr, partitionList *pr, int perSiteScores)
{
  INT_TYPE
    allOne = SET_ALL_BITS_ONE;

  size_t
    pNumber = (size_t)tr->ti[1],
    qNumber = (size_t)tr->ti[2];

  int
    model;

  unsigned int
    bestScore = tr->bestParsimony,
    sum;

  if(tr->ti[0] > 4)
    newviewParsimonyIterativeFast(tr, pr, perSiteScores);

  sum = tr->parsimonyScore[pNumber] + tr->parsimonyScore[qNumber];

  if(perSiteScores){
	  resetPerSiteNodeScores(pr, tr->start->number);
	  addPerSiteSubtreeScores(pr, tr->start->number, pNumber, qNumber);
  }

  for(model = 0; model < pr->numberOfPartitions; model++)
    {
      size_t
        k,
        states = pr->partitionData[model]->states,
        width  = pr->partitionData[model]->parsimonyLength,
        i;

       switch(states)
         {
         case 2:
           {
             parsimonyNumber
               *left[2],
               *right[2];

             for(k = 0; k < 2; k++)
               {
                 left[k]  = &(pr->partitionData[model]->parsVect[(width * 2 * qNumber) + width * k]);
                 right[k] = &(pr->partitionData[model]->parsVect[(width * 2 * pNumber) + width * k]);
               }

             for(i = 0; i < width; i += INTS_PER_VECTOR)
               {
                 INT_TYPE
                   l_A = VECTOR_BIT_AND(VECTOR_LOAD((CAST)(&left[0][i])), VECTOR_LOAD((CAST)(&right[0][i]))),
                   l_C = VECTOR_BIT_AND(VECTOR_LOAD((CAST)(&left[1][i])), VECTOR_LOAD((CAST)(&right[1][i]))),
                   v_N = VECTOR_BIT_OR(l_A, l_C);

                 v_N = VECTOR_AND_NOT(v_N, allOne);

                 sum += vectorPopcount(v_N);
                 if(perSiteScores)
                	 storePerSiteNodeScores(pr, model, v_N, i, tr->start->number);

//                 if(sum >= bestScore)
//                   return sum;
               }
           }
           break;
         case 4:
           {
             parsimonyNumber
               *left[4],
               *right[4];

             for(k = 0; k < 4; k++)
               {
                 left[k]  = &(pr->partitionData[model]->parsVect[(width * 4 * qNumber) + width * k]);
                 right[k] = &(pr->partitionData[model]->parsVect[(width * 4 * pNumber) + width * k]);
               }

             for(i = 0; i < width; i += INTS_PER_VECTOR)
               {
                 INT_TYPE
                   l_A = VECTOR_BIT_AND(VECTOR_LOAD((CAST)(&left[0][i])), VECTOR_LOAD((CAST)(&right[0][i]))),
                   l_C = VECTOR_BIT_AND(VECTOR_LOAD((CAST)(&left[1][i])), VECTOR_LOAD((CAST)(&right[1][i]))),
                   l_G = VECTOR_BIT_AND(VECTOR_LOAD((CAST)(&left[2][i])), VECTOR_LOAD((CAST)(&right[2][i]))),
                   l_T = VECTOR_BIT_AND(VECTOR_LOAD((CAST)(&left[3][i])), VECTOR_LOAD((CAST)(&right[3][i]))),
                   v_N = VECTOR_BIT_OR(VECTOR_BIT_OR(l_A, l_C), VECTOR_BIT_OR(l_G, l_T));

                 v_N = VECTOR_AND_NOT(v_N, allOne);

                 sum += vectorPopcount(v_N);
                 if(perSiteScores)
                	 storePerSiteNodeScores(pr, model, v_N, i, tr->start->number);
//                 if(sum >= bestScore)
//                   return sum;
               }
           }
           break;
         case 20:
           {
             parsimonyNumber
               *left[20],
               *right[20];

              for(k = 0; k < 20; k++)
                {
                  left[k]  = &(pr->partitionData[model]->parsVect[(width * 20 * qNumber) + width * k]);
                  right[k] = &(pr->partitionData[model]->parsVect[(width * 20 * pNumber) + width * k]);
                }

              for(i = 0; i < width; i += INTS_PER_VECTOR)
                {
                  int
                    j;

                  INT_TYPE
                    l_A,
                    v_N = SET_ALL_BITS_ZERO;

                  for(j = 0; j < 20; j++)
                    {
                      l_A = VECTOR_BIT_AND(VECTOR_LOAD((CAST)(&left[j][i])), VECTOR_LOAD((CAST)(&right[j][i])));
                      v_N = VECTOR_BIT_OR(l_A, v_N);
                    }

                  v_N = VECTOR_AND_NOT(v_N, allOne);

                  sum += vectorPopcount(v_N);
                  if(perSiteScores)
                 	 storePerSiteNodeScores(pr, model, v_N, i, tr->start->number);
//                  if(sum >= bestScore)
//                    return sum;
                }
           }
           break;
         default:
           {
             parsimonyNumber
               *left[32],
               *right[32];

             assert(states <= 32);

             for(k = 0; k < states; k++)
               {
                 left[k]  = &(pr->partitionData[model]->parsVect[(width * states * qNumber) + width * k]);
                 right[k] = &(pr->partitionData[model]->parsVect[(width * states * pNumber) + width * k]);
               }

             for(i = 0; i < width; i += INTS_PER_VECTOR)
               {
                 size_t
                   j;

                 INT_TYPE
                   l_A,
                   v_N = SET_ALL_BITS_ZERO;

                 for(j = 0; j < states; j++)
                   {
                     l_A = VECTOR_BIT_AND(VECTOR_LOAD((CAST)(&left[j][i])), VECTOR_LOAD((CAST)(&right[j][i])));
                     v_N = VECTOR_BIT_OR(l_A, v_N);
                   }

                 v_N = VECTOR_AND_NOT(v_N, allOne);

                 sum += vectorPopcount(v_N);
                 if(perSiteScores)
                	 storePerSiteNodeScores(pr, model, v_N, i, tr->start->number);
//                 if(sum >= bestScore)
//                   return sum;
               }
           }
         }
    }

  return sum;
}


#else
static void newviewParsimonyIterativeFast(pllInstance *tr, partitionList * pr, int perSiteScores)
{
  int
    model,
    *ti = tr->ti,
    count = ti[0],
    index;

  for(index = 4; index < count; index += 4)
    {
      unsigned int
        totalScore = 0;

      size_t
        pNumber = (size_t)ti[index],
        qNumber = (size_t)ti[index + 1],
        rNumber = (size_t)ti[index + 2];

      for(model = 0; model < pr->numberOfPartitions; model++)
        {
          size_t
            k,
            states = pr->partitionData[model]->states,
            width = pr->partitionData[model]->parsimonyLength;

          unsigned int
            i;

          switch(states)
            {
            case 2:
              {
                parsimonyNumber
                  *left[2],
                  *right[2],
                  *cur[2];

                parsimonyNumber
                   o_A,
                   o_C,
                   t_A,
                   t_C,
                   t_N;

                for(k = 0; k < 2; k++)
                  {
                    left[k]  = &(pr->partitionData[model]->parsVect[(width * 2 * qNumber) + width * k]);
                    right[k] = &(pr->partitionData[model]->parsVect[(width * 2 * rNumber) + width * k]);
                    cur[k]  = &(pr->partitionData[model]->parsVect[(width * 2 * pNumber) + width * k]);
                  }

                for(i = 0; i < width; i++)
                  {
                    t_A = left[0][i] & right[0][i];
                    t_C = left[1][i] & right[1][i];

                    o_A = left[0][i] | right[0][i];
                    o_C = left[1][i] | right[1][i];

                    t_N = ~(t_A | t_C);

                    cur[0][i] = t_A | (t_N & o_A);
                    cur[1][i] = t_C | (t_N & o_C);

                    totalScore += ((unsigned int) __builtin_popcount(t_N));
                  }
              }
              break;
            case 4:
              {
                parsimonyNumber
                  *left[4],
                  *right[4],
                  *cur[4];

                for(k = 0; k < 4; k++)
                  {
                    left[k]  = &(pr->partitionData[model]->parsVect[(width * 4 * qNumber) + width * k]);
                    right[k] = &(pr->partitionData[model]->parsVect[(width * 4 * rNumber) + width * k]);
                    cur[k]  = &(pr->partitionData[model]->parsVect[(width * 4 * pNumber) + width * k]);
                  }

                parsimonyNumber
                   o_A,
                   o_C,
                   o_G,
                   o_T,
                   t_A,
                   t_C,
                   t_G,
                   t_T,
                   t_N;

                for(i = 0; i < width; i++)
                  {
                    t_A = left[0][i] & right[0][i];
                    t_C = left[1][i] & right[1][i];
                    t_G = left[2][i] & right[2][i];
                    t_T = left[3][i] & right[3][i];

                    o_A = left[0][i] | right[0][i];
                    o_C = left[1][i] | right[1][i];
                    o_G = left[2][i] | right[2][i];
                    o_T = left[3][i] | right[3][i];

                    t_N = ~(t_A | t_C | t_G | t_T);

                    cur[0][i] = t_A | (t_N & o_A);
                    cur[1][i] = t_C | (t_N & o_C);
                    cur[2][i] = t_G | (t_N & o_G);
                    cur[3][i] = t_T | (t_N & o_T);

                    totalScore += ((unsigned int) __builtin_popcount(t_N));
                  }
              }
              break;
            case 20:
              {
                parsimonyNumber
                  *left[20],
                  *right[20],
                  *cur[20];

                parsimonyNumber
                  o_A[20],
                  t_A[20],
                  t_N;

                for(k = 0; k < 20; k++)
                  {
                    left[k]  = &(pr->partitionData[model]->parsVect[(width * 20 * qNumber) + width * k]);
                    right[k] = &(pr->partitionData[model]->parsVect[(width * 20 * rNumber) + width * k]);
                    cur[k]  = &(pr->partitionData[model]->parsVect[(width * 20 * pNumber) + width * k]);
                  }

                for(i = 0; i < width; i++)
                  {
                    size_t k;

                    t_N = 0;

                    for(k = 0; k < 20; k++)
                      {
                        t_A[k] = left[k][i] & right[k][i];
                        o_A[k] = left[k][i] | right[k][i];
                        t_N = t_N | t_A[k];
                      }

                    t_N = ~t_N;

                    for(k = 0; k < 20; k++)
                      cur[k][i] = t_A[k] | (t_N & o_A[k]);

                    totalScore += ((unsigned int) __builtin_popcount(t_N));
                  }
              }
              break;
            default:
              {
                parsimonyNumber
                  *left[32],
                  *right[32],
                  *cur[32];

                parsimonyNumber
                  o_A[32],
                  t_A[32],
                  t_N;

                assert(states <= 32);

                for(k = 0; k < states; k++)
                  {
                    left[k]  = &(pr->partitionData[model]->parsVect[(width * states * qNumber) + width * k]);
                    right[k] = &(pr->partitionData[model]->parsVect[(width * states * rNumber) + width * k]);
                    cur[k]  = &(pr->partitionData[model]->parsVect[(width * states * pNumber) + width * k]);
                  }

                for(i = 0; i < width; i++)
                  {
                    t_N = 0;

                    for(k = 0; k < states; k++)
                      {
                        t_A[k] = left[k][i] & right[k][i];
                        o_A[k] = left[k][i] | right[k][i];
                        t_N = t_N | t_A[k];
                      }

                    t_N = ~t_N;

                    for(k = 0; k < states; k++)
                      cur[k][i] = t_A[k] | (t_N & o_A[k]);

                    totalScore += ((unsigned int) __builtin_popcount(t_N));
                  }
              }
            }
        }

      tr->parsimonyScore[pNumber] = totalScore + tr->parsimonyScore[rNumber] + tr->parsimonyScore[qNumber];
    }
}

/* Sankoff weighted parsimony */
static void newviewSankoffParsimonyIterativeFast(pllInstance *tr, partitionList * pr, int perSiteScores)
{
  int
    model,
    *ti = tr->ti,
    count = ti[0],
    index;

  for(index = 4; index < count; index += 4)
    {
      unsigned int
        totalScore = 0;

      size_t
        pNumber = (size_t)ti[index],
        qNumber = (size_t)ti[index + 1],
        rNumber = (size_t)ti[index + 2];

      for(model = 0; model < pr->numberOfPartitions; model++)
        {
          size_t
            k,
            states = pr->partitionData[model]->states,
            width = pr->partitionData[model]->parsimonyLength;

          unsigned int
            i;

          switch(states)
            {
            case 2:
              {
                parsimonyNumber
                  *left[2],
                  *right[2],
                  *cur[2];

                parsimonyNumber
                   o_A,
                   o_C,
                   t_A,
                   t_C,
                   t_N;

                for(k = 0; k < 2; k++)
                  {
                    left[k]  = &(pr->partitionData[model]->parsVect[(width * 2 * qNumber) + width * k]);
                    right[k] = &(pr->partitionData[model]->parsVect[(width * 2 * rNumber) + width * k]);
                    cur[k]  = &(pr->partitionData[model]->parsVect[(width * 2 * pNumber) + width * k]);
                  }

                for(i = 0; i < width; i++)
                  {
                    t_A = left[0][i] & right[0][i];
                    t_C = left[1][i] & right[1][i];

                    o_A = left[0][i] | right[0][i];
                    o_C = left[1][i] | right[1][i];

                    t_N = ~(t_A | t_C);

                    cur[0][i] = t_A | (t_N & o_A);
                    cur[1][i] = t_C | (t_N & o_C);

                    totalScore += ((unsigned int) __builtin_popcount(t_N));
                  }
              }
              break;
            case 4:
              {
                parsimonyNumber
                  *left[4],
                  *right[4],
                  *cur[4];

                for(k = 0; k < 4; k++)
                  {
                    left[k]  = &(pr->partitionData[model]->parsVect[(width * 4 * qNumber) + width * k]);
                    right[k] = &(pr->partitionData[model]->parsVect[(width * 4 * rNumber) + width * k]);
                    cur[k]  = &(pr->partitionData[model]->parsVect[(width * 4 * pNumber) + width * k]);
                  }

                parsimonyNumber
                   o_A,
                   o_C,
                   o_G,
                   o_T,
                   t_A,
                   t_C,
                   t_G,
                   t_T,
                   t_N;

                /*
                                  cur
                             /         \
                            /           \
                           /             \
                        left             right
                   score_left(A,C,G,T)   score_right(A,C,G,T)
                       
                        score_cur(z) = min_x,y { cost(z->x)+score_left(x) + cost(z->y)+score_right(y)}
                                     = left_contribution + right_contribution
                                      
                        left_contribution  =  min_x{ cost(z->x)+score_left(x)}
                        right_contribution =  min_x{ cost(z->x)+score_right(x)} 
                        
                */
                
                parsimonyNumber *cost = tr->costMatrix; // this is the cost matrix
                // cost[x*4+y] ---> cost of change from x to y
                
                for(i = 0; i < width; i++)
                  {
                    for (z = 0; z < 4; z++) {
                        parsimonyNumber left_contrib = 100000;
                        parsimonyNumber right_contrib = 100000;
                        for (x = 0; x < 4; x++)
                        {
                          parsimonyNumber value = cost[z*4+x] + left[x][i];
                          if (value < left_contrib) 
                            left_contrib = value;

                          value = cost[z*4+x] + right[x][i];
                          if (value < right_contrib) 
                            right_contrib = value;
                        }
                        cur[z][i] = left_contrib + right_contrib;
                    }
                    
                    totalScore += min(cur[0][i], cur[1][i], cur[2][i], cur[3][i]);
                  }
              }
              break;
            case 20:
              {
                parsimonyNumber
                  *left[20],
                  *right[20],
                  *cur[20];

                parsimonyNumber
                  o_A[20],
                  t_A[20],
                  t_N;

                for(k = 0; k < 20; k++)
                  {
                    left[k]  = &(pr->partitionData[model]->parsVect[(width * 20 * qNumber) + width * k]);
                    right[k] = &(pr->partitionData[model]->parsVect[(width * 20 * rNumber) + width * k]);
                    cur[k]  = &(pr->partitionData[model]->parsVect[(width * 20 * pNumber) + width * k]);
                  }

                for(i = 0; i < width; i++)
                  {
                    size_t k;

                    t_N = 0;

                    for(k = 0; k < 20; k++)
                      {
                        t_A[k] = left[k][i] & right[k][i];
                        o_A[k] = left[k][i] | right[k][i];
                        t_N = t_N | t_A[k];
                      }

                    t_N = ~t_N;

                    for(k = 0; k < 20; k++)
                      cur[k][i] = t_A[k] | (t_N & o_A[k]);

                    totalScore += ((unsigned int) __builtin_popcount(t_N));
                  }
              }
              break;
            default:
              {
                parsimonyNumber
                  *left[32],
                  *right[32],
                  *cur[32];

                parsimonyNumber
                  o_A[32],
                  t_A[32],
                  t_N;

                assert(states <= 32);

                for(k = 0; k < states; k++)
                  {
                    left[k]  = &(pr->partitionData[model]->parsVect[(width * states * qNumber) + width * k]);
                    right[k] = &(pr->partitionData[model]->parsVect[(width * states * rNumber) + width * k]);
                    cur[k]  = &(pr->partitionData[model]->parsVect[(width * states * pNumber) + width * k]);
                  }

                for(i = 0; i < width; i++)
                  {
                    t_N = 0;

                    for(k = 0; k < states; k++)
                      {
                        t_A[k] = left[k][i] & right[k][i];
                        o_A[k] = left[k][i] | right[k][i];
                        t_N = t_N | t_A[k];
                      }

                    t_N = ~t_N;

                    for(k = 0; k < states; k++)
                      cur[k][i] = t_A[k] | (t_N & o_A[k]);

                    totalScore += ((unsigned int) __builtin_popcount(t_N));
                  }
              }
            }
        }

      tr->parsimonyScore[pNumber] = totalScore + tr->parsimonyScore[rNumber] + tr->parsimonyScore[qNumber];
    }
}

static unsigned int evaluateParsimonyIterativeFast(pllInstance *tr, partitionList * pr, int perSiteScores)
{
  size_t
    pNumber = (size_t)tr->ti[1],
    qNumber = (size_t)tr->ti[2];

  int
    model;

  unsigned int
    bestScore = tr->bestParsimony,
    sum;

  if(tr->ti[0] > 4)
    newviewParsimonyIterativeFast(tr, pr, perSiteScores);

  sum = tr->parsimonyScore[pNumber] + tr->parsimonyScore[qNumber];

  for(model = 0; model < pr->numberOfPartitions; model++)
    {
      size_t
        k,
        states = pr->partitionData[model]->states,
        width  = pr->partitionData[model]->parsimonyLength,
        i;

       switch(states)
         {
         case 2:
           {
             parsimonyNumber
               t_A,
               t_C,
               t_N,
               *left[2],
               *right[2];

             for(k = 0; k < 2; k++)
               {
                 left[k]  = &(pr->partitionData[model]->parsVect[(width * 2 * qNumber) + width * k]);
                 right[k] = &(pr->partitionData[model]->parsVect[(width * 2 * pNumber) + width * k]);
               }

             for(i = 0; i < width; i++)
               {
                 t_A = left[0][i] & right[0][i];
                 t_C = left[1][i] & right[1][i];

                  t_N = ~(t_A | t_C);

                  sum += ((unsigned int) __builtin_popcount(t_N));

//                 if(sum >= bestScore)
//                   return sum;
               }
           }
           break;
         case 4:
           {
             parsimonyNumber
               t_A,
               t_C,
               t_G,
               t_T,
               t_N,
               *left[4],
               *right[4];

             for(k = 0; k < 4; k++)
               {
                 left[k]  = &(pr->partitionData[model]->parsVect[(width * 4 * qNumber) + width * k]);
                 right[k] = &(pr->partitionData[model]->parsVect[(width * 4 * pNumber) + width * k]);
               }

             for(i = 0; i < width; i++)
               {
                  t_A = left[0][i] & right[0][i];
                  t_C = left[1][i] & right[1][i];
                  t_G = left[2][i] & right[2][i];
                  t_T = left[3][i] & right[3][i];

                  t_N = ~(t_A | t_C | t_G | t_T);

                  sum += ((unsigned int) __builtin_popcount(t_N));

//                 if(sum >= bestScore)
//                   return sum;
               }
           }
           break;
         case 20:
           {
             parsimonyNumber
               t_A,
               t_N,
               *left[20],
               *right[20];

              for(k = 0; k < 20; k++)
                {
                  left[k]  = &(pr->partitionData[model]->parsVect[(width * 20 * qNumber) + width * k]);
                  right[k] = &(pr->partitionData[model]->parsVect[(width * 20 * pNumber) + width * k]);
                }

              for(i = 0; i < width; i++)
                {
                  t_N = 0;

                  for(k = 0; k < 20; k++)
                    {
                      t_A = left[k][i] & right[k][i];
                      t_N = t_N | t_A;
                    }

                  t_N = ~t_N;

                  sum += ((unsigned int) __builtin_popcount(t_N));

//                  if(sum >= bestScore)
//                    return sum;
                }
           }
           break;
         default:
           {
             parsimonyNumber
               t_A,
               t_N,
               *left[32],
               *right[32];

             assert(states <= 32);

             for(k = 0; k < states; k++)
               {
                 left[k]  = &(pr->partitionData[model]->parsVect[(width * states * qNumber) + width * k]);
                 right[k] = &(pr->partitionData[model]->parsVect[(width * states * pNumber) + width * k]);
               }

             for(i = 0; i < width; i++)
               {
                 t_N = 0;

                 for(k = 0; k < states; k++)
                   {
                     t_A = left[k][i] & right[k][i];
                     t_N = t_N | t_A;
                   }

                  t_N = ~t_N;

                  sum += ((unsigned int) __builtin_popcount(t_N));

//                 if(sum >= bestScore)
//                   return sum;
               }
           }
         }
    }

  return sum;
}


static unsigned int evaluateSankoffParsimonyIterativeFast(pllInstance *tr, partitionList * pr, int perSiteScores)
{
  size_t
    pNumber = (size_t)tr->ti[1],
    qNumber = (size_t)tr->ti[2];

  int
    model;

  unsigned int
    bestScore = tr->bestParsimony,
    sum;

  if(tr->ti[0] > 4)
    newviewParsimonyIterativeFast(tr, pr, perSiteScores);

  sum = tr->parsimonyScore[pNumber] + tr->parsimonyScore[qNumber];

  for(model = 0; model < pr->numberOfPartitions; model++)
    {
      size_t
        k,
        states = pr->partitionData[model]->states,
        width  = pr->partitionData[model]->parsimonyLength,
        i;

       switch(states)
         {
         case 2:
           {
             parsimonyNumber
               t_A,
               t_C,
               t_N,
               *left[2],
               *right[2];

             for(k = 0; k < 2; k++)
               {
                 left[k]  = &(pr->partitionData[model]->parsVect[(width * 2 * qNumber) + width * k]);
                 right[k] = &(pr->partitionData[model]->parsVect[(width * 2 * pNumber) + width * k]);
               }

             for(i = 0; i < width; i++)
               {
                 t_A = left[0][i] & right[0][i];
                 t_C = left[1][i] & right[1][i];

                  t_N = ~(t_A | t_C);

                  sum += ((unsigned int) __builtin_popcount(t_N));

//                 if(sum >= bestScore)
//                   return sum;
               }
           }
           break;
         case 4:
           {
             parsimonyNumber
               t_A,
               t_C,
               t_G,
               t_T,
               t_N,
               *left[4],
               *right[4];

             for(k = 0; k < 4; k++)
               {
                 left[k]  = &(pr->partitionData[model]->parsVect[(width * 4 * qNumber) + width * k]);
                 right[k] = &(pr->partitionData[model]->parsVect[(width * 4 * pNumber) + width * k]);
               }


            /*
            
                    for each branch (left --- right), compute the score
                    
                    
                     left ----------------- right
                score_left(A,C,G,T)   score_right(A,C,G,T)
                
                
                score = min_x,y  { score_left(x) + cost(x-->y) + score_right(y)  }
                
                    
            */

             for(i = 0; i < width; i++)
               {
               
                best_score = 1000000;
                for (x = 0; x < 4; x++)
                    for (y = 0; y < 4; y++) {
                        parsimonyNumber value = left[x][i] + cost[x*4+y] + right[y][i];
                        if (value < best_score) 
                          best_score = value;
                    }
               
                  sum += best_score;

//                 if(sum >= bestScore)
//                   return sum;
               }
           }
           break;
         case 20:
           {
             parsimonyNumber
               t_A,
               t_N,
               *left[20],
               *right[20];

              for(k = 0; k < 20; k++)
                {
                  left[k]  = &(pr->partitionData[model]->parsVect[(width * 20 * qNumber) + width * k]);
                  right[k] = &(pr->partitionData[model]->parsVect[(width * 20 * pNumber) + width * k]);
                }

              for(i = 0; i < width; i++)
                {
                  t_N = 0;

                  for(k = 0; k < 20; k++)
                    {
                      t_A = left[k][i] & right[k][i];
                      t_N = t_N | t_A;
                    }

                  t_N = ~t_N;

                  sum += ((unsigned int) __builtin_popcount(t_N));

//                  if(sum >= bestScore)
//                    return sum;
                }
           }
           break;
         default:
           {
             parsimonyNumber
               t_A,
               t_N,
               *left[32],
               *right[32];

             assert(states <= 32);

             for(k = 0; k < states; k++)
               {
                 left[k]  = &(pr->partitionData[model]->parsVect[(width * states * qNumber) + width * k]);
                 right[k] = &(pr->partitionData[model]->parsVect[(width * states * pNumber) + width * k]);
               }

             for(i = 0; i < width; i++)
               {
                 t_N = 0;

                 for(k = 0; k < states; k++)
                   {
                     t_A = left[k][i] & right[k][i];
                     t_N = t_N | t_A;
                   }

                  t_N = ~t_N;

                  sum += ((unsigned int) __builtin_popcount(t_N));

//                 if(sum >= bestScore)
//                   return sum;
               }
           }
         }
    }

  return sum;
}


#endif






static unsigned int evaluateParsimony(pllInstance *tr, partitionList *pr, nodeptr p, pllBoolean full, int perSiteScores)
{
	volatile unsigned int result;
	nodeptr q = p->back;
	int
		*ti = tr->ti,
		counter = 4;

	ti[1] = p->number;
	ti[2] = q->number;

	if(full){
		if(p->number > tr->mxtips)
			computeTraversalInfoParsimony(p, ti, &counter, tr->mxtips, full, perSiteScores);
		if(q->number > tr->mxtips)
			computeTraversalInfoParsimony(q, ti, &counter, tr->mxtips, full, perSiteScores);
	}else{
		if(p->number > tr->mxtips && !p->xPars)
			computeTraversalInfoParsimony(p, ti, &counter, tr->mxtips, full, perSiteScores);
		if(q->number > tr->mxtips && !q->xPars)
			computeTraversalInfoParsimony(q, ti, &counter, tr->mxtips, full, perSiteScores);
	}

	ti[0] = counter;

	result = evaluateParsimonyIterativeFast(tr, pr, perSiteScores);

	return result;
}


static void newviewParsimony(pllInstance *tr, partitionList *pr, nodeptr  p, int perSiteScores)
{
  if(p->number <= tr->mxtips)
    return;

  {
    int
      counter = 4;

    computeTraversalInfoParsimony(p, tr->ti, &counter, tr->mxtips, PLL_FALSE, perSiteScores);
    tr->ti[0] = counter;

    newviewParsimonyIterativeFast(tr, pr, perSiteScores);
  }
}





/****************************************************************************************************************************************/

static void insertParsimony (pllInstance *tr, partitionList *pr, nodeptr p, nodeptr q, int perSiteScores)
{
  nodeptr  r;

  r = q->back;

  hookupDefault(p->next,       q);
  hookupDefault(p->next->next, r);

  newviewParsimony(tr, pr, p, perSiteScores);
}



static nodeptr buildNewTip (pllInstance *tr, nodeptr p)
{
  nodeptr  q;

  q = tr->nodep[(tr->nextnode)++];
  hookupDefault(p, q);
  q->next->back = (nodeptr)NULL;
  q->next->next->back = (nodeptr)NULL;

  return  q;
}

static void buildSimpleTree (pllInstance *tr, partitionList *pr, int ip, int iq, int ir)
{
  nodeptr  p, s;
  int  i;

  i = PLL_MIN(ip, iq);
  if (ir < i)  i = ir;
  tr->start = tr->nodep[i];
  tr->ntips = 3;
  p = tr->nodep[ip];
  hookupDefault(p, tr->nodep[iq]);
  s = buildNewTip(tr, tr->nodep[ir]);
  insertParsimony(tr, pr, s, p, PLL_FALSE);
}

// Copied from Tung's nnisearch.cpp
static topol *_setupTopol(pllInstance* tr) {
	static topol* tree;
	if (tree == NULL)
		tree = setupTopol(tr->mxtips);
	return tree;
}

// Copied from PLL topologies.c
/* @brief Transform tree to a given topology and evaluate likelihood

   Transform our current tree topology to the one stored in \a tpl and
   evaluates the likelihood

   @param tr
     PLL instance

   @param pr
     List of partitions

   @return
     \b PLL_TRUE

   @todo
     Remove the return value, unnecessary

*/
static pllBoolean _restoreTree (topol *tpl, pllInstance *tr, partitionList *pr)
{
  connptr  r;
  nodeptr  p, p0;
  int  i;

  /* first of all set all backs to NULL so that tips do not point anywhere */
  for (i = 1; i <= 2*(tr->mxtips) - 2; i++)
    {
      /* Uses p = p->next at tip */
      p0 = p = tr->nodep[i];
      do
	{
	  p->back = (nodeptr) NULL;
	  p = p->next;
	}
      while (p != p0);
    }

  /*  Copy connections from topology */

  /* then connect the nodes together */
  for (r = tpl->links, i = 0; i < tpl->nextlink; r++, i++)
    hookup(r->p, r->q, r->z, pr->perGeneBranchLengths?pr->numberOfPartitions:1);

//  tr->likelihood = tpl->likelihood;
  tr->start      = tpl->start;
  tr->ntips      = tpl->ntips;

  tr->nextnode   = tpl->nextnode;

//  pllEvaluateLikelihood (tr, pr, tr->start, PLL_TRUE, PLL_FALSE);
  return PLL_TRUE;
}


static void reorderNodes(pllInstance *tr, nodeptr *np, nodeptr p, int *count)
{
  int i, found = 0;

  if((p->number <= tr->mxtips))
    return;
  else
    {
      for(i = tr->mxtips + 1; (i <= (tr->mxtips + tr->mxtips - 1)) && (found == 0); i++)
        {
          if (p == np[i] || p == np[i]->next || p == np[i]->next->next)
            {
              if(p == np[i])
                tr->nodep[*count + tr->mxtips + 1] = np[i];
              else
                {
                  if(p == np[i]->next)
                    tr->nodep[*count + tr->mxtips + 1] = np[i]->next;
                  else
                    tr->nodep[*count + tr->mxtips + 1] = np[i]->next->next;
                }

              found = 1;
              *count = *count + 1;
            }
        }

      assert(found != 0);

      reorderNodes(tr, np, p->next->back, count);
      reorderNodes(tr, np, p->next->next->back, count);
    }
}




static void nodeRectifierPars(pllInstance *tr)
{
  nodeptr *np = (nodeptr *)rax_malloc(2 * tr->mxtips * sizeof(nodeptr));
  int i;
  int count = 0;

  tr->start       = tr->nodep[1];
  tr->rooted      = PLL_FALSE;

  /* TODO why is tr->rooted set to PLL_FALSE here ?*/

  for(i = tr->mxtips + 1; i <= (tr->mxtips + tr->mxtips - 1); i++)
    np[i] = tr->nodep[i];

  reorderNodes(tr, np, tr->start->back, &count);


  rax_free(np);
}




static void testInsertParsimony (pllInstance *tr, partitionList *pr, nodeptr p, nodeptr q, pllBoolean saveBranches, int perSiteScores)
{
  unsigned int
    mp;

  nodeptr
    r = q->back;

  pllBoolean
    doIt = PLL_TRUE;

  int numBranches = pr->perGeneBranchLengths?pr->numberOfPartitions:1;

  if(tr->grouped)
    {
      int
        rNumber = tr->constraintVector[r->number],
        qNumber = tr->constraintVector[q->number],
        pNumber = tr->constraintVector[p->number];

      doIt = PLL_FALSE;

      if(pNumber == -9)
        pNumber = checkerPars(tr, p->back);
      if(pNumber == -9)
        doIt = PLL_TRUE;
      else
        {
          if(qNumber == -9)
            qNumber = checkerPars(tr, q);

          if(rNumber == -9)
            rNumber = checkerPars(tr, r);

          if(pNumber == rNumber || pNumber == qNumber)
            doIt = PLL_TRUE;
        }
    }

  if(doIt)
    {
      double
        *z = (double *)rax_malloc(numBranches*sizeof(double)); // Diep: copy from the latest pllrepo (fastDNAparsimony.c)

      if(saveBranches)
        {
          int i;

          for(i = 0; i < numBranches; i++)
            z[i] = q->z[i];
        }

      insertParsimony(tr, pr, p, q, perSiteScores);

      mp = evaluateParsimony(tr, pr, p->next->next, PLL_FALSE, perSiteScores);

//		if(globalParam->gbo_replicates > 0 && perSiteScores){
		if(perSiteScores){
			// If UFBoot is enabled ...
			pllSaveCurrentTreeSprParsimony(tr, pr, mp); // run UFBoot
		}

		if(mp < tr->bestParsimony) bestTreeScoreHits = 1;
		else if(mp == tr->bestParsimony) bestTreeScoreHits++;

		if((mp < tr->bestParsimony) ||
			((mp == tr->bestParsimony) && (random_double() <= 1.0 / bestTreeScoreHits))){
			tr->bestParsimony = mp;
			tr->insertNode = q;
			tr->removeNode = p;
		}

      if(saveBranches)
        hookup(q, r, z, numBranches);
      else
        hookupDefault(q, r);

      p->next->next->back = p->next->back = (nodeptr) NULL;
      rax_free(z); // Diep: copy from the latest pllrepo (fastDNAparsimony.c)
    }

  return;
}


static void restoreTreeParsimony(pllInstance *tr, partitionList *pr, nodeptr p, nodeptr q, int perSiteScores)
{
  nodeptr
    r = q->back;

  int counter = 4;

  hookupDefault(p->next,       q);
  hookupDefault(p->next->next, r);

  computeTraversalInfoParsimony(p, tr->ti, &counter, tr->mxtips, PLL_FALSE, perSiteScores);
  tr->ti[0] = counter;

  newviewParsimonyIterativeFast(tr, pr, perSiteScores);
}


static void addTraverseParsimony (pllInstance *tr, partitionList *pr, nodeptr p, nodeptr q, int mintrav, int maxtrav, pllBoolean doAll, pllBoolean saveBranches, int perSiteScores)
{
  if (doAll || (--mintrav <= 0))
    testInsertParsimony(tr, pr, p, q, saveBranches, perSiteScores);

  if (((q->number > tr->mxtips)) && ((--maxtrav > 0) || doAll))
    {
      addTraverseParsimony(tr, pr, p, q->next->back, mintrav, maxtrav, doAll, saveBranches, perSiteScores);
      addTraverseParsimony(tr, pr, p, q->next->next->back, mintrav, maxtrav, doAll, saveBranches, perSiteScores);
    }
}


static void makePermutationFast(int *perm, int n, pllInstance *tr)
{
  int
    i,
    j,
    k;

  for (i = 1; i <= n; i++)
    perm[i] = i;

  for (i = 1; i <= n; i++)
    {
      double d =  randum(&tr->randomNumberSeed);

      k =  (int)((double)(n + 1 - i) * d);

      j        = perm[i];

      perm[i]     = perm[i + k];
      perm[i + k] = j;
    }
}

//static nodeptr  removeNodeParsimony (nodeptr p, tree *tr)
static nodeptr  removeNodeParsimony (nodeptr p)
{
  nodeptr  q, r;

  q = p->next->back;
  r = p->next->next->back;

  hookupDefault(q, r);

  p->next->next->back = p->next->back = (node *) NULL;

  return  q;
}

static int rearrangeParsimony(pllInstance *tr, partitionList *pr, nodeptr p, int mintrav, int maxtrav, pllBoolean doAll, int perSiteScores)
{
	nodeptr
		p1,
		p2,
		q,
		q1,
		q2;

  int
    mintrav2;

  pllBoolean
    doP = PLL_TRUE,
    doQ = PLL_TRUE;

  if (maxtrav > tr->ntips - 3)
    maxtrav = tr->ntips - 3;

  assert(mintrav == 1);

  if(maxtrav < mintrav)
    return 0;

  q = p->back;

	unsigned int mp = evaluateParsimony(tr, pr, p, PLL_FALSE, perSiteScores); // Diep: This is VERY important to make sure SPR is accurate*****
	if(perSiteScores){
		// If UFBoot is enabled ...
		pllSaveCurrentTreeSprParsimony(tr, pr, mp); // run UFBoot
	}

  if(tr->constrained)
    {
      if(! tipHomogeneityCheckerPars(tr, p->back, 0))
        doP = PLL_FALSE;

      if(! tipHomogeneityCheckerPars(tr, q->back, 0))
        doQ = PLL_FALSE;

      if(doQ == PLL_FALSE && doP == PLL_FALSE)
        return 0;
    }

  if((p->number > tr->mxtips) && doP)
    {
      p1 = p->next->back;
      p2 = p->next->next->back;

      if ((p1->number > tr->mxtips) || (p2->number > tr->mxtips))
        {
          //removeNodeParsimony(p, tr);
          removeNodeParsimony(p);

          if ((p1->number > tr->mxtips))
            {
              addTraverseParsimony(tr, pr, p, p1->next->back, mintrav, maxtrav, doAll, PLL_FALSE, perSiteScores);
              addTraverseParsimony(tr, pr, p, p1->next->next->back, mintrav, maxtrav, doAll, PLL_FALSE, perSiteScores);
            }

          if ((p2->number > tr->mxtips))
            {
              addTraverseParsimony(tr, pr, p, p2->next->back, mintrav, maxtrav, doAll, PLL_FALSE, perSiteScores);
              addTraverseParsimony(tr, pr, p, p2->next->next->back, mintrav, maxtrav, doAll, PLL_FALSE, perSiteScores);
            }


          hookupDefault(p->next,       p1);
          hookupDefault(p->next->next, p2);

          newviewParsimony(tr, pr, p, perSiteScores);
        }
    }

  if ((q->number > tr->mxtips) && (maxtrav > 0) && doQ)
    {
      q1 = q->next->back;
      q2 = q->next->next->back;

      if (
          (
           (q1->number > tr->mxtips) &&
           ((q1->next->back->number > tr->mxtips) || (q1->next->next->back->number > tr->mxtips))
           )
          ||
          (
           (q2->number > tr->mxtips) &&
           ((q2->next->back->number > tr->mxtips) || (q2->next->next->back->number > tr->mxtips))
           )
          )
        {

          //removeNodeParsimony(q, tr);
          removeNodeParsimony(q);

          mintrav2 = mintrav > 2 ? mintrav : 2;

          if ((q1->number > tr->mxtips))
            {
              addTraverseParsimony(tr, pr, q, q1->next->back, mintrav2 , maxtrav, doAll, PLL_FALSE, perSiteScores);
              addTraverseParsimony(tr, pr, q, q1->next->next->back, mintrav2 , maxtrav, doAll, PLL_FALSE, perSiteScores);
            }

          if ((q2->number > tr->mxtips))
            {
              addTraverseParsimony(tr, pr, q, q2->next->back, mintrav2 , maxtrav, doAll, PLL_FALSE, perSiteScores);
              addTraverseParsimony(tr, pr, q, q2->next->next->back, mintrav2 , maxtrav, doAll, PLL_FALSE, perSiteScores);
            }

          hookupDefault(q->next,       q1);
          hookupDefault(q->next->next, q2);

          newviewParsimony(tr, pr, q, perSiteScores);
        }
    }

  return 1;
}


static void restoreTreeRearrangeParsimony(pllInstance *tr, partitionList *pr, int perSiteScores)
{
  removeNodeParsimony(tr->removeNode);
  //removeNodeParsimony(tr->removeNode, tr);
  restoreTreeParsimony(tr, pr, tr->removeNode, tr->insertNode, perSiteScores);
}

/*
static pllBoolean isInformative2(pllInstance *tr, int site)
{
  int
    informativeCounter = 0,
    check[256],
    j,
    undetermined = 15;

  unsigned char
    nucleotide,
    target = 0;

  for(j = 0; j < 256; j++)
    check[j] = 0;

  for(j = 1; j <= tr->mxtips; j++)
    {
      nucleotide = tr->yVector[j][site];
      check[nucleotide] =  check[nucleotide] + 1;
    }


  if(check[1] > 1)
    {
      informativeCounter++;
      target = target | 1;
    }
  if(check[2] > 1)
    {
      informativeCounter++;
      target = target | 2;
    }
  if(check[4] > 1)
    {
      informativeCounter++;
      target = target | 4;
    }
  if(check[8] > 1)
    {
      informativeCounter++;
      target = target | 8;
    }

  if(informativeCounter >= 2)
    return PLL_TRUE;
  else
    {
      for(j = 0; j < undetermined; j++)
        {
          if(j == 3 || j == 5 || j == 6 || j == 7 || j == 9 || j == 10 || j == 11 ||
             j == 12 || j == 13 || j == 14)
            {
              if(check[j] > 1)
                {
                  if(!(target & j))
                    return PLL_TRUE;
                }
            }
        }
    }

  return PLL_FALSE;
}
*/

/*
 * Diep: copy new version from Tomas's code for site pars
 */
/* check whether site contains at least 2 different letters, i.e.
   whether it will generate a score */
static pllBoolean isInformative(pllInstance *tr, int dataType, int site)
{
	if(globalParam && !globalParam->sort_alignment)
		return PLL_TRUE; // because of the sync between IQTree and PLL alignment (to get correct freq of pattern)

	int
		informativeCounter = 0,
		check[256],
		j,
		undetermined = getUndetermined(dataType);

	const unsigned int
		*bitVector = getBitVector(dataType);

	unsigned char
		nucleotide;


	for(j = 0; j < 256; j++)
		check[j] = 0;

	for(j = 1; j <= tr->mxtips; j++)
	{
		nucleotide = tr->yVector[j][site];
		check[nucleotide] = 1;
		assert(bitVector[nucleotide] > 0);
	}

	for(j = 0; j < undetermined; j++)
	{
		if(check[j] > 0)
		informativeCounter++;
	}

	if(informativeCounter > 1)
		return PLL_TRUE;

	return PLL_FALSE;

}

static bool isUnambiguous(char nucleotide, int dataType){
	// PLL_DNA_DATA or PLL_DNA_AA
	if(dataType == PLL_DNA_DATA)
		return (nucleotide == 1) || (nucleotide == 2) || (nucleotide == 4) || (nucleotide == 8);
	else if(dataType == PLL_DNA_DATA)
		return nucleotide < 20;
	else return true;
}
/*
 * Diep: compute minimum parsimony of the given site
 *
 */
int pllCalcMinParsScorePattern(pllInstance *tr, int dataType, int site)
{
	int
		informativeCounter = 0,
		check[256],
		j,
		undetermined = getUndetermined(dataType);

	const unsigned int
		*bitVector = getBitVector(dataType);

	unsigned char
		nucleotide;


	for(j = 0; j < 256; j++)
		check[j] = 0;

	for(j = 1; j <= tr->mxtips; j++)
	{
		nucleotide = tr->yVector[j][site];
		check[nucleotide] = 1;
		assert(bitVector[nucleotide] > 0);
	}

	for(j = 0; j < undetermined; j++)
	{
		if(check[j] > 0 && isUnambiguous(j, dataType))
			informativeCounter++;
	}

	return informativeCounter - 1;
}

//
//static pllBoolean isInformative(pllInstance *tr, int dataType, int site)
//{
//  int
//    informativeCounter = 0,
//    check[256],
//    j,
//    undetermined = getUndetermined(dataType);
//
//  const unsigned int
//    *bitVector = getBitVector(dataType);
//
//  unsigned char
//    nucleotide;
//
//
//  for(j = 0; j < 256; j++)
//    check[j] = 0;
//
//  for(j = 1; j <= tr->mxtips; j++)
//    {
//      nucleotide = tr->yVector[j][site];
//      check[nucleotide] =  check[nucleotide] + 1;
//      assert(bitVector[nucleotide] > 0);
//    }
//
//  for(j = 0; j < undetermined; j++)
//    {
//      if(check[j] > 0)
//        informativeCounter++;
//    }
//
//  if(informativeCounter <= 1)
//    return PLL_FALSE;
//  else
//    {
//      for(j = 0; j < undetermined; j++)
//        {
//          if(check[j] > 1)
//            return PLL_TRUE;
//        }
//    }
//
//  return PLL_FALSE;
//}


static void determineUninformativeSites(pllInstance *tr, partitionList *pr, int *informative)
{
  int
    model,
    number = 0,
    i;

  /*
     Not all characters are useful in constructing a parsimony tree.
     Invariant characters, those that have the same state in all taxa,
     are obviously useless and are ignored by the method. Characters in
     which a state occurs in only one taxon are also ignored.
     All these characters are called parsimony uninformative.

     Alternative definition: informative columns contain at least two types
     of nucleotides, and each nucleotide must appear at least twice in each
     column. Kind of a pain if we intend to check for this when using, e.g.,
     amibiguous DNA encoding.
  */


  for(model = 0; model < pr->numberOfPartitions; model++)
    {

      for(i = pr->partitionData[model]->lower; i < pr->partitionData[model]->upper; i++)
        {
           if(isInformative(tr, pr->partitionData[model]->dataType, i)){
             informative[i] = 1;
           }
           else
             {
               informative[i] = 0;
             }
        }
    }


  /* printf("Uninformative Patterns: %d\n", number); */
}


static void compressDNA(pllInstance *tr, partitionList *pr, int *informative, int perSiteScores)
{
//	printf("compress with fastDNAparsimony.c\n");
  size_t
    totalNodes,
    i,
    model;

  totalNodes = 2 * (size_t)tr->mxtips;



  for(model = 0; model < (size_t) pr->numberOfPartitions; model++)
    {
      size_t
        k,
        states = (size_t)pr->partitionData[model]->states,
        compressedEntries,
        compressedEntriesPadded,
        entries = 0,
        lower = pr->partitionData[model]->lower,
        upper = pr->partitionData[model]->upper;

      parsimonyNumber
        **compressedTips = (parsimonyNumber **)rax_malloc(states * sizeof(parsimonyNumber*)),
        *compressedValues = (parsimonyNumber *)rax_malloc(states * sizeof(parsimonyNumber));

      for(i = lower; i < upper; i++)
        if(informative[i])
          entries += (size_t)tr->aliaswgt[i];

      compressedEntries = entries / PLL_PCF;

      if(entries % PLL_PCF != 0)
        compressedEntries++;

#if (defined(__SSE3) || defined(__AVX))
      if(compressedEntries % INTS_PER_VECTOR != 0)
        compressedEntriesPadded = compressedEntries + (INTS_PER_VECTOR - (compressedEntries % INTS_PER_VECTOR));
      else
        compressedEntriesPadded = compressedEntries;
#else
      compressedEntriesPadded = compressedEntries;
#endif


      rax_posix_memalign ((void **) &(pr->partitionData[model]->parsVect), PLL_BYTE_ALIGNMENT, (size_t)compressedEntriesPadded * states * totalNodes * sizeof(parsimonyNumber));

      for(i = 0; i < compressedEntriesPadded * states * totalNodes; i++)
        pr->partitionData[model]->parsVect[i] = 0;

      if (perSiteScores)
       {
         /* for per site parsimony score at each node */
         rax_posix_memalign ((void **) &(pr->partitionData[model]->perSitePartialPars), PLL_BYTE_ALIGNMENT, totalNodes * (size_t)compressedEntriesPadded * PLL_PCF * sizeof (parsimonyNumber));
         for (i = 0; i < totalNodes * (size_t)compressedEntriesPadded * PLL_PCF; ++i)
        	 pr->partitionData[model]->perSitePartialPars[i] = 0;
       }

      for(i = 0; i < (size_t)tr->mxtips; i++)
        {
          size_t
            w = 0,
            compressedIndex = 0,
            compressedCounter = 0,
            index = 0;

          for(k = 0; k < states; k++)
            {
              compressedTips[k] = &(pr->partitionData[model]->parsVect[(compressedEntriesPadded * states * (i + 1)) + (compressedEntriesPadded * k)]);
              compressedValues[k] = 0;
            }

          for(index = lower; index < (size_t)upper; index++)
            {
              if(informative[index])
                {
                  const unsigned int
                    *bitValue = getBitVector(pr->partitionData[model]->dataType);

                  parsimonyNumber
                    value = bitValue[tr->yVector[i + 1][index]];

                  for(w = 0; w < (size_t)tr->aliaswgt[index]; w++)
                    {
                      for(k = 0; k < states; k++)
                        {
                          if(value & mask32[k])
                            compressedValues[k] |= mask32[compressedCounter];
                        }

                      compressedCounter++;

                      if(compressedCounter == PLL_PCF)
                        {
                          for(k = 0; k < states; k++)
                            {
                              compressedTips[k][compressedIndex] = compressedValues[k];
                              compressedValues[k] = 0;
                            }

                          compressedCounter = 0;
                          compressedIndex++;
                        }
                    }
                }
            }

          for(;compressedIndex < compressedEntriesPadded; compressedIndex++)
            {
              for(;compressedCounter < PLL_PCF; compressedCounter++)
                for(k = 0; k < states; k++)
                  compressedValues[k] |= mask32[compressedCounter];

              for(k = 0; k < states; k++)
                {
                  compressedTips[k][compressedIndex] = compressedValues[k];
                  compressedValues[k] = 0;
                }

              compressedCounter = 0;
            }
        }

      pr->partitionData[model]->parsimonyLength = compressedEntriesPadded;

      rax_free(compressedTips);
      rax_free(compressedValues);
    }

  rax_posix_memalign ((void **) &(tr->parsimonyScore), PLL_BYTE_ALIGNMENT, sizeof(unsigned int) * totalNodes);

  for(i = 0; i < totalNodes; i++)
    tr->parsimonyScore[i] = 0;
}


static void stepwiseAddition(pllInstance *tr, partitionList *pr, nodeptr p, nodeptr q)
{
  nodeptr
    r = q->back;

  unsigned int
    mp;

  int
    counter = 4;

  p->next->back = q;
  q->back = p->next;

  p->next->next->back = r;
  r->back = p->next->next;

  computeTraversalInfoParsimony(p, tr->ti, &counter, tr->mxtips, PLL_FALSE, PLL_FALSE);
  tr->ti[0] = counter;
  tr->ti[1] = p->number;
  tr->ti[2] = p->back->number;

  mp = evaluateParsimonyIterativeFast(tr, pr, PLL_FALSE);

  if(mp < tr->bestParsimony) bestTreeScoreHits = 1;
  else if(mp == tr->bestParsimony) bestTreeScoreHits++;

  if((mp < tr->bestParsimony) || ((mp == tr->bestParsimony) && (random_double() <= 1.0 / bestTreeScoreHits)))
    {
      tr->bestParsimony = mp;
      tr->insertNode = q;
    }

  q->back = r;
  r->back = q;

  if(q->number > tr->mxtips && tr->parsimonyScore[q->number] > 0)
    {
      stepwiseAddition(tr, pr, p, q->next->back);
      stepwiseAddition(tr, pr, p, q->next->next->back);
    }
}


void _updateInternalPllOnRatchet(pllInstance *tr, partitionList *pr){
//	cout << "lower = " << pr->partitionData[0]->lower << ", upper = " << pr->partitionData[0]->upper << ", aln->size() = " << iqtree->aln->size() << endl;
	for(int i = 0; i < pr->numberOfPartitions; i++){
		for(int ptn = pr->partitionData[i]->lower; ptn < pr->partitionData[i]->upper; ptn++){
			tr->aliaswgt[ptn] = iqtree->aln->at(ptn).frequency;
		}
	}
}


void _allocateParsimonyDataStructures(pllInstance *tr, partitionList *pr, int perSiteScores)
{
	  int i;
	  int * informative = (int *)rax_malloc(sizeof(int) * (size_t)tr->originalCrunchedLength);
	  determineUninformativeSites(tr, pr, informative);

	  compressDNA(tr, pr, informative, perSiteScores);

	  for(i = tr->mxtips + 1; i <= tr->mxtips + tr->mxtips - 1; i++)
	    {
	      nodeptr
	        p = tr->nodep[i];

	      p->xPars = 1;
	      p->next->xPars = 0;
	      p->next->next->xPars = 0;
	    }

	  tr->ti = (int*)rax_malloc(sizeof(int) * 4 * (size_t)tr->mxtips);

	  rax_free(informative);
}

void _pllFreeParsimonyDataStructures(pllInstance *tr, partitionList *pr)
{
  size_t
    model;

  if(tr->parsimonyScore != NULL){
	  rax_free(tr->parsimonyScore);
	  tr->parsimonyScore = NULL;
  }

  for(model = 0; model < (size_t) pr->numberOfPartitions; ++model){
	  if(pr->partitionData[model]->parsVect != NULL){
		  rax_free(pr->partitionData[model]->parsVect);
		  pr->partitionData[model]->parsVect = NULL;
	  }
	  if(pr->partitionData[model]->perSitePartialPars != NULL){
		  rax_free(pr->partitionData[model]->perSitePartialPars);
		  pr->partitionData[model]->perSitePartialPars = NULL;
	  }
  }

  if(tr->ti != NULL){
	  rax_free(tr->ti);
	  tr->ti = NULL;
  }
}


static void _pllMakeParsimonyTreeFast(pllInstance *tr, partitionList *pr, int sprDist)
{
  nodeptr
    p,
    f;

  int
    i,
    nextsp,
    *perm        = (int *)rax_malloc((size_t)(tr->mxtips + 1) * sizeof(int));

  unsigned int
    randomMP,
    startMP;

  assert(!tr->constrained);

  makePermutationFast(perm, tr->mxtips, tr);

  tr->ntips = 0;

  tr->nextnode = tr->mxtips + 1;

  buildSimpleTree(tr, pr, perm[1], perm[2], perm[3]);

  f = tr->start;

  bestTreeScoreHits = 1;

  while(tr->ntips < tr->mxtips)
    {
      nodeptr q;

      tr->bestParsimony = INT_MAX;
      nextsp = ++(tr->ntips);
      p = tr->nodep[perm[nextsp]];
      q = tr->nodep[(tr->nextnode)++];
      p->back = q;
      q->back = p;

      if(tr->grouped)
        {
          int
            number = p->back->number;

          tr->constraintVector[number] = -9;
        }

      stepwiseAddition(tr, pr, q, f->back);

      {
        nodeptr
          r = tr->insertNode->back;

        int counter = 4;

        hookupDefault(q->next,       tr->insertNode);
        hookupDefault(q->next->next, r);

        computeTraversalInfoParsimony(q, tr->ti, &counter, tr->mxtips, PLL_FALSE, 0);
        tr->ti[0] = counter;

        newviewParsimonyIterativeFast(tr, pr, 0);
      }
    }

  nodeRectifierPars(tr);

  randomMP = tr->bestParsimony;

	int * hill_climbing_perm = (int *)rax_malloc((size_t)(tr->mxtips + tr->mxtips - 1) * sizeof(int));
	int j;
//	makePermutationFast(hill_climbing_perm, tr->mxtips + tr->mxtips - 2, tr);

	unsigned int bestIterationScoreHits = 1;

	do
	{
		startMP = randomMP;
		nodeRectifierPars(tr);
		for(i = 1; i <= tr->mxtips + tr->mxtips - 2; i++)
//		for(j = 1; j <= tr->mxtips + tr->mxtips - 2; j++)
		{
//			i = hill_climbing_perm[j];
			tr->removeNode = tr->insertNode = NULL;
			bestTreeScoreHits = 1;
			rearrangeParsimony(tr, pr, tr->nodep[i], 1, sprDist, PLL_FALSE, 0);
			if(tr->bestParsimony == randomMP) bestIterationScoreHits++;
			if(tr->bestParsimony < randomMP) bestIterationScoreHits = 1;
			if(((tr->bestParsimony < randomMP) ||
				((tr->bestParsimony == randomMP) && (random_double() <= 1.0 / bestIterationScoreHits))) &&
					tr->removeNode && tr->insertNode)
			{
				restoreTreeRearrangeParsimony(tr, pr, 0);
				randomMP = tr->bestParsimony;
			}
		}
	}while(randomMP < startMP);

  rax_free(perm);
}

/** @brief Compute a randomized stepwise addition oder parsimony tree

    Implements the RAxML randomized stepwise addition order algorithm

    @todo
      check functions that are invoked for potential memory leaks!

    @param tr
      The PLL instance

    @param partitions
      The partitions
*/
void _pllComputeRandomizedStepwiseAdditionParsimonyTree(pllInstance * tr, partitionList * partitions, int sprDist)
{
	_allocateParsimonyDataStructures(tr, partitions, PLL_FALSE);
	_pllMakeParsimonyTreeFast(tr, partitions, sprDist);
	_pllFreeParsimonyDataStructures(tr, partitions);
}

/**
 * DTH: optimize whatever tree is stored in tr by parsimony SPR
 * @param tr: the tree instance :)
 * @param partition: the data partition :)
 * @param mintrav, maxtrav are PLL limitations for SPR radius
 * @return best parsimony score found
 */
int pllOptimizeSprParsimony(pllInstance * tr, partitionList * pr, int mintrav, int maxtrav, IQTree *_iqtree){
	int perSiteScores = globalParam->gbo_replicates > 0;
	bool first_call = false; // is this the first call to pllOptimizeSprParsimony()
	if(!iqtree){
		iqtree = _iqtree;
		first_call = true;
	}
	if(globalParam->ratchet_iter >= 0){
		_updateInternalPllOnRatchet(tr, pr);
		_allocateParsimonyDataStructures(tr, pr, perSiteScores); // called once if not running ratchet
	}else if(first_call || (iqtree && iqtree->on_opt_btree))
		_allocateParsimonyDataStructures(tr, pr, perSiteScores); // called once if not running ratchet

//	if(((globalParam->ratchet_iter >= 0 || globalParam->optimize_boot_trees) && (!globalParam->hclimb1_nni)) || (!iqtree)){
//		iqtree = _iqtree;
//		// consider updating tr->yVector, then tr->aliaswgt (similar as in pllLoadAlignment)
//		if((globalParam->ratchet_iter >= 0  || globalParam->optimize_boot_trees) && (!globalParam->hclimb1_nni))
//			_updateInternalPllOnRatchet(tr, pr);
//		_allocateParsimonyDataStructures(tr, pr, perSiteScores); // called once if not running ratchet
//	}

	int i;
	unsigned int
		randomMP,
		startMP;

	assert(!tr->constrained);

	nodeRectifierPars(tr);
	tr->bestParsimony = UINT_MAX;
	tr->bestParsimony = evaluateParsimony(tr, pr, tr->start, PLL_TRUE, perSiteScores);
//	cout << "\ttr->bestParsimony (initial tree) = " << tr->bestParsimony << endl;
	/*
	// Diep: to be investigated
	tr->bestParsimony = -iqtree->logl_cutoff;
	evaluateParsimony(tr, pr, tr->start, PLL_TRUE, perSiteScores);
	*/

	int j, *perm        = (int *)rax_malloc((size_t)(tr->mxtips + tr->mxtips - 1) * sizeof(int));
//	makePermutationFast(perm, tr->mxtips + tr->mxtips - 2, tr);

	unsigned int bestIterationScoreHits = 1;
	randomMP = tr->bestParsimony;
	tr->ntips = tr->mxtips;
	do{
		startMP = randomMP;
		nodeRectifierPars(tr);
		for(i = 1; i <= tr->mxtips + tr->mxtips - 2; i++){
//		for(j = 1; j <= tr->mxtips + tr->mxtips - 2; j++){
//			i = perm[j];
			tr->insertNode = NULL;
			tr->removeNode = NULL;
			bestTreeScoreHits = 1;

			rearrangeParsimony(tr, pr, tr->nodep[i], mintrav, maxtrav, PLL_FALSE, perSiteScores);
			if(tr->bestParsimony == randomMP) bestIterationScoreHits++;
			if(tr->bestParsimony < randomMP) bestIterationScoreHits = 1;
			if(((tr->bestParsimony < randomMP) ||
					((tr->bestParsimony == randomMP) &&
						(random_double() <= 1.0 / bestIterationScoreHits))) &&
					tr->removeNode && tr->insertNode){
				restoreTreeRearrangeParsimony(tr, pr, perSiteScores);
				randomMP = tr->bestParsimony;
			}
		}
	}while(randomMP < startMP);

	return startMP;
}


int pllSaveCurrentTreeSprParsimony(pllInstance * tr, partitionList * pr, int cur_search_pars){
	iqtree->saveCurrentTree(-cur_search_pars);
	return (int)(cur_search_pars);
}

// TODO: modify for Sankoff $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$
void pllComputePatternParsimony(pllInstance * tr, partitionList * pr, double *ptn_npars, double *cur_npars){
	int ptn = 0;
	int site = 0;
	int sum = 0;

	for(int i = 0; i < pr->numberOfPartitions; i++){
		int partialParsLength = pr->partitionData[i]->parsimonyLength * PLL_PCF;
		parsimonyNumber * p = &(pr->partitionData[i]->perSitePartialPars[partialParsLength * tr->start->number]);

		for(ptn = pr->partitionData[i]->lower; ptn < pr->partitionData[i]->upper; ptn++){
			ptn_npars[ptn] = -p[site];
			sum += ptn_npars[ptn] * tr->aliaswgt[ptn];
			site += tr->aliaswgt[ptn];
		}
	}
	if(cur_npars) *cur_npars = -sum;
}

void pllComputePatternParsimony(pllInstance * tr, partitionList * pr, unsigned short *ptn_pars, int *cur_pars){
	int ptn = 0;
	int site = 0;
	int sum = 0;

//	cout << "Pattern pars by PLL: ";
	for(int i = 0; i < pr->numberOfPartitions; i++){
		int partialParsLength = pr->partitionData[i]->parsimonyLength * PLL_PCF;
		parsimonyNumber * p = &(pr->partitionData[i]->perSitePartialPars[partialParsLength * tr->start->number]);

		for(ptn = pr->partitionData[i]->lower; ptn < pr->partitionData[i]->upper; ptn++){
//			cout << p[site] << ", ";
			ptn_pars[ptn] = p[site];
			sum += ptn_pars[ptn] * tr->aliaswgt[ptn];
			site += tr->aliaswgt[ptn];
		}
	}
//	cout << endl;

	if(cur_pars) *cur_pars = sum;
}


void pllComputePatternParsimonySlow(pllInstance * tr, partitionList * pr, double *ptn_npars, double *cur_npars){
	iqtree->initializeAllPartialPars();
	iqtree->clearAllPartialLH();
	if(cur_npars) *cur_npars = -(iqtree->computeParsimony());
	else
		iqtree->computeParsimony();
}

void pllComputeSiteParsimony(pllInstance * tr, partitionList * pr, int *site_pars, int nsite, int *cur_pars){
	int site = 0;
	int sum = 0;

	for(int i = 0; i < pr->numberOfPartitions; i++){
		int partialParsLength = pr->partitionData[i]->parsimonyLength * PLL_PCF;
		parsimonyNumber * p = &(pr->partitionData[i]->perSitePartialPars[partialParsLength * tr->start->number]);

		for(int k = 0; k < pr->partitionData[i]->width; k++){
			site_pars[site] = p[k];
			sum += site_pars[site];
			site++;
		}
	}

	for(; site < nsite; site++){
		site_pars[site] = 0;
	}
	if(cur_pars) *cur_pars = sum;
}

void pllComputeSiteParsimony(pllInstance * tr, partitionList * pr, unsigned short *site_pars, int nsite, int *cur_pars){
	int site = 0;
	int sum = 0;

	for(int i = 0; i < pr->numberOfPartitions; i++){
		int partialParsLength = pr->partitionData[i]->parsimonyLength * PLL_PCF;
		parsimonyNumber * p = &(pr->partitionData[i]->perSitePartialPars[partialParsLength * tr->start->number]);

		for(int k = 0; k < pr->partitionData[i]->width; k++){
			site_pars[site] = p[k];
			sum += site_pars[site];
			site++;
		}
	}

	for(; site < nsite; site++){
		site_pars[site] = 0;
	}

//	cout << "Site parsimony by PLL: " << endl;
//	for(site = 0; site < nsite; site++){
//		cout << site_pars[site] << ", ";
//	}
//	cout << endl;

	if(cur_pars) *cur_pars = sum;
}

void testSiteParsimony(Params &params) {
//	IQTree * iqtree;
//	iqtree = new IQTree;
//	iqtree->readTree(params.user_file, params.is_rooted);
//	Alignment alignment(params.aln_file, params.sequence_type, params.intype);
//	iqtree->setAlignment(&alignment);
//	iqtree->setParams(params);
//
//	/* Initialized all data structure for PLL*/
//	iqtree->pllAttr.rateHetModel = PLL_GAMMA;
//	iqtree->pllAttr.fastScaling = PLL_FALSE;
//	iqtree->pllAttr.saveMemory = PLL_FALSE;
//	iqtree->pllAttr.useRecom = PLL_FALSE;
//	iqtree->pllAttr.randomNumberSeed = params.ran_seed;
//	iqtree->pllAttr.numberOfThreads = 2; /* This only affects the pthreads version */
//
//	if (iqtree->pllInst != NULL) {
//		pllDestroyInstance(iqtree->pllInst);
//	}
//	/* Create a PLL instance */
//	iqtree->pllInst = pllCreateInstance(&iqtree->pllAttr);
//	cout << "is_rooted = " << params.is_rooted << endl;;
//
//	/* Read in the alignment file */
//	string pllAln = params.out_prefix;
//	pllAln += ".pllaln";
//	alignment.printPhylip(pllAln.c_str());
//
//	iqtree->pllAlignment = pllParseAlignmentFile(PLL_FORMAT_PHYLIP, pllAln.c_str());
//
//	/* Read in the partition information */
//	pllQueue *partitionInfo;
//	ofstream pllPartitionFileHandle;
//	string pllPartitionFileName = string(params.out_prefix) + ".pll_partitions";
//	pllPartitionFileHandle.open(pllPartitionFileName.c_str());
//
//	/* create a partition file */
//	string model;
//	if (alignment.seq_type == SEQ_DNA) {
//		model = "DNA";
//	} else if (alignment.seq_type == SEQ_PROTEIN) {
//		if (params.model_name != "" && params.model_name.substr(0, 4) != "TEST")
//			model = params.model_name.substr(0, params.model_name.find_first_of("+{"));
//		else
//			model = "WAG";
//	} else {
//		outError("PLL only works with DNA/protein alignments");
//	}
//	pllPartitionFileHandle << model << ", p1 = " << "1-" << iqtree->getAlnNSite() << endl;
//
//	pllPartitionFileHandle.close();
//	partitionInfo = pllPartitionParse(pllPartitionFileName.c_str());
//
//	/* Validate the partitions */
//	if (!pllPartitionsValidate(partitionInfo, iqtree->pllAlignment)) {
//		fprintf(stderr, "Error: Partitions do not cover all sites\n");
//		exit(EXIT_FAILURE);
//	}
//
//	/* Commit the partitions and build a partitions structure */
//	iqtree->pllPartitions = pllPartitionsCommit(partitionInfo, iqtree->pllAlignment);
//
//	/* We don't need the the intermediate partition queue structure anymore */
//	pllQueuePartitionsDestroy(&partitionInfo);
//
//	/* eliminate duplicate sites from the alignment and update weights vector */
//	pllAlignmentRemoveDups(iqtree->pllAlignment, iqtree->pllPartitions);
//
//
//	cout << "Finished initialization!" << endl;
//
//	/************************************ END: Initialization for PLL and sNNI *************************************************/
//	pllNewickTree * newick;
//	newick = pllNewickParseString (iqtree->getTreeString().c_str());
//	iqtree->pllInst->randomNumberSeed = params.ran_seed;
//	pllTreeInitTopologyNewick (iqtree->pllInst, newick, PLL_FALSE);
//	/* Connect the alignment and partition structure with the tree structure */
//	if (!pllLoadAlignment(iqtree->pllInst, iqtree->pllAlignment, iqtree->pllPartitions)) {
//		printf("Incompatible tree/alignment combination\n");
//		exit(1);
//	}
//
//	int npatterns = iqtree->aln->getNPattern();
//
//	int i, j, uninformative_count = 0;
//	pllInitParsimonyStructures (iqtree->pllInst, iqtree->pllPartitions, PLL_TRUE);   // the last parameter is that you want to allocate buffers for the per site computation
//
//	iqtree->pllInst->bestParsimony = UINT_MAX;
//
//	for (i = 0; i < iqtree->pllPartitions->numberOfPartitions; ++i)
//	{
//		for (j = 0; j < iqtree->pllPartitions->partitionData[i]->width; ++j){
//			if(!isInformative(iqtree->pllInst, iqtree->pllPartitions->partitionData[i]->dataType, j))
//				uninformative_count++;
//		}
//	}
//	cout << "There are " << uninformative_count << " uninformative sites." << endl;
//
//	cout << "************** PARSIMONY SCORES BY PLL **************************" << endl;
//	printf ("Tree score: %d\n", pllEvaluateParsimony(iqtree->pllInst, iqtree->pllPartitions, iqtree->pllInst->start->back, PLL_TRUE, PLL_TRUE));
//	int sum_test = 0;
//	int zero_count = 0;
//	for (i = 0; i < iqtree->pllPartitions->numberOfPartitions; ++i)
//	{
//		cout << "parsimonyLength of partition " << i << " is " << iqtree->pllPartitions->partitionData[i]->parsimonyLength << endl;
//		for (j = 0; j < iqtree->pllPartitions->partitionData[i]->parsimonyLength * PLL_PCF; ++j){
//			cout << iqtree->pllPartitions->partitionData[i]->perSiteParsScores[j] << " ";
//			sum_test += iqtree->pllPartitions->partitionData[i]->perSiteParsScores[j];
//			if(iqtree->pllPartitions->partitionData[i]->perSiteParsScores[j] == 0) zero_count++;
//		}
//	}
//
//	cout << endl << "sum_test = " << sum_test << ", zero_count = " << zero_count << endl;
//
//	cout << endl << "************** PARSIMONY SCORES BY IQTREE **************************" << endl;
//    pllTreeToNewick(iqtree->pllInst->tree_string, iqtree->pllInst, iqtree->pllPartitions, iqtree->pllInst->start->back,
//            PLL_FALSE, PLL_TRUE, PLL_FALSE, PLL_FALSE, PLL_FALSE, PLL_SUMMARIZE_LH, PLL_FALSE, PLL_FALSE);
//    iqtree->readTreeString(string(iqtree->pllInst->tree_string));
//    iqtree->initializeAllPartialPars();
//    iqtree->clearAllPartialLH();
//
//	cout << "Tree score = " << iqtree->computeParsimony() << endl;
//
//	double * site_scores = new double[npatterns];
//	iqtree->computePatternParsimony(site_scores);
//	for(int k = 0; k < npatterns; k++)
//		for(int t = 0; t < iqtree->aln->at(k).frequency; t++)
//			cout << -int(site_scores[k]) << " ";
//	cout << endl;
//
//	if(site_scores) delete [] site_scores;
//	if(iqtree) delete iqtree;
}


void computeUserTreeParsimomy(Params &params) {
    Alignment alignment(params.aln_file, params.sequence_type, params.intype);
    IQTree tree;
    tree.readTree(params.user_file, params.is_rooted);
	tree.setAlignment(&alignment);
	cout << "Parsimony score is: " << tree.computeParsimony() << endl;
}

/****************************************** UTILS ***************************/

/* Diep begin */
void
pllSortedAlignmentRemoveDups (pllAlignmentData * alignmentData, partitionList * pl)
{
  int i, j, k, p;
  char *** sites;
  void ** memptr;
  int ** oi;
  int dups = 0;
  int lower;

  /* allocate space for the transposed alignments (sites) for every partition */
  sites  = (char ***) rax_malloc (pl->numberOfPartitions * sizeof (char **));
  memptr = (void **)  rax_malloc (pl->numberOfPartitions * sizeof (void *));
  oi     = (int **)   rax_malloc (pl->numberOfPartitions * sizeof (int *));

  /* transpose the sites by partition */
  for (p = 0; p < pl->numberOfPartitions; ++ p)
   {
     sites[p]  = (char **) rax_malloc (pl->partitionData[p]->width * sizeof (char *));
     memptr[p] = rax_malloc ((alignmentData->sequenceCount + 1) * pl->partitionData[p]->width * sizeof (char));

     for (i = 0; i < pl->partitionData[p]->width; ++ i)
      {
        sites[p][i] = (char *) ((char*)memptr[p] + i * (alignmentData->sequenceCount + 1) * sizeof (char));
      }

     for (i = 0; i < pl->partitionData[p]->width; ++ i)
      {
        for (j = 0; j < alignmentData->sequenceCount; ++ j)
         {
           sites[p][i][j] = alignmentData->sequenceData[j + 1][pl->partitionData[p]->lower + i];
         }
        sites[p][i][j] = 0;
      }

//     oi[p] = pllssort1main (sites[p], pl->partitionData[p]->width);

     oi[p] = (int *) rax_malloc (pl->partitionData[p]->width * sizeof (int)); //

     for (i = 0; i < pl->partitionData[p]->width; ++ i) oi[p][i] = 1;

     for (i = 1; i < pl->partitionData[p]->width; ++ i)
      {
        if (! strcmp (sites[p][i], sites[p][i - 1]))
         {
           ++ dups;
           oi[p][i] = 0;
         }
      }
   }

  /* allocate memory for the alignment without duplicates*/
  rax_free (alignmentData->sequenceData[1]);
  rax_free (alignmentData->siteWeights);

  alignmentData->sequenceLength = alignmentData->sequenceLength - dups;
  alignmentData->sequenceData[0] = (unsigned char *) rax_malloc ((alignmentData->sequenceLength + 1) * sizeof (unsigned char) * alignmentData->sequenceCount);
  for (i = 0; i < alignmentData->sequenceCount; ++ i)
   {
     alignmentData->sequenceData[i + 1] = (unsigned char *) (alignmentData->sequenceData[0] + i * (alignmentData->sequenceLength + 1) * sizeof (unsigned char));
     alignmentData->sequenceData[i + 1][alignmentData->sequenceLength] = 0;
   }

  alignmentData->siteWeights    = (int *) rax_malloc ((alignmentData->sequenceLength) * sizeof (int));
  alignmentData->siteWeights[0] = 1;

  /* transpose sites back to alignment */
  for (p = 0, k = 0; p < pl->numberOfPartitions; ++ p)
   {
     lower = k;
     for (i = 0; i < pl->partitionData[p]->width; ++ i)
      {
        if (!oi[p][i])
         {
           ++ alignmentData->siteWeights[k - 1];
         }
        else
         {
           alignmentData->siteWeights[k] = 1;
           for (j = 0; j < alignmentData->sequenceCount; ++ j)
            {
              alignmentData->sequenceData[j + 1][k] = sites[p][i][j];
            }
           ++ k;
         }
      }
     pl->partitionData[p]->lower = lower;
     pl->partitionData[p]->upper = k;
     pl->partitionData[p]->width = k - lower;
   }

  /* deallocate storage for transposed alignment (sites) */
  for (p = 0; p < pl->numberOfPartitions; ++ p)
   {
     rax_free (oi[p]);
     rax_free (memptr[p]);
     rax_free (sites[p]);
   }
  rax_free (oi);
  rax_free (sites);
  rax_free (memptr);
}

/* Diep end */


