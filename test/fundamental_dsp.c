/* Independent checks of K-weighting, rational timing and extended peaks. */
#include <assert.h>
#include <stdio.h>
#include "../src/loudness.c"

typedef struct { long double b[3], a[3], x1, x2, y1, y2; } oracle_biquad;
static long double oracle_filter(oracle_biquad *q, long double x)
{
	long double y = q->b[0]*x + q->b[1]*q->x1 + q->b[2]*q->x2 - q->a[1]*q->y1 - q->a[2]*q->y2;
	q->x2=q->x1; q->x1=x; q->y2=q->y1; q->y1=y;
	return y;
}

static void check_k_weighting(void)
{
	const long rates[] = {8000, 48000, 96000, 192000};
	for (unsigned int r = 0; r < sizeof(rates)/sizeof(*rates); r++) {
		long rate = rates[r];
		ebur128_state *st = ebur128_init(1, rate, EBUR128_MODE_M);
		oracle_biquad shelf = {0}, highpass = {0};
		long double k = tanl(acosl(-1.L)*1681.974450955533L/rate), q=.7071752369554196L;
		long double vh=powl(10.L,3.999843853973347L/20.L), vb=powl(vh,.4996667741545416L), a0=1+k/q+k*k;
		shelf.b[0]=(vh+vb*k/q+k*k)/a0; shelf.b[1]=2*(k*k-vh)/a0; shelf.b[2]=(vh-vb*k/q+k*k)/a0;
		shelf.a[1]=2*(k*k-1)/a0; shelf.a[2]=(1-k/q+k*k)/a0;
		k=tanl(acosl(-1.L)*38.13547087602444L/rate); q=.5003270373238773L;
		highpass.b[0]=highpass.b[2]=1; highpass.b[1]=-2;
		highpass.a[1]=2*(k*k-1)/(1+k/q+k*k); highpass.a[2]=(1-k/q+k*k)/(1+k/q+k*k);
		assert(st);
		for (long start=0; start<rate*2; start+=257) {
			double input[257]; int n=(int)fmin(257,rate*2-start);
			for (int i=0;i<n;i++) input[i]=.5+1e-5*sin(2*M_PI*997*(start+i)/rate);
			ebur128_filter_double(st,input,(size_t)n);
			for (int i=0;i<n;i++) {
				long double expected=oracle_filter(&highpass,oracle_filter(&shelf,input[i]));
				assert(fabsl(expected-st->d->audio_data[i]) < 5e-10L);
			}
		}
		ebur128_destroy(&st);
	}
}

static void check_window_clock(void)
{
	const unsigned long rates[]={11025,11026,44101};
	for(unsigned int r=0;r<sizeof(rates)/sizeof(*rates);r++) {
		unsigned long rate=rates[r];
		ebur128_state *st=ebur128_init(1,rate,EBUR128_MODE_I);
		size_t total=0;
		assert(st && st->d->needed_frames==((uint64_t)rate*4+5)/10);
		for (unsigned int window=0;window<37;window++) {
			size_t end=((uint64_t)rate*(window+4)+5)/10;
			while(total<end) {
				double input[257]; size_t n=end-total>257?257:end-total;
				for(size_t i=0;i<n;i++) input[i]=.1*sin(2*M_PI*997*(total+i)/(double)rate);
				assert(ebur128_add_frames_double(st,input,n)==EBUR128_SUCCESS); total+=n;
			}
			assert(st->d->block_list_size==window+1);
			assert(st->d->needed_frames==((uint64_t)rate*(window+5)+5)/10-end);
		}
		ebur128_destroy(&st);
	}
}

