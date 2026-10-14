/* Private-state checks are intentional: rounded gain alone can hide IIR drift. */
#include <assert.h>
#include <stdint.h>
#include <float.h>
#include "../gain_analysis.c"

#define COUNT 12000
static double left[COUNT + MAX_ORDER], right[COUNT + MAX_ORDER];
static double yl[COUNT + MAX_ORDER], yr[COUNT + MAX_ORDER];
static double bl[COUNT + MAX_ORDER], br[COUNT + MAX_ORDER];
#ifdef WAVEGAIN_HAVE_FILTER_PAIR
static double pl[COUNT + MAX_ORDER], pr[COUNT + MAX_ORDER];
static double ql[COUNT + MAX_ORDER], qr[COUNT + MAX_ORDER];
#endif
static const long rates[] = {96000,88200,64000,48000,44100,32000,
                            24000,22050,16000,12000,11025,8000};

static double test_signal(int pattern, size_t n, long rate, int channel)
{
    uint32_t x = (uint32_t)n + 1234567u + (uint32_t)channel * 7654321u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    switch (pattern) {
    case 0: return 0.;
    case 1: return n == 0 ? (channel ? -32767. : 32767.) : 0.;
    case 2: return channel ? -4096. : 8192.;
    case 3: return (n & 1) ? -32767. : 32767.;
    case 4: return (int32_t)x * (32767. / 2147483648.);
    case 5: return 6553.4 * sin(2 * 3.141592653589793 *
                              (channel ? 997. : 440.) * n / rate);
    case 6: return 1e-12 * sin(2 * 3.141592653589793 * 440. * n / rate);
    default: return 12000. * sin(2 * 3.141592653589793 *
                                (40. * n / rate + 8000. * n * n / rate / rate));
    }
}

static void kernels(void)
{
    size_t i, k;
    int pattern;
    for (k = 0; k < sizeof(rates)/sizeof(*rates); k++) {
#ifdef FILTER_COMPATIBILITY_TEST
        if (rates[k] == 88200) continue; /* Deliberate coefficient correction. */
#endif
        for (pattern = 0; pattern < 8; pattern++) {
            memset(left,0,sizeof(left)); memset(right,0,sizeof(right));
            memset(yl,0,sizeof(yl)); memset(yr,0,sizeof(yr));
            memset(bl,0,sizeof(bl)); memset(br,0,sizeof(br));
            for (i = 0; i < COUNT; i++) {
                left[i+MAX_ORDER] = test_signal(pattern,i,rates[k],0);
                right[i+MAX_ORDER] = test_signal(pattern,i,rates[k],1);
            }
            filterYule(left+MAX_ORDER,yl+MAX_ORDER,COUNT,ABYule[k]);
            filterYule(right+MAX_ORDER,yr+MAX_ORDER,COUNT,ABYule[k]);
            filterButter(yl+MAX_ORDER,bl+MAX_ORDER,COUNT,ABButter[k]);
            filterButter(yr+MAX_ORDER,br+MAX_ORDER,COUNT,ABButter[k]);
            for (i = MAX_ORDER; i < COUNT+MAX_ORDER; i++) {
                assert(isfinite(yl[i]) && isfinite(yr[i]));
                assert(isfinite(bl[i]) && isfinite(br[i]));
                assert(fabs(bl[i]) < 1e6 && fabs(br[i]) < 1e6);
            }
#ifdef WAVEGAIN_HAVE_FILTER_PAIR
            memset(pl,0,sizeof(pl)); memset(pr,0,sizeof(pr));
            memset(ql,0,sizeof(ql)); memset(qr,0,sizeof(qr));
            /* Include one-sample calls and all history/window boundaries. */
            for (i = 0; i < COUNT;) {
                size_t n = (i % 37) + 1;
                if (n > COUNT-i) n = COUNT-i;
                filterYulePair(left+MAX_ORDER+i,right+MAX_ORDER+i,
                               pl+MAX_ORDER+i,pr+MAX_ORDER+i,n,ABYule[k]);
                filterButterPair(pl+MAX_ORDER+i,pr+MAX_ORDER+i,
                                 ql+MAX_ORDER+i,qr+MAX_ORDER+i,n,ABButter[k]);
                i += n;
            }
            assert(!memcmp(yl,pl,sizeof(yl)) && !memcmp(yr,pr,sizeof(yr)));
            assert(!memcmp(bl,ql,sizeof(bl)) && !memcmp(br,qr,sizeof(br)));
#endif
        }
    }
}

static void dump(const void *data, size_t bytes)
{
    assert(fwrite(data,1,bytes,stdout) == bytes);
}

