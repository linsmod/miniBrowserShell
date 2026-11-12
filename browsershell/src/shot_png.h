#ifndef SHOT_PNG_H
#define SHOT_PNG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Writes a top-down (or bottom-up) 32bpp RGB image to a PNG through WIC.
 * `stride` is in bytes. Returns 1 on success. */
int shotWritePng(const char* path, const unsigned char* bits, int width,
                 int height, int stride);

#ifdef __cplusplus
}
#endif

#endif /* SHOT_PNG_H */
