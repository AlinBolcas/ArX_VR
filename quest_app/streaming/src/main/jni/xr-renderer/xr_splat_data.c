#include "xr_splat_data.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Round to nearest, subnormals kept, anything past the range clamped to the largest half
uint16_t splatHalf(float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    uint16_t sign = (uint16_t)((bits >> 16) & 0x8000u);
    float magnitude = fabsf(value);
    if (!(magnitude == magnitude)) return (uint16_t)(sign | 0x7e00u);
    if (magnitude >= 65504.0f) return (uint16_t)(sign | 0x7bffu);
    if (magnitude < 5.9604645e-8f * 0.5f) return sign;
    if (magnitude < 6.1035156e-5f) {
        // Subnormal: a whole number of 2^-24 steps
        return (uint16_t)(sign | (uint16_t)lrintf(magnitude * 16777216.0f));
    }
    memcpy(&bits, &magnitude, 4);
    uint32_t exponent = (bits >> 23) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    uint32_t half = (exponent << 10) | (mantissa >> 13);
    // Round half to even on the dropped bits; a carry rolls into the exponent, as it should
    uint32_t rest = mantissa & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) half++;
    return (uint16_t)(sign | half);
}

static float readFloat(const unsigned char* p) {
    float value;
    memcpy(&value, p, 4);
    return value;
}

// One record into its two texels: the centre as float bits and the colour, then the
// covariance R S S R^T divided by its largest entry so the halves keep their
// precision however small the splat is
static void packSplat(const unsigned char* record, float* center, float* radius, uint32_t* texel) {
    float s[3];
    for (int i = 0; i < 3; i++) {
        center[i] = readFloat(record + 4 * i);
        s[i] = readFloat(record + 12 + 4 * i);
    }
    *radius = 3.0f * fmaxf(fabsf(s[0]), fmaxf(fabsf(s[1]), fabsf(s[2])));
    const unsigned char* rgba = record + 24;
    const unsigned char* r = record + 28;
    float w = (r[0] - 128) / 128.0f, x = (r[1] - 128) / 128.0f;
    float y = (r[2] - 128) / 128.0f, z = (r[3] - 128) / 128.0f;
    float length = sqrtf(w * w + x * x + y * y + z * z);
    if (length < 1e-6f) { w = 1.0f; x = y = z = 0.0f; length = 1.0f; }
    w /= length; x /= length; y /= length; z /= length;

    float rot[3][3] = {
        { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y) },
        { 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x) },
        { 2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) },
    };
    float m[3][3];
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) m[i][k] = rot[i][k] * s[k];
    }
    // xx xy xz yy yz zz
    float cov[6];
    const int rows[6] = { 0, 0, 0, 1, 1, 2 };
    const int cols[6] = { 0, 1, 2, 1, 2, 2 };
    float largest = 0.0f;
    for (int e = 0; e < 6; e++) {
        float sum = 0.0f;
        for (int k = 0; k < 3; k++) sum += m[rows[e]][k] * m[cols[e]][k];
        cov[e] = sum;
        if (fabsf(sum) > largest) largest = fabsf(sum);
    }
    if (largest < 1e-30f) largest = 1e-30f;

    memcpy(&texel[0], &center[0], 4);
    memcpy(&texel[1], &center[1], 4);
    memcpy(&texel[2], &center[2], 4);
    texel[3] = (uint32_t)rgba[0] | ((uint32_t)rgba[1] << 8) | ((uint32_t)rgba[2] << 16) | ((uint32_t)rgba[3] << 24);
    for (int pair = 0; pair < 3; pair++) {
        texel[4 + pair] = (uint32_t)splatHalf(cov[2 * pair] / largest)
                        | ((uint32_t)splatHalf(cov[2 * pair + 1] / largest) << 16);
    }
    memcpy(&texel[7], &largest, 4);
}

SplatCloud* splatParse(const unsigned char* data, size_t length) {
    if (data == NULL || length < SPLAT_RECORD_BYTES || length % SPLAT_RECORD_BYTES != 0) return NULL;
    size_t count = length / SPLAT_RECORD_BYTES;
    if (count > SPLAT_MAX) count = SPLAT_MAX;
    SplatCloud* cloud = calloc(1, sizeof(SplatCloud));
    if (cloud == NULL) return NULL;
    cloud->count = (int)count;
    cloud->texHeight = (int)((count + SPLAT_PER_ROW - 1) / SPLAT_PER_ROW);
    cloud->centers = malloc(count * 3 * sizeof(float));
    cloud->radii = malloc(count * sizeof(float));
    cloud->texels = calloc((size_t)cloud->texHeight * SPLAT_TEX_WIDTH * 4, sizeof(uint32_t));
    if (cloud->centers == NULL || cloud->radii == NULL || cloud->texels == NULL) {
        splatFree(cloud);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        packSplat(data + i * SPLAT_RECORD_BYTES, cloud->centers + 3 * i, cloud->radii + i, cloud->texels + 8 * i);
    }
    return cloud;
}