static void api_case(long rate, int pattern, int channels, int chunk_mode,
                     double amplitude)
{
    size_t n, pos = 0, total = (size_t)rate / 5 + 17;
    const size_t chunks[] = {1,2,9,10,11,15,16,17,511,4095,4096};
    int index = 0;
    while (pos < total) {
        if (chunk_mode == 0) n = 4096;
        else if (chunk_mode == 1) n = chunks[index++ % (sizeof(chunks)/sizeof(*chunks))];
        else n = (size_t)sampleWindow + (index++ % 3) - 1;
        if (n > total-pos) n = total-pos;
        assert(n <= COUNT);
        for (size_t j = 0; j < n; j++) {
            left[j] = amplitude * test_signal(pattern,pos+j,rate,0);
            right[j] = amplitude * test_signal(pattern,pos+j,rate,1);
        }
        assert(AnalyzeSamples(left,channels == 2 ? right : NULL,n,channels) == GAIN_ANALYSIS_OK);
        /* Check state as well as histogram, including incomplete RMS windows. */
        dump(linprebuf,MAX_ORDER*sizeof(double)); dump(rinprebuf,MAX_ORDER*sizeof(double));
        dump(lstepbuf,MAX_ORDER*sizeof(double)); dump(rstepbuf,MAX_ORDER*sizeof(double));
        dump(loutbuf,MAX_ORDER*sizeof(double)); dump(routbuf,MAX_ORDER*sizeof(double));
        dump(&totsamp,sizeof(totsamp));
        dump(&lsum,sizeof(lsum)); dump(&rsum,sizeof(rsum));
        pos += n;
    }
    dump(A,sizeof(A));
    {
        double gain = GetTitleGain(), album = GetAlbumGain();
        assert(isfinite(gain) && gain != GAIN_NOT_ENOUGH_SAMPLES);
        assert(isfinite(album) && album != GAIN_NOT_ENOUGH_SAMPLES);
        dump(&gain,sizeof(gain)); dump(&album,sizeof(album));
        for (n = 0; n < sizeof(A)/sizeof(*A); n++) assert(A[n] == 0);
        for (n = 0; n < MAX_ORDER; n++) {
            assert(linprebuf[n] == 0 && rinprebuf[n] == 0);
            assert(lstepbuf[n] == 0 && rstepbuf[n] == 0);
            assert(loutbuf[n] == 0 && routbuf[n] == 0);
        }
    }
}

/* Locate an actual histogram-bin transition, then test both adjacent doubles
 * and tiny level offsets. This catches differences hidden by ordinary tones. */
static int tone_bin(long rate, double amplitude)
{
    assert(InitGainAnalysis(rate) == INIT_GAIN_ANALYSIS_OK);
    for (size_t j = 0; j < (size_t)sampleWindow; j++) {
        left[j] = amplitude * test_signal(5,j,rate,0);
        right[j] = amplitude * test_signal(5,j,rate,1);
    }
    assert(AnalyzeSamples(left,right,(size_t)sampleWindow,2) == GAIN_ANALYSIS_OK);
    for (size_t j = 0; j < sizeof(A)/sizeof(*A); j++)
        if (A[j]) return (int)j;
    assert(0); return -1;
}

static void api(void)
{
    size_t k;
    int pattern, channels, chunks;
    for (k = 0; k < sizeof(rates)/sizeof(*rates); k++) {
#ifdef FILTER_COMPATIBILITY_TEST
        if (rates[k] == 88200) continue;
#endif
        for (channels = 1; channels <= 2; channels++) {
            assert(InitGainAnalysis(rates[k]) == INIT_GAIN_ANALYSIS_OK);
            for (pattern = 0; pattern < 8; pattern++)
                for (chunks = 0; chunks < 3; chunks++)
                    api_case(rates[k],pattern,channels,chunks,1.);
        }
    }
    for (k = 0; k < sizeof(rates)/sizeof(*rates); k++) {
#ifdef FILTER_COMPATIBILITY_TEST
        if (rates[k] == 88200) continue;
#endif
        double lo = 1., hi = 1.01, boundary;
        int target = tone_bin(rates[k],lo) + 1;
        assert(tone_bin(rates[k],hi) >= target);
        for (int j = 0; j < 64; j++) {
            double mid = (lo+hi)*.5;
            if (tone_bin(rates[k],mid) < target) lo = mid;
            else hi = mid;
        }
        boundary = hi;
        dump(&boundary,sizeof(boundary));
        assert(InitGainAnalysis(rates[k]) == INIT_GAIN_ANALYSIS_OK);
        api_case(rates[k],5,2,1,nextafter(boundary,0.));
        api_case(rates[k],5,2,1,nextafter(boundary,INFINITY));
        for (int j = -4; j <= 4; j++)
            api_case(rates[k],5,2,1,boundary*pow(10.,j*1e-8));
    }
}

static void errors(void)
{
    const double invalid[] = {NAN,INFINITY,DBL_MAX};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); i++) {
        assert(InitGainAnalysis(88200) == INIT_GAIN_ANALYSIS_OK);
        for (size_t j = 0; j < (size_t)sampleWindow; j++) left[j] = invalid[i];
        assert(AnalyzeSamples(left,NULL,(size_t)sampleWindow,1) == GAIN_ANALYSIS_ERROR);
        assert(GetTitleGain() == GAIN_NOT_ENOUGH_SAMPLES);
    }
    assert(InitGainAnalysis(44100) == INIT_GAIN_ANALYSIS_OK);
    assert(AnalyzeSamples(NULL,NULL,0,2) == GAIN_ANALYSIS_OK);
    assert(AnalyzeSamples(left,right,1,3) == GAIN_ANALYSIS_ERROR);
}

int main(void)
{
#ifdef FILTER_EXPECT_SIMD
# ifdef WAVEGAIN_HAVE_FILTER_PAIR
    _Static_assert(FILTER_EXPECT_SIMD == 1, "Expected scalar backend");
# else
    _Static_assert(FILTER_EXPECT_SIMD == 0, "Expected paired SIMD backend");
# endif
#endif
    kernels(); api();
#ifndef FILTER_COMPATIBILITY_TEST
    errors();
#endif
    fprintf(stderr,"Filters: 12 rates, kernel bits, mono/stereo API state/histograms, chunks, album resets and non-finite guards passed (SIMD=%d).\n",
#ifdef WAVEGAIN_HAVE_FILTER_PAIR
            1
#else
            0
#endif
    );
    return 0;
}
