/*
* Copyright 2008, 2011 Free Software Foundation, Inc.
*
* This software is distributed under the terms of the GNU Affero Public License.
* See the COPYING file in the main directory for details.
*
* This use of this software may be subject to additional restrictions.
* See the LEGAL file in the main directory for details.

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/



#define NDEBUG

#include "sigProcLib.h"
#include "GSMCommon.h"
#include "sendLPF_961.h"
#include "rcvLPF_651.h"

#include <Logger.h>

#define TABLESIZE 1024

/** Lookup tables for trigonometric approximation */
float cosTable[TABLESIZE+1]; // add 1 element for wrap around
float sinTable[TABLESIZE+1];

/** Constants */
static const float M_PI_F = (float)M_PI;
static const float M_2PI_F = (float)(2.0*M_PI);
static const float M_1_2PI_F = 1/M_2PI_F;

/** Static vectors that contain a precomputed +/- f_b/4 sinusoid */ 
signalVector *GMSKRotation = NULL;
signalVector *GMSKReverseRotation = NULL;

/*
 * RACH and midamble correlation waveforms
 */
struct CorrelationSequence {
  CorrelationSequence() : sequence(NULL)
  {
  }

  ~CorrelationSequence()
  {
    delete sequence;
  }

  signalVector *sequence;
  float        TOA;
  complex      gain;
};

/*
 * Gaussian and empty modulation pulses
 */
struct PulseSequence {
  PulseSequence() : gaussian(NULL), empty(NULL)
  {
  }

  ~PulseSequence()
  {
    delete gaussian;
    delete empty;
  }

  signalVector *gaussian;
  signalVector *empty;
};

CorrelationSequence *gMidambles[] = {NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL};
CorrelationSequence *gRACHSequence = NULL;
PulseSequence *GSMPulse = NULL;

void sigProcLibDestroy()
{
  for (int i = 0; i < 8; i++) {
    delete gMidambles[i];
    gMidambles[i] = NULL;
  }

  delete GMSKRotation;
  delete GMSKReverseRotation;
  delete gRACHSequence;
  delete GSMPulse;

  GMSKRotation = NULL;
  GMSKReverseRotation = NULL;
  gRACHSequence = NULL;
  GSMPulse = NULL;
}

// dB relative to 1.0.
// if > 1.0, then return 0 dB
float dB(float x) {
  
  float arg = 1.0F;
  float dB = 0.0F;
  
  if (x >= 1.0F) return 0.0F;
  if (x <= 0.0F) return -200.0F;

  float prevArg = arg;
  float prevdB = dB;
  float stepSize = 16.0F;
  float dBstepSize = 12.0F;
  while (stepSize > 1.0F) {
    do {
      prevArg = arg;
      prevdB = dB;
      arg /= stepSize;
      dB -= dBstepSize;
    } while (arg > x);
    arg = prevArg;
    dB = prevdB;
    stepSize *= 0.5F;
    dBstepSize -= 3.0F;
  }
 return ((arg-x)*(dB-3.0F) + (x-arg*0.5F)*dB)/(arg - arg*0.5F);

}

// 10^(-dB/10), inverse of dB func.
float dBinv(float x) {
  
  float arg = 1.0F;
  float dB = 0.0F;
  
  if (x >= 0.0F) return 1.0F;
  if (x <= -200.0F) return 0.0F;

  float prevArg = arg;
  float prevdB = dB;
  float stepSize = 16.0F;
  float dBstepSize = 12.0F;
  while (stepSize > 1.0F) {
    do {
      prevArg = arg;
      prevdB = dB;
      arg /= stepSize;
      dB -= dBstepSize;
    } while (dB > x);
    arg = prevArg;
    dB = prevdB;
    stepSize *= 0.5F;
    dBstepSize -= 3.0F;
  }

  return ((dB-x)*(arg*0.5F)+(x-(dB-3.0F))*(arg))/3.0F;

}

float vectorNorm2(const signalVector &x) 
{
  signalVector::const_iterator xPtr = x.begin();
  float Energy = 0.0;
  for (;xPtr != x.end();xPtr++) {
	Energy += xPtr->norm2();
  }
  return Energy;
}


float vectorPower(const signalVector &x) 
{
  return vectorNorm2(x)/x.size();
}

/** compute cosine via lookup table */
float cosLookup(const float x)
{
  float arg = x*M_1_2PI_F;
  while (arg > 1.0F) arg -= 1.0F;
  while (arg < 0.0F) arg += 1.0F;

  const float argT = arg*((float)TABLESIZE);
  const int argI = (int)argT;
  const float delta = argT-argI;
  const float iDelta = 1.0F-delta;
  return iDelta*cosTable[argI] + delta*cosTable[argI+1];
}

/** compute sine via lookup table */
float sinLookup(const float x) 
{
  float arg = x*M_1_2PI_F;
  while (arg > 1.0F) arg -= 1.0F;
  while (arg < 0.0F) arg += 1.0F;

  const float argT = arg*((float)TABLESIZE);
  const int argI = (int)argT;
  const float delta = argT-argI;
  const float iDelta = 1.0F-delta;
  return iDelta*sinTable[argI] + delta*sinTable[argI+1];
}


/** compute e^(-jx) via lookup table. */
complex expjLookup(float x)
{
  float arg = x*M_1_2PI_F;
  while (arg > 1.0F) arg -= 1.0F;
  while (arg < 0.0F) arg += 1.0F;

  const float argT = arg*((float)TABLESIZE);
  const int argI = (int)argT;
  const float delta = argT-argI;
  const float iDelta = 1.0F-delta;
   return complex(iDelta*cosTable[argI] + delta*cosTable[argI+1],
		   iDelta*sinTable[argI] + delta*sinTable[argI+1]);
}

/** Library setup functions */
void initTrigTables() {
  for (int i = 0; i < TABLESIZE+1; i++) {
    cosTable[i] = cos(2.0*M_PI*i/TABLESIZE);
    sinTable[i] = sin(2.0*M_PI*i/TABLESIZE);
  }
}

