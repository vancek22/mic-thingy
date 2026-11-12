/* RBJ-cookbook biquads, transposed direct form II. */
#ifndef VF_BIQUAD_H
#define VF_BIQUAD_H

typedef struct Biquad {
    float b0, b1, b2, a1, a2;   /* normalised (a0 = 1) */
    float z1, z2;
} Biquad;

void biquad_reset(Biquad *b);                 /* identity filter, clears state */
void biquad_highpass(Biquad *b, float sr, float freq, float q);
void biquad_lowshelf(Biquad *b, float sr, float freq, float gain_db);
void biquad_highshelf(Biquad *b, float sr, float freq, float gain_db);
void biquad_peaking(Biquad *b, float sr, float freq, float q, float gain_db);
void biquad_lowpass(Biquad *b, float sr, float freq, float q);
void biquad_notch(Biquad *b, float sr, float freq, float q);

static inline float biquad_tick(Biquad *b, float x)
{
    float y = b->b0 * x + b->z1;
    b->z1 = b->b1 * x - b->a1 * y + b->z2;
    b->z2 = b->b2 * x - b->a2 * y;
    return y;
}

#endif
