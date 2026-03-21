// dicomvolume.h
// CHAI3D / FleX scene: loads a DICOM series folder via GDCM, renders it as a
// GPU-raycasted 3D volume (OpenGL), and provides haptic force feedback from the
// volume gradient so the CHAI3D tool can "feel" tissue boundaries.
//
// Build requirements (Windows, Visual Studio):
//   Set the GDCM paths in your project's property sheets (or in vcxproj), e.g.:
//     AdditionalIncludeDirectories : $(GDCM_ROOT)\include\gdcm-2.8
//     AdditionalLibraryDirectories : $(GDCM_ROOT)\lib  (or \build\bin\Debug)
//   where GDCM_ROOT is an environment variable / macro pointing to your GDCM
//   installation (e.g. E:\DICOM_GL\external\GDCM-2.8.9-Windows-x86_64).
//
//   Libraries (linked via #pragma comment below):
//     gdcmDSED.lib  gdcmMSFF.lib  gdcmDICT.lib
//     gdcmIOD.lib   gdcmEXPAT.lib gdcmCommon.lib
//   DLLs: copy the matching gdcm*.dll files next to the .exe, or add the
//         GDCM bin directory to your PATH.
//
// Usage: Select "DICOM Volume" in the scene list, then press "Select DICOM
//        Folder" in the Options panel to load a CT/MR series.  The haptic
//        tool will feel boundaries (gradient-based repulsion) when the volume
//        is loaded.

#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <mutex>

// --------------------------------------------------------------------------
// GDCM headers  (installed at the path configured in your project settings)
// --------------------------------------------------------------------------
#include <gdcmDirectory.h>
#include <gdcmIPPSorter.h>
#include <gdcmImageReader.h>
#include <gdcmImage.h>
#include <gdcmAttribute.h>
#include <gdcmStringFilter.h>

// --------------------------------------------------------------------------
// OpenGL (only used when not building the D3D variant)
// --------------------------------------------------------------------------
#ifndef FLEX_DX
#include "../external/glad/include/glad/glad.h"
#endif

// --------------------------------------------------------------------------
// Windows folder-picker dialog
// --------------------------------------------------------------------------
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdcmDSED.lib")
#pragma comment(lib, "gdcmMSFF.lib")
#pragma comment(lib, "gdcmDICT.lib")
#pragma comment(lib, "gdcmIOD.lib")
#pragma comment(lib, "gdcmEXPAT.lib")
#pragma comment(lib, "gdcmCommon.lib")
#endif

// --------------------------------------------------------------------------
// Forward-declare OGL_Renderer::CompileProgram so we can call it without
// including the full opengl/shader.h (which has path-relative sub-includes
// that already work when the include path is set up correctly).
// --------------------------------------------------------------------------
#ifndef FLEX_DX
namespace OGL_Renderer
{
    GLuint CompileProgram(const char* vsource, const char* fsource,
                          const char* gsource = nullptr);
}
#endif

// ==========================================================================
class DicomVolume : public Scene
{
public:

    DicomVolume(const char* name)
        : Scene(name)
        , mVolumeLoaded(false)
        , mVolumeTex(0)
        , mLabelTex(0)
        , mVolumeShader(0)
        , mProxyVAO(0), mProxyVBO(0), mProxyEBO(0)
        , mDimX(0), mDimY(0), mDimZ(0)
        , mSpacingX(1.f), mSpacingY(1.f), mSpacingZ(1.f)
        , mDataMin(0.f), mDataMax(1.f)
        , mWindowCenter(0.5f), mWindowWidth(1.0f)
        , mRaySteps(128.f)
        , mHapticForceScale(1.0f)
        , mPaintMode(false)
        , mPaintRadius(3)
        , mLabelDirty(false)
        , mProxySizeHX(-1.f), mProxySizeHY(-1.f), mProxySizeHZ(-1.f)
    {
        mFolderPath[0] = '\0';
        mStatus = "No volume loaded.  Press 'Select DICOM Folder'.";
    }