void initGMSKRotationTables(int sps)
{
  GMSKRotation = new signalVector(157 * sps);
  GMSKReverseRotation = new signalVector(157 * sps);
  signalVector::iterator rotPtr = GMSKRotation->begin();
  signalVector::iterator revPtr = GMSKReverseRotation->begin();
  float phase = 0.0;
  while (rotPtr != GMSKRotation->end()) {
    *rotPtr++ = expjLookup(phase);
    *revPtr++ = expjLookup(-phase);
    phase += M_PI_F / 2.0F / (float) sps;
  }
}

void sigProcLibSetup(int sps)
{
  initTrigTables();
  initGMSKRotationTables(sps);
  generateGSMPulse(sps, 2);
}

void GMSKRotate(signalVector &x) {
  signalVector::iterator xPtr = x.begin();
  signalVector::iterator rotPtr = GMSKRotation->begin();
  if (x.isRealOnly()) {
    while (xPtr < x.end()) {
      *xPtr = *rotPtr++ * (xPtr->real());
      xPtr++;
    }
  }
  else {
    while (xPtr < x.end()) {
      *xPtr = *rotPtr++ * (*xPtr);
      xPtr++;
    }
  }
}

void GMSKReverseRotate(signalVector &x) {
  signalVector::iterator xPtr= x.begin();
  signalVector::iterator rotPtr = GMSKReverseRotation->begin();
  if (x.isRealOnly()) {
    while (xPtr < x.end()) {
      *xPtr = *rotPtr++ * (xPtr->real());
      xPtr++;
    }
  }
  else {
    while (xPtr < x.end()) {
      *xPtr = *rotPtr++ * (*xPtr);
      xPtr++;
    }
  }
}


signalVector* convolve(const signalVector *a,
		       const signalVector *b,
		       signalVector *c,
		       ConvType spanType,
		       unsigned startIx,
		       unsigned len)
{
  if ((a==NULL) || (b==NULL)) return NULL; 
  int La = a->size();
  int Lb = b->size();

  int startIndex;
  unsigned int outSize;
  switch (spanType) {
    case FULL_SPAN:
      startIndex = 0;
      outSize = La+Lb-1;
      break;
    case OVERLAP_ONLY:
      startIndex = La;
      outSize = abs(La-Lb)+1;
      break;
    case START_ONLY:
      startIndex = 0;
      outSize = La;
      break;
    case WITH_TAIL:
      startIndex = Lb;
      outSize = La;
      break;
    case NO_DELAY:
      if (Lb % 2) 
	startIndex = Lb/2;
      else
	startIndex = Lb/2-1;
      outSize = La;
      break;
    case CUSTOM:
      startIndex = startIx;
      outSize = len;
      break;
    default:
      return NULL;
  }

  
  if (c==NULL)
    c = new signalVector(outSize);
  else if (c->size()!=outSize)
    return NULL;

  signalVector::const_iterator aStart = a->begin();
  signalVector::const_iterator bStart = b->begin();
  signalVector::const_iterator aEnd = a->end();
  signalVector::const_iterator bEnd = b->end();
  signalVector::iterator cPtr = c->begin();
  int t = startIndex;
  int stopIndex = startIndex + outSize;
  switch (b->getSymmetry()) {
  case NONE:
    {
      while (t < stopIndex) {
	signalVector::const_iterator aP = aStart+t;
	signalVector::const_iterator bP = bStart;
	if (a->isRealOnly() && b->isRealOnly()) {
	  float sum = 0.0;
	  while (bP < bEnd) {
	    if (aP < aStart) break;
	    if (aP < aEnd) sum += (aP->real())*(bP->real());
	    aP--;
	    bP++;
	  }
	  *cPtr++ = sum;
	}
	else if (a->isRealOnly()) {
	  complex sum = 0.0;
	  while (bP < bEnd) {
	    if (aP < aStart) break;
	    if (aP < aEnd) sum += (*bP)*(aP->real());
	    aP--;
	    bP++;
	  }
	  *cPtr++ = sum;
	}
	else if (b->isRealOnly()) {
	  complex sum = 0.0;
	  while (bP < bEnd) {
	    if (aP < aStart) break;
	    if (aP < aEnd) sum += (*aP)*(bP->real());
	    aP--;
	    bP++;
	  }
	  *cPtr++ = sum;
	}
	else {
	  complex sum = 0.0;
	  while (bP < bEnd) {
	    if (aP < aStart) break;
	    if (aP < aEnd) sum += (*aP)*(*bP);
	    aP--;
	    bP++;
	  }
	  *cPtr++ = sum;
	}
	t++;
      }
    }
    break;
  case ABSSYM:
    {
      complex sum = 0.0;
      bool isOdd = (bool) (Lb % 2);
      if (isOdd) 
	bEnd = bStart + (Lb+1)/2;
      else 
	bEnd = bStart + Lb/2;
      while (t < stopIndex) {
	signalVector::const_iterator aP = aStart+t;
	signalVector::const_iterator aPsym = aP-Lb+1;
	signalVector::const_iterator bP = bStart;
	sum = 0.0;
        if (!b->isRealOnly()) {
	  while (bP < bEnd) {
	    if (aP < aStart) break;
	    if (aP == aPsym)
	      sum+= (*aP)*(*bP);
	    else if ((aP < aEnd) && (aPsym >= aStart)) 
	      sum+= ((*aP)+(*aPsym))*(*bP);
	    else if (aP < aEnd)
	      sum += (*aP)*(*bP);
	    else if (aPsym >= aStart)
	      sum += (*aPsym)*(*bP);
	    aP--;
	    aPsym++;
	    bP++;
	  }
        }
        else {
          while (bP < bEnd) {
            if (aP < aStart) break;
            if (aP == aPsym)
              sum+= (*aP)*(bP->real());
            else if ((aP < aEnd) && (aPsym >= aStart))
              sum+= ((*aP)+(*aPsym))*(bP->real());
            else if (aP < aEnd)
              sum += (*aP)*(bP->real());
            else if (aPsym >= aStart)
              sum += (*aPsym)*(bP->real());
            aP--;
            aPsym++;
            bP++;
          }
        }
	*cPtr++ = sum;
	t++;
      }
    }
    break;
  default:
    return NULL;
    break;
  }
    
    
  return c;
}


