
//==============================================================================
/*!
    \file       dicomvolume.h

    \brief
    FleX scene that loads a directory of DICOM slices into a CHAI3D
    cVoxelObject, renders the volume via DVR/isosurface, and provides
    haptic feedback when the haptic cursor penetrates the iso-surface.

    Data path:  ../../data/dicom/   (override with command-line --dicom <dir>)

    Coordinate conventions
    ----------------------
    FleX/OpenGL world : Y-up, standard OpenGL right-hand axes.
    CHAI3D haptic world: different axis ordering (see helpers.h FromChai/ToChai).
      ToChai(flex_vec)  = cVector3d(flex.z, flex.x, flex.y)
      FromChai(chai_vec)= Vec3(chai.y(),  chai.z(),  chai.x())

    The voxel object is placed in the CHAI3D world (for haptics) and a
    CHAI3D-to-FleX rotation matrix is applied when rendering so that the
    volume appears at the correct location in FleX world space.
*/
//==============================================================================

#pragma once

// Path to the directory containing DICOM slice files.
// Set via the --dicom command-line flag before scene initialisation.
// Included only from main.cpp (via scenes.h) so a single definition is safe.
static const char* g_dicomDirectory = "../../data/dicom";


//==============================================================================
class DicomVolume : public Scene
{
public:

    DicomVolume(const char* name) : Scene(name), m_voxelObject(nullptr) {}

    //--------------------------------------------------------------------------
    virtual ~DicomVolume()
    {
        // The voxel object belongs to g_chaiWorld; it is cleaned up when the
        // world is destroyed.  We just clear our pointer.
        m_voxelObject = nullptr;
    }

    //--------------------------------------------------------------------------
    virtual void Initialize() override
    {
        // -----------------------------------------------------------------------
        // FleX simulation settings – minimal scene (no particles)
        // -----------------------------------------------------------------------
        g_numSubsteps        = 2;
        g_params.radius      = 0.1f;
        g_params.gravity[1]  = -9.8f;

        // Floor plane (FleX)
        g_params.planes[0][0] = 0.0f;
        g_params.planes[0][1] = 1.0f;
        g_params.planes[0][2] = 0.0f;
        g_params.planes[0][3] = 0.0f;
        g_params.numPlanes    = 1;

        // Scene bounds (for camera centering)
        g_sceneLower = Vec3(-2.0f, 0.0f, -2.0f);
        g_sceneUpper = Vec3( 2.0f, 3.0f,  2.0f);

        // Disable particle drawing – we have no particles
        g_drawPoints = false;
    }

    //--------------------------------------------------------------------------
    virtual void PostInitialize() override
    {
        // Create the FleX haptic cursor shape
        Scene::PostInitialize();

        // -----------------------------------------------------------------------
        // Create CHAI3D voxel object
        // -----------------------------------------------------------------------
        m_voxelObject = new cVoxelObject();

        // -----------------------------------------------------------------------
        // Load DICOM volume
        // -----------------------------------------------------------------------
        // The volume center in FleX world space: (0, 1.5, 0)
        // Half-size: 1.5 in each direction.
        //
        // In CHAI3D space (ToChai maps FleX→CHAI3D):
        //   ToChai(0, 1.5, 0)  = cVector3d(0, 0, 1.5)
        //   ToChai(-1.5, 0,-1.5) = cVector3d(-1.5,-1.5, 0)
        //   ToChai( 1.5, 3, 1.5) = cVector3d( 1.5, 1.5, 3)
        //
        // Note: axes permuted but extent is symmetric so shape is the same.

        cMultiImagePtr image = cMultiImage::create();
        bool loaded = image->loadFromDirectory(g_dicomDirectory, "dcm");

        if (!loaded)
        {
            // Generate a synthetic 32³ gradient test volume when no DICOM data
            // is present.  This allows the scene to run without real DICOM files.
            const int DIM = 32;
            image->allocate((unsigned int)DIM, (unsigned int)DIM,
                            (unsigned int)DIM, GL_LUMINANCE, GL_UNSIGNED_BYTE);

            for (int z = 0; z < DIM; ++z)
            {
                for (int y = 0; y < DIM; ++y)
                {
                    for (int x = 0; x < DIM; ++x)
                    {
                        // Spherical shell: bright at ~radius 10 voxels from centre
                        double cx = x - DIM / 2.0 + 0.5;
                        double cy = y - DIM / 2.0 + 0.5;
                        double cz = z - DIM / 2.0 + 0.5;
                        double r = sqrt(cx*cx + cy*cy + cz*cz);
                        double shell = exp(-0.5 * (r - 10.0) * (r - 10.0) / 4.0);
                        unsigned char val = (unsigned char)(cClamp(shell, 0.0, 1.0) * 255.0);
                        image->setVoxelColor((unsigned int)x,
                                            (unsigned int)y,
                                            (unsigned int)z, val);
                    }
                }
            }
        }

        // -----------------------------------------------------------------------
        // Attach the image to a 3D texture and assign to the voxel object
        // -----------------------------------------------------------------------
        cTexture3dPtr texture = cTexture3d::create();
        texture->m_image = image;
        m_voxelObject->setTexture(texture);

        // -----------------------------------------------------------------------
        // Set volume bounds in CHAI3D haptic world coordinates
        //   CHAI3D min = ToChai(-1.5, 0, -1.5) = (-1.5, -1.5, 0)
        //   CHAI3D max = ToChai( 1.5, 3,  1.5) = ( 1.5,  1.5, 3)
        // -----------------------------------------------------------------------
        m_voxelObject->m_minCorner.set(-1.5, -1.5,  0.0);
        m_voxelObject->m_maxCorner.set( 1.5,  1.5,  3.0);
        m_voxelObject->m_minTextureCoord.set(0.0, 0.0, 0.0);
        m_voxelObject->m_maxTextureCoord.set(1.0, 1.0, 1.0);

        // Place the object at the CHAI3D world origin; the bounds above already
        // express the correct CHAI3D position.
        m_voxelObject->setLocalPos(cVector3d(0.0, 0.0, 0.0));

        // -----------------------------------------------------------------------
        // Rendering mode: isosurface with material colour
        // (does not require a camera in update(), suitable for our context)
        // -----------------------------------------------------------------------
        m_voxelObject->setRenderingModeIsosurfaceMaterial();
        m_voxelObject->setIsosurfaceValue(0.15f);
        m_voxelObject->setQuality(0.5);

        // Material colour for the isosurface
        m_voxelObject->m_material->setWhite();
        m_voxelObject->m_material->setStiffness(500.0);
        m_voxelObject->m_material->setStaticFriction(0.3);
        m_voxelObject->m_material->setDynamicFriction(0.2);

        // -----------------------------------------------------------------------
        // Register in the CHAI3D world so the haptic tool can interact with it
        // -----------------------------------------------------------------------
        g_chaiWorld->addChild(m_voxelObject);
    }

