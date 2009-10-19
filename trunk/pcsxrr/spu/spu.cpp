//spu.cpp
//original (C) 2002 by Pete Bernert
//nearly entirely rewritten for pcsxrr by zeromus
//original changelog is at end of file, for reference

//This program is free software; you can redistribute it and/or modify
//it under the terms of the GNU General Public License as published by
//the Free Software Foundation; either version 2 of the License, or
//(at your option) any later version. See also the license.txt file for
//additional informations.                                              

//TODO - volume sweep not emulated

//-------------------------------

#include "stdafx.h"

#define _USE_MATH_DEFINES
#include <math.h>
#ifndef M_PI
#define M_PI 3.1415926535897932386
#endif

#include <stdio.h>
#include <queue>

//FILE* wavout = NULL;

#define _IN_SPU

#include "externals.h"
#include "cfg.h"
#include "dsoundoss.h"
#include "record.h"
#include "resource.h"
#include "PsxCommon.h"
#include "spu.h"
#include "adsr.h"
#include "reverb.h"
#include "gauss_i.h"

SPU_struct *SPU_core, *SPU_user;

//global spu values
u16 regArea[10000]; //register cache
u16 spuMem[256*1024]; //spu memory


class ISynchronizingAudioBuffer
{
public:
	virtual void enqueue_samples(s16* buf, int samples_provided) = 0;

	//returns the number of samples actually supplied, which may not match the number requested
	virtual int output_samples(s16* buf, int samples_requested) = 0;
};

template<typename T> inline T _abs(T val)
{
	if(val<0) return -val;
	else return val;
}

template<typename T> inline T moveValueTowards(T val, T target, T incr)
{
	incr = _abs(incr);
	T delta = _abs(target-val);
	if(val<target) val += incr;
	else if(val>target) val -= incr;
	T newDelta = _abs(target-val);
	if(newDelta >= delta)
		val = target;
	return val;
}


class ZeromusSynchronizer : public ISynchronizingAudioBuffer
{
public:
	ZeromusSynchronizer()
		: mixqueue_go(false)
		,
		#ifdef NDEBUG
		adjustobuf(200,1000)
		#else
		adjustobuf(22000,44000)
		#endif
	{

	}

	bool mixqueue_go;

	virtual void enqueue_samples(s16* buf, int samples_provided)
	{
		for(int i=0;i<samples_provided;i++) {
			s16 left = *buf++;
			s16 right = *buf++;
			adjustobuf.enqueue(left,right);
		}
	}

	//returns the number of samples actually supplied, which may not match the number requested
	virtual int output_samples(s16* buf, int samples_requested)
	{
		int done = 0;
		if(!mixqueue_go) {
			if(adjustobuf.size > 200)
				mixqueue_go = true;
		}
		else
		{
			for(int i=0;i<samples_requested;i++) {
				if(adjustobuf.size==0) {
					mixqueue_go = false;
					break;
				}
				done++;
				s16 left, right;
				adjustobuf.dequeue(left,right);
				*buf++ = left;
				*buf++ = right;
			}
		}
		
		return done;
	}

private:
	class Adjustobuf
	{
	public:
		Adjustobuf(int _minLatency, int _maxLatency)
			: size(0)
			, minLatency(_minLatency)
			, maxLatency(_maxLatency)
		{
			rollingTotalSize = 0;
			targetLatency = (maxLatency + minLatency)/2;
			rate = 1.0f;
			cursor = 0.0f;
			curr[0] = curr[1] = 0;
			kAverageSize = 80000;
		}

		float rate, cursor;
		int minLatency, targetLatency, maxLatency;
		std::queue<s16> buffer;
		int size;
		s16 curr[2];

		std::queue<int> statsHistory;

		void enqueue(s16 left, s16 right) 
		{
			buffer.push(left);
			buffer.push(right);
			size++;
		}

		s64 rollingTotalSize;

		u32 kAverageSize;

		void addStatistic()
		{
			statsHistory.push(size);
			rollingTotalSize += size;
			if(statsHistory.size()>kAverageSize)
			{
				rollingTotalSize -= statsHistory.front();
				statsHistory.pop();

				float averageSize = (float)(rollingTotalSize / kAverageSize);
				//static int ctr=0;  ctr++; if((ctr&127)==0) printf("avg size: %f curr size: %d rate: %f\n",averageSize,size,rate);
				{
					float targetRate;
					if(averageSize < targetLatency)
					{
						targetRate = 1.0f - (targetLatency-averageSize)/kAverageSize;
					}
					else if(averageSize > targetLatency) {
						targetRate = 1.0f + (averageSize-targetLatency)/kAverageSize;
					} else targetRate = 1.0f;
				
					//rate = moveValueTowards(rate,targetRate,0.001f);
					rate = targetRate;
				}

			}


		}

		void dequeue(s16& left, s16& right)
		{
			left = right = 0; 
			addStatistic();
			if(size==0) { return; }
			cursor += rate;
			while(cursor>1.0f) {
				cursor -= 1.0f;
				if(size>0) {
					curr[0] = buffer.front(); buffer.pop();
					curr[1] = buffer.front(); buffer.pop();
					size--;
				}
			}
			left = curr[0]; 
			right = curr[1];
		}
	} adjustobuf;
};