void generateGSMPulse(int sps, int symbolLength)
{
  int len;
  float arg, center;

  delete GSMPulse;

  /* Store a single tap filter used for correlation sequence generation */
  GSMPulse = new PulseSequence();
  GSMPulse->empty = new signalVector(1);
  GSMPulse->empty->isRealOnly(true);
  *(GSMPulse->empty->begin()) = 1.0f;

  /* GSM pulse approximation */
  GSMPulse->gaussian = new signalVector(len);
  GSMPulse->gaussian->isRealOnly(true);
  signalVector::iterator xP = GSMPulse->gaussian->begin();

  center = (float) (len - 1.0) / 2.0;

  for (int i = 0; i < len; i++) {
    arg = ((float) i - center) / (float) sps;
    *xP++ = 0.96 * exp(-1.1380 * arg * arg -
                        0.527 * arg * arg * arg * arg);
  }

  float avgAbsval = sqrtf(vectorNorm2(*GSMPulse->gaussian)/sps);
  xP = GSMPulse->gaussian->begin();
  for (int i = 0; i < len; i++) 
    *xP++ /= avgAbsval;
}

signalVector* frequencyShift(signalVector *y,
			     signalVector *x,
			     float freq,
			     float startPhase,
			     float *finalPhase)
{

  if (!x) return NULL;
 
  if (y==NULL) {
    y = new signalVector(x->size());
    y->isRealOnly(x->isRealOnly());
    if (y==NULL) return NULL;
  }

  if (y->size() < x->size()) return NULL;

  float phase = startPhase;
  signalVector::iterator yP = y->begin();
  signalVector::iterator xPEnd = x->end();
  signalVector::iterator xP = x->begin();

  if (x->isRealOnly()) {
    while (xP < xPEnd) {
      (*yP++) = expjLookup(phase)*( (xP++)->real() );
      phase += freq;
    }
  }
  else {
    while (xP < xPEnd) {
      (*yP++) = (*xP++)*expjLookup(phase);
      phase += freq;
    }
  }


  if (finalPhase) *finalPhase = phase;

  return y;
}

signalVector* reverseConjugate(signalVector *b)
{
    signalVector *tmp = new signalVector(b->size());
    tmp->isRealOnly(b->isRealOnly());
    signalVector::iterator bP = b->begin();
    signalVector::iterator bPEnd = b->end();
    signalVector::iterator tmpP = tmp->end()-1;
    if (!b->isRealOnly()) {
      while (bP < bPEnd) {
        *tmpP-- = bP->conj();
        bP++;
      }
    }
    else {
      while (bP < bPEnd) {
        *tmpP-- = bP->real();
        bP++;
      }
    }

    return tmp;
}

signalVector* correlate(signalVector *a,
			signalVector *b,
			signalVector *c,
			ConvType spanType,
			bool bReversedConjugated,
		        unsigned startIx,
			unsigned len)
{
  signalVector *tmp = NULL;

  if (!bReversedConjugated) {
    tmp = reverseConjugate(b);
  }
  else {
    tmp = b;
  }

  c = convolve(a,tmp,c,spanType,startIx,len);

  if (!bReversedConjugated) delete tmp;

  return c;
}


/* soft output slicer */
bool vectorSlicer(signalVector *x) 
{

  signalVector::iterator xP = x->begin();
  signalVector::iterator xPEnd = x->end();
  while (xP < xPEnd) {
    *xP = (complex) (0.5*(xP->real()+1.0F));
    if (xP->real() > 1.0) *xP = 1.0;
    if (xP->real() < 0.0) *xP = 0.0;
    xP++;
  }
  return true;
}
  
signalVector *modulateBurst(const BitVector &wBurst, int guardPeriodLength,
			    int sps, bool emptyPulse)
{
  int burstLen;
  signalVector *pulse, modBurst;
  signalVector::iterator modBurstItr;

  if (emptyPulse)
    pulse = GSMPulse->empty;
  else
    pulse = GSMPulse->gaussian;

  burstLen = sps * (wBurst.size() + guardPeriodLength);
  modBurst = signalVector(burstLen);
  modBurstItr = modBurst.begin();

  for (unsigned int i = 0; i < wBurst.size(); i++) {
    *modBurstItr = 2.0*(wBurst[i] & 0x01)-1.0;
    modBurstItr += sps;
  }

  // shift up pi/2
  // ignore starting phase, since spec allows for discontinuous phase
  GMSKRotate(modBurst);

  modBurst.isRealOnly(false);

  // filter w/ pulse shape
  signalVector *shapedBurst = convolve(&modBurst, pulse, NULL, NO_DELAY);

  return shapedBurst;
}

float sinc(float x) 
{
  if ((x >= 0.01F) || (x <= -0.01F)) return (sinLookup(x)/x);
  return 1.0F;
}

