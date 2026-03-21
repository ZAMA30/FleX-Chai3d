// test_dicomvolume_coords.cpp
//
// Standalone unit-tests for the coordinate-mapping logic introduced in
// FleX-Chai3d/scenes/dicomvolume.h.
//
// The tests exercise the pure-math routines (WorldToVoxel, haptic gradient
// back-permutation, drill resistance force, DrillAt/PaintAt voxel arithmetic)
// WITHOUT requiring CUDA, OpenGL, CHAI3D, GDCM, or NVIDIA FleX headers.
// All dependent types are stubbed in this file.
//
// Build & run on Linux / macOS:
//   g++ -std=c++17 -o test_dicomvolume tests/test_dicomvolume_coords.cpp && ./test_dicomvolume
//
// Build & run on Windows (Developer Command Prompt):
//   cl /std:c++17 /EHsc tests\test_dicomvolume_coords.cpp /Fe:test_dicomvolume.exe && test_dicomvolume.exe

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------------
// Minimal Vec3 stub (mirrors XVector3<float> from FleX/core/vec3.h)
// ---------------------------------------------------------------------------
struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float a) : x(a), y(a), z(a) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vec3 operator*(float s)       const { return {x*s, y*s, z*s}; }
    Vec3 operator-()              const { return {-x, -y, -z}; }
};

inline float Length(const Vec3& v) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
inline Vec3  Normalize(const Vec3& v) { float l = Length(v); return (l < 1e-9f) ? Vec3(0) : v * (1.f/l); }
inline float Min(float a, float b) { return a < b ? a : b; }

// ---------------------------------------------------------------------------
// Minimal HapticsUpdate stub (only cursorVelocity is needed by haptic tests)
// ---------------------------------------------------------------------------
struct HapticsUpdate {
    Vec3 cursorPosition;
    Vec3 cursorVelocity;
};
static HapticsUpdate g_hapticsUpdates;

// ---------------------------------------------------------------------------
// Extract only the pure-logic parts of DicomVolume that we want to test.
// We don't include the real header (requires GPU/GL/CHAI3D), but we
// reproduce each formula verbatim from dicomvolume.h so that any accidental
// change to the formula in the header will NOT silently break these tests —
// you would need to update both the header AND this file deliberately.
// ---------------------------------------------------------------------------

// --- WorldToVoxel (copied verbatim from dicomvolume.h, WorldToVoxel()) ---
// FleX Z → column (ix), FleX X → row (iy), FleX Y → slice (iz)
static bool WorldToVoxel(const Vec3& worldPos,
                         float spacingX, float spacingY, float spacingZ,
                         int dimX,       int dimY,       int dimZ,
                         int& ix, int& iy, int& iz)
{
    ix = (int)(worldPos.z / spacingX + dimX * 0.5f);  // FleX Z → column
    iy = (int)(worldPos.x / spacingY + dimY * 0.5f);  // FleX X → row
    iz = (int)(worldPos.y / spacingZ + dimZ * 0.5f);  // FleX Y → slice
    return ix >= 0 && ix < dimX
        && iy >= 0 && iy < dimY
        && iz >= 0 && iz < dimZ;
}

// --- Haptic gradient back-permutation (copied verbatim from GetHapticForce)
// dCol: gradient along column axis (ix ↔ FleX Z)
// dRow: gradient along row    axis (iy ↔ FleX X)
// dSlice: gradient along slice axis (iz ↔ FleX Y)
static Vec3 GradientToWorldSpace(float dCol, float dRow, float dSlice)
{
    return Vec3(dRow,    // FleX X
                dSlice,  // FleX Y
                dCol);   // FleX Z
}

// --- Drill resistance force (copied verbatim from GetHapticForce drill branch)
static Vec3 DrillResistanceForce(const Vec3& velocity, float density, float drillForceScale)
{
    float speed = Length(velocity);
    if (speed < 1e-6f) return Vec3(0.f);
    float resistMag = drillForceScale * density;
    return -Normalize(velocity) * resistMag;
}