class NitsujaSynchronizer : public ISynchronizingAudioBuffer
{
private:
	template<typename T>
	struct ssampT
	{
		T l, r;
		enum { TMAX = (1 << ((sizeof(T) * 8) - 1)) - 1 };
		ssampT() {}
		ssampT(T ll, T rr) : l(ll), r(rr) {}
		template<typename T2>
		ssampT(ssampT<T2> s) : l(s.l), r(s.r) {}
		ssampT operator+(const ssampT& rhs)
		{
			s32 l2 = l+rhs.l;
			s32 r2 = r+rhs.r;
			if(l2 > TMAX) l2 = TMAX;
			if(l2 < -TMAX) l2 = -TMAX;
			if(r2 > TMAX) r2 = TMAX;
			if(r2 < -TMAX) r2 = -TMAX;
			return ssampT(l2, r2);
		}
		ssampT operator/(int rhs)
		{
			return ssampT(l/rhs,r/rhs);
		}
		ssampT operator*(int rhs)
		{
			s32 l2 = l*rhs;
			s32 r2 = r*rhs;
			if(l2 > TMAX) l2 = TMAX;
			if(l2 < -TMAX) l2 = -TMAX;
			if(r2 > TMAX) r2 = TMAX;
			if(r2 < -TMAX) r2 = -TMAX;
			return ssampT(l2, r2);
		}
		ssampT muldiv (int num, int den)
		{
			num = std::max<T>(0,num);
			return ssampT(((s32)l * num) / den, ((s32)r * num) / den);
		}
		ssampT faded (ssampT rhs, int cur, int start, int end)
		{
			if(cur <= start)
				return *this;
			if(cur >= end)
				return rhs;

			//float ang = 3.14159f * (float)(cur - start) / (float)(end - start);
			//float amt = (1-cosf(ang))*0.5f;
			//cur = start + (int)(amt * (end - start));

			int inNum = cur - start;
			int outNum = end - cur;
			int denom = end - start;

			int lrv = ((int)l * outNum + (int)rhs.l * inNum) / denom;
			int rrv = ((int)r * outNum + (int)rhs.r * inNum) / denom;

			return ssampT<T>(lrv,rrv);
		}
	};

	typedef ssampT<s16> ssamp;

	std::vector<ssamp> sampleQueue;

	// reflects x about y if x exceeds y.
	FORCEINLINE int reflectAbout(int x, int y)
	{
			//return (x)+(x)/(y)*(2*((y)-(x))-1);
			return (x<y) ? x : (2*y-x-1);
	}

	void emit_samples(s16* outbuf, ssamp* samplebuf, int samples)
	{
		for(int i=0;i<samples;i++) {
			*outbuf++ = samplebuf[i].l;
			*outbuf++ = samplebuf[i].r;
		}
	}
	 
public:
	NitsujaSynchronizer()
	{}

	virtual void enqueue_samples(s16* buf, int samples_provided)
	{
		for(int i=0;i<samples_provided;i++)
		{
			sampleQueue.push_back(ssamp(buf[0],buf[1]));
			buf += 2;
		}
	}

