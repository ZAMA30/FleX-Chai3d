// DICOM Volume scene
// Loads a directory of DICOM (.dcm) files (e.g. a Dixon MRI series) as a
// 3D voxel volume and adds it to the CHAI3D world so it can be rendered and
// haptically explored alongside the FleX fluid simulation.
//
// Coordinate mapping (FleX/OpenGL  <->  CHAI3D  <->  DICOM voxel):
//   FleX X (up)      = CHAI3D Y  ->  DICOM row
//   FleX Y (forward) = CHAI3D Z  ->  DICOM slice
//   FleX Z (right)   = CHAI3D X  ->  DICOM column

#pragma once

#include "chai3d.h"
#include "graphics/CMultiImage.h"
#include "materials/CTexture3d.h"
#include "world/CVoxelObject.h"

#include <algorithm>
#include <cstdio>
#include <string>

using namespace chai3d;

class DicomVolume : public Scene
{
public:

    DicomVolume(const char* name) : Scene(name)
    {
        m_voxelObject   = nullptr;
        m_volumeLoaded  = false;
        // Default directory – change via the in-scene GUI or by passing the
        // path directly to the constructor below.
        m_dixonDir = "../../data/dixon";
    }

    DicomVolume(const char* name, const std::string& a_dixonDir)
        : Scene(name)
    {
        m_voxelObject  = nullptr;
        m_volumeLoaded = false;
        m_dixonDir     = a_dixonDir;
    }

    // -----------------------------------------------------------------------
    virtual void Initialize() override
    {
        // Set up a standard fluid scene (gravity, fluid parameters).
        g_params.radius             = 0.1f;
        g_params.fluidRestDistance  = g_params.radius * 0.55f;
        g_params.dynamicFriction    = 0.0f;
        g_params.viscosity          = 0.0f;
        g_params.numIterations      = 3;
        g_params.numPlanes          = 5;
        g_params.vorticityConfinement = 40.f;

        g_drawDensity    = true;
        g_drawDiffuse    = true;
        g_drawEllipsoids = true;
        g_drawPoints     = false;

        LoadVolume();
    }

    // -----------------------------------------------------------------------
    virtual void DoGui() override
    {
        imguiLabel("Dixon / DICOM directory:");
        imguiValue(m_dixonDir.c_str());

        if (imguiButton("Reload Volume"))
        {
            UnloadVolume();
            LoadVolume();
        }

        if (m_volumeLoaded && m_voxelObject)
        {
            imguiLabel("Volume loaded");
            float iso = m_voxelObject->getIsosurfaceValue();
            if (imguiSlider("Iso Value", &iso, 0.0f, 1.0f, 0.001f))
                m_voxelObject->setIsosurfaceValue(iso);

            float quality = (float)m_voxelObject->getQuality();
            if (imguiSlider("Quality", &quality, 0.1f, 1.0f, 0.05f))
                m_voxelObject->setQuality((double)quality);
        }
        else
        {
            imguiLabel("No volume loaded");
        }
    }

    // -----------------------------------------------------------------------
    // Convert a FleX world-space position to a DICOM voxel coordinate.
    //
    // FleX/OpenGL  ->  CHAI3D  ->  DICOM voxel
    //   FleX X (up)      = CHAI3D Y  ->  row
    //   FleX Y (forward) = CHAI3D Z  ->  slice
    //   FleX Z (right)   = CHAI3D X  ->  column
    //
    // The voxel object occupies the CHAI3D bounding box
    //   [m_minCorner, m_maxCorner] in CHAI3D coordinates.
    // The texture coordinates are normalised to [0,1] within that box.
    //
    // \param  a_worldPos  FleX world-space position.
    // \param  a_col       Output: DICOM column index (0 … width-1).
    // \param  a_row       Output: DICOM row index    (0 … height-1).
    // \param  a_slice     Output: DICOM slice index  (0 … depth-1).
    // \return true if the position is inside the volume.
    bool WorldToVoxel(const Vec3& a_worldPos,
                      int& a_col,
                      int& a_row,
                      int& a_slice) const
    {
        if (!m_volumeLoaded || !m_voxelObject || !m_voxelObject->m_texture)
            return false;

        // Convert FleX world coords to CHAI3D coords:
        //   CHAI3D X  =  FleX Z  (right)
        //   CHAI3D Y  =  FleX X  (up)
        //   CHAI3D Z  =  FleX Y  (forward)
        cVector3d chaiPos(a_worldPos.z, a_worldPos.x, a_worldPos.y);

        const cVector3d& lo = m_voxelObject->m_minCorner;
        const cVector3d& hi = m_voxelObject->m_maxCorner;

        // Normalised position in [0,1].
        double nx = (chaiPos.x() - lo.x()) / (hi.x() - lo.x());
        double ny = (chaiPos.y() - lo.y()) / (hi.y() - lo.y());
        double nz = (chaiPos.z() - lo.z()) / (hi.z() - lo.z());

        if (nx < 0.0 || nx > 1.0 ||
            ny < 0.0 || ny > 1.0 ||
            nz < 0.0 || nz > 1.0)
            return false;

        cImagePtr img = m_voxelObject->m_texture->m_image;
        if (!img) return false;

        a_col   = (int)(nx * (img->getWidth()       - 1));  // CHAI3D X -> column
        a_row   = (int)(ny * (img->getHeight()      - 1));  // CHAI3D Y -> row
        a_slice = (int)(nz * (img->getImageCount()  - 1));  // CHAI3D Z -> slice

        return true;
    }