void delayVector(signalVector &wBurst,
		 float delay)
{
  
  int   intOffset = (int) floor(delay);
  float fracOffset = delay - intOffset;
  
  // do fractional shift first, only do it for reasonable offsets
  if (fabs(fracOffset) > 1e-2) {
    // create sinc function
    signalVector sincVector(21); 
    sincVector.isRealOnly(true);
    signalVector::iterator sincBurstItr = sincVector.begin();
    for (int i = 0; i < 21; i++) 
      *sincBurstItr++ = (complex) sinc(M_PI_F*(i-10-fracOffset));
  
    signalVector shiftedBurst(wBurst.size());
    convolve(&wBurst,&sincVector,&shiftedBurst,NO_DELAY);
    wBurst.clone(shiftedBurst);
  }

  if (intOffset < 0) {
    intOffset = -intOffset;
    signalVector::iterator wBurstItr = wBurst.begin();
    signalVector::iterator shiftedItr = wBurst.begin()+intOffset;
    while (shiftedItr < wBurst.end())
      *wBurstItr++ = *shiftedItr++;
    while (wBurstItr < wBurst.end())
      *wBurstItr++ = 0.0;
  }
  else {
    signalVector::iterator wBurstItr = wBurst.end()-1;
    signalVector::iterator shiftedItr = wBurst.end()-1-intOffset;
    while (shiftedItr >= wBurst.begin())
      *wBurstItr-- = *shiftedItr--;
    while (wBurstItr >= wBurst.begin())
      *wBurstItr-- = 0.0;
  }
}
  
signalVector *gaussianNoise(int length, 
			    float variance, 
			    complex mean)
{

  signalVector *noise = new signalVector(length);
  signalVector::iterator nPtr = noise->begin();
  float stddev = sqrtf(variance);
  while (nPtr < noise->end()) {
    float u1 = (float) rand()/ (float) RAND_MAX;
    while (u1==0.0)
      u1 = (float) rand()/ (float) RAND_MAX;
    float u2 = (float) rand()/ (float) RAND_MAX;
    float arg = 2.0*M_PI*u2;
    *nPtr = mean + stddev*complex(cos(arg),sin(arg))*sqrtf(-2.0*log(u1));
    nPtr++;
  }

  return noise;
}

complex interpolatePoint(const signalVector &inSig,
			 float ix)
{
  
  int start = (int) (floor(ix) - 10);
  if (start < 0) start = 0;
  int end = (int) (floor(ix) + 11);
  if ((unsigned) end > inSig.size()-1) end = inSig.size()-1;
  
  complex pVal = 0.0;
  if (!inSig.isRealOnly()) {
    for (int i = start; i < end; i++) 
      pVal += inSig[i] * sinc(M_PI_F*(i-ix));
  }
  else {
    for (int i = start; i < end; i++) 
      pVal += inSig[i].real() * sinc(M_PI_F*(i-ix));
  }
   
  return pVal;
}

  
 
complex peakDetect(const signalVector &rxBurst,
		   float *peakIndex,
		   float *avgPwr) 
{
  

  complex maxVal = 0.0;
  float maxIndex = -1;
  float sumPower = 0.0;

  for (unsigned int i = 0; i < rxBurst.size(); i++) {
    float samplePower = rxBurst[i].norm2();
    if (samplePower > maxVal.real()) {
      maxVal = samplePower;
      maxIndex = i;
    }
    sumPower += samplePower;
  }

  // interpolate around the peak
  // to save computation, we'll use early-late balancing
  float earlyIndex = maxIndex-1;
  float lateIndex = maxIndex+1;
  
  float incr = 0.5;
  while (incr > 1.0/1024.0) {
    complex earlyP = interpolatePoint(rxBurst,earlyIndex);
    complex lateP =  interpolatePoint(rxBurst,lateIndex);
    if (earlyP < lateP) 
      earlyIndex += incr;
    else if (earlyP > lateP)
      earlyIndex -= incr;
    else break;
    incr /= 2.0;
    lateIndex = earlyIndex + 2.0;
  }

  maxIndex = earlyIndex + 1.0;
  maxVal = interpolatePoint(rxBurst,maxIndex);

  if (peakIndex!=NULL)
    *peakIndex = maxIndex;

  if (avgPwr!=NULL)
    *avgPwr = (sumPower-maxVal.norm2()) / (rxBurst.size()-1);

  return maxVal;

}

void scaleVector(signalVector &x,
		 complex scale)
{
  signalVector::iterator xP = x.begin();
  signalVector::iterator xPEnd = x.end();
  if (!x.isRealOnly()) {
    while (xP < xPEnd) {
      *xP = *xP * scale;
      xP++;
    }
  }
  else {
    while (xP < xPEnd) {
      *xP = xP->real() * scale;
      xP++;
    }
  }
}

/** in-place conjugation */
void conjugateVector(signalVector &x)
{
  if (x.isRealOnly()) return;
  signalVector::iterator xP = x.begin();
  signalVector::iterator xPEnd = x.end();
  while (xP < xPEnd) {
    *xP = xP->conj();
    xP++;
  }
}


// in-place addition!!
bool addVector(signalVector &x,
	       signalVector &y)
{
  signalVector::iterator xP = x.begin();
  signalVector::iterator yP = y.begin();
  signalVector::iterator xPEnd = x.end();
  signalVector::iterator yPEnd = y.end();
  while ((xP < xPEnd) && (yP < yPEnd)) {
    *xP = *xP + *yP;
    xP++; yP++;
  }
  return true;
}

// in-place multiplication!!
bool multVector(signalVector &x,
                 signalVector &y)
{
  signalVector::iterator xP = x.begin();
  signalVector::iterator yP = y.begin();
  signalVector::iterator xPEnd = x.end();
  signalVector::iterator yPEnd = y.end();
  while ((xP < xPEnd) && (yP < yPEnd)) {
    *xP = (*xP) * (*yP);
    xP++; yP++;
  }
  return true;
}


void offsetVector(signalVector &x,
		  complex offset)
{
  signalVector::iterator xP = x.begin();
  signalVector::iterator xPEnd = x.end();
  if (!x.isRealOnly()) {
    while (xP < xPEnd) {
      *xP += offset;
      xP++;
    }
  }
  else {
    while (xP < xPEnd) {
      *xP = xP->real() + offset;
      xP++;
    }      
  }
}