	virtual int output_samples(s16* buf, int samples_requested)
	{
		int audiosize = samples_requested;
		int queued = sampleQueue.size();
		// truncate input and output sizes to 8 because I am too lazy to deal with odd numbers
		audiosize &= ~7;
		queued &= ~7;

		if(queued > 0x200 && audiosize > 0) // is there any work to do?
		{
			// are we going at normal speed?
			// or more precisely, are the input and output queues/buffers of similar size?
			if(queued > 900 || audiosize > queued * 2)
			{
				// not normal speed. we have to resample it somehow in this case.
				static std::vector<ssamp> outsamples;
				outsamples.clear();
				if(audiosize <= queued)
				{
					// fast forward speed
					// this is the easy case, just crossfade it and it sounds ok
					for(int i = 0; i < audiosize; i++)
					{
						int j = i + queued - audiosize;
						ssamp outsamp = sampleQueue[i].faded(sampleQueue[j], i,0,audiosize);
						outsamples.push_back(ssamp(outsamp));
					}
				}
				else
				{
					// slow motion speed
					// here we take a very different approach,
					// instead of crossfading it, we select a single sample from the queue
					// and make sure that the index we use to select a sample is constantly moving
					// and that it starts at the first sample in the queue and ends on the last one.
					//
					// hopefully the index doesn't move discontinuously or we'll get slight crackling
					// (there might still be a minor bug here that causes this occasionally)
					//
					// here's a diagram of how the index we sample from moves:
					//
					// queued (this axis represents the index we sample from. the top means the end of the queue)
					// ^
					// |   --> audiosize (this axis represents the output index we write to, right meaning forward in output time/position)
					// |   A           C       C  end
					//    A A     B   C C     C
					//   A   A   A B C   C   C
					//  A     A A   B     C C
					// A       A           C
					// start
					//
					// yes, this means we are spending some stretches of time playing the sound backwards,
					// but the stretches are short enough that this doesn't sound weird.
					// apparently this also sounds less "echoey" or "robotic" than only playing it forwards.

					int midpointX = audiosize >> 1;
					int midpointY = queued >> 1;

					// all we need to do here is calculate the X position of the leftmost "B" in the above diagram.
					// TODO: we should calculate it with a simple equation like
					//   midpointXOffset = min(something,somethingElse);
					// but it's a little difficult to work it out exactly
					// so here's a stupid search for the value for now:

					int prevA = 999999;
					int midpointXOffset = queued/2;
					while(true)
					{
						int a = abs(reflectAbout((midpointX - midpointXOffset) % (queued*2), queued) - midpointY) - midpointXOffset;
						if(((a > 0) != (prevA > 0) || (a < 0) != (prevA < 0)) && prevA != 999999)
						{
							if((a + prevA)&1) // there's some sort of off-by-one problem with this search since we're moving diagonally...
								midpointXOffset++; // but this fixes it most of the time...
							break; // found it
						}
						prevA = a;
						midpointXOffset--;
						if(midpointXOffset < 0)
						{
							midpointXOffset = 0;
							break; // failed somehow? let's just omit the "B" stretch in this case.
						}
					}
					int leftMidpointX = midpointX - midpointXOffset;
					int rightMidpointX = midpointX + midpointXOffset;
					int leftMidpointY = reflectAbout((leftMidpointX) % (queued*2), queued);
					int rightMidpointY = (queued-1) - reflectAbout((((int)audiosize-1 - rightMidpointX + queued*2) % (queued*2)), queued);

					// output the left almost-half of the sound (section "A")
					for(int x = 0; x < leftMidpointX; x++)
					{
						int i = reflectAbout(x % (queued*2), queued);
						outsamples.push_back(sampleQueue[i]);
					}

					// output the middle stretch (section "B")
					int y = leftMidpointY;
					int dyMidLeft  = (leftMidpointY  < midpointY) ? 1 : -1;
					int dyMidRight = (rightMidpointY > midpointY) ? 1 : -1;
					for(int x = leftMidpointX; x < midpointX; x++, y+=dyMidLeft)
						outsamples.push_back(sampleQueue[y]);
					for(int x = midpointX; x < rightMidpointX; x++, y+=dyMidRight)
						outsamples.push_back(sampleQueue[y]);

					// output the end of the queued sound (section "C")
					for(int x = rightMidpointX; x < audiosize; x++)
					{
						int i = (queued-1) - reflectAbout((((int)audiosize-1 - x + queued*2) % (queued*2)), queued);
						outsamples.push_back(sampleQueue[i]);
					}
					assert(outsamples.back().l == sampleQueue[queued-1].l);
				} //end else

				// if the user SPU mixed some channels, mix them in with our output now
#ifdef HYBRID_SPU
				SPU_MixAudio<2>(SPU_user,audiosize);
				for(int i = 0; i < audiosize; i++)
					outsamples[i] = outsamples[i] + *(ssamp*)(&SPU_user->outbuf[i*2]);
#endif

				emit_samples(buf,&outsamples[0],audiosize);
				sampleQueue.erase(sampleQueue.begin(), sampleQueue.begin() + queued);
				return audiosize;
			}
			else
			{
				// normal speed
				// just output the samples straightforwardly.
				//
				// at almost-full speeds (like 50/60 FPS)
				// what will happen is that we rapidly fluctuate between entering this branch
				// and entering the "slow motion speed" branch above.
				// but that's ok! because all of these branches sound similar enough that we can get away with it.
				// so the two cases actually complement each other.

				if(audiosize >= queued)
				{
#ifdef HYBRID_SPU
					SPU_MixAudio<2>(SPU_user,queued);
					for(int i = 0; i < queued; i++)
						sampleQueue[i] = sampleQueue[i] + *(ssamp*)(&SPU_user->outbuf[i*2]);
#endif
					emit_samples(buf,&sampleQueue[0],queued);
					sampleQueue.erase(sampleQueue.begin(), sampleQueue.begin() + queued);
					return queued;
				}
				else
				{
#ifdef HYBRID_SPU
					SPU_MixAudio<2>(SPU_user,audiosize);
					for(int i = 0; i < audiosize; i++)
						sampleQueue[i] = sampleQueue[i] + *(ssamp*)(&SPU_user->outbuf[i*2]);
#endif
					emit_samples(buf,&sampleQueue[0],audiosize);
					sampleQueue.erase(sampleQueue.begin(), sampleQueue.begin()+audiosize);
					return audiosize;
				}

			} //end normal speed

		} //end if there is any work to do
		else
		{
			return 0;
		}

	} //output_samples

private:

}; //NitsujaSynchronizer

//static ISynchronizingAudioBuffer* synchronizer = new ZeromusSynchronizer();
static ISynchronizingAudioBuffer* synchronizer = new NitsujaSynchronizer();

static inline u8 readSpuMem(u32 addr) { return ((u8*)spuMem)[addr]; }


// user settings

int iUseXA=1;
int iVolume=3;
int iXAPitch=0;
int iUseTimer=0;
int iSPUIRQWait=1;
int iRecordMode=0;
int iUseReverb=1;
int iUseInterpolation=2;
int iDisStereo=0;
int iUseDBufIrq=0;

FORCEINLINE bool isStreamingMode() { return iUseTimer==2; }

// MAIN infos struct for each channel

u32 dwNoiseVal=1;                          // global noise generator

u16 spuCtrl=0;                             // some vars to store psx reg infos
u16 spuStat=0;
u16 spuIrq=0;
u32 spuAddr=0xffffffff;                    // address into spu mem
int bSpuInit=0;
int bSPUIsOpen=0;

#ifdef _WINDOWS
HWND    hWMain=0;                                      // window handle
HWND    hWRecord=0;
static HANDLE   hMainThread;                           
#else
// 2003/06/07 - Pete
#ifndef NOTHREADLIB
static pthread_t thread = -1;                          // thread id (linux)
#endif
#endif

unsigned long dwNewChannel=0;                          // flags for faster testing, if new channel starts


void (CALLBACK *cddavCallback)(unsigned short,unsigned short)=0;

void irqCallback(void) {
	psxHu32ref(0x1070)|= SWAPu32(0x200);
}

INLINE int limit( int v )
{
	if( v < -32768 )
	{
		return -32768;
	}
	else if( v > 32767 )
	{
		return 32767;
	}
	return v;
}

// certain globals (were local before, but with the new timeproc I need em global)

//this is used by the BRR decoder
const int f[5][2] = 
{   {    0,  0  },
	{   60,  0  },
	{  115, -52 },
	{   98, -55 },
	{  122, -60 }
};

