/* ambisonics.c — 3rd-order real SH encode (ACN/SN3D). See ambisonics.h. */
#include "ambisonics.h"

/* SN3D-normalized real spherical harmonics in Cartesian form (unit dir; x=front, y=left, z=up).
 * Constants: sqrt(3)=1.7320508, sqrt(3)/2=0.8660254, sqrt(15)=3.8729833, sqrt(15)/2=1.9364917,
 *            sqrt(5/8)=0.7905694, sqrt(3/8)=0.6123724. */
void ambi_encode_sn3d(const float dir[3], float y[BW_AMBI_CH]) {
    const float x = dir[0], yy = dir[1], z = dir[2];

    y[0]  = 1.0f;                                            /* l=0          */

    y[1]  = yy;                                              /* l=1, m=-1  y */
    y[2]  = z;                                               /*      m= 0  z */
    y[3]  = x;                                               /*      m=+1  x */

    y[4]  = 1.7320508f * x * yy;                             /* l=2, m=-2    */
    y[5]  = 1.7320508f * yy * z;                             /*      m=-1    */
    y[6]  = 0.5f * (3.0f * z * z - 1.0f);                    /*      m= 0    */
    y[7]  = 1.7320508f * x * z;                              /*      m=+1    */
    y[8]  = 0.8660254f * (x * x - yy * yy);                  /*      m=+2    */

    y[9]  = 0.7905694f * yy * (3.0f * x * x - yy * yy);      /* l=3, m=-3    */
    y[10] = 3.8729833f * x * yy * z;                         /*      m=-2    */
    y[11] = 0.6123724f * yy * (5.0f * z * z - 1.0f);         /*      m=-1    */
    y[12] = 0.5f * z * (5.0f * z * z - 3.0f);                /*      m= 0    */
    y[13] = 0.6123724f * x * (5.0f * z * z - 1.0f);          /*      m=+1    */
    y[14] = 1.9364917f * z * (x * x - yy * yy);              /*      m=+2    */
    y[15] = 0.7905694f * x * (x * x - 3.0f * yy * yy);       /*      m=+3    */
}

/* sqrt(2l+1)/sqrt(4pi) per ACN channel — SN3D (AmbiX) -> phonon's orthonormal real SH (see header).
 * 1/sqrt(4pi)=0.2820948, sqrt(3)/sqrt(4pi)=0.4886025, sqrt(5)/sqrt(4pi)=0.6307832, sqrt(7)/..=0.7463527. */
const float ambi_phonon_scale[BW_AMBI_CH] = {
    0.2820948f,                                                                     /* l=0 */
    0.4886025f, 0.4886025f, 0.4886025f,                                             /* l=1 */
    0.6307832f, 0.6307832f, 0.6307832f, 0.6307832f, 0.6307832f,                     /* l=2 */
    0.7463527f, 0.7463527f, 0.7463527f, 0.7463527f, 0.7463527f, 0.7463527f, 0.7463527f  /* l=3 */
};