bool generateMidamble(int sps, int tsc)
{
  bool status = true;
  complex *data = NULL;
  signalVector *autocorr = NULL, *midamble = NULL;
  signalVector *midMidamble = NULL;

  if ((tsc < 0) || (tsc > 7)) 
    return false;

  delete gMidambles[tsc];
 
  /* Use middle 16 bits of each TSC. Correlation sequence is not pulse shaped */
  midMidamble = modulateBurst(gTrainingSequence[tsc].segment(5,16), 0, sps, true);
  if (!midMidamble)
    return false;

  /* Simulated receive sequence is pulse shaped */ 
  midamble = modulateBurst(gTrainingSequence[tsc], 0, sps, false);
  if (!midamble) {
    status = false;
    goto release;
  }
 
  // NOTE: Because ideal TSC 16-bit midamble is 66 symbols into burst,
  //       the ideal TSC has an + 180 degree phase shift,
  //       due to the pi/2 frequency shift, that 
  //       needs to be accounted for.
  //       26-midamble is 61 symbols into burst, has +90 degree phase shift.
  scaleVector(*midMidamble, complex(-1.0, 0.0));
  scaleVector(*midamble, complex(0.0, 1.0));

  conjugateVector(*midMidamble);

  autocorr = correlate(midamble, midMidamble, NULL, NO_DELAY);
  if (!autocorr) {
    status = false;
    goto release;
  }

  gMidambles[tsc] = new CorrelationSequence;
  gMidambles[tsc]->sequence = midMidamble;
  gMidambles[tsc]->gain = peakDetect(*autocorr,&gMidambles[tsc]->TOA,NULL);

release:
  delete autocorr;
  delete midamble;

  if (!status) {
    delete midMidamble;
    gMidambles[tsc] = NULL;
  }

  return status;
}

bool generateRACHSequence(int sps)
{
  bool status = true;
  complex *data = NULL;
  signalVector *autocorr = NULL;
  signalVector *seq0 = NULL, *seq1 = NULL;

  delete gRACHSequence;

  seq0 = modulateBurst(gRACHSynchSequence, 0, sps, false);
  if (!seq0)
    return false;

  seq1 = modulateBurst(gRACHSynchSequence.segment(0, 40), 0, sps, true);
  if (!seq1) {
    status = false;
    goto release;
  }

  conjugateVector(*seq1);

  autocorr = new signalVector(seq0->size());
  if (!convolve(seq0, seq1, autocorr, NO_DELAY)) {
    status = false;
    goto release;
  }

  gRACHSequence = new CorrelationSequence;
  gRACHSequence->sequence = seq1;
  gRACHSequence->gain = peakDetect(*autocorr,&gRACHSequence->TOA,NULL);

release:
  delete autocorr;
  delete seq0;

  if (!status) {
    delete seq1;
    gRACHSequence = NULL;
  }

  return status;
}
				
bool detectRACHBurst(signalVector &rxBurst,
		     float detectThreshold,
		     int sps,
		     complex *amplitude,
		     float* TOA)
{

  //static complex staticData[500];
 
  //signalVector correlatedRACH(staticData,0,rxBurst.size());
  signalVector correlatedRACH(rxBurst.size());
  correlate(&rxBurst,gRACHSequence->sequence,&correlatedRACH,NO_DELAY,true);

  float meanPower;
  complex peakAmpl = peakDetect(correlatedRACH,TOA,&meanPower);

  float valleyPower = 0.0; 

  // check for bogus results
  if ((*TOA < 0.0) || (*TOA > correlatedRACH.size())) {
        *amplitude = 0.0;
	return false;
  }
  complex *peakPtr = correlatedRACH.begin() + (int) rint(*TOA);

  LOG(DEBUG) << "RACH corr: " << correlatedRACH;

  float numSamples = 0.0;
  for (int i = 57 * sps; i <= 107 * sps; i++) {
    if (peakPtr+i >= correlatedRACH.end())
      break;
    valleyPower += (peakPtr+i)->norm2();
    numSamples++;
  }

  if (numSamples < 2) {
        *amplitude = 0.0;
        return false;
  }

  float RMS = sqrtf(valleyPower/(float) numSamples)+0.00001;
  float peakToMean = peakAmpl.abs()/RMS;

  LOG(DEBUG) << "RACH peakAmpl=" << peakAmpl << " RMS=" << RMS << " peakToMean=" << peakToMean;
  *amplitude = peakAmpl/(gRACHSequence->gain);

  *TOA = (*TOA) - gRACHSequence->TOA - 8 * sps;

  LOG(DEBUG) << "RACH thresh: " << peakToMean;

  return (peakToMean > detectThreshold);
}

bool energyDetect(signalVector &rxBurst,
		  unsigned windowLength,
		  float detectThreshold,
                  float *avgPwr)
{

  signalVector::const_iterator windowItr = rxBurst.begin(); //+rxBurst.size()/2 - 5*windowLength/2;
  float energy = 0.0;
  if (windowLength < 0) windowLength = 20;
  if (windowLength > rxBurst.size()) windowLength = rxBurst.size();
  for (unsigned i = 0; i < windowLength; i++) {
    energy += windowItr->norm2();
    windowItr+=4;
  }
  if (avgPwr) *avgPwr = energy/windowLength;
  LOG(DEBUG) << "detected energy: " << energy/windowLength;
  return (energy/windowLength > detectThreshold*detectThreshold);
}
  