int iCycle=0;
short * pS;

static int lastch=-1;      // last channel processed on spu irq in timer mode
static int lastns=0;       // last ns pos
static int iSecureStart=0; // secure start counter

//--------------------------------------------------------------

void SPU_chan::updatePitch(u16 pitch)
{
	smpinc = 44100.0 / 44100.0 / 4096 * pitch;
	//smpinc = 0.078f;
	//smpinc = 1.0f;
}

void SPU_chan::keyon()
{
	//? what happens when we keyon an already on channel? just guessing that its benign
	status = CHANSTATUS_PLAY;
	
	//DO NOT DO THIS even though spu.txt made you believe it was necessary.
	//ff7 sets loop starts before keyon and this overrides it
	//loopStartAddr = 0;

	iOldNoise = 0;
	flags = 0;
	smpcnt = 28;

	updatePitch(rawPitch);
	
	blockAddress = (rawStartAddr<<3);
	
	//printf("[%02d] Keyon at %08X with smpinc %f\n",ch,blockAddress,smpinc);

	//init interpolation state with zeros
	block[24] = block[25] = block[26] = block[27] = 0;

	//init BRR
	s_1 = s_2 = 0;

	StartADSR(this);
	StartREVERB(this);
};

void SPU_chan::keyoff()
{
	//printf("[%02d] keyoff\n",ch);
	status = CHANSTATUS_KEYOFF;
}

//---------------------------------------

#include "xa.cpp"

//old functions handy to refer to

//INLINE void VoiceChangeFrequency(SPUCHAN * pChannel)
//{
//	pChannel->iUsedFreq=pChannel->iActFreq;               // -> take it and calc steps
//	pChannel->sinc=pChannel->iRawPitch<<4;
//	if (!pChannel->sinc) pChannel->sinc=1;
//	if (iUseInterpolation==1) pChannel->SB[32]=1;         // -> freq change in simle imterpolation mode: set flag
//}

//INLINE void FModChangeFrequency(SPUCHAN * pChannel,int ns)
//{
//	int NP=pChannel->iRawPitch;
//
//	NP=((32768L+iFMod[ns])*NP)/32768L;
//
//	if (NP>0x3fff) NP=0x3fff;
//	if (NP<0x1)    NP=0x1;
//
//	NP=(44100L*NP)/(4096L);                               // calc frequency
//
//	pChannel->iActFreq=NP;
//	pChannel->iUsedFreq=NP;
//	pChannel->sinc=(((NP/10)<<16)/4410);
//	if (!pChannel->sinc) pChannel->sinc=1;
//	if (iUseInterpolation==1) pChannel->SB[32]=1;         // freq change in simple interpolation mode
//
//	iFMod[ns]=0;
//}


//-----------------------------------------------------------------------

// noise handler... just produces some noise data
// surely wrong... and no noise frequency (spuCtrl&0x3f00) will be used...
// and sometimes the noise will be used as fmod modulation... pfff

INLINE int iGetNoiseVal(SPU_chan* pChannel)
{
	int fa;

	if ((dwNoiseVal<<=1)&0x80000000L)
	{
		dwNoiseVal^=0x0040001L;
		fa=((dwNoiseVal>>2)&0x7fff);
		fa=-fa;
	}
	else fa=(dwNoiseVal>>2)&0x7fff;

//// mmm... depending on the noise freq we allow bigger/smaller changes to the previous val
	fa=pChannel->iOldNoise+((fa-pChannel->iOldNoise)/((0x001f-((spuCtrl&0x3f00)>>9))+1));
	if (fa>32767L)  fa=32767L;
	if (fa<-32767L) fa=-32767L;
	pChannel->iOldNoise=fa;

//	if (iUseInterpolation<2)                              // no gauss/cubic interpolation?
//		pChannel->SB[29] = fa;                               // -> store noise val in "current sample" slot

	return fa;


}

//these functions are an unreliable, inaccurate floor.
//it should only be used for positive numbers
//this isnt as fast as it could be if we used a visual c++ intrinsic, but those appear not to be universally available
FORCEINLINE u32 u32floor(float f)
{
#ifdef ENABLE_SSE2
	return (u32)_mm_cvtt_ss2si(_mm_set_ss(f));
#else
	return (u32)f;
#endif
}
FORCEINLINE u32 u32floor(double d)
{
#ifdef ENABLE_SSE2
	return (u32)_mm_cvttsd_si32(_mm_set_sd(d));
#else
	return (u32)d;
#endif
}

//same as above but works for negative values too.
//be sure that the results are the same thing as floorf!
FORCEINLINE s32 s32floor(float f)
{
#ifdef ENABLE_SSE2
	return _mm_cvtss_si32( _mm_add_ss(_mm_set_ss(-0.5f),_mm_add_ss(_mm_set_ss(f), _mm_set_ss(f))) ) >> 1;
#else
	return (s32)floorf(f);
#endif
}

static FORCEINLINE u32 sputrunc(float f) { return u32floor(f); }
static FORCEINLINE u32 sputrunc(double d) { return u32floor(d); }

enum SPUInterpolationMode
{
	SPUInterpolation_None = 0,
	SPUInterpolation_Linear = 1,
	SPUInterpolation_Gaussian = 2,
	SPUInterpolation_Cubic = 3,
	SPUInterpolation_Cosine = 4,
};

