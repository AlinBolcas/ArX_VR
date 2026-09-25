// Gaussian splats as the headset keeps them: the file read, the packing the shader
// reads and the back to front order. No GL and no OpenXR in here, so the Mac can
// build and test exactly this code (tests/splat_data_test.c).
#ifndef XR_SPLAT_DATA_H
#define XR_SPLAT_DATA_H

#include <stddef.h>
#include <stdint.h>

// antimatter15 .splat: position and scale as 3 float32 each, then RGBA and the
// rotation (w x y z, as q * 128 + 128) as 4 uint8 each
#define SPLAT_RECORD_BYTES 32
// Two RGBA32UI texels a splat: centre and colour, then the covariance as halves
// with the one float that scales it back
#define SPLAT_TEXELS 2
#define SPLAT_TEX_WIDTH 4096
#define SPLAT_PER_ROW (SPLAT_TEX_WIDTH / SPLAT_TEXELS)
#define SPLAT_MAX 4000000
#define SPLAT_SORT_BUCKETS 65536

typedef struct SplatCloud {
    int count;
    // xyz a splat, what the sort reads, and how far each reaches (three sigma of its
    // widest axis), which is what its cost to draw is judged by
    float* centers;
    float* radii;
    // SPLAT_TEXELS * 4 words a splat, padded out to whole rows of the texture
    uint32_t* texels;
    int texHeight;
} SplatCloud;

SplatCloud* splatParse(const unsigned char* data, size_t length);
void splatFree(SplatCloud* cloud);
uint16_t splatHalf(float value);
// Splats nearer than this are always kept, whatever the cone: a big one close by
// reaches into view from beside you
#define SPLAT_CONE_NEAR_M 1.0f

// The splats inside a cone around where the head points, furthest first by distance
// from the eye. Distance and not depth, so small turns inside the cone need no new
// order. Behind you costs nothing. cosLimit -1 keeps every splat. picked and keys hold
// count words each, counts SPLAT_SORT_BUCKETS; returns how many went into order.
int splatSortByDistance(const float* centers, int count, const float eye[3],
                        const float forward[3], float cosLimit,
                        uint32_t* order, uint32_t* picked, uint32_t* keys, uint32_t* counts);

// What drawing the splats in order costs, as a running total: each one's share of the
// view (its reach over its distance, squared, capped at a screenful) plus a little for
// the vertices. cumulative[k] is the cost of order[0..k].
void splatOrderCost(const float* centers, const float* radii, const uint32_t* order, int count,
                    const float eye[3], float* cumulative);

// The six faces of a cube round the eye, in GL's order: +X -X +Y -Y +Z -Z
#define SPLAT_FACES 6
// A splat goes to every face any of it could show on: its centre inside the face's
// frustum widened by how far the splat reaches. Anything this near goes to all of them.
#define SPLAT_FACE_NEAR_M 1.0f

// The order split by cube face, each face's splats keeping their order, one after
// another in out, which holds capacity. offsets[f] to offsets[f + 1] is face f.
// Returns the total written.
int splatSplitFaces(const float* centers, const float* radii, const uint32_t* order, int count,
                    const float eye[3], uint32_t* out, int capacity, int offsets[SPLAT_FACES + 1]);

#endif