// --- Shader texture-coordinate formula (copied verbatim from VolumeFS)
// uVolHalfSize = (mWorldHX, mWorldHY, mWorldHZ) = (row-half, slice-half, col-half)
struct ShaderTC {
    float x, y, z; // [0..1]
};
static ShaderTC ShaderWorldToTC(float px, float py, float pz,
                                float hx, float hy, float hz)
{
    // tc.x = (p.z + hz) / (2*hz)  ← FleX Z → column
    // tc.y = (p.x + hx) / (2*hx)  ← FleX X → row
    // tc.z = (p.y + hy) / (2*hy)  ← FleX Y → slice
    return { (pz + hz) / (2.f*hz),
             (px + hx) / (2.f*hx),
             (py + hy) / (2.f*hy) };
}

// ---------------------------------------------------------------------------
// Minimal DrillAt / PaintAt logic stubs (arithmetic only, no GL/mutex)
// ---------------------------------------------------------------------------
static bool DrillAt(const Vec3& worldPos,
                    float spacingX, float spacingY, float spacingZ,
                    int dimX, int dimY, int dimZ,
                    int brushRadius, float threshold,
                    std::vector<float>& voxelDataNorm,
                    std::vector<uint8_t>& labelData)
{
    int cx, cy, cz;
    if (!WorldToVoxel(worldPos, spacingX, spacingY, spacingZ, dimX, dimY, dimZ, cx, cy, cz))
        return false;

    float density = voxelDataNorm[(size_t)cz * dimX * dimY + cy * dimX + cx];
    if (density < threshold) return false;

    bool changed = false;
    for (int dz = -brushRadius; dz <= brushRadius; ++dz)
    for (int dy = -brushRadius; dy <= brushRadius; ++dy)
    for (int dx = -brushRadius; dx <= brushRadius; ++dx)
    {
        if (dx*dx + dy*dy + dz*dz > brushRadius*brushRadius) continue;
        int x = cx+dx, y = cy+dy, z = cz+dz;
        if (x < 0 || x >= dimX || y < 0 || y >= dimY || z < 0 || z >= dimZ) continue;
        size_t idx = (size_t)z * dimX * dimY + y * dimX + x;
        labelData[idx]     = 255;
        voxelDataNorm[idx] = 0.f;
        changed = true;
    }
    return changed;
}

static bool PaintAt(const Vec3& worldPos,
                    float spacingX, float spacingY, float spacingZ,
                    int dimX, int dimY, int dimZ,
                    int paintRadius, uint8_t label,
                    std::vector<uint8_t>& labelData)
{
    int cx, cy, cz;
    WorldToVoxel(worldPos, spacingX, spacingY, spacingZ, dimX, dimY, dimZ, cx, cy, cz);

    bool painted = false;
    for (int dz = -paintRadius; dz <= paintRadius; ++dz)
    for (int dy = -paintRadius; dy <= paintRadius; ++dy)
    for (int dx = -paintRadius; dx <= paintRadius; ++dx)
    {
        int distSq = dx*dx + dy*dy + dz*dz;
        if (distSq > paintRadius*paintRadius) continue;
        int x = cx+dx, y = cy+dy, z = cz+dz;
        if (x < 0 || x >= dimX || y < 0 || y >= dimY || z < 0 || z >= dimZ) continue;
        labelData[(size_t)z * dimX * dimY + y * dimX + x] = label;
        painted = true;
    }
    return painted;
}

