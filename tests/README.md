# FleX-Chai3d Unit Tests

This folder contains **standalone, self-contained unit tests** for the
coordinate-mapping logic in the DICOM volume drilling scene.  
No GPU, OpenGL, CHAI3D, GDCM, or NVIDIA FleX installation is required.

## What is tested?

`test_dicomvolume_coords.cpp` exercises the core math introduced by the
coordinate-system fix and drill simulation (`dicomvolume.h`):

| Test | What it checks |
|---|---|
| `test_WorldToVoxel_axisMapping` | FleX Z → column, FleX X → row, FleX Y → slice |
| `test_WorldToVoxel_bounds` | in/out-of-bounds detection |
| `test_WorldToVoxel_corners` | correct voxel indices at volume edges |
| `test_gradientBackPermutation` | haptic gradient maps back to the right FleX world axes |
| `test_drillResistanceForce` | force opposes velocity; zero velocity → zero force |
| `test_drillAt_removesMaterial` | drill zeroes density and sets label 255 |
| `test_drillAt_belowThreshold` | no material removed when density < threshold |
| `test_drillAt_outOfBounds` | position outside volume returns false safely |
| `test_paintAt_correctAxis` | paint uses corrected axis mapping |
| `test_shaderTCConsistencyWithWorldToVoxel` | GLSL tc formula agrees with WorldToVoxel |

## Background: why the coordinate fix was needed

The CHAI3D haptic device reports its position as a `cVector3d`, which is
converted to FleX world-space by `FromChai()`:

```
FromChai(Xc, Yc, Zc) = Vec3(Yc, Zc, Xc)
```

So in FleX world coordinates:
* **FleX X** = CHAI3D Y = physical **UP**
* **FleX Y** = CHAI3D Z = physical **FORWARD** (natural drill direction)
* **FleX Z** = CHAI3D X = physical **RIGHT**

The original code mapped `FleX X→column`, `FleX Y→row`, `FleX Z→slice`,
so physically moving the device **right** drove the haptic cursor into the
DICOM **slice** direction instead of the column direction.

The fix: a single consistent permutation in both the GLSL shader and
`WorldToVoxel()`:

| Physical motion | CHAI3D axis | FleX world | → DICOM |
|---|---|---|---|
| Right | X | Z | Column (tc.x) |
| Up | Y | X | Row (tc.y) |
| Forward / drill | Z | Y | Slice (tc.z) |

## How to run

### Linux / macOS
```bash
# From the repository root:
make -C tests run
```

Or manually:
```bash
cd tests
g++ -std=c++17 -o test_dicomvolume_coords test_dicomvolume_coords.cpp
./test_dicomvolume_coords
```

### Windows (Visual Studio Developer Command Prompt)
```cmd
cd tests
cl /std:c++17 /EHsc test_dicomvolume_coords.cpp /Fe:test_dicomvolume_coords.exe
test_dicomvolume_coords.exe
```

Expected output:
```
=== DicomVolume coordinate-mapping unit tests ===

ALL 1064 tests PASSED.
```

## How to integrate your own tests

Add a new `static void test_myFeature()` function in
`test_dicomvolume_coords.cpp` following the existing pattern, then call it
from `main()`.  Use the `CHECK`, `CHECK_EQ`, and `CHECK_NEAR` macros for
assertions.