bool analyzeTrafficBurst(signalVector &rxBurst,
			 unsigned TSC,
			 float detectThreshold,
			 int sps,
			 complex *amplitude,
			 float *TOA,
			 unsigned maxTOA,
                         bool requestChannel,
                         signalVector **channelResponse,
			 float *channelResponseOffset) 
{

  assert(TSC<8);
  assert(amplitude);
  assert(TOA);
  assert(gMidambles[TSC]);

  if (maxTOA < 3*sps) maxTOA = 3*sps;
  unsigned spanTOA = maxTOA;
  if (spanTOA < 5*sps) spanTOA = 5*sps;

  unsigned startIx = 66*sps-spanTOA;
  unsigned endIx = (66+16)*sps+spanTOA;
  unsigned windowLen = endIx - startIx;
  unsigned corrLen = 2*maxTOA+1;

  unsigned expectedTOAPeak = (unsigned) round(gMidambles[TSC]->TOA + (gMidambles[TSC]->sequence->size()-1)/2);

  signalVector burstSegment(rxBurst.begin(),startIx,windowLen);

  //static complex staticData[200];
  //signalVector correlatedBurst(staticData,0,corrLen);
  signalVector correlatedBurst(corrLen);
  correlate(&burstSegment, gMidambles[TSC]->sequence,
					    &correlatedBurst, CUSTOM,true,
					    expectedTOAPeak-maxTOA,corrLen);

  float meanPower;
  *amplitude = peakDetect(correlatedBurst,TOA,&meanPower);
  float valleyPower = 0.0; //amplitude->norm2();
  complex *peakPtr = correlatedBurst.begin() + (int) rint(*TOA);

  // check for bogus results
  if ((*TOA < 0.0) || (*TOA > correlatedBurst.size())) {
        *amplitude = 0.0;
        return false;
  }

  int numRms = 0;
  for (int i = 2*sps; i <= 5*sps;i++) {
    if (peakPtr - i >= correlatedBurst.begin()) { 
      valleyPower += (peakPtr-i)->norm2();
      numRms++;
    }
    if (peakPtr + i < correlatedBurst.end()) {
      valleyPower += (peakPtr+i)->norm2();
      numRms++;
    }
  }

  if (numRms < 2) {
        // check for bogus results
        *amplitude = 0.0;
        return false;
  }

  float RMS = sqrtf(valleyPower/(float)numRms)+0.00001;
  float peakToMean = (amplitude->abs())/RMS;

  // NOTE: Because ideal TSC is 66 symbols into burst,
  //       the ideal TSC has an +/- 180 degree phase shift,
  //       due to the pi/4 frequency shift, that 
  //       needs to be accounted for.
  
  *amplitude = (*amplitude)/gMidambles[TSC]->gain;
  *TOA = (*TOA) - (maxTOA); 

  LOG(DEBUG) << "TCH peakAmpl=" << amplitude->abs() << " RMS=" << RMS << " peakToMean=" << peakToMean << " TOA=" << *TOA;

  LOG(DEBUG) << "autocorr: " << correlatedBurst;
  
  if (requestChannel && (peakToMean > detectThreshold)) {
    float TOAoffset = maxTOA;
    delayVector(correlatedBurst,-(*TOA));
    // midamble only allows estimation of a 6-tap channel
    signalVector chanVector(6 * sps);
    float maxEnergy = -1.0;
    int maxI = -1;
    for (int i = 0; i < 7; i++) {
      if (TOAoffset + (i-5) * sps + chanVector.size() > correlatedBurst.size())
        continue;
      if (TOAoffset + (i-5) * sps < 0)
        continue;
      correlatedBurst.segmentCopyTo(chanVector,
                                    (int) floor(TOAoffset + (i - 5) * sps),
                                    chanVector.size());
      float energy = vectorNorm2(chanVector);
      if (energy > 0.95*maxEnergy) {
	maxI = i;
	maxEnergy = energy;
      }
    }
	
    *channelResponse = new signalVector(chanVector.size());
    correlatedBurst.segmentCopyTo(**channelResponse,
                                  (int) floor(TOAoffset + (maxI - 5) * sps),
                                  (*channelResponse)->size());
    scaleVector(**channelResponse, complex(1.0, 0.0) / gMidambles[TSC]->gain);
    LOG(DEBUG) << "channelResponse: " << **channelResponse;
    
    if (channelResponseOffset) 
      *channelResponseOffset = 5 * sps - maxI;
      
  }

  return (peakToMean > detectThreshold);
		  
}

signalVector *decimateVector(signalVector &wVector,
			     int decimationFactor) 
{
  
  if (decimationFactor <= 1) return NULL;

  signalVector *decVector = new signalVector(wVector.size()/decimationFactor);
  decVector->isRealOnly(wVector.isRealOnly());

  signalVector::iterator vecItr = decVector->begin();
  for (unsigned int i = 0; i < wVector.size();i+=decimationFactor) 
    *vecItr++ = wVector[i];

  return decVector;
}


SoftVector *demodulateBurst(signalVector &rxBurst, int sps,
                            complex channel, float TOA) 
{
  scaleVector(rxBurst,((complex) 1.0)/channel);
  delayVector(rxBurst,-TOA);

  signalVector *shapedBurst = &rxBurst;

  // shift up by a quarter of a frequency
  // ignore starting phase, since spec allows for discontinuous phase
  GMSKReverseRotate(*shapedBurst);

  // run through slicer
  if (sps > 1) {
     signalVector *decShapedBurst = decimateVector(*shapedBurst, sps);
     shapedBurst = decShapedBurst;
  }

  LOG(DEBUG) << "shapedBurst: " << *shapedBurst;

  vectorSlicer(shapedBurst);

  SoftVector *burstBits = new SoftVector(shapedBurst->size());

  SoftVector::iterator burstItr = burstBits->begin();
  signalVector::iterator shapedItr = shapedBurst->begin();
  for (; shapedItr < shapedBurst->end(); shapedItr++) 
    *burstItr++ = shapedItr->real();

  if (sps > 1)
    delete shapedBurst;

  return burstBits;

}