    ~DicomVolume()
    {
#ifndef FLEX_DX
        if (mVolumeTex)  { glDeleteTextures(1, &mVolumeTex);  mVolumeTex  = 0; }
        if (mLabelTex)   { glDeleteTextures(1, &mLabelTex);   mLabelTex   = 0; }
        if (mVolumeShader){ glDeleteProgram(mVolumeShader);   mVolumeShader = 0; }
        if (mProxyVAO)   { glDeleteVertexArrays(1, &mProxyVAO); mProxyVAO = 0; }
        if (mProxyVBO)   { glDeleteBuffers(1, &mProxyVBO);    mProxyVBO = 0; }
        if (mProxyEBO)   { glDeleteBuffers(1, &mProxyEBO);    mProxyEBO = 0; }
#endif
    }

    // ----------------------------------------------------------------------
    // Scene overrides
    // ----------------------------------------------------------------------

    void Initialize() override
    {
        // No FleX particles needed for pure volume rendering.
        // PostInitialize() will call CreateCursor() for the haptic tool shape.
        g_params.radius    = 0.1f;
        g_numSubsteps      = 1;
        g_pause            = false;
        g_drawPoints       = false;
        g_drawMesh         = false;
        g_drawEllipsoids   = false;
    }

    void DoGui() override
    {
        imguiSeparatorLine();
        imguiLabel("DICOM Volume");
        imguiLabel(mStatus.c_str());

        if (imguiButton("Select DICOM Folder"))
        {
            std::string path = BrowseForFolder("Select DICOM Series Folder");
            if (!path.empty())
            {
                size_t len = path.size() < sizeof(mFolderPath) - 1
                           ? path.size() : sizeof(mFolderPath) - 1;
                memcpy(mFolderPath, path.c_str(), len);
                mFolderPath[len] = '\0';
                LoadDicomSeries(mFolderPath);
            }
        }

        if (mVolumeLoaded)
        {
            char info[128];
            snprintf(info, sizeof(info), "Dims: %d x %d x %d", mDimX, mDimY, mDimZ);
            imguiLabel(info);

            imguiSeparatorLine();
            imguiLabel("Window / Level (normalized 0..1)");
            imguiSlider("W-Center",    &mWindowCenter,      0.0f, 1.0f,  0.005f);
            imguiSlider("W-Width",     &mWindowWidth,       0.01f, 1.0f, 0.005f);
            imguiSlider("Ray Steps",   &mRaySteps,          32.f, 512.f, 8.f);
            imguiSlider("Haptic Scale",&mHapticForceScale,  0.f,  5.f,   0.05f);

            if (imguiCheck("Paint Mode (segmentation)", mPaintMode))
                mPaintMode = !mPaintMode;
        }
    }

    void Update() override
    {
        // Upload label texture updates on the main thread (OpenGL context thread).
#ifndef FLEX_DX
        if (mLabelDirty && mLabelTex)
        {
            glBindTexture(GL_TEXTURE_3D, mLabelTex);
            glTexSubImage3D(GL_TEXTURE_3D, 0,
                            0, 0, 0, mDimX, mDimY, mDimZ,
                            GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                            mLabelData.data());
            glBindTexture(GL_TEXTURE_3D, 0);
            mLabelDirty = false;
        }
#endif

        // If in paint mode, paint voxels under the cursor.
        if (mPaintMode && mVolumeLoaded)
            PaintAt(g_hapticsUpdates.cursorPosition, /*label=*/1);
    }

    void Draw(int pass) override
    {
        // Only custom-render on the color pass; skip the shadow pass.
        if (pass != 0 || !mVolumeLoaded) return;

#ifndef FLEX_DX
        RenderVolume();
#endif
    }