//a is the most recent sample, going back to d as the oldest
static FORCEINLINE s32 Interpolate(s16 a, s16 b, s16 c, s16 d, double _ratio)
{
	SPUInterpolationMode INTERPOLATE_MODE = (SPUInterpolationMode)iUseInterpolation;
	float ratio = (float)_ratio;

	switch(INTERPOLATE_MODE)
	{
	case SPUInterpolation_None:
		return a;
	case SPUInterpolation_Cosine:
		{
			//why did we change it away from the lookup table? somebody should research that
			ratio = ratio - (int)ratio;
			double ratio2 = ((1.0 - cos(ratio * M_PI)) * 0.5);
			//double ratio2 = (1.0f - cos_lut[((int)(ratio*256.0))&0xFF]) / 2.0f;
			return (s32)(((1-ratio2)*b) + (ratio2*a));
		}
	case SPUInterpolation_Linear:
		{
			//linear interpolation
			ratio = ratio - sputrunc(ratio);
			s32 temp = s32floor((1-ratio)*b + ratio*a);
			//printf("%d %d %d %f\n",a,b,temp,_ratio);
			return temp;
		} 
	case SPUInterpolation_Gaussian:
		{
			//I'm not sure whether I am doing this right.. 
			//low frequency things (try low notes on channel 10 of ff7 prelude song credits screen)
			//pop a little bit
			//the bit logic is taken from libopenspc
			ratio = ratio - sputrunc(ratio);
			ratio *= 256;
			s32 index = sputrunc(ratio)*4;
			s32 result = (gauss[index]*d)&~2047;
			result += (gauss[index+1]*c)&~2047;
			result += (gauss[index+2]*b)&~2047;
			result += (gauss[index+3]*a)&~2047;
			result = (result>>11)&~1;
			return result;
		}
	case SPUInterpolation_Cubic:
		{
			float y0 = d, y1 = c, y2 = b, y3 = a;
			float mu = ratio - (int)ratio;
			float mu2 = mu*mu;
			float a0 = y3 - y2 - y0 + y1;
			float a1 = y0 - y1 - a0;
			float a2 = y2 - y0;
			float a3 = y1;

			float result = a0*mu*mu2+a1*mu2+a2*mu+a3;
			return (s32)result;
		}
	default:
		return 0;
	}
}

////////////////////////////////////////////////////////////////////////

//triggers an irq if the irq address is in the specified range
void SPU_struct::triggerIrqRange(u32 base, u32 size)
{
	if(!isCore) return;

	//can't trigger an irq without the flag enabled
	if(!(spuCtrl&0x40)) return;

	u32 irqAddr = ((u32)spuIrq)<<3;
	if(base <= irqAddr && irqAddr < base+size) {
		//printf("triggering irq with args %08X %08X\n",base,size);
		irqCallback();
	}
}

SPU_chan::SPU_chan()
	: status(CHANSTATUS_STOPPED)
	, bFMod(FALSE)
	, bReverb(FALSE)
	, bNoise(FALSE)
{
}

SPU_struct::SPU_struct(bool _isCore)
: isCore(_isCore)
{
	//for debugging purposes it is handy for each channel to know what index he is.
	for(int i=0;i<24;i++)
		channels[i].ch = i;

	mixIrqCounter = 0;
}

s32 SPU_chan::decodeBRR(SPU_struct* spu)
{
	//find out which block we need and decode a new one if necessary.
	//it is safe to only check for overflow once since we can only play samples at +2 octaves (4x too fast)
	//and so as long as we are outputting at near psx resolution (44100) we can only advance through samples
	//at rates that are low enough to be safe.
	//if we permitted 11025khz output the story would be different.

restart:

	//printf("%d %f\n",ch,smpcnt);
	int sampnum = (int)smpcnt;
	if(sampnum>=28)
	{
		//end or loop
		if(flags&1)
		{
			//there seems to be a special condition which triggers the irq if the loop address equals the irq address
			//(even if this sample is stopping)
			//either that, or the hardware always reads the block referenced by loop target even if the sample is ending.
			//thousand arms will need this in order to trigger the very first voiceover
			//should this be 1 or 16? equality or range?
			spu->triggerIrqRange(loopStartAddr,16);

			//when the old spu would kill a sound this way, it was immediate.
			//maybe adsr release is only for keyoff
			if(flags != 3) {
				status = CHANSTATUS_STOPPED;
				return 0;
			}
			else {
				//printf("[%02d] looping to %d\n",ch,loopStartAddr);
				blockAddress = loopStartAddr;
				flags = 0;
				goto restart;
			}
		}

		//do not do this before "end or loop" or else looping will glitch as it fails to
		//immediately decode a block.
		//why did I ever try this? I can't remember.
		smpcnt -= 28;

		sampnum = (int)smpcnt;

		//this will be tested by the impressive valkyrie profile new game sound
		//which is large and apparently streams in
		spu->triggerIrqRange(blockAddress,16);

		u8 header0 = readSpuMem(blockAddress);
		
		flags = readSpuMem(blockAddress+1);

		//this flag bit means that we are setting the loop start point
		if(flags&4) {
			loopStartAddr = blockAddress;
			//printf("[%02d] embedded loop addr set to %d\n",ch,loopStartAddr);
		}

		blockAddress+=2;

		s32 predict_nr = header0;
		s32 shift_factor = predict_nr&0xf;
		predict_nr >>= 4;

		//before we decode a new block, save the last 4 values of the old block
		block[28] = block[24];
		block[29] = block[25];
		block[30] = block[26];
		block[31] = block[27];

		//decode 
		for(int i=0,j=0;i<14;i++)
		{
			s32 d = readSpuMem(blockAddress++);
			s32 s = (d&0xf)<<12;
			if(s&0x8000) s|=0xffff0000;
			s32 fa = (s>>shift_factor);
			fa = fa + ((s_1 * f[predict_nr][0])>>6) + ((s_2 * f[predict_nr][1])>>6);
			s_2 = s_1;
			s_1 = fa;
			
			block[j++] = fa;

			s = (d&0xf0)<<8;
			if(s&0x8000) s|=0xffff0000;

			fa = (s>>shift_factor);
			fa = fa + ((s_1 * f[predict_nr][0])>>6) + ((s_2 * f[predict_nr][1])>>6);
			s_2 = s_1;
			s_1 = fa;

			block[j++] = fa;
		}
	}

	//perform interpolation. hardcoded for now
	//if(GetAsyncKeyState('I'))
	if(true)
	{
		s16 a = block[sampnum];
		s16 b = block[(sampnum-1)&31];
		s16 c = block[(sampnum-2)&31];
		s16 d = block[(sampnum-3)&31];
		return Interpolate(a,b,c,d,smpcnt);
	}
	else return block[sampnum];

	//printf("%d\n",*out);
}