void splatFree(SplatCloud* cloud) {
    if (cloud == NULL) return;
    free(cloud->centers);
    free(cloud->radii);
    free(cloud->texels);
    free(cloud);
}

int splatSortByDistance(const float* centers, int count, const float eye[3],
                        const float forward[3], float cosLimit,
                        uint32_t* order, uint32_t* picked, uint32_t* keys, uint32_t* counts) {
    float nearest = FLT_MAX;
    float furthest = 0.0f;
    int kept = 0;
    for (int i = 0; i < count; i++) {
        float dx = centers[3 * i] - eye[0];
        float dy = centers[3 * i + 1] - eye[1];
        float dz = centers[3 * i + 2] - eye[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        // Inside the cone when the direction to it is within the limit of forward
        float along = dx * forward[0] + dy * forward[1] + dz * forward[2];
        if (d > SPLAT_CONE_NEAR_M && along < cosLimit * d) continue;
        picked[kept] = (uint32_t)i;
        memcpy(&keys[kept], &d, 4);
        kept++;
        if (d < nearest) nearest = d;
        if (d > furthest) furthest = d;
    }
    // Sixteen bits spread over this frame's own range: under a millimetre a step
    // for a world fifty metres deep
    float scale = furthest > nearest ? (SPLAT_SORT_BUCKETS - 1) / (furthest - nearest) : 0.0f;
    memset(counts, 0, SPLAT_SORT_BUCKETS * sizeof(uint32_t));
    for (int k = 0; k < kept; k++) {
        float d;
        memcpy(&d, &keys[k], 4);
        uint32_t key = (SPLAT_SORT_BUCKETS - 1) - (uint32_t)((d - nearest) * scale);
        keys[k] = key;
        counts[key]++;
    }
    uint32_t start = 0;
    for (int b = 0; b < SPLAT_SORT_BUCKETS; b++) {
        uint32_t n = counts[b];
        counts[b] = start;
        start += n;
    }
    for (int k = 0; k < kept; k++) order[counts[keys[k]]++] = picked[k];
    return kept;
}

void splatOrderCost(const float* centers, const float* radii, const uint32_t* order, int count,
                    const float eye[3], float* cumulative) {
    float total = 0.0f;
    for (int k = 0; k < count; k++) {
        uint32_t i = order[k];
        float dx = centers[3 * i] - eye[0];
        float dy = centers[3 * i + 1] - eye[1];
        float dz = centers[3 * i + 2] - eye[2];
        float d2 = dx * dx + dy * dy + dz * dz + 1e-4f;
        float share = radii[i] * radii[i] / d2;
        total += (share < 0.25f ? share : 0.25f) + 2e-5f;
        cumulative[k] = total;
    }
}

int splatSplitFaces(const float* centers, const float* radii, const uint32_t* order, int count,
                    const float eye[3], uint32_t* out, int capacity, int offsets[SPLAT_FACES + 1]) {
    // Each splat's faces as six bits, counted per face on the way; then the faces laid
    // out one after another, each in the order's order
    unsigned char* mask = malloc(count > 0 ? (size_t)count : 1);
    if (mask == NULL) { memset(offsets, 0, sizeof(int) * (SPLAT_FACES + 1)); return 0; }
    int sizes[SPLAT_FACES] = { 0 };
    for (int k = 0; k < count; k++) {
        uint32_t i = order[k];
        float v[3] = { centers[3 * i] - eye[0], centers[3 * i + 1] - eye[1], centers[3 * i + 2] - eye[2] };
        float reach = 1.5f * radii[i];
        int near = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < SPLAT_FACE_NEAR_M * SPLAT_FACE_NEAR_M;
        unsigned char bits = 0;
        for (int face = 0; face < SPLAT_FACES; face++) {
            int axis = face / 2;
            float major = (face & 1) ? -v[axis] : v[axis];
            float a = fabsf(v[(axis + 1) % 3]), b = fabsf(v[(axis + 2) % 3]);
            // The face's side planes lean at 45 degrees, so a sphere of this reach
            // touches the frustum while it is within reach times root two of them
            if (near || (major > -reach && a <= 1.02f * major + reach && b <= 1.02f * major + reach)) {
                bits |= (unsigned char)(1u << face);
                sizes[face]++;
            }
        }
        mask[k] = bits;
    }
    int written = 0;
    for (int face = 0; face < SPLAT_FACES; face++) {
        offsets[face] = written;
        written += sizes[face];
    }
    offsets[SPLAT_FACES] = written;
    // Past capacity the last faces are cut short rather than overrun
    if (written > capacity) {
        for (int face = 0; face <= SPLAT_FACES; face++) if (offsets[face] > capacity) offsets[face] = capacity;
        written = capacity;
    }
    int fill[SPLAT_FACES];
    for (int face = 0; face < SPLAT_FACES; face++) fill[face] = offsets[face];
    for (int k = 0; k < count; k++) {
        for (int face = 0; face < SPLAT_FACES; face++) {
            if ((mask[k] & (1u << face)) && fill[face] < offsets[face + 1]) out[fill[face]++] = order[k];
        }
    }
    free(mask);
    return written;
}