    // ----------------------------------------------------------------------
    // Haptic force feedback (called from the haptics thread)
    // Returns a gradient-based repulsion force so the tool "feels" tissue
    // boundaries.  Returns zero when no volume is loaded or the tool is
    // outside the volume bounding box.
    // ----------------------------------------------------------------------
    virtual Vec3 GetHapticForce(const Vec3& worldPos) override
    {
        if (!mVolumeLoaded) return Vec3(0.f);

        // World → voxel indices (volume is centered at the world origin).
        float fx = worldPos.x / mSpacingX + mDimX * 0.5f;
        float fy = worldPos.y / mSpacingY + mDimY * 0.5f;
        float fz = worldPos.z / mSpacingZ + mDimZ * 0.5f;

        int ix = (int)fx, iy = (int)fy, iz = (int)fz;
        if (ix < 1 || ix >= mDimX - 1 ||
            iy < 1 || iy >= mDimY - 1 ||
            iz < 1 || iz >= mDimZ - 1)
            return Vec3(0.f);

        // Thread-safe read: mVoxelDataNorm is written once during load and
        // never modified again, so we can read from any thread.
        auto sample = [&](int x, int y, int z) -> float
        {
            if (x < 0 || x >= mDimX || y < 0 || y >= mDimY ||
                z < 0 || z >= mDimZ) return 0.f;
            return mVoxelDataNorm[(size_t)z * mDimX * mDimY + y * mDimX + x];
        };

        // Central-difference gradient.
        Vec3 grad(
            sample(ix + 1, iy, iz) - sample(ix - 1, iy, iz),
            sample(ix, iy + 1, iz) - sample(ix, iy - 1, iz),
            sample(ix, iy, iz + 1) - sample(ix, iy, iz - 1)
        );

        float gradMag = Length(grad);
        if (gradMag < 1e-5f) return Vec3(0.f);

        // Repulsion along the gradient (push away from high-density boundary).
        float forceMag = mHapticForceScale * Min(gradMag, 1.0f);
        return -Normalize(grad) * forceMag;
    }

    // ----------------------------------------------------------------------
    // Segmentation painting helper (called from Update on main thread)
    // ----------------------------------------------------------------------
    void PaintAt(const Vec3& worldPos, uint8_t label)
    {
        if (!mVolumeLoaded) return;

        int cx = (int)(worldPos.x / mSpacingX + mDimX * 0.5f);
        int cy = (int)(worldPos.y / mSpacingY + mDimY * 0.5f);
        int cz = (int)(worldPos.z / mSpacingZ + mDimZ * 0.5f);

        bool painted = false;
        for (int dz = -mPaintRadius; dz <= mPaintRadius; ++dz)
        for (int dy = -mPaintRadius; dy <= mPaintRadius; ++dy)
        for (int dx = -mPaintRadius; dx <= mPaintRadius; ++dx)
        {
            int distSquared = dx*dx + dy*dy + dz*dz;
            if (distSquared > mPaintRadius*mPaintRadius) continue;
            int x = cx+dx, y = cy+dy, z = cz+dz;
            if (x < 0 || x >= mDimX || y < 0 || y >= mDimY ||
                z < 0 || z >= mDimZ) continue;
            mLabelData[(size_t)z * mDimX * mDimY + y * mDimX + x] = label;
            painted = true;
        }
        if (painted) mLabelDirty = true;
    }

private:

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------
    char        mFolderPath[512];
    std::string mStatus;
    bool        mVolumeLoaded;

    int   mDimX, mDimY, mDimZ;
    float mSpacingX, mSpacingY, mSpacingZ; // world-space voxel size (metres)
    float mDataMin, mDataMax;               // raw min/max of loaded data

    float mWindowCenter;    // normalized [0..1]
    float mWindowWidth;     // normalized [0..1]
    float mRaySteps;
    float mHapticForceScale;

    bool  mPaintMode;
    int   mPaintRadius;
    bool  mLabelDirty;

    // Normalized [0..1] float voxel data for haptics (CPU side).
    // Written once during load; safe to read from haptics thread thereafter.
    std::vector<float>    mVoxelDataNorm;

    // Segmentation label volume (uint8, 0 = unlabeled).
    std::vector<uint8_t>  mLabelData;