void mixAudio(bool kill, SPU_struct* spu, int length)
{
	memset(spu->outbuf, 0, length*4*2);

	//(todo - analyze master volumes etc.)

	bool checklog[24]; for(int i=0;i<24;i++) checklog[i] = false;

	for(int j=0;j<length;j++)
	{
		s32 left_accum = 0, right_accum = 0;
		s32 fmod;

		REVERB_initSample();

		for(int i=0;i<24;i++)
		{
			SPU_chan *chan = &spu->channels[i];

			if (chan->status == CHANSTATUS_STOPPED) continue;

			s32 samp;
			if(chan->bNoise)
				samp = iGetNoiseVal(chan);
			else
				samp = chan->decodeBRR(spu);


			s32 adsrLevel = MixADSR(chan);
			//printf("[%02d] adsr: %d\n",i,adsrLevel);

			//channel may have ended at any time
			if (chan->status == CHANSTATUS_STOPPED) {
				fmod = 0;
				continue;
			}

			checklog[i] = true;

			samp = samp * adsrLevel/1023;

			//apply the modulation
			if(chan->bFMod)
			{
				//this was a little hard to test. ff7 battle fx were using it, but 
				//its hard to tell since I dont think our noise is very good. need a better test.
				chan->smpcnt += chan->smpinc;
				s32 pitch = chan->rawPitch;
				pitch = ((32768L+fmod)*pitch)/32768L;
				if (pitch>0x3fff) pitch=0x3fff;
				if (pitch<0x1)    pitch=0x1;
				chan->updatePitch((u16)pitch);
			
				//just some diagnostics
				//static u16 lastPitch=0xFFFF, lastRawPitch=0xFFFF; if(chan->rawPitch != lastRawPitch || lastPitch != pitch) printf("fmod bending pitch from %04X to %04X\n",lastRawPitch=chan->rawPitch,lastPitch=pitch);
				//printf("fmod: %d\n",fmod);
			}

			//don't output this channel; it is used to modulate the next channel
			if(i<23 && spu->channels[i+1].bFMod)
			{
				//should this be limited? lets check it.
				if(samp < -32768 || samp > 32767) printf("[%02d]: limiting fmod value of %d !\n",samp);
				fmod = limit(samp);
				continue;
			}

			chan->smpcnt += chan->smpinc;

			s32 left = (samp * chan->iLeftVolume) / 0x4000;
			s32 right = (samp * chan->iRightVolume) / 0x4000;

			left_accum += left;
			right_accum += right;

			if(!kill)
				if (chan->bRVBActive) 
					StoreREVERB(chan,left,right);
		} //channel loop

		if(!kill)
			left_accum += MixREVERBLeft();
			right_accum += MixREVERBRight();

		{
			s32 left, right;
			MixXA(&left,&right);

			left_accum += left;
			right_accum += right;
		}

		//handle spu mute
		if ((spuCtrl&0x4000)==0) {
			left_accum = 0;
			right_accum = 0;
		}

		s16 output[] = { limit(left_accum), limit(right_accum) };

		if(isStreamingMode())
			synchronizer->enqueue_samples(output,1);
		else
		{
			spu->outbuf[j*2] = output[0];
			spu->outbuf[j*2+1] = output[1];
		}

		//fwrite(&left_out,2,1,wavout);
		//fwrite(&right_out,2,1,wavout);
		//fflush(wavout);

		// special irq handling in the decode buffers (0x0000-0x1000)
		// we know:
		// the decode buffers are located in spu memory in the following way:
		// 0x0000-0x03ff  CD audio left
		// 0x0400-0x07ff  CD audio right
		// 0x0800-0x0bff  Voice 1
		// 0x0c00-0x0fff  Voice 3
		// and decoded data is 16 bit for one sample
		// we assume:
		// even if voices 1/3 are off or no cd audio is playing, the internal
		// play positions will move on and wrap after 0x400 bytes.
		// Therefore: we just need a pointer from spumem+0 to spumem+3ff, and
		// increase this pointer on each sample by 2 bytes. If this pointer
		// (or 0x400 offsets of this pointer) hits the spuirq address, we generate
		// an IRQ. 

		if(spu->isCore)
		{
			spu->triggerIrqRange(spu->mixIrqCounter,2);
			spu->triggerIrqRange(spu->mixIrqCounter+0x400,2);
			spu->triggerIrqRange(spu->mixIrqCounter+0x800,2);
			spu->triggerIrqRange(spu->mixIrqCounter+0xC00,2);

			spu->mixIrqCounter += 2;
			spu->mixIrqCounter &= 0x3FF;
		}

	} //sample loop

	//this prints which channels are active
	//for(int i=0;i<24;i++) {
	//	if(i==16) printf(" ");
	//	if(checklog[i]) printf("%1x",i&15);
	//	else printf(" ");
	//}
	//printf("\n");
}


