//==============================================================================
/*
    Software License Agreement (BSD License)
    Copyright (c) 2003-2016, CHAI3D.
    (www.chai3d.org)

    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following
    disclaimer in the documentation and/or other materials provided
    with the distribution.

    * Neither the name of CHAI3D nor the names of its contributors may
    be used to endorse or promote products derived from this software
    without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
    ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.

    \author    <http://www.chai3d.org>
    \version   3.2.0
*/
//==============================================================================

//------------------------------------------------------------------------------
#include "files/CFileImageDCM.h"
//------------------------------------------------------------------------------
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <climits>
//------------------------------------------------------------------------------
using namespace std;
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
namespace chai3d {
//------------------------------------------------------------------------------


//==============================================================================
// DICOM helper structures and functions
//==============================================================================

namespace {

// Endian-safe read of 16-bit little-endian value
static uint16_t readLE16(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Endian-safe read of 32-bit little-endian value
static uint32_t readLE32(const uint8_t* p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

// Check if a 2-byte string looks like an explicit VR (two uppercase ASCII letters)
static bool isExplicitVR(const char vr[2])
{
    return (vr[0] >= 'A' && vr[0] <= 'Z') && (vr[1] >= 'A' && vr[1] <= 'Z');
}

// VR types that use 4-byte length (after 2 reserved bytes)
static bool isLongVR(const char vr[2])
{
    // OB OD OF OL OW SQ UC UN UR UT
    const char* longVRs[] = { "OB","OD","OF","OL","OW","SQ","UC","UN","UR","UT", nullptr };
    for (int i = 0; longVRs[i]; ++i)
    {
        if (vr[0] == longVRs[i][0] && vr[1] == longVRs[i][1])
            return true;
    }
    return false;
}

// Parse a DICOM decimal string (DS) value, return the first number found
static double parseDicomDS(const char* str, size_t len)
{
    // Skip leading whitespace
    size_t i = 0;
    while (i < len && (str[i] == ' ' || str[i] == '\0')) ++i;
    if (i >= len) return 0.0;

    // null-terminate a local copy
    char buf[64] = {};
    size_t copyLen = (len < 63) ? len : 63;
    memcpy(buf, str + i, copyLen);
    return strtod(buf, nullptr);
}

// Parse a DICOM integer string (IS) value
static int parseDicomIS(const char* str, size_t len)
{
    char buf[32] = {};
    size_t copyLen = (len < 31) ? len : 31;
    memcpy(buf, str, copyLen);
    return atoi(buf);
}

//==============================================================================
// DicomReader: parses one DICOM file and extracts pixel data + metadata
//==============================================================================
struct DicomMetadata
{
    uint16_t rows          = 0;
    uint16_t cols          = 0;
    uint16_t bitsAllocated = 16;
    uint16_t bitsStored    = 16;
    uint16_t pixelRepr     = 0;   // 0=unsigned, 1=signed
    uint16_t samplesPerPix = 1;
    double   windowCenter  = 0.0;
    double   windowWidth   = 0.0;
    double   rescaleIntercept = 0.0;
    double   rescaleSlope     = 1.0;
    double   sliceLocation    = 0.0;
    int      instanceNumber   = 0;
    bool     hasWindowCenter  = false;
    bool     hasWindowWidth   = false;
    bool     hasSliceLocation = false;
    bool     invertPixels     = false; // MONOCHROME1
};

static bool parseDicomFile(const string& filename,
                           DicomMetadata& meta,
                           vector<uint8_t>& pixelData,
                           bool headerOnly = false)
{
    ifstream f(filename, ios::binary);
    if (!f.is_open()) return false;

    // Read entire file into memory for simpler parsing
    f.seekg(0, ios::end);
    size_t fileSize = (size_t)f.tellg();
    f.seekg(0, ios::beg);

    if (fileSize < 132) return false;

    vector<uint8_t> buf(fileSize);
    f.read(reinterpret_cast<char*>(buf.data()), (streamsize)fileSize);
    if (!f) return false;

    // Check DICOM magic at offset 128
    if (memcmp(&buf[128], "DICM", 4) != 0) return false;

    size_t pos = 132;
    bool   explicitVR = true; // assume explicit VR (most common); detect implicitly

    // Detect transfer syntax from first element to decide VR style.
    // Element at pos should be (0002,xxxx) which is always explicit little-endian.
    // After the file meta group, the default is explicit VR little-endian unless
    // indicated otherwise. We'll detect it lazily.

    // Flag: once we have seen a non-meta group element, apply our VR detection heuristic
    bool metaGroupDone = false;

    while (pos + 4 <= fileSize)
    {
        uint16_t group   = readLE16(&buf[pos]);
        uint16_t element = readLE16(&buf[pos + 2]);
        pos += 4;

        // In header-only mode, stop before pixel data (7FE0,0010)
        if (headerOnly && group == 0x7FE0 && element == 0x0010)
            break;

        if (pos + 4 > fileSize) break;

        uint32_t length = 0;
        char vr[3]      = {};

        if (!metaGroupDone && group == 0x0002)
        {
            // File meta information group — always explicit VR
            vr[0] = (char)buf[pos];
            vr[1] = (char)buf[pos + 1];
            pos += 2;

            if (isExplicitVR(vr) && isLongVR(vr))
            {
                pos += 2; // skip reserved
                if (pos + 4 > fileSize) break;
                length = readLE32(&buf[pos]);
                pos += 4;
            }
            else if (isExplicitVR(vr))
            {
                if (pos + 2 > fileSize) break;
                length = readLE16(&buf[pos]);
                pos += 2;
            }
            else
            {
                // Shouldn't happen in meta group, but fall back
                pos -= 2;
                length = readLE32(&buf[pos]);
                pos += 4;
            }

            // Check TransferSyntaxUID (0002,0010) to determine if implicit VR
            if (element == 0x0010 && length > 0 && pos + length <= fileSize)
            {
                // Implicit VR little-endian UID = "1.2.840.10008.1.2"
                const char* implicitUID = "1.2.840.10008.1.2";
                size_t uidLen = strlen(implicitUID);
                if (length >= uidLen &&
                    memcmp(&buf[pos], implicitUID, uidLen) == 0 &&
                    (length == uidLen || buf[pos + uidLen] == '\0' ||
                     buf[pos + uidLen] == ' '))
                {
                    explicitVR = false;
                }
            }
        }
        else
        {
            // Dataset groups
            if (!metaGroupDone && group != 0x0002)
                metaGroupDone = true;

            if (explicitVR && pos + 2 <= fileSize)
            {
                vr[0] = (char)buf[pos];
                vr[1] = (char)buf[pos + 1];

                if (isExplicitVR(vr))
                {
                    pos += 2;
                    if (isLongVR(vr))
                    {
                        pos += 2; // skip reserved
                        if (pos + 4 > fileSize) break;
                        length = readLE32(&buf[pos]);
                        pos += 4;
                    }
                    else
                    {
                        if (pos + 2 > fileSize) break;
                        length = readLE16(&buf[pos]);
                        pos += 2;
                    }
                }
                else
                {
                    // Looks implicit; fall through to implicit handling
                    if (pos + 2 > fileSize) break;
                    length = readLE32(&buf[pos - 2]);
                    pos += 2;
                }
            }
            else
            {
                // Implicit VR: 4-byte length
                if (pos + 4 > fileSize) break;
                length = readLE32(&buf[pos]);
                pos += 4;
            }
        }

        // Undefined length (0xFFFFFFFF) — skip; we handle pixel data specially
        if (length == 0xFFFFFFFF)
        {
            // Sequence or encapsulated pixel data - skip to next delimiter
            // For simplicity, break here; proper handling requires full SQ parsing
            break;
        }

        if (pos + length > fileSize) break;

        const uint8_t* val = &buf[pos];

        // Parse key tags
        switch ((uint32_t)group << 16 | element)
        {
        case 0x00280002: // SamplesPerPixel
            if (length >= 2) meta.samplesPerPix = readLE16(val);
            break;
        case 0x00280004: // PhotometricInterpretation
            if (length > 0)
            {
                string pmi(reinterpret_cast<const char*>(val), length);
                // Trim trailing whitespace/null
                while (!pmi.empty() && (pmi.back() == ' ' || pmi.back() == '\0'))
                    pmi.pop_back();
                if (pmi == "MONOCHROME1")
                    meta.invertPixels = true;
            }
            break;
        case 0x00280010: // Rows
            if (length >= 2) meta.rows = readLE16(val);
            break;
        case 0x00280011: // Columns
            if (length >= 2) meta.cols = readLE16(val);
            break;
        case 0x00280100: // BitsAllocated
            if (length >= 2) meta.bitsAllocated = readLE16(val);
            break;
        case 0x00280101: // BitsStored
            if (length >= 2) meta.bitsStored = readLE16(val);
            break;
        case 0x00280103: // PixelRepresentation
            if (length >= 2) meta.pixelRepr = readLE16(val);
            break;
        case 0x00201041: // SliceLocation
            if (length > 0)
            {
                meta.sliceLocation = parseDicomDS(reinterpret_cast<const char*>(val), length);
                meta.hasSliceLocation = true;
            }
            break;
        case 0x00200013: // InstanceNumber
            if (length > 0)
                meta.instanceNumber = parseDicomIS(reinterpret_cast<const char*>(val), length);
            break;
        case 0x00281050: // WindowCenter
            if (length > 0)
            {
                meta.windowCenter  = parseDicomDS(reinterpret_cast<const char*>(val), length);
                meta.hasWindowCenter = true;
            }
            break;
        case 0x00281051: // WindowWidth
            if (length > 0)
            {
                meta.windowWidth   = parseDicomDS(reinterpret_cast<const char*>(val), length);
                meta.hasWindowWidth = true;
            }
            break;
        case 0x00281052: // RescaleIntercept
            if (length > 0)
                meta.rescaleIntercept = parseDicomDS(reinterpret_cast<const char*>(val), length);
            break;
        case 0x00281053: // RescaleSlope
            if (length > 0)
                meta.rescaleSlope = parseDicomDS(reinterpret_cast<const char*>(val), length);
            break;
        case 0x7FE00010: // PixelData
            if (length > 0)
            {
                pixelData.resize(length);
                memcpy(pixelData.data(), val, length);
            }
            break;
        default:
            break;
        }

        pos += length;
    }

    // In header-only mode success means the file has a valid DICOM magic header.
    // In full mode we also require pixel data to be present.
    if (headerOnly)
        return true; // DICOM magic was already validated before entering the loop
    return !pixelData.empty() && meta.rows > 0 && meta.cols > 0;
}

//------------------------------------------------------------------------------
// Convert raw pixel data to 8-bit luminance
//------------------------------------------------------------------------------
static bool convertToLuminance8(const DicomMetadata& meta,
                                const vector<uint8_t>& rawPixels,
                                vector<uint8_t>& output)
{
    int width  = meta.cols;
    int height = meta.rows;
    int numPix = width * height;

    output.resize((size_t)numPix);

    if (meta.bitsAllocated == 8)
    {
        // Direct 8-bit copy
        if ((int)rawPixels.size() < numPix) return false;
        memcpy(output.data(), rawPixels.data(), (size_t)numPix);
    }
    else if (meta.bitsAllocated == 16)
    {
        if ((int)rawPixels.size() < numPix * 2) return false;

        // Extract 16-bit values
        vector<int32_t> values(numPix);
        if (meta.pixelRepr == 1)
        {
            // Signed 16-bit
            for (int i = 0; i < numPix; ++i)
            {
                int16_t v = (int16_t)readLE16(&rawPixels[(size_t)i * 2]);
                values[i] = (int32_t)v;
            }
        }
        else
        {
            // Unsigned 16-bit
            for (int i = 0; i < numPix; ++i)
            {
                values[i] = (int32_t)readLE16(&rawPixels[(size_t)i * 2]);
            }
        }

        // Apply windowing to map to [0, 255]
        double wc, ww;
        if (meta.hasWindowCenter && meta.hasWindowWidth &&
            meta.windowWidth > 0.0)
        {
            wc = meta.windowCenter;
            ww = meta.windowWidth;
        }
        else
        {
            // No window: find min/max and use full range
            int32_t minVal = values[0];
            int32_t maxVal = values[0];
            for (int i = 1; i < numPix; ++i)
            {
                if (values[i] < minVal) minVal = values[i];
                if (values[i] > maxVal) maxVal = values[i];
            }
            if (maxVal == minVal)
            {
                // Flat image
                memset(output.data(), 128, (size_t)numPix);
                return true;
            }
            ww = (double)(maxVal - minVal);
            wc = (double)(minVal + maxVal) / 2.0;
        }

        double lower = wc - 0.5 * ww;
        double upper = wc + 0.5 * ww;
        double scale = 255.0 / ww;

        for (int i = 0; i < numPix; ++i)
        {
            double v = (double)values[i];
            // Apply rescale if present
            v = v * meta.rescaleSlope + meta.rescaleIntercept;

            uint8_t pixel;
            if (v <= lower)
                pixel = 0;
            else if (v >= upper)
                pixel = 255;
            else
                pixel = (uint8_t)((v - lower) * scale + 0.5);

            output[i] = meta.invertPixels ? (255 - pixel) : pixel;
        }
    }
    else
    {
        // Unsupported bit depth
        return false;
    }

    if (meta.bitsAllocated == 8 && meta.invertPixels)
    {
        for (int i = 0; i < numPix; ++i)
            output[i] = 255 - output[i];
    }

    return true;
}

} // anonymous namespace


//==============================================================================
/*!
    Load a DICOM image file into a cImage as GL_LUMINANCE 8-bit.

    \param  a_image     Pointer to the cImage object to populate.
    \param  a_filename  Path to the DICOM file.

    \return __true__ if loaded successfully, __false__ otherwise.
*/
//==============================================================================
bool cLoadFileDCM(cImage* a_image, const string& a_filename)
{
    if (a_image == nullptr) return false;

    DicomMetadata meta;
    vector<uint8_t> rawPixels;

    if (!parseDicomFile(a_filename, meta, rawPixels)) return false;

    vector<uint8_t> luma;
    if (!convertToLuminance8(meta, rawPixels, luma)) return false;

    int width  = meta.cols;
    int height = meta.rows;

    if (!a_image->allocate((unsigned int)width, (unsigned int)height,
                           GL_LUMINANCE, GL_UNSIGNED_BYTE))
        return false;

    unsigned char* dst = a_image->getData();
    if (dst == nullptr) return false;

    // DICOM images are stored top-to-bottom; OpenGL expects bottom-to-top.
    // Flip vertically when copying into the cImage buffer.
    for (int row = 0; row < height; ++row)
    {
        int srcRow = height - 1 - row;
        memcpy(dst + (size_t)row * width,
               luma.data() + (size_t)srcRow * width,
               (size_t)width);
    }

    return true;
}


//==============================================================================
/*!
    Read slice location metadata from a DICOM file header for sorting.

    \param  a_filename  Path to the DICOM file.

    \return Slice location value (SliceLocation tag), or InstanceNumber * 1000
            if SliceLocation is absent, or 0.0 on failure.
*/
//==============================================================================
double cGetDicomSliceLocation(const string& a_filename)
{
    DicomMetadata meta;
    vector<uint8_t> rawPixels;

    // Use header-only mode: skip pixel data for efficiency
    if (!parseDicomFile(a_filename, meta, rawPixels, true))
        return 0.0;

    if (meta.hasSliceLocation)
        return meta.sliceLocation;

    // Fall back to instance number.
    // Multiply by 1000 to keep instance numbers in a range well above typical
    // SliceLocation values (which are in millimetres, usually -500 to +500) so
    // that the two fallback keys don't interleave when mixed data is present.
    return (double)meta.instanceNumber * 1000.0;
}

//------------------------------------------------------------------------------
} // namespace chai3d
//------------------------------------------------------------------------------