    // OpenGL handles (stored as unsigned int to avoid requiring GL headers
    // in the class definition; cast to GLuint when calling GL functions).
    unsigned int mVolumeTex;
    unsigned int mLabelTex;
    unsigned int mVolumeShader;
    unsigned int mProxyVAO, mProxyVBO, mProxyEBO;
    float  mProxySizeHX, mProxySizeHY, mProxySizeHZ;

    // ------------------------------------------------------------------
    // DICOM loading
    // ------------------------------------------------------------------
    bool LoadDicomSeries(const char* folder)
    {
        mVolumeLoaded = false;
        mVoxelDataNorm.clear();
        mLabelData.clear();
        mStatus = "Loading DICOM series...";

        // Gather filenames.
        gdcm::Directory dir;
        dir.Load(folder, /*recursive=*/false);
        const gdcm::Directory::FilenamesType& files = dir.GetFilenames();
        if (files.empty())
        {
            mStatus = "No files found in the selected folder.";
            return false;
        }

        // Sort slices by Image Position Patient (IPP z-coordinate).
        gdcm::IPPSorter sorter;
        sorter.SetComputeZSpacing(true);
        sorter.SetZSpacingTolerance(1e-3);
        bool sorted = sorter.Sort(files);

        const gdcm::Directory::FilenamesType& orderedFiles =
            (sorted && !sorter.GetFilenames().empty())
                ? sorter.GetFilenames()
                : files;

        int nSlices = (int)orderedFiles.size();
        if (nSlices == 0)
        {
            mStatus = "DICOM series is empty after sorting.";
            return false;
        }

        // Read the first slice to obtain dimensions, pixel format, and spacing.
        gdcm::ImageReader r0;
        r0.SetFileName(orderedFiles[0].c_str());
        if (!r0.Read())
        {
            mStatus = "Failed to read the first DICOM file.";
            return false;
        }
        const gdcm::Image& img0 = r0.GetImage();
        const unsigned int* d0  = img0.GetDimensions();
        mDimX = (int)d0[0];
        mDimY = (int)d0[1];
        mDimZ = nSlices;

        // Pixel spacing (DICOM is in mm; we convert to metres for world space).
        const double* sp = img0.GetSpacing();
        mSpacingX = (sp && sp[0] > 0.0) ? (float)(sp[0] * 0.001) : 0.001f;
        mSpacingY = (sp && sp[1] > 0.0) ? (float)(sp[1] * 0.001) : 0.001f;
        double zsp = sorted ? sorter.GetZSpacing() : 0.0;
        mSpacingZ = (zsp > 0.0) ? (float)(zsp * 0.001) : mSpacingX;

        gdcm::PixelFormat pf         = img0.GetPixelFormat();
        unsigned int      bpp        = pf.GetPixelSize(); // bytes per pixel
        size_t            slicePixels = (size_t)mDimX * mDimY;
        size_t            total       = slicePixels * mDimZ;

        // Accumulate raw 16-bit signed data (CT HU range or MR unsigned).
        std::vector<int16_t> rawData(total, 0);

        for (int s = 0; s < nSlices; ++s)
        {
            gdcm::ImageReader rdr;
            rdr.SetFileName(orderedFiles[s].c_str());
            if (!rdr.Read()) continue;

            const gdcm::Image& img = rdr.GetImage();
            unsigned long      len = img.GetBufferLength();
            std::vector<char>  buf(len);
            img.GetBuffer(buf.data());

            int16_t* dst = &rawData[s * slicePixels];
            if (bpp >= 2)
            {
                const int16_t* src = reinterpret_cast<const int16_t*>(buf.data());
                memcpy(dst, src, slicePixels * sizeof(int16_t));
            }
            else // 8-bit
            {
                const uint8_t* src = reinterpret_cast<const uint8_t*>(buf.data());
                for (size_t i = 0; i < slicePixels; ++i)
                    dst[i] = (int16_t)src[i];
            }
        }

        // Compute data range for normalization using standard library.
        auto minmax = std::minmax_element(rawData.begin(), rawData.end());
        mDataMin = (float)*minmax.first;
        mDataMax = (float)*minmax.second;
        float range = (mDataMax > mDataMin) ? (mDataMax - mDataMin) : 1.f;

        // Build normalized [0..1] float data (haptics) and uint16 texture data.
        mVoxelDataNorm.resize(total);
        std::vector<uint16_t> texData(total);
        for (size_t i = 0; i < total; ++i)
        {
            float n         = (rawData[i] - mDataMin) / range;
            mVoxelDataNorm[i] = n;
            texData[i]        = (uint16_t)(n * 65535.f);
        }

        // Label volume: all zeros.
        mLabelData.assign(total, 0);
        mLabelDirty = false;

        // Default window/level: show full data range.
        mWindowCenter = 0.5f;
        mWindowWidth  = 1.0f;

#ifndef FLEX_DX
        UploadVolumeTextures(texData);
        if (!mVolumeShader)
            mVolumeShader = BuildVolumeShader();
#endif

        mVolumeLoaded = true;
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "Loaded %d x %d x %d  (%.2f x %.2f x %.2f mm/voxel)",
                 mDimX, mDimY, mDimZ,
                 mSpacingX * 1000.f, mSpacingY * 1000.f, mSpacingZ * 1000.f);
        mStatus = msg;
        return true;
    }