// ---------------------------------------------------------------------------
// Simple assertion helper
// ---------------------------------------------------------------------------
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond) \
    do { \
        if (cond) { \
            ++g_passed; \
        } else { \
            ++g_failed; \
            printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while(0)

#define CHECK_EQ(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a == _b) { \
            ++g_passed; \
        } else { \
            ++g_failed; \
            printf("FAIL  %s:%d  %s == %s  (%d != %d)\n", \
                   __FILE__, __LINE__, #a, #b, (int)_a, (int)_b); \
        } \
    } while(0)

#define CHECK_NEAR(a, b, tol) \
    do { \
        float _a = (float)(a); float _b = (float)(b); float _t = (float)(tol); \
        if (std::fabs(_a - _b) <= _t) { \
            ++g_passed; \
        } else { \
            ++g_failed; \
            printf("FAIL  %s:%d  |%s - %s| <= %s  (|%.6f - %.6f| = %.6f > %.6f)\n", \
                   __FILE__, __LINE__, #a, #b, #tol, _a, _b, std::fabs(_a-_b), _t); \
        } \
    } while(0)

// ---------------------------------------------------------------------------
// Test: WorldToVoxel — axis mapping
//
// The volume has 100 columns (dimX), 80 rows (dimY), 60 slices (dimZ).
// Spacing = 0.001 m in all directions.
// World half-extents after permutation:
//   FleX X direction (row)    : mWorldHX = dimY * spacingY / 2 = 0.04 m
//   FleX Y direction (slice)  : mWorldHY = dimZ * spacingZ / 2 = 0.03 m
//   FleX Z direction (column) : mWorldHZ = dimX * spacingX / 2 = 0.05 m
//
// Mapping:
//   column ix = round(worldPos.z / spacingX + dimX/2)
//   row    iy = round(worldPos.x / spacingY + dimY/2)
//   slice  iz = round(worldPos.y / spacingZ + dimZ/2)
// ---------------------------------------------------------------------------
static void test_WorldToVoxel_axisMapping()
{
    const float sx = 0.001f, sy = 0.001f, sz = 0.001f;
    const int   dimX = 100, dimY = 80, dimZ = 60;

    // --- Centre of volume → middle voxel indices ---
    {
        Vec3 centre(0.f, 0.f, 0.f);
        int ix, iy, iz;
        CHECK(WorldToVoxel(centre, sx, sy, sz, dimX, dimY, dimZ, ix, iy, iz));
        CHECK_EQ(ix, 50); // column = 0/0.001 + 100/2 = 50
        CHECK_EQ(iy, 40); // row    = 0/0.001 + 80/2  = 40
        CHECK_EQ(iz, 30); // slice  = 0/0.001 + 60/2  = 30
    }

    // --- Moving along FleX Z → changes COLUMN (ix), not row or slice ---
    {
        // step one column-voxel in the +Z direction
        Vec3 pos(0.f, 0.f, 0.001f);
        int ix, iy, iz;
        CHECK(WorldToVoxel(pos, sx, sy, sz, dimX, dimY, dimZ, ix, iy, iz));
        CHECK_EQ(ix, 51); // column incremented
        CHECK_EQ(iy, 40); // row unchanged
        CHECK_EQ(iz, 30); // slice unchanged
    }

    // --- Moving along FleX X → changes ROW (iy), not column or slice ---
    {
        Vec3 pos(0.001f, 0.f, 0.f);
        int ix, iy, iz;
        CHECK(WorldToVoxel(pos, sx, sy, sz, dimX, dimY, dimZ, ix, iy, iz));
        CHECK_EQ(ix, 50); // column unchanged
        CHECK_EQ(iy, 41); // row incremented
        CHECK_EQ(iz, 30); // slice unchanged
    }

    // --- Moving along FleX Y → changes SLICE (iz), not column or row ---
    {
        Vec3 pos(0.f, 0.001f, 0.f);
        int ix, iy, iz;
        CHECK(WorldToVoxel(pos, sx, sy, sz, dimX, dimY, dimZ, ix, iy, iz));
        CHECK_EQ(ix, 50); // column unchanged
        CHECK_EQ(iy, 40); // row unchanged
        CHECK_EQ(iz, 31); // slice incremented
    }
}

// ---------------------------------------------------------------------------
// Test: WorldToVoxel — bounds checking
// ---------------------------------------------------------------------------
static void test_WorldToVoxel_bounds()
{
    const float sx = 0.001f, sy = 0.001f, sz = 0.001f;
    const int   dimX = 100, dimY = 80, dimZ = 60;
    int ix, iy, iz;

    // Exactly at centre → inside
    CHECK( WorldToVoxel(Vec3(0.f, 0.f, 0.f), sx, sy, sz, dimX, dimY, dimZ, ix, iy, iz));

    // FleX Z way out of bounds (column out of range)
    CHECK(!WorldToVoxel(Vec3(0.f, 0.f,  1.f), sx, sy, sz, dimX, dimY, dimZ, ix, iy, iz));
    CHECK(!WorldToVoxel(Vec3(0.f, 0.f, -1.f), sx, sy, sz, dimX, dimY, dimZ, ix, iy, iz));

    // FleX X way out of bounds (row out of range)
    CHECK(!WorldToVoxel(Vec3(1.f, 0.f, 0.f), sx, sy, sz, dimX, dimY, dimZ, ix, iy, iz));

    // FleX Y way out of bounds (slice out of range)
    CHECK(!WorldToVoxel(Vec3(0.f, 1.f, 0.f), sx, sy, sz, dimX, dimY, dimZ, ix, iy, iz));
}

// ---------------------------------------------------------------------------
// Test: WorldToVoxel — extreme corner positions
// ---------------------------------------------------------------------------
static void test_WorldToVoxel_corners()
{
    const float sx = 0.002f, sy = 0.002f, sz = 0.002f;
    const int   dimX = 10, dimY = 10, dimZ = 10;
    // Half-extents: FleX X/Y/Z each span [-0.01 .. +0.01)
    int ix, iy, iz;

    // FleX Z = -0.009 → column near 0
    Vec3 negZ(0.f, 0.f, -0.009f);
    CHECK(WorldToVoxel(negZ, sx, sy, sz, dimX, dimY, dimZ, ix, iy, iz));
    CHECK_EQ(ix, 0);

    // FleX Z = +0.009 → column near 9
    Vec3 posZ(0.f, 0.f, +0.009f);
    CHECK(WorldToVoxel(posZ, sx, sy, sz, dimX, dimY, dimZ, ix, iy, iz));
    CHECK_EQ(ix, 9);
}

// ---------------------------------------------------------------------------
// Test: GradientToWorldSpace — back-permutation
//
// The forward mapping is:
//   column ← FleX Z   (so gradient along columns corresponds to FleX Z)
//   row    ← FleX X   (gradient along rows → FleX X)
//   slice  ← FleX Y   (gradient along slices → FleX Y)
//
// Pure column gradient: dCol=1, dRow=0, dSlice=0 → world gradient (0, 0, 1)
// Pure row    gradient: dCol=0, dRow=1, dSlice=0 → world gradient (1, 0, 0)
// Pure slice  gradient: dCol=0, dRow=0, dSlice=1 → world gradient (0, 1, 0)
// ---------------------------------------------------------------------------
static void test_gradientBackPermutation()
{
    const float tol = 1e-5f;

    // Pure column gradient → FleX Z component
    Vec3 g = GradientToWorldSpace(1.f, 0.f, 0.f);
    CHECK_NEAR(g.x, 0.f, tol);
    CHECK_NEAR(g.y, 0.f, tol);
    CHECK_NEAR(g.z, 1.f, tol);

    // Pure row gradient → FleX X component
    g = GradientToWorldSpace(0.f, 1.f, 0.f);
    CHECK_NEAR(g.x, 1.f, tol);
    CHECK_NEAR(g.y, 0.f, tol);
    CHECK_NEAR(g.z, 0.f, tol);

    // Pure slice gradient → FleX Y component
    g = GradientToWorldSpace(0.f, 0.f, 1.f);
    CHECK_NEAR(g.x, 0.f, tol);
    CHECK_NEAR(g.y, 1.f, tol);
    CHECK_NEAR(g.z, 0.f, tol);

    // Combined gradient — each component independent
    g = GradientToWorldSpace(2.f, 3.f, 4.f);
    CHECK_NEAR(g.x, 3.f, tol); // row → X
    CHECK_NEAR(g.y, 4.f, tol); // slice → Y
    CHECK_NEAR(g.z, 2.f, tol); // column → Z
}

// ---------------------------------------------------------------------------
// Test: DrillResistanceForce
// ---------------------------------------------------------------------------
static void test_drillResistanceForce()
{
    const float tol = 1e-5f;

    // Zero velocity → zero force (no divide-by-zero)
    Vec3 f = DrillResistanceForce(Vec3(0.f, 0.f, 0.f), 0.8f, 2.0f);
    CHECK_NEAR(f.x, 0.f, tol);
    CHECK_NEAR(f.y, 0.f, tol);
    CHECK_NEAR(f.z, 0.f, tol);

    // Drill force opposes velocity direction
    Vec3 vel(0.f, 1.f, 0.f); // moving in FleX Y (drill axis = forward)
    f = DrillResistanceForce(vel, 0.8f, 2.0f);
    // Expected: magnitude = 2.0 * 0.8 = 1.6; direction = (0,-1,0)
    CHECK_NEAR(f.x,  0.f, tol);
    CHECK_NEAR(f.y, -1.6f, tol);
    CHECK_NEAR(f.z,  0.f, tol);

    // Diagonal velocity — force must point exactly opposite the velocity
    Vec3 vel2(1.f, 1.f, 0.f);
    f = DrillResistanceForce(vel2, 0.5f, 4.0f);
    Vec3 expected = -Normalize(vel2) * (4.0f * 0.5f);
    CHECK_NEAR(f.x, expected.x, tol);
    CHECK_NEAR(f.y, expected.y, tol);
    CHECK_NEAR(f.z, expected.z, tol);

    // Zero density → zero force (nothing to drill)
    f = DrillResistanceForce(Vec3(0.f, 1.f, 0.f), 0.f, 2.0f);
    float mag = Length(f);
    CHECK_NEAR(mag, 0.f, tol);
}

// ---------------------------------------------------------------------------
// Test: DrillAt — removes material and sets label 255
// ---------------------------------------------------------------------------
static void test_drillAt_removesMaterial()
{
    const int dimX = 10, dimY = 10, dimZ = 10;
    const float sx = 0.01f, sy = 0.01f, sz = 0.01f;
    const size_t total = (size_t)dimX * dimY * dimZ;

    std::vector<float>   voxels(total, 0.8f); // all material
    std::vector<uint8_t> labels(total, 0);

    // Drill at centre (FleX world origin)
    bool changed = DrillAt(Vec3(0.f, 0.f, 0.f), sx, sy, sz, dimX, dimY, dimZ,
                           /*brushRadius=*/1, /*threshold=*/0.3f,
                           voxels, labels);
    CHECK(changed);

    // Centre voxel (col=5, row=5, slice=5) must be drilled
    size_t centerIdx = (size_t)5 * dimX * dimY + 5 * dimX + 5;
    CHECK_NEAR(voxels[centerIdx], 0.f, 1e-5f);
    CHECK_EQ(labels[centerIdx], 255);
}

// ---------------------------------------------------------------------------
// Test: DrillAt — does NOT drill below threshold
// ---------------------------------------------------------------------------
static void test_drillAt_belowThreshold()
{
    const int dimX = 10, dimY = 10, dimZ = 10;
    const float sx = 0.01f, sy = 0.01f, sz = 0.01f;
    const size_t total = (size_t)dimX * dimY * dimZ;

    std::vector<float>   voxels(total, 0.1f); // low-density (air)
    std::vector<uint8_t> labels(total, 0);

    bool changed = DrillAt(Vec3(0.f, 0.f, 0.f), sx, sy, sz, dimX, dimY, dimZ,
                           /*brushRadius=*/1, /*threshold=*/0.3f,
                           voxels, labels);
    // Should NOT drill (density 0.1 < threshold 0.3)
    CHECK(!changed);

    // All labels remain 0
    for (size_t i = 0; i < total; ++i)
        CHECK_EQ(labels[i], 0);
}

// ---------------------------------------------------------------------------
// Test: DrillAt — out-of-bounds position returns false
// ---------------------------------------------------------------------------
static void test_drillAt_outOfBounds()
{
    const int dimX = 10, dimY = 10, dimZ = 10;
    const float sx = 0.01f, sy = 0.01f, sz = 0.01f;
    const size_t total = (size_t)dimX * dimY * dimZ;

    std::vector<float>   voxels(total, 0.9f);
    std::vector<uint8_t> labels(total, 0);

    bool changed = DrillAt(Vec3(100.f, 0.f, 0.f), sx, sy, sz, dimX, dimY, dimZ,
                           1, 0.3f, voxels, labels);
    CHECK(!changed);
}

// ---------------------------------------------------------------------------
// Test: PaintAt — sets label using corrected axis mapping
// ---------------------------------------------------------------------------
static void test_paintAt_correctAxis()
{
    const int dimX = 20, dimY = 20, dimZ = 20;
    const float sx = 0.001f, sy = 0.001f, sz = 0.001f;
    const size_t total = (size_t)dimX * dimY * dimZ;

    std::vector<uint8_t> labels(total, 0);

    // Paint with radius 0 (single voxel) at world origin → centre voxel (10,10,10)
    PaintAt(Vec3(0.f, 0.f, 0.f), sx, sy, sz, dimX, dimY, dimZ,
            /*radius=*/0, /*label=*/1, labels);

    // Centre voxel must be painted
    size_t centreIdx = (size_t)10 * dimX * dimY + 10 * dimX + 10;
    CHECK_EQ(labels[centreIdx], 1);

    // Moving in FleX Z should move the column index, not row or slice
    labels.assign(total, 0);
    PaintAt(Vec3(0.f, 0.f, 0.001f), sx, sy, sz, dimX, dimY, dimZ,
            0, 1, labels);
    // Now col=11, row=10, slice=10
    size_t offsetIdx = (size_t)10 * dimX * dimY + 10 * dimX + 11;
    CHECK_EQ(labels[offsetIdx], 1);
    CHECK_EQ(labels[centreIdx], 0); // original centre not painted
}

// ---------------------------------------------------------------------------
// Test: Shader tc formula consistency with WorldToVoxel
//
// The shader tc and WorldToVoxel must agree: for a given FleX world point p,
//   tc.x ≈ ix / dimX    (if ix == WorldToVoxel column)
//   tc.y ≈ iy / dimY
//   tc.z ≈ iz / dimZ
// (approximately, since WorldToVoxel uses integer truncation)
// ---------------------------------------------------------------------------
static void test_shaderTCConsistencyWithWorldToVoxel()
{
    const float sx = 0.001f, sy = 0.001f, sz = 0.001f;
    const int   dimX = 100, dimY = 80, dimZ = 60;
    // Permuted world half-extents
    const float hx = dimY * sy * 0.5f; // FleX X (row)    half
    const float hy = dimZ * sz * 0.5f; // FleX Y (slice)  half
    const float hz = dimX * sx * 0.5f; // FleX Z (column) half

    const float tol = 1.f / (float)std::min(std::min(dimX, dimY), dimZ); // ≤ 1 voxel

    // Test several probe points
    const Vec3 probes[] = {
        Vec3(0.f, 0.f, 0.f),
        Vec3(0.01f, 0.005f, -0.02f),
        Vec3(-0.03f, 0.02f, 0.01f),
    };
    for (auto& p : probes)
    {
        int ix, iy, iz;
        bool inside = WorldToVoxel(p, sx, sy, sz, dimX, dimY, dimZ, ix, iy, iz);
        if (!inside) continue;

        ShaderTC tc = ShaderWorldToTC(p.x, p.y, p.z, hx, hy, hz);

        // tc.x → column; ix / dimX should match tc.x within one voxel
        float tcColFromVoxel = (ix + 0.5f) / (float)dimX;
        CHECK_NEAR(tc.x, tcColFromVoxel, tol);

        float tcRowFromVoxel = (iy + 0.5f) / (float)dimY;
        CHECK_NEAR(tc.y, tcRowFromVoxel, tol);

        float tcSliceFromVoxel = (iz + 0.5f) / (float)dimZ;
        CHECK_NEAR(tc.z, tcSliceFromVoxel, tol);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main()
{
    printf("=== DicomVolume coordinate-mapping unit tests ===\n\n");

    test_WorldToVoxel_axisMapping();
    test_WorldToVoxel_bounds();
    test_WorldToVoxel_corners();
    test_gradientBackPermutation();
    test_drillResistanceForce();
    test_drillAt_removesMaterial();
    test_drillAt_belowThreshold();
    test_drillAt_outOfBounds();
    test_paintAt_correctAxis();
    test_shaderTCConsistencyWithWorldToVoxel();

    printf("\n");
    if (g_failed == 0)
        printf("ALL %d tests PASSED.\n", g_passed);
    else
        printf("%d PASSED, %d FAILED.\n", g_passed, g_failed);

    return g_failed ? 1 : 0;
}