    //--------------------------------------------------------------------------
    virtual void Draw(int pass) override
    {
        // Only draw in the forward (non-shadow) pass
        if (pass != 0 || m_voxelObject == nullptr) return;

        // -----------------------------------------------------------------------
        // Render the CHAI3D voxel object using the current OpenGL (FleX) matrices.
        //
        // CHAI3D haptic world has permuted axes relative to FleX/OpenGL world:
        //   FromChai(chai) = Vec3(chai.y, chai.z, chai.x)
        //
        // Applying the CHAI3D→FleX rotation before calling renderSceneGraph()
        // makes the volume appear at the correct FleX world position (0, 1.5, 0).
        //
        // The 4×4 column-major matrix that maps CHAI3D→FleX:
        //   [fx]   [ 0  1  0  0 ] [cx]
        //   [fy] = [ 0  0  1  0 ] [cy]
        //   [fz]   [ 1  0  0  0 ] [cz]
        //   [ 1]   [ 0  0  0  1 ] [ 1]
        // -----------------------------------------------------------------------
        GLfloat chai2flex[16] = {
            0.0f, 0.0f, 1.0f, 0.0f,   // column 0
            1.0f, 0.0f, 0.0f, 0.0f,   // column 1
            0.0f, 1.0f, 0.0f, 0.0f,   // column 2
            0.0f, 0.0f, 0.0f, 1.0f    // column 3 (no translation here)
        };

        glPushMatrix();
        glMultMatrixf(chai2flex);

        // Build render options for a single opaque pass
        cRenderOptions options;
        options.m_camera                              = nullptr;
        options.m_single_pass_only                    = true;
        options.m_render_opaque_objects_only          = false;
        options.m_render_transparent_front_faces_only = false;
        options.m_render_transparent_back_faces_only  = false;
        options.m_enable_lighting                     = true;
        options.m_render_materials                    = true;
        options.m_render_textures                     = true;
        options.m_creating_shadow_map                 = false;
        options.m_rendering_shadow                    = false;
        options.m_shadow_light_level                  = 0.5;
        options.m_storeObjectPositions                = true;
        options.m_markForUpdate                       = false;

        m_voxelObject->renderSceneGraph(options);

        glPopMatrix();
    }

    //--------------------------------------------------------------------------
    virtual void CenterCamera() override
    {
        g_camPos   = Vec3(0.0f, 1.5f, 8.0f);
        g_camAngle = Vec3(0.0f, -DegToRad(5.0f), 0.0f);
    }

    //--------------------------------------------------------------------------
    virtual void DoGui() override
    {
        if (m_voxelObject == nullptr) return;

        // Simple ImGui panel to tweak iso-surface threshold at runtime
        imguiLabel("DICOM Volume");
        static float isoValue = 0.15f;
        if (imguiSlider("Isosurface", &isoValue, 0.01f, 1.0f, 0.01f))
            m_voxelObject->setIsosurfaceValue(isoValue);
    }

private:
    cVoxelObject* m_voxelObject;
};