#ifndef FLEX_DX

    // ------------------------------------------------------------------
    // OpenGL texture upload
    // ------------------------------------------------------------------
    void UploadVolumeTextures(const std::vector<uint16_t>& texData)
    {
        // Intensity volume (R16 normalized, linear interpolation).
        if (mVolumeTex) { glDeleteTextures(1, &mVolumeTex); mVolumeTex = 0; }
        glGenTextures(1, &mVolumeTex);
        glBindTexture(GL_TEXTURE_3D, mVolumeTex);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_R16,
                     mDimX, mDimY, mDimZ, 0,
                     GL_RED, GL_UNSIGNED_SHORT, texData.data());
        glBindTexture(GL_TEXTURE_3D, 0);

        // Segmentation label volume (R8UI, nearest, no interpolation).
        if (mLabelTex) { glDeleteTextures(1, &mLabelTex); mLabelTex = 0; }
        glGenTextures(1, &mLabelTex);
        glBindTexture(GL_TEXTURE_3D, mLabelTex);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_R8UI,
                     mDimX, mDimY, mDimZ, 0,
                     GL_RED_INTEGER, GL_UNSIGNED_BYTE, mLabelData.data());
        glBindTexture(GL_TEXTURE_3D, 0);
    }

    // ------------------------------------------------------------------
    // GLSL shaders (embedded as string literals)
    // ------------------------------------------------------------------

    // Vertex shader: pass world-space box corner through to fragment shader.
    static const char* VolumeVS()
    {
        return
            "#version 330 core\n"
            "layout(location = 0) in vec3 inPos;\n"
            "uniform mat4 uMVP;\n"
            "out vec3 vWorldPos;\n"
            "void main() {\n"
            "    vWorldPos   = inPos;\n"
            "    gl_Position = uMVP * vec4(inPos, 1.0);\n"
            "}\n";
    }

    // Fragment shader: ray-march through the volume.
    static const char* VolumeFS()
    {
        return
            "#version 330 core\n"
            "in  vec3 vWorldPos;\n"
            "out vec4 fragColor;\n"
            "\n"
            "uniform sampler3D  uVolume;\n"
            "uniform usampler3D uLabels;\n"
            "uniform vec3  uVolHalfSize;\n"  // AABB half-extents in world units
            "uniform vec3  uCamPos;\n"
            "uniform float uWinCenter;\n"    // normalized [0..1]
            "uniform float uWinWidth;\n"     // normalized [0..1]
            "uniform int   uRaySteps;\n"
            "\n"
            "void main() {\n"
            "    vec3 rayDir = normalize(vWorldPos - uCamPos);\n"
            "\n"
            "    // Ray-box intersection (AABB = [-hs, +hs]).\n"
            "    vec3 volMin = -uVolHalfSize;\n"
            "    vec3 volMax =  uVolHalfSize;\n"
            "    vec3 invDir  = 1.0 / (rayDir + vec3(1e-7));\n"
            "    vec3 tMin3   = (volMin - uCamPos) * invDir;\n"
            "    vec3 tMax3   = (volMax - uCamPos) * invDir;\n"
            "    vec3 t1      = min(tMin3, tMax3);\n"
            "    vec3 t2      = max(tMin3, tMax3);\n"
            "    float tNear  = max(max(t1.x, t1.y), t1.z);\n"
            "    float tFar   = min(min(t2.x, t2.y), t2.z);\n"
            "    if (tNear >= tFar || tFar <= 0.0) discard;\n"
            "    tNear = max(tNear, 0.0);\n"
            "\n"
            "    float stepSize = (tFar - tNear) / float(uRaySteps);\n"
            "    vec4  accum    = vec4(0.0);\n"
            "    float wLo      = uWinCenter - uWinWidth * 0.5;\n"
            "\n"
            "    for (int i = 0; i < uRaySteps; ++i) {\n"
            "        vec3  p  = uCamPos + rayDir * (tNear + (float(i) + 0.5) * stepSize);\n"
            "        vec3  tc = (p - volMin) / (volMax - volMin);\n"  // [0..1]
            "        if (any(lessThan(tc, vec3(0.0))) || any(greaterThan(tc, vec3(1.0)))) continue;\n"
            "\n"
            "        float raw = texture(uVolume, tc).r;\n"
            "        float v   = clamp((raw - wLo) / max(uWinWidth, 0.001), 0.0, 1.0);\n"
            "\n"
            "        // Transfer function: simple gray ramp.\n"
            "        vec4 s = vec4(v, v, v, v * 0.04);\n"
            "\n"
            "        // Segmentation overlay (label > 0 -> red).\n"
            "        uint lbl = texture(uLabels, tc).r;\n"
            "        if (lbl > 0u) {\n"
            "            s = vec4(1.0, 0.2, 0.2, 0.5);\n"
            "        }\n"
            "\n"
            "        // Front-to-back compositing.\n"
            "        accum.rgb += (1.0 - accum.a) * s.a * s.rgb;\n"
            "        accum.a   += (1.0 - accum.a) * s.a;\n"
            "        if (accum.a > 0.99) break;\n"
            "    }\n"
            "\n"
            "    if (accum.a < 0.005) discard;\n"
            "    fragColor = accum;\n"
            "}\n";
    }

    static GLuint BuildVolumeShader()
    {
        return OGL_Renderer::CompileProgram(VolumeVS(), VolumeFS());
    }

    // ------------------------------------------------------------------
    // Proxy cube geometry (axis-aligned bounding box of the volume)
    // ------------------------------------------------------------------
    void BuildProxyCube(float hx, float hy, float hz)
    {
        mProxySizeHX = hx; mProxySizeHY = hy; mProxySizeHZ = hz;

        float v[8 * 3] = {
            -hx, -hy, -hz,   hx, -hy, -hz,   hx,  hy, -hz,  -hx,  hy, -hz,
            -hx, -hy,  hz,   hx, -hy,  hz,   hx,  hy,  hz,  -hx,  hy,  hz
        };
        static const unsigned short idx[36] = {
            0,1,2, 2,3,0,   4,7,6, 6,5,4,   // back / front
            0,3,7, 7,4,0,   1,5,6, 6,2,1,   // left / right
            0,4,5, 5,1,0,   3,2,6, 6,7,3    // bottom / top
        };

        if (!mProxyVAO) glGenVertexArrays(1, &mProxyVAO);
        if (!mProxyVBO) glGenBuffers(1, &mProxyVBO);
        if (!mProxyEBO) glGenBuffers(1, &mProxyEBO);

        glBindVertexArray(mProxyVAO);

        glBindBuffer(GL_ARRAY_BUFFER, mProxyVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mProxyEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

        glBindVertexArray(0);
    }

    // ------------------------------------------------------------------
    // Volume rendering
    // ------------------------------------------------------------------
    void RenderVolume()
    {
        if (!mVolumeShader || !mVolumeTex || !mLabelTex) return;

        // Volume half-extents in world space (metres).
        float hx = mDimX * mSpacingX * 0.5f;
        float hy = mDimY * mSpacingY * 0.5f;
        float hz = mDimZ * mSpacingZ * 0.5f;

        // Rebuild proxy cube if dimensions changed.
        if (mProxySizeHX != hx || mProxySizeHY != hy || mProxySizeHZ != hz)
            BuildProxyCube(hx, hy, hz);

        // Recompute MVP matrix from current camera state (same as main loop).
        const float fov    = kPi / 4.0f;
        const float aspect = (g_screenHeight > 0)
                             ? (float)g_screenWidth / (float)g_screenHeight
                             : 1.f;
        Matrix44 proj = ProjectionMatrix(RadToDeg(fov), aspect, g_camNear, g_camFar);
        Matrix44 view = RotationMatrix(-g_camAngle.x, Vec3(0.f, 1.f, 0.f))
                      * RotationMatrix(-g_camAngle.y,
                            Vec3(cosf(-g_camAngle.x), 0.f, sinf(-g_camAngle.x)))
                      * TranslationMatrix(-Point3(g_camPos));
        Matrix44 mvp  = proj * view;   // no model transform; volume at world origin

        glUseProgram(mVolumeShader);

        // Bind intensity volume.
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_3D, mVolumeTex);
        glUniform1i(glGetUniformLocation(mVolumeShader, "uVolume"), 0);

        // Bind label volume.
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, mLabelTex);
        glUniform1i(glGetUniformLocation(mVolumeShader, "uLabels"), 1);

        // Upload uniforms.
        glUniformMatrix4fv(glGetUniformLocation(mVolumeShader, "uMVP"),
                           1, GL_FALSE, (const float*)&mvp);
        glUniform3f(glGetUniformLocation(mVolumeShader, "uVolHalfSize"), hx, hy, hz);
        glUniform3f(glGetUniformLocation(mVolumeShader, "uCamPos"),
                    g_camPos.x, g_camPos.y, g_camPos.z);
        glUniform1f(glGetUniformLocation(mVolumeShader, "uWinCenter"), mWindowCenter);
        glUniform1f(glGetUniformLocation(mVolumeShader, "uWinWidth"),  mWindowWidth);
        glUniform1i(glGetUniformLocation(mVolumeShader, "uRaySteps"),  (int)mRaySteps);

        // Draw proxy cube with alpha blending.
        GLboolean  prevBlend  = glIsEnabled(GL_BLEND);
        GLboolean  prevCull   = glIsEnabled(GL_CULL_FACE);
        GLboolean  prevDepthW = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthW);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);   // Don't write depth for transparent volume

        glBindVertexArray(mProxyVAO);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
        glBindVertexArray(0);

        // Restore state.
        glDepthMask(prevDepthW);
        if (!prevBlend)  glDisable(GL_BLEND);
        if (prevCull)    glEnable(GL_CULL_FACE);

        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);
    }

#endif // !FLEX_DX

    // ------------------------------------------------------------------
    // Windows folder-picker (returns empty string if cancelled or N/A)
    // ------------------------------------------------------------------
    static std::string BrowseForFolder(const char* title)
    {
#ifdef _WIN32
        // Initialize COM only if not already done on this thread.
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool needsUninit = (hrCom == S_OK);   // only uninit if WE initialized

        BROWSEINFOA bi = {};
        bi.lpszTitle = title;
        bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);

        std::string result;
        if (pidl)
        {
            char path[MAX_PATH] = {};
            if (SHGetPathFromIDListA(pidl, path))
                result = path;
            CoTaskMemFree(pidl);
        }

        if (needsUninit)
            CoUninitialize();
        return result;
#else
        // TODO: implement folder picker for Linux (e.g. via zenity / nfd).
        return {};
#endif
    }
};
