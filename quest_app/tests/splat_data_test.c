// The headset's splat core (xr_splat_data.c) built and checked on the Mac.
//
//   splat_data_test               run the checks
//   splat_data_test pack IN OUT   write IN's packed texels to OUT, for the WebGL test page
//
// Built by tests/arxvr_world_test.py with the system clang.
#include "../streaming/src/main/jni/xr-renderer/xr_splat_data.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static float halfToFloat(uint16_t h) {
    int exponent = (h >> 10) & 31;
    int mantissa = h & 1023;
    float value = exponent == 0 ? ldexpf((float)mantissa, -24) : ldexpf((float)(mantissa | 1024), exponent - 25);
    return (h & 0x8000) ? -value : value;
}

static void writeRecord(unsigned char* r, const float p[3], const float s[3], const unsigned char rgba[4], const float q[4]) {
    memcpy(r, p, 12);
    memcpy(r + 12, s, 12);
    memcpy(r + 24, rgba, 4);
    for (int i = 0; i < 4; i++) {
        float v = q[i] * 128.0f + 128.0f;
        r[28 + i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : lrintf(v));
    }
}

static void checkHalves(void) {
    CHECK(splatHalf(1.0f) == 0x3c00, "half 1.0 = %04x", splatHalf(1.0f));
    CHECK(splatHalf(0.5f) == 0x3800, "half 0.5");
    CHECK(splatHalf(-2.0f) == 0xc000, "half -2");
    CHECK(splatHalf(65504.0f) == 0x7bff, "half max");
    CHECK(splatHalf(1e9f) == 0x7bff, "half clamps");
    CHECK(splatHalf(0.0f) == 0, "half 0");
    CHECK(splatHalf(1e-7f) == 2, "half subnormal = %d", splatHalf(1e-7f));
    for (float v = -3.0f; v < 3.0f; v += 0.0137f) {
        float back = halfToFloat(splatHalf(v));
        CHECK(fabsf(back - v) <= fabsf(v) / 1024.0f + 1e-7f, "half round trip %f -> %f", v, back);
    }
}