//measured 16123034 cycles per second
//this is around 15.3761 MHZ
//this is pasted from the gpu:
//fFrameRateHz=33868800.0f/677343.75f;        // 50.00238
//else fFrameRateHz=33868800.0f/680595.00f;        // 49.76351
//16934400 is half this. we'll go with that assumption for now
static const double time_per_cycle = (double)1.0/(PSXCLK);
static const double samples_per_cycle = time_per_cycle * 44100;
static double mixtime = 0;

void SPUasync(unsigned long cycle)
{
	u32 SNDDXGetAudioSpace();
	u32 len = SNDDXGetAudioSpace();

	mixtime += samples_per_cycle*cycle;
	int mixtodo = (int)mixtime;
	mixtime -= mixtodo;

	if(isStreamingMode())
	{
		//printf("mixing %d cycles (%d samples) (adjustobuf size:%d)\n",cycle,mixtodo,adjustobuf.size);

		mixAudio(false,SPU_core,mixtodo);

		//u32 todo = len;
		//printf("%d\n",len);
		//while(todo>0)
		//{
		//	//8 is chosen to accomodate nitsuja's synchronizer
		//	if(todo<8) { printf("todo<8\n"); break; }
		//	u32 unit = 8;
		//	todo -= unit;
		//	printf("-- %d\n",todo);
		//	s16 sample[16];
		//	int done = synchronizer->output_samples(sample,unit);
		//	if(done) {
		//		void SNDDXUpdateAudio(s16 *buffer, u32 num_samples);
		//		for(int j=0;j<done;j++)
		//			SNDDXUpdateAudio(&sample[j*2],1);
		//	}
		//	if(done<(int)unit) { 
		//		printf("done<unit ( %d<%d) \n", done, unit); break;
		//	}
		//}

		if(len>0)
		{
			static std::vector<s16> outbuf;
			if(outbuf.size() < len*2)
				outbuf.resize(len*2
				);

			int done = synchronizer->output_samples(&outbuf[0],len);
			void SNDDXUpdateAudio(s16 *buffer, u32 num_samples);
			for(int j=0;j<done;j++)
				SNDDXUpdateAudio(&outbuf[j*2],1);
		}

		//for(u32 i=0;i<len;i++) {
		//	s16 sample[2];
		//	int done = synchronizer->output_samples(sample,1);
		//	if(done) {
		//		void SNDDXUpdateAudio(s16 *buffer, u32 num_samples);
		//		SNDDXUpdateAudio(sample,1);
		//	} else break;
		//}

	}
	else
	{
		void SNDDXUpdateAudio(s16 *buffer, u32 num_samples);
		if(iUseTimer==1)
		{
			//dual synch/asynch mode
			//(do timing with core and mixing with user)
			mixAudio(false,SPU_user,len);
			mixAudio(true,SPU_core,mixtodo);
			SNDDXUpdateAudio(SPU_user->outbuf,len);
		}
		else
		{
			//pure asynch mode
			mixAudio(false,SPU_core,len);
			SNDDXUpdateAudio(SPU_core->outbuf,len);
		}
		
	}
}


////////////////////////////////////////////////////////////////////////
// XA AUDIO
////////////////////////////////////////////////////////////////////////

//this is called from the cdrom system with a buffer of decoded xa audio
//we are supposed to grab it and then play it. 
//FeedXA takes care of stashing it for us
void SPUplayADPCMchannel(xa_decode_t *xap)
{
	if (!iUseXA)    return;                               // no XA? bye
	if (!xap)       return;
	if (!xap->freq) return;                               // no xa freq ? bye

	FeedXA(xap);                                          // call main XA feeder
}

//this func will be called first by the main emu
long SPUinit(void)
{
	//wavout = fopen("c:\\pcsx.raw","wb");
	SPU_core = new SPU_struct(true);
	SPU_user = new SPU_struct(false);
	mixtime = 0;
	InitADSR();
	return 0;
}



////////////////////////////////////////////////////////////////////////
// SETUPSTREAMS: init most of the spu buffers
////////////////////////////////////////////////////////////////////////

void SetupStreams(void)
{
	int i;

	if (iUseReverb==1) i=88200*2;
	else              i=NSSIZE*2;

	InitREVERB();

	XAStart =                                             // alloc xa buffer
	  (unsigned long *)malloc(44100*4);
	XAPlay  = XAStart;
	XAFeed  = XAStart;
	XAEnd   = XAStart + 44100;
}

////////////////////////////////////////////////////////////////////////
// REMOVESTREAMS: free most buffer
////////////////////////////////////////////////////////////////////////

void RemoveStreams(void)
{
	free(XAStart);                                        // free XA buffer
	XAStart=0;

	ShutdownREVERB();

	/*
	 int i;
	 for(i=0;i<MAXCHAN;i++)
	  {
	   WaitForSingleObject(s_chan[i].hMutex,2000);
	   ReleaseMutex(s_chan[i].hMutex);
	   if(s_chan[i].hMutex)
	    {CloseHandle(s_chan[i].hMutex);s_chan[i].hMutex=0;}
	  }
	*/
}


////////////////////////////////////////////////////////////////////////
// SPUOPEN: called by main emu after init
////////////////////////////////////////////////////////////////////////

