/*
 * asio_session.h — arbitration for the ASIO SDK's ONE process-wide "current driver" slot.
 *
 * The SDK host (the global AsioDrivers + ASIOInit/ASIOStart/ASIOExit) acts on a single current
 * driver. bwa_calib_view links TWO capture units (zylia_capture: ZM-1 input; calib_capture: the
 * 26-out sweep), so an open while the other unit is streaming would clobber its driver — and
 * either unit's close would ASIOExit the other's stream. Units must acquire the slot before
 * loadAsioDriver and release it after ASIOExit. Control/UI/worker threads only (never the audio
 * callback); acquisition is a CAS so a worker-thread open can't race a UI-thread open.
 */
#ifndef BWA_ASIO_SESSION_H
#define BWA_ASIO_SESSION_H

/* 1 = the slot is yours; 0 = busy (stderr names the holder — close that first). `who` must be a
 * string literal / static (it is shown to the user while held). */
int  asio_session_acquire(const char* who);
void asio_session_release(void);

#endif /* BWA_ASIO_SESSION_H */