// The covariance, worked out the long way: each axis of the splat turned by the quaternion
static void expectedCovariance(const unsigned char* record, float out[6]) {
    float s[3];
    memcpy(s, record + 12, 12);
    float q[4];
    for (int i = 0; i < 4; i++) q[i] = (record[28 + i] - 128) / 128.0f;
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= n;
    float axes[3][3];
    for (int a = 0; a < 3; a++) {
        float v[3] = { a == 0, a == 1, a == 2 };
        // v' = v + 2w(u x v) + 2u x (u x v), u = (x, y, z)
        float u[3] = { q[1], q[2], q[3] };
        float c[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        float cc[3] = { u[1] * c[2] - u[2] * c[1], u[2] * c[0] - u[0] * c[2], u[0] * c[1] - u[1] * c[0] };
        for (int i = 0; i < 3; i++) axes[a][i] = (v[i] + 2 * q[0] * c[i] + 2 * cc[i]) * s[a];
    }
    const int rows[6] = { 0, 0, 0, 1, 1, 2 };
    const int cols[6] = { 0, 1, 2, 1, 2, 2 };
    for (int e = 0; e < 6; e++) {
        out[e] = 0;
        for (int a = 0; a < 3; a++) out[e] += axes[a][rows[e]] * axes[a][cols[e]];
    }
}

static void checkPacking(void) {
    const int count = 5000;
    unsigned char* data = malloc((size_t)count * SPLAT_RECORD_BYTES);
    srand(3);
    for (int i = 0; i < count; i++) {
        float p[3] = { (rand() % 2000 - 1000) / 100.0f, (rand() % 400 - 200) / 100.0f, (rand() % 2000 - 1000) / 100.0f };
        // From a millimetre to half a metre, stretched unevenly, the cases halves lose first
        float s[3];
        for (int k = 0; k < 3; k++) s[k] = 0.001f * powf(500.0f, (rand() % 1000) / 1000.0f);
        float q[4] = { (rand() % 2000 - 1000) / 1000.0f, (rand() % 2000 - 1000) / 1000.0f,
                       (rand() % 2000 - 1000) / 1000.0f, (rand() % 2000 - 1000) / 1000.0f };
        float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) + 1e-6f;
        for (int k = 0; k < 4; k++) q[k] /= n;
        unsigned char rgba[4] = { (unsigned char)(i & 255), 17, 200, 255 };
        writeRecord(data + (size_t)i * SPLAT_RECORD_BYTES, p, s, rgba, q);
    }
    SplatCloud* cloud = splatParse(data, (size_t)count * SPLAT_RECORD_BYTES);
    CHECK(cloud != NULL && cloud->count == count, "parse");
    CHECK(cloud->texHeight == (count + SPLAT_PER_ROW - 1) / SPLAT_PER_ROW, "texture height");
    float worst = 0.0f;
    for (int i = 0; i < count; i++) {
        const unsigned char* r = data + (size_t)i * SPLAT_RECORD_BYTES;
        const uint32_t* t = cloud->texels + 8 * i;
        float c[3];
        memcpy(c, t, 12);
        CHECK(memcmp(c, r, 12) == 0 && memcmp(cloud->centers + 3 * i, r, 12) == 0, "centre %d", i);
        CHECK(t[3] == ((uint32_t)r[24] | ((uint32_t)r[25] << 8) | ((uint32_t)r[26] << 16) | ((uint32_t)r[27] << 24)), "colour %d", i);
        float sc[3];
        memcpy(sc, r + 12, 12);
        CHECK(fabsf(cloud->radii[i] - 3.0f * fmaxf(sc[0], fmaxf(sc[1], sc[2]))) < 1e-6f, "radius %d", i);
        float k;
        memcpy(&k, &t[7], 4);
        float got[6];
        for (int pair = 0; pair < 3; pair++) {
            got[2 * pair] = halfToFloat((uint16_t)(t[4 + pair] & 0xffff)) * k;
            got[2 * pair + 1] = halfToFloat((uint16_t)(t[4 + pair] >> 16)) * k;
        }
        float want[6];
        expectedCovariance(r, want);
        for (int e = 0; e < 6; e++) {
            // Relative to the splat's own largest entry, which is what the eye sees
            float err = fabsf(got[e] - want[e]) / k;
            if (err > worst) worst = err;
        }
    }
    CHECK(worst < 2e-3f, "covariance off by %g of its size", worst);
    printf("packing: %d splats, covariance within %.1e of its size\n", count, worst);
    splatFree(cloud);
    CHECK(splatParse(data, 31) == NULL, "a torn file is refused");
    free(data);
}

