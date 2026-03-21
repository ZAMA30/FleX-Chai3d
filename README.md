# FleX-Chai3d
## Haptic Rendering of Fluids

![shot1](images/shot1.png)

![shot2](images/shot2.jpg)

![shot3](images/shot3.png)

---

## DICOM Volume Scene

The **DICOM Volume** scene (`FleX-Chai3d/scenes/dicomvolume.h`) lets you load a
CT or MR DICOM series from a folder, renders it as a 3D raycasted volume
(OpenGL), and feeds haptic force feedback from the volume gradient to the
CHAI3D tool so you can "feel" tissue boundaries.

### Features
- **Series loading**: uses GDCM (`gdcm::IPPSorter`) to sort slices by Image
  Position Patient, then reads each slice and builds a 3D buffer.
- **3D volume rendering**: GPU ray-march shader, front-to-back compositing,
  configurable window/level and ray-step count.
- **Segmentation painting**: toggle "Paint Mode" to paint a red overlay in the
  volume at the haptic cursor position (label volume updated on GPU each frame).
- **Haptic force feedback**: gradient-based repulsion – the CHAI3D tool resists
  moving through high-intensity-gradient boundaries (e.g. bone/soft-tissue edges).

### Build requirements (Windows / Visual Studio)

1. **Install GDCM 2.8.9** (or newer).

2. Define an environment variable (or MSBuild macro) `GDCM_ROOT` pointing to
   your GDCM installation root, e.g.:
   ```
   GDCM_ROOT = E:\DICOM_GL\external\GDCM-2.8.9-Windows-x86_64
   ```
   (The lib output may be in a separate build tree; set `GDCM_LIB_DIR`
   accordingly, e.g. `E:\gdcm\GDCM-2.8.9\build\bin\Debug`.)

3. In your Visual Studio project / property sheet:
   - **Additional Include Directories**: `$(GDCM_ROOT)\include\gdcm-2.8`
   - **Additional Library Directories**: `$(GDCM_LIB_DIR)`
   - The scene header adds `#pragma comment(lib, ...)` for the following libs
     automatically (MSVC):
     `gdcmDSED.lib`, `gdcmMSFF.lib`, `gdcmDICT.lib`, `gdcmIOD.lib`,
     `gdcmEXPAT.lib`, `gdcmCommon.lib`.

4. **DLLs at runtime**: copy the matching `gdcm*.dll` files from your GDCM build
   output directory next to the application `.exe`, or add the directory to your
   `PATH`.

### Usage

1. Launch the application and select **"DICOM Volume"** in the scene list on the
   left panel.
2. In the **Options** panel on the right, click **"Select DICOM Folder"** and
   choose the folder that contains your DICOM `.dcm` slice files.
3. The volume loads and renders immediately.  Use the **W-Center / W-Width**
   sliders to adjust the display window/level.
4. Connect a CHAI3D haptic device: the tool will feel repulsion near tissue
   boundaries.  Increase **"Haptic Scale"** to strengthen the effect.
5. Enable **"Paint Mode"** to paint a red segmentation overlay at the cursor
   position while moving the tool through the volume.