// 1.0 is sampling frequency
// must satisfy cutoffFreq > 1/filterLen
signalVector *createLPF(float cutoffFreq,
			int filterLen,
			float gainDC)
{
#if 0
  signalVector *LPF = new signalVector(filterLen-1);
  LPF->isRealOnly(true);
  LPF->setSymmetry(ABSSYM);
  signalVector::iterator itr = LPF->begin();
  double sum = 0.0;
  for (int i = 1; i < filterLen; i++) {
    float ys = sinc(M_2PI_F*cutoffFreq*((float)i-(float)(filterLen)/2.0F));
    float yg = 4.0F * cutoffFreq;
    // Blackman -- less brickwall (sloping transition) but larger stopband attenuation
    float yw = 0.42 - 0.5*cos(((float)i)*M_2PI_F/(float)(filterLen)) + 0.08*cos(((float)i)*2*M_2PI_F/(float)(filterLen));
    // Hamming -- more brickwall with smaller stopband attenuation
    //float yw = 0.53836F - 0.46164F * cos(((float)i)*M_2PI_F/(float)(filterLen+1));
    *itr++ = (complex) ys*yg*yw;
    sum += ys*yg*yw;
  }
#else
  double sum = 0.0;
  signalVector *LPF;
  signalVector::iterator itr;
  if (filterLen == 651) { // receive LPF
    LPF = new signalVector(651);
    LPF->isRealOnly(true);
    itr = LPF->begin();
    for (int i = 0; i < filterLen; i++) {
       *itr++ = complex(rcvLPF_651[i],0.0);
       sum += rcvLPF_651[i];
    }
  }
  else { 
    LPF = new signalVector(961);
    LPF->isRealOnly(true);
    itr = LPF->begin();
    for (int i = 0; i < filterLen; i++) {
       *itr++ = complex(sendLPF_961[i],0.0);
       sum += sendLPF_961[i];
    }
  }
#endif

  float normFactor = gainDC/sum; //sqrtf(gainDC/vectorNorm2(*LPF));
  // normalize power
  itr = LPF->begin();
  for (int i = 0; i < filterLen; i++) {
    *itr = *itr*normFactor;
    itr++;
  }
  return LPF;

}
    


#define POLYPHASESPAN 10

// assumes filter group delay is 0.5*(length of filter)
signalVector *polyphaseResampleVector(signalVector &wVector,
				      int P, int Q,
				      signalVector *LPF)

{
 
  bool deleteLPF = false;
 
  if (LPF==NULL) {
    float cutoffFreq = (P < Q) ? (1.0/(float) Q) : (1.0/(float) P);
    LPF = createLPF(cutoffFreq/3.0,100*POLYPHASESPAN+1,Q);
    deleteLPF = true;
  }

  signalVector *resampledVector = new signalVector((int) ceil(wVector.size()*(float) P / (float) Q));
  resampledVector->fill(0);
  resampledVector->isRealOnly(wVector.isRealOnly());
  signalVector::iterator newItr = resampledVector->begin();

  //FIXME: need to update for real-only vectors
  int outputIx = (LPF->size()+1)/2/Q; //((P > Q) ? P : Q); 
  while (newItr < resampledVector->end()) {
    int outputBranch = (outputIx*Q) % P; 
    int inputOffset = (outputIx*Q - outputBranch)/P;
    signalVector::const_iterator inputItr = wVector.begin() + inputOffset;
    signalVector::const_iterator filtItr  = LPF->begin() + outputBranch;
    while (inputItr >= wVector.end()) {
      inputItr--;
      filtItr+=P;
    }
    complex sum = 0.0;
    if ((LPF->getSymmetry()!=ABSSYM) || (P>1)) {
      if (!LPF->isRealOnly()) {
        while ( (inputItr >= wVector.begin()) && (filtItr < LPF->end()) ) {
	  sum += (*inputItr)*(*filtItr);
	  inputItr--;
	  filtItr += P;
        }
      }
      else {
        while ( (inputItr >= wVector.begin()) && (filtItr < LPF->end()) ) {
	  sum += (*inputItr)*(filtItr->real());
	  inputItr--;
	  filtItr += P;
        }
      }
    }
    else {
      signalVector::const_iterator revInputItr = inputItr- LPF->size() + 1;  
      signalVector::const_iterator filtMidpoint = LPF->begin()+(LPF->size()-1)/2;
      if (!LPF->isRealOnly()) {
	while (filtItr <= filtMidpoint) {
	  if (inputItr < revInputItr) break;
	  if (inputItr == revInputItr) 
	    sum += (*inputItr)*(*filtItr);
          else if ( (inputItr < wVector.end()) && (revInputItr >= wVector.begin()) )
            sum += (*inputItr + *revInputItr)*(*filtItr);
          else if ( inputItr < wVector.end() ) 
	    sum += (*inputItr)*(*filtItr);
          else if ( revInputItr >= wVector.begin() )
	    sum += (*revInputItr)*(*filtItr);
          inputItr--;
	  revInputItr++;
          filtItr++;
        }
      }
      else {
        while (filtItr <= filtMidpoint) {
          if (inputItr < revInputItr) break;
          if (inputItr == revInputItr)
            sum += (*inputItr)*(filtItr->real());
          else if ( (inputItr < wVector.end()) && (revInputItr >= wVector.begin()) ) 
            sum += (*inputItr + *revInputItr)*(filtItr->real());
          else if ( inputItr < wVector.end() ) 
            sum += (*inputItr)*(filtItr->real());
          else if ( revInputItr >= wVector.begin() )
            sum += (*revInputItr)*(filtItr->real());
          inputItr--;
          revInputItr++;
          filtItr++;
        }
      }
    }
    *newItr = sum;
    newItr++;
    outputIx++;
  }
      
  if (deleteLPF) delete LPF;

  return resampledVector;
}


signalVector *resampleVector(signalVector &wVector,
			     float expFactor,
			     complex endPoint)

{

  if (expFactor < 1.0) return NULL;

  signalVector *retVec = new signalVector((int) ceil(wVector.size()*expFactor));

  float t = 0.0;
  
  signalVector::iterator retItr = retVec->begin();
  while (retItr < retVec->end()) {
    unsigned tLow = (unsigned int) floor(t);
    unsigned tHigh = tLow + 1;
    if (tLow > wVector.size()-1) break;
    if (tHigh > wVector.size()) break;
    complex lowPoint = wVector[tLow];
    complex highPoint = (tHigh == wVector.size()) ? endPoint : wVector[tHigh];
    complex a = (tHigh-t);
    complex b = (t-tLow);
    *retItr = (a*lowPoint + b*highPoint);
    t += 1.0/expFactor;
  }

  return retVec;

}
		   

