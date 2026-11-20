#include "calib_job.h"
#include <stddef.h>
#include <string.h>

int calib_job_block(CalibJobState* st, unsigned r, const CalibJob job[2], int nspk, long n,
                    float* out, int* cap_at, int* take) {
    /* A new request starts from zero whatever the last one left: an aborted capture stops mid-way,
     * and a finished one leaves both positions past its own lengths. */
    if (r != st->cur) { st->cur = r; st->running = r != 0; st->play_pos = st->cap_pos = 0; }
    const CalibJob* J = st->running ? &job[r & 1u] : NULL;
    for (int c = 0; c < nspk; ++c) {
        float* blk = out + (size_t)c * (size_t)n;
        const int off = !J ? -1 : (c == J->ch) ? 0 : (c == J->ch2) ? J->gap2 : -1;
        if (off >= 0 && st->play_pos - off < J->play_len && st->play_pos + n > off) {
            for (long i = 0; i < n; ++i) {
                const long k = st->play_pos + i - off;
                blk[i] = (k >= 0 && k < J->play_len) ? J->play[k] : 0.f;
            }
        } else {
            memset(blk, 0, (size_t)n * sizeof(float));
        }
    }
    *cap_at = 0; *take = 0;
    if (!J) return 0;
    const int rem = J->cap_len - st->cap_pos;
    const long tk = n < rem ? n : rem;
    if (tk > 0) { *cap_at = st->cap_pos; *take = (int)tk; }
    st->play_pos += (int)n;
    st->cap_pos  += (int)n;
    if (st->cap_pos >= J->cap_len) { st->running = 0; return 1; }
    return 0;
}
