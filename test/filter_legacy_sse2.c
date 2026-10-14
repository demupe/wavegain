/* Validate the historical Windows/SSE2 layout independently of new SIMD. */
#include <assert.h>
#include <float.h>
#include "../gain_analysis.c"
#define COUNT 8192
static double in[COUNT+10], yule[COUNT+10], butter[COUNT+10];
static long double reference[COUNT+10], filtered[COUNT+10];

int main(void)
{
    double max_error = 0.;
    for (int k = 0; k < 12; k++) {
        for (int pattern = 0; pattern < 3; pattern++) {
            memset(in,0,sizeof(in)); memset(yule,0,sizeof(yule));
            memset(butter,0,sizeof(butter)); memset(reference,0,sizeof(reference));
            memset(filtered,0,sizeof(filtered));
            for (int i = 10; i < COUNT+10; i++) {
                in[i] = pattern == 0 ? (i == 10 ? 1. : 0.) :
                        pattern == 1 ? .2 : .5*sin(i*.73);
                long double value = 1e-10L + (long double)in[i]*ABYule[k][0];
                for (int j = 1; j <= 10; j++)
                    value = value - reference[i-j]*ABYule[k][11+j] +
                            (long double)in[i-j]*ABYule[k][j];
                reference[i] = value;
                filtered[i] = reference[i]*ABButter[k][0] - filtered[i-1]*ABButter[k][4] +
                              reference[i-1]*ABButter[k][1] - filtered[i-2]*ABButter[k][5] +
                              reference[i-2]*ABButter[k][2];
            }
            filterYule(in+10,yule+10,COUNT,ABYule[k]);
            filterButter(yule+10,butter+10,COUNT,ABButter[k]);
            for (int i = 10; i < COUNT+10; i++) {
                double error = (double)fabsl(yule[i]-reference[i]);
                if (error > max_error) max_error = error;
                assert(isfinite(yule[i]) && isfinite(butter[i]));
                assert(error < 1e-8);
                assert(fabsl(butter[i]-filtered[i]) < 1e-8);
            }
        }
    }
    {
        const double invalid[] = {NAN,INFINITY,DBL_MAX};
        for (size_t k = 0; k < sizeof(invalid)/sizeof(*invalid); k++) {
            assert(InitGainAnalysis(88200) == INIT_GAIN_ANALYSIS_OK);
            for (long i = 0; i < sampleWindow; i++) in[i] = invalid[k];
            assert(AnalyzeSamples(in,in,(size_t)sampleWindow,2) == GAIN_ANALYSIS_ERROR);
            assert(GetTitleGain() == GAIN_NOT_ENOUGH_SAMPLES);
        }
    }
    printf("Legacy SSE2 impulse/DC/tone responses and non-finite guards passed; max Yule error vs long-double reference %.9g.\n",max_error);
    return 0;
}