// Assumes symbol-spaced sampling!!!
// Based upon paper by Al-Dhahir and Cioffi
bool designDFE(signalVector &channelResponse,
	       float SNRestimate,
	       int Nf,
	       signalVector **feedForwardFilter,
	       signalVector **feedbackFilter)
{
  
  signalVector G0(Nf);
  signalVector G1(Nf);
  signalVector::iterator G0ptr = G0.begin();
  signalVector::iterator G1ptr = G1.begin();
  signalVector::iterator chanPtr = channelResponse.begin();

  int nu = channelResponse.size()-1;

  *G0ptr = 1.0/sqrtf(SNRestimate);
  for(int j = 0; j <= nu; j++) {
    *G1ptr = chanPtr->conj();
    G1ptr++; chanPtr++;
  }

  signalVector *L[Nf];
  signalVector::iterator Lptr;
  float d;
  for(int i = 0; i < Nf; i++) {
    d = G0.begin()->norm2() + G1.begin()->norm2();
    L[i] = new signalVector(Nf+nu);
    Lptr = L[i]->begin()+i;
    G0ptr = G0.begin(); G1ptr = G1.begin();
    while ((G0ptr < G0.end()) &&  (Lptr < L[i]->end())) {
      *Lptr = (*G0ptr*(G0.begin()->conj()) + *G1ptr*(G1.begin()->conj()) )/d;
      Lptr++;
      G0ptr++;
      G1ptr++;
    }
    complex k = (*G1.begin())/(*G0.begin());

    if (i != Nf-1) {
      signalVector G0new = G1;
      scaleVector(G0new,k.conj());
      addVector(G0new,G0);

      signalVector G1new = G0;
      scaleVector(G1new,k*(-1.0));
      addVector(G1new,G1);
      delayVector(G1new,-1.0);

      scaleVector(G0new,1.0/sqrtf(1.0+k.norm2()));
      scaleVector(G1new,1.0/sqrtf(1.0+k.norm2()));
      G0 = G0new;
      G1 = G1new;
    }
  }

  *feedbackFilter = new signalVector(nu);
  L[Nf-1]->segmentCopyTo(**feedbackFilter,Nf,nu);
  scaleVector(**feedbackFilter,(complex) -1.0);
  conjugateVector(**feedbackFilter);

  signalVector v(Nf);
  signalVector::iterator vStart = v.begin();
  signalVector::iterator vPtr;
  *(vStart+Nf-1) = (complex) 1.0;
  for(int k = Nf-2; k >= 0; k--) {
    Lptr = L[k]->begin()+k+1;
    vPtr = vStart + k+1;
    complex v_k = 0.0;
    for (int j = k+1; j < Nf; j++) {
      v_k -= (*vPtr)*(*Lptr);
      vPtr++; Lptr++;
    }
     *(vStart + k) = v_k;
  }

  *feedForwardFilter = new signalVector(Nf);
  signalVector::iterator w = (*feedForwardFilter)->begin();
  for (int i = 0; i < Nf; i++) {
    delete L[i];
    complex w_i = 0.0;
    int endPt = ( nu < (Nf-1-i) ) ? nu : (Nf-1-i);
    vPtr = vStart+i;
    chanPtr = channelResponse.begin();
    for (int k = 0; k < endPt+1; k++) {
      w_i += (*vPtr)*(chanPtr->conj());
      vPtr++; chanPtr++;
    }
    *w = w_i/d;
    w++;
  }


  return true;
  
}

// Assumes symbol-rate sampling!!!!
SoftVector *equalizeBurst(signalVector &rxBurst,
		       float TOA,
		       int sps,
		       signalVector &w, // feedforward filter
		       signalVector &b) // feedback filter
{

  delayVector(rxBurst,-TOA);

  signalVector* postForwardFull = convolve(&rxBurst,&w,NULL,FULL_SPAN);

  signalVector* postForward = new signalVector(rxBurst.size());
  postForwardFull->segmentCopyTo(*postForward,w.size()-1,rxBurst.size());
  delete postForwardFull;

  signalVector::iterator dPtr = postForward->begin();
  signalVector::iterator dBackPtr;
  signalVector::iterator rotPtr = GMSKRotation->begin();
  signalVector::iterator revRotPtr = GMSKReverseRotation->begin();

  signalVector *DFEoutput = new signalVector(postForward->size());
  signalVector::iterator DFEItr = DFEoutput->begin();

  // NOTE: can insert the midamble and/or use midamble to estimate BER
  for (; dPtr < postForward->end(); dPtr++) {
    dBackPtr = dPtr-1;
    signalVector::iterator bPtr = b.begin();
    while ( (bPtr < b.end()) && (dBackPtr >= postForward->begin()) ) {
      *dPtr = *dPtr + (*bPtr)*(*dBackPtr);
      bPtr++;
      dBackPtr--;
    }
    *dPtr = *dPtr * (*revRotPtr);
    *DFEItr = *dPtr;
    // make decision on symbol
    *dPtr = (dPtr->real() > 0.0) ? 1.0 : -1.0;
    //*DFEItr = *dPtr;
    *dPtr = *dPtr * (*rotPtr);
    DFEItr++;
    rotPtr++;
    revRotPtr++;
  }

  vectorSlicer(DFEoutput);

  SoftVector *burstBits = new SoftVector(postForward->size());
  SoftVector::iterator burstItr = burstBits->begin();
  DFEItr = DFEoutput->begin();
  for (; DFEItr < DFEoutput->end(); DFEItr++) 
    *burstItr++ = DFEItr->real();

  delete postForward;

  delete DFEoutput;

  return burstBits;
}