static void checkSort(int count, int timed, float cosLimit) {
    float* centers = malloc((size_t)count * 12);
    for (int i = 0; i < 3 * count; i++) centers[i] = (rand() % 100000 - 50000) / 1000.0f;
    uint32_t* order = malloc((size_t)count * 4);
    uint32_t* picked = malloc((size_t)count * 4);
    uint32_t* keys = malloc((size_t)count * 4);
    uint32_t* counts = malloc(SPLAT_SORT_BUCKETS * 4);
    unsigned char* seen = calloc((size_t)count, 1);
    float eye[3] = { 0.3f, 1.1f, -0.4f };
    float forward[3] = { 0.0f, 0.0f, -1.0f };
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int kept = splatSortByDistance(centers, count, eye, forward, cosLimit, order, picked, keys, counts);
    clock_gettime(CLOCK_MONOTONIC, &b);
    // Exactly the splats in the cone or within reach, each once
    int want = 0;
    for (int i = 0; i < count; i++) {
        float dx = centers[3 * i] - eye[0], dy = centers[3 * i + 1] - eye[1], dz = centers[3 * i + 2] - eye[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        want += d <= SPLAT_CONE_NEAR_M || -dz >= cosLimit * d;
    }
    CHECK(kept == want, "cone %.2f kept %d, want %d", cosLimit, kept, want);
    float step = 0.0f;
    float last = INFINITY;
    int bad = 0;
    for (int i = 0; i < kept; i++) {
        uint32_t k = order[i];
        if (k >= (uint32_t)count || seen[k]) { bad++; continue; }
        seen[k] = 1;
        float dx = centers[3 * k] - eye[0], dy = centers[3 * k + 1] - eye[1], dz = centers[3 * k + 2] - eye[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        // Within one bucket of each other is as good as the order gets
        if (d > last + 0.002f) { bad++; }
        if (d - last > step) step = d - last;
        last = d;
    }
    CHECK(bad == 0, "sort of %d: %d out of place", count, bad);
    if (timed) {
        printf("sort: %d splats, %d in view, %.1f ms on this Mac\n", count, kept,
               (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6);
    }
    // The running cost only ever grows, and the far end is the cheap end
    float* radii = malloc((size_t)count * 4);
    for (int i = 0; i < count; i++) radii[i] = 0.05f;
    float* cost = malloc((size_t)(kept > 0 ? kept : 1) * 4);
    splatOrderCost(centers, radii, order, kept, eye, cost);
    int rising = 1;
    for (int k = 1; k < kept; k++) rising &= cost[k] > cost[k - 1];
    CHECK(rising, "order cost never falls");
    if (kept > 10) {
        float first = cost[kept / 10], last = cost[kept - 1] - cost[kept - 1 - kept / 10];
        CHECK(last > first, "the near tenth costs more than the far tenth (%g vs %g)", last, first);
    }
    // Every splat lands on at least one face, and a far one straight ahead only on -Z
    uint32_t* faces = malloc((size_t)(kept > 0 ? kept : 1) * 3 * 4);
    int offsets[SPLAT_FACES + 1];
    int total = splatSplitFaces(centers, radii, order, kept, eye, faces, 3 * kept, offsets);
    CHECK(total >= kept, "face split total %d of %d", total, kept);
    unsigned char* covered = calloc((size_t)count, 1);
    for (int k = 0; k < total; k++) covered[faces[k]] = 1;
    int missing = 0;
    for (int k = 0; k < kept; k++) missing += !covered[order[k]];
    CHECK(missing == 0, "%d splats on no face", missing);
    float ahead[3] = { eye[0], eye[1], eye[2] - 20.0f };
    uint32_t one = 0;
    int single[SPLAT_FACES + 1];
    splatSplitFaces(ahead, radii, &one, 1, eye, faces, 3, single);
    CHECK(single[5] == 0 && single[6] == 1, "a splat straight ahead is on -Z only");
    free(faces); free(covered);
    free(radii); free(cost);
    free(centers); free(order); free(picked); free(keys); free(counts); free(seen);
}

static int pack(const char* in, const char* out) {
    FILE* f = fopen(in, "rb");
    if (!f) { printf("no file %s\n", in); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* data = malloc((size_t)size);
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    SplatCloud* cloud = splatParse(data, got);
    free(data);
    if (!cloud) { printf("did not parse\n"); return 1; }
    FILE* o = fopen(out, "wb");
    fwrite(cloud->texels, 4, (size_t)cloud->texHeight * SPLAT_TEX_WIDTH * 4, o);
    fclose(o);
    printf("%d splats, %dx%d\n", cloud->count, SPLAT_TEX_WIDTH, cloud->texHeight);
    splatFree(cloud);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && strcmp(argv[1], "pack") == 0) return pack(argv[2], argv[3]);
    checkHalves();
    checkPacking();
    checkSort(1000, 0, -1.0f);
    checkSort(20000, 0, 0.1736f);
    checkSort(500000, 1, -1.0f);
    checkSort(1800000, 1, 0.1736f);
    printf(failures ? "%d FAILED\n" : "all splat checks pass\n", failures);
    return failures != 0;
}