#ifdef _WINDOWS
long SPUopen(HWND hW)
#else
long SPUopen(void)
#endif
{
	if (bSPUIsOpen) return false;                             // security for some stupid main emus

	iUseXA=1;                                             // just small setup
	iVolume=3;
	spuIrq=0;
	spuAddr=0xffffffff;
	iSPUIRQWait=1;

#ifdef _WINDOWS
//	LastWrite=0xffffffff;
//	LastPlay=0;                      // init some play vars
	if (!IsWindow(hW)) hW=GetActiveWindow();
	hWMain = hW;                                          // store hwnd
#endif

	ReadConfig();                                         // read user stuff

	SetupSound();                                         // setup sound (before init!)

	SetupStreams();                                       // prepare streaming

	bSPUIsOpen=1;

#ifdef _WINDOWS
	if (iRecordMode)                                      // windows recording dialog
	{
		hWRecord=CreateDialog(0,MAKEINTRESOURCE(IDD_RECORD),
		                      NULL,(DLGPROC)RecordDlgProc);
		SetWindowPos(hWRecord,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW|SWP_NOACTIVATE);
		UpdateWindow(hWRecord);
		SetFocus(hWMain);
	}
#endif

	return PSE_SPU_ERR_SUCCESS;
}

////////////////////////////////////////////////////////////////////////

#ifndef _WINDOWS
void SPUsetConfigFile(char * pCfg)
{
	pConfigFile=pCfg;
}
#endif

////////////////////////////////////////////////////////////////////////
// SPUCLOSE: called before shutdown
////////////////////////////////////////////////////////////////////////

long SPUclose(void)
{
	if (!bSPUIsOpen) return 0;                            // some security

	bSPUIsOpen=0;                                         // no more open

#ifdef _WINDOWS
	if (IsWindow(hWRecord)) DestroyWindow(hWRecord);
	hWRecord=0;
#endif

	RemoveSound();                                        // no more sound handling

	RemoveStreams();                                      // no more streaming

	return 0;
}

////////////////////////////////////////////////////////////////////////
// SPUSHUTDOWN: called by main emu on final exit
////////////////////////////////////////////////////////////////////////

long SPUshutdown(void)
{
	return 0;
}

////////////////////////////////////////////////////////////////////////
// SPUCONFIGURE: call config dialog
////////////////////////////////////////////////////////////////////////

long SPUconfigure(void)
{
#ifdef _WINDOWS
	INT_PTR wtf = DialogBox(gApp.hInstance,MAKEINTRESOURCE(IDD_CFGDLG),
	          GetActiveWindow(),(DLGPROC)DSoundDlgProc);
	DWORD arg = GetLastError();
#else
	StartCfgTool("CFG");
#endif
	return 0;
}

////////////////////////////////////////////////////////////////////////
// SETUP CALLBACKS
// this functions will be called once,
// passes a callback that should be called on SPU-IRQ/cdda volume change
////////////////////////////////////////////////////////////////////////

void SPUregisterCDDAVolume(void (CALLBACK *CDDAVcallback)(unsigned short,unsigned short))
{
	cddavCallback = CDDAVcallback;
}

////////////////////////////////////////////////////////////////////////
// COMMON PLUGIN INFO FUNCS
////////////////////////////////////////////////////////////////////////

void SPUstartWav(char* filename)
{
	strncpy(szRecFileName,filename,260);
	iDoRecord=1;
	RecordStart();
}
void SPUstopWav()
{
	RecordStop();
}


//---------------------------------------------------------------
//original changelog

// History of changes:
//
// 2004/09/19 - Pete
// - added option: IRQ handling in the decoded sound buffer areas (Crash Team Racing)
//
// 2004/09/18 - Pete
// - changed global channel var handling to local pointers (hopefully it will help LDChen's port)
//
// 2004/04/22 - Pete
// - finally fixed frequency modulation and made some cleanups
//
// 2003/04/07 - Eric
// - adjusted cubic interpolation algorithm
//
// 2003/03/16 - Eric
// - added cubic interpolation
//
// 2003/03/01 - linuzappz
// - libraryName changes using ALSA
//
// 2003/02/28 - Pete
// - added option for type of interpolation
// - adjusted spu irqs again (Thousant Arms, Valkyrie Profile)
// - added MONO support for MSWindows DirectSound
//
// 2003/02/20 - kode54
// - amended interpolation code, goto GOON could skip initialization of gpos and cause segfault
//
// 2003/02/19 - kode54
// - moved SPU IRQ handler and changed sample flag processing
//
// 2003/02/18 - kode54
// - moved ADSR calculation outside of the sample decode loop, somehow I doubt that
//   ADSR timing is relative to the frequency at which a sample is played... I guess
//   this remains to be seen, and I don't know whether ADSR is applied to noise channels...
//
// 2003/02/09 - kode54
// - one-shot samples now process the end block before stopping
// - in light of removing fmod hack, now processing ADSR on frequency channel as well
//
// 2003/02/08 - kode54
// - replaced easy interpolation with gaussian
// - removed fmod averaging hack
// - changed .sinc to be updated from .iRawPitch, no idea why it wasn't done this way already (<- Pete: because I sometimes fail to see the obvious, haharhar :)
//
// 2003/02/08 - linuzappz
// - small bugfix for one usleep that was 1 instead of 1000
// - added iDisStereo for no stereo (Linux)
//
// 2003/01/22 - Pete
// - added easy interpolation & small noise adjustments
//
// 2003/01/19 - Pete
// - added Neill's reverb
//
// 2003/01/12 - Pete
// - added recording window handlers
//
// 2003/01/06 - Pete
// - added Neill's ADSR timings
//
// 2002/12/28 - Pete
// - adjusted spu irq handling, fmod handling and loop handling
//
// 2002/08/14 - Pete
// - added extra reverb
//
// 2002/06/08 - linuzappz
// - SPUupdate changed for SPUasync
//
// 2002/05/15 - Pete
// - generic cleanup for the Peops release