static void check_extended_peak(void)
{
	double data[128], *pcm[]={data}, precise, extended, expected=0.;
	WG_METER *a=wg_meter_create_mode(48000,1,0,1,WG_PEAK_PRECISE);
	WG_METER *b=wg_meter_create_mode(48000,1,0,1,WG_PEAK_EXTENDED);
	assert(a && b);
	for(int i=0;i<128;i++) {
		double d=63.5-i, sinc=sin(M_PI*d)/(M_PI*d);
		data[i]=sinc>0?.5:-.5; expected+=fabs(.5*sinc);
	}
	assert(wg_meter_add(a,pcm,128));
	for(int start=0;start<128;start+=7) {
		double *part[]={data+start}; size_t n=128-start<7?128-start:7;
		assert(wg_meter_add(b,part,n));
	}
	assert(wg_meter_finish(a,NULL,&precise) && wg_meter_finish(b,NULL,&extended));
	assert(extended>precise*1.5 && extended<=expected*1.001);
	assert(20*log10(expected/extended)<.6);
	/* Independent, unpruned convolution of all 128 input samples at the
	 * symmetry center. It avoids the online kernel's ordering and bounds. */
	{
		long double sum=0., normalization=0.;
		for(int j=-WG_REFINE_RADIUS+1;j<=WG_REFINE_RADIUS;j++) {
			long double d=.5L-j;
			long double c=sinl(acosl(-1.L)*d)/(acosl(-1.L)*d)*(.5L+.5L*cosl(acosl(-1.L)*d/WG_REFINE_RADIUS));
			normalization+=c;
			if(63+j>=0 && 63+j<128) sum+=data[63+j]*c;
		}
		assert(fabsl(extended-fabsl(sum/normalization))<1e-12L);
	}
	{
		double again; assert(wg_meter_finish(b,NULL,&again) && again==extended);
	}
	wg_meter_free(a);wg_meter_free(b);
}

static void check_refinement_bounds(void)
{
	wg_peak_refiner *r=wg_peak_refiner_create();
	double data[2*WG_REFINE_LENGTH], *p=data+WG_REFINE_LENGTH;
	uint32_t rng=9127;
	assert(r);
	for(int trial=0;trial<200;trial++) {
		double maximum=0., expected;
		for(int j=0;j<WG_REFINE_LENGTH;j++) {
			rng=rng*1664525u+1013904223u;
			p[-j]=ldexp((double)rng/2147483648.-1.,trial%201-100);
			maximum=fmax(maximum,fabs(p[-j]));
		}
		double initial=maximum*(trial%7)/2.; expected=initial;
		for(unsigned int group=0;group<4;group++) {
			double v[4]={0};
			for(unsigned int j=0;j<WG_REFINE_LENGTH;j++)
				for(unsigned int lane=0;lane<4;lane++) v[lane]+=p[-(int)r->index[j]]*r->coefficient[group][j][lane];
			for(unsigned int lane=0;lane<4;lane++) expected=fmax(expected,fabs(v[lane]));
		}
		double room=maximum*r->norm*(8.*WG_REFINE_LENGTH*DBL_EPSILON)+DBL_MIN;
		r->peak=initial;wg_peak_refiner_frame(r,p,maximum,room);assert(r->peak==expected);
#if WG_REFINE_AVX2
		if(__builtin_cpu_supports("avx2")) {
			r->peak=initial;wg_peak_refiner_frame_avx2(r,p,maximum,room);assert(r->peak==expected);
		}
#endif
	}
	free(r);
}

static void check_extended_stereo_chunks(void)
{
	double data[2][512], *pcm[2]={data[0],data[1]}, a, b;
	WG_METER *whole=wg_meter_create_mode(48000,2,1,1,WG_PEAK_EXTENDED);
	WG_METER *split=wg_meter_create_mode(48000,2,0,1,WG_PEAK_EXTENDED);
	assert(whole && split);
	for(int ch=0;ch<2;ch++) for(int i=0;i<512;i++)
		data[ch][i]=(i<123?.02:.5)*sin(i*(.13+ch*1.1)+ch*.7);
	assert(wg_meter_add(whole,pcm,512));
	for(int start=0;start<512;) {
		int n=start%67+1; if(n>512-start)n=512-start;
		double *part[2]={data[0]+start,data[1]+start};
		assert(wg_meter_add(split,part,(size_t)n));start+=n;
	}
	assert(wg_meter_finish(whole,NULL,&a) && wg_meter_finish(split,NULL,&b) && a==b);
	wg_meter_free(whole);wg_meter_free(split);
}

int main(void)
{
	check_k_weighting();check_window_clock();check_extended_peak();check_refinement_bounds();
	check_extended_stereo_chunks();
	puts("Fundamental DSP: independent K-weighting, fractional window clock and extended peak reconstruction passed");
	return 0;
}