    // -----------------------------------------------------------------------
    // Public members for direct access.
    cVoxelObject* m_voxelObject;
    bool          m_volumeLoaded;

private:

    std::string   m_dixonDir;

    // -----------------------------------------------------------------------
    void LoadVolume()
    {
        // Create a multi-image and scan the Dixon directory for .dcm files.
        auto multiImage = cMultiImage::create();
        int nLoaded = multiImage->loadFromDirectory(m_dixonDir, "dcm");

        if (nLoaded <= 0)
        {
            printf("[DicomVolume] WARNING: No .dcm files loaded from \"%s\"\n",
                   m_dixonDir.c_str());
            m_volumeLoaded = false;
            return;
        }

        printf("[DicomVolume] Loaded %d DICOM slices from \"%s\" (%dx%d)\n",
               nLoaded,
               m_dixonDir.c_str(),
               multiImage->getWidth(),
               multiImage->getHeight());

        // Create and configure the 3D texture.
        auto texture = cTexture3d::create();
        texture->m_image = multiImage;
        texture->setWrapModeS(GL_CLAMP_TO_EDGE);
        texture->setWrapModeT(GL_CLAMP_TO_EDGE);
        texture->setWrapModeR(GL_CLAMP_TO_EDGE);
        texture->setMinFunction(GL_LINEAR);
        texture->setMagFunction(GL_LINEAR);

        // Create the voxel object and add it to the CHAI3D world.
        m_voxelObject = new cVoxelObject();
        m_voxelObject->setTexture(texture);

        // Size the bounding box: use a 3-unit cube centred at the origin in
        // CHAI3D space.  Aspect ratio is preserved from the actual image dims
        // when they are available.
        double w = multiImage->getWidth();
        double h = multiImage->getHeight();
        double d = multiImage->getImageCount();
        double maxDim = std::max({w, h, d});

        double sx = (w / maxDim) * 3.0;
        double sy = (h / maxDim) * 3.0;
        double sz = (d / maxDim) * 3.0;

        m_voxelObject->m_minCorner.set(-sx * 0.5, -sy * 0.5, -sz * 0.5);
        m_voxelObject->m_maxCorner.set( sx * 0.5,  sy * 0.5,  sz * 0.5);
        m_voxelObject->m_minTextureCoord.set(0.0, 0.0, 0.0);
        m_voxelObject->m_maxTextureCoord.set(1.0, 1.0, 1.0);

        m_voxelObject->setRenderingModeDVRColorMap();
        m_voxelObject->setQuality(0.5);
        m_voxelObject->setIsosurfaceValue(0.2f);
        m_voxelObject->setUseLinearInterpolation(true);
        m_voxelObject->setEnabled(true);
        m_voxelObject->setTransparencyLevel(0.8f);

        g_chaiWorld->addChild(m_voxelObject);
        m_volumeLoaded = true;
    }

    // -----------------------------------------------------------------------
    void UnloadVolume()
    {
        if (m_voxelObject)
        {
            g_chaiWorld->removeChild(m_voxelObject);
            delete m_voxelObject;
            m_voxelObject = nullptr;
        }
        m_volumeLoaded = false;
    }
};
