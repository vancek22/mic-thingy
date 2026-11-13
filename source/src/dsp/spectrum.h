/* Spectrum analyser for the GUI: 1024-point Hann-windowed FFT of the newest input and output
 * samples, in dBFS per bin (46.875 Hz resolution), smoothed over time. UI thread only; the audio
 * thread feeds it through Pipeline.scope_ring (interleaved input/output floats). */
#ifndef VF_SPECTRUM_H
#define VF_SPECTRUM_H

#define VF_SPEC_N    1024
#define VF_SPEC_BINS (VF_SPEC_N / 2 + 1)

typedef struct VfSpectrum {
    float cs[VF_SPEC_N / 2], sn[VF_SPEC_N / 2];
    unsigned short rev[VF_SPEC_N];
    float win[VF_SPEC_N];
    float in_hist[VF_SPEC_N], out_hist[VF_SPEC_N];   /* newest samples, oldest first */
    float re[VF_SPEC_N], im[VF_SPEC_N];
    float in_db[VF_SPEC_BINS], out_db[VF_SPEC_BINS]; /* smoothed result, dBFS (>= -120) */
    int   primed;
} VfSpectrum;

void vf_spec_init(VfSpectrum *s);
/* Appends n samples of input and output (n <= 1024). */
void vf_spec_push(VfSpectrum *s, const float *in, const float *out, int n);
/* Recomputes in_db/out_db; `smooth` 0..1 keeps that fraction of the previous values (attack is instant). */
void vf_spec_update(VfSpectrum *s, float smooth);

#endif
