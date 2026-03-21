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

/*!
    \brief
    Minimal DICOM reader supporting:
      - Explicit VR Little Endian  (transfer syntax 1.2.840.10008.1.2.1)
      - Implicit VR Little Endian  (transfer syntax 1.2.840.10008.1.2)
      - 8-bit and 16-bit monochrome pixel data
      - Automatic exposure/window normalisation to 8-bit GL_LUMINANCE output
*/

//------------------------------------------------------------------------------
#include "files/CFileImageDCM.h"
//------------------------------------------------------------------------------
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
//------------------------------------------------------------------------------
using namespace std;
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
namespace chai3d {
//------------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

// Read a little-endian 16-bit unsigned integer from a buffer.
static inline uint16_t readU16LE(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// Read a little-endian 32-bit unsigned integer from a buffer.
static inline uint32_t readU32LE(const uint8_t* p)
{
    return  static_cast<uint32_t>(p[0])        |
           (static_cast<uint32_t>(p[1]) <<  8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Returns true when the two-character VR code uses a 4-byte length field
// (with 2 reserved bytes before it) in Explicit VR mode.
static inline bool vrHas4ByteLength(const char vr[2])
{
    // These VRs always use a 4-byte length: OB OD OF OL OW SQ UC UN UR UT
    static const char* longVRs[] = {
        "OB","OD","OF","OL","OW","SQ","UC","UN","UR","UT", nullptr
    };
    for (int i = 0; longVRs[i]; ++i)
    {
        if (vr[0] == longVRs[i][0] && vr[1] == longVRs[i][1])
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// DICOM data set parser state
// ---------------------------------------------------------------------------
struct DicomState
{
    const uint8_t* data;
    size_t         size;
    size_t         pos;
    bool           explicitVR;      // true = Explicit VR Little Endian
};

// Peek the tag at the current position without advancing.
static bool peekTag(const DicomState& s, uint16_t& group, uint16_t& element)
{
    if (s.pos + 4 > s.size) return false;
    group   = readU16LE(s.data + s.pos);
    element = readU16LE(s.data + s.pos + 2);
    return true;
}

// Read the next data element.
// Returns false when the file is exhausted or malformed.
static bool readElement(DicomState& s,
                        uint16_t& group,
                        uint16_t& element,
                        char      vr[2],
                        uint32_t& length,
                        const uint8_t*& valuePtr)
{
    if (s.pos + 4 > s.size) return false;

    group   = readU16LE(s.data + s.pos);
    element = readU16LE(s.data + s.pos + 2);
    s.pos  += 4;

    if (s.explicitVR)
    {
        if (s.pos + 2 > s.size) return false;
        vr[0] = static_cast<char>(s.data[s.pos]);
        vr[1] = static_cast<char>(s.data[s.pos + 1]);
        s.pos += 2;

        if (vrHas4ByteLength(vr))
        {
            // 2 reserved bytes + 4-byte length
            if (s.pos + 6 > s.size) return false;
            s.pos += 2;   // skip reserved
            length = readU32LE(s.data + s.pos);
            s.pos += 4;
        }
        else
        {
            // 2-byte length
            if (s.pos + 2 > s.size) return false;
            length = readU16LE(s.data + s.pos);
            s.pos += 2;
        }
    }
    else
    {
        // Implicit VR: no VR bytes, 4-byte length
        vr[0] = vr[1] = '\0';
        if (s.pos + 4 > s.size) return false;
        length = readU32LE(s.data + s.pos);
        s.pos += 4;
    }

    // 0xFFFFFFFF is the undefined length sentinel for SQ / encapsulated pixel data.
    if (length == 0xFFFFFFFFu)
    {
        valuePtr = s.data + s.pos;
        // Caller must handle undefined-length items specially;
        // we just leave pos where it is and return.
        return true;
    }

    if (s.pos + length > s.size) return false;
    valuePtr  = s.data + s.pos;
    s.pos    += length;
    return true;
}

// ---------------------------------------------------------------------------
// Parse DS (Decimal String) tag value to double.
// ---------------------------------------------------------------------------
static bool parseDSValue(const uint8_t* data, uint32_t length, double& out)
{
    if (length == 0) return false;
    string s(reinterpret_cast<const char*>(data), length);
    // DS may contain multiple values separated by '\'; take the first
    size_t bsl = s.find('\\');
    if (bsl != string::npos) s = s.substr(0, bsl);
    // strip padding
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == string::npos) return false;
    s = s.substr(start);
    istringstream iss(s);
    iss >> out;
    return !iss.fail();
}

// ---------------------------------------------------------------------------
// Normalise a 16-bit pixel buffer to 8-bit using min/max windowing.
// ---------------------------------------------------------------------------
static void normalise16to8(const uint16_t* src,
                           uint8_t*        dst,
                           size_t          count,
                           bool            isSigned)
{
    if (count == 0) return;

    if (isSigned)
    {
        const int16_t* ssrc = reinterpret_cast<const int16_t*>(src);
        int16_t lo = ssrc[0], hi = ssrc[0];
        for (size_t i = 1; i < count; ++i)
        {
            if (ssrc[i] < lo) lo = ssrc[i];
            if (ssrc[i] > hi) hi = ssrc[i];
        }
        int32_t range = static_cast<int32_t>(hi) - static_cast<int32_t>(lo);
        if (range <= 0) range = 1;
        for (size_t i = 0; i < count; ++i)
        {
            int32_t v = (static_cast<int32_t>(ssrc[i]) - lo) * 255 / range;
            dst[i] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    else
    {
        uint16_t lo = src[0], hi = src[0];
        for (size_t i = 1; i < count; ++i)
        {
            if (src[i] < lo) lo = src[i];
            if (src[i] > hi) hi = src[i];
        }
        uint32_t range = static_cast<uint32_t>(hi) - static_cast<uint32_t>(lo);
        if (range == 0) range = 1;
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t v = (static_cast<uint32_t>(src[i]) - lo) * 255u / range;
            dst[i] = static_cast<uint8_t>(v > 255u ? 255u : v);
        }
    }
}

} // anonymous namespace


// ---------------------------------------------------------------------------
// cLoadFileDCM  –  public API
// ---------------------------------------------------------------------------

/*!
    Loads a DICOM (.dcm) image file into a cImage structure.

    Supports:
      - Explicit VR Little Endian  (1.2.840.10008.1.2.1, default / most common)
      - Implicit VR Little Endian  (1.2.840.10008.1.2)
      - 8-bit monochrome (stored as-is, GL_LUMINANCE / GL_UNSIGNED_BYTE)
      - 16-bit monochrome (normalised to 8-bit, GL_LUMINANCE / GL_UNSIGNED_BYTE)

    \param  a_image     Destination cImage object.
    \param  a_filename  Full path to the .dcm file.

    \return __true__ on success, __false__ otherwise.
*/
bool cLoadFileDCM(cImage* a_image, const std::string& a_filename)
{
    if (!a_image) return false;

    // ------------------------------------------------------------------
    // 1.  Read the whole file into memory.
    // ------------------------------------------------------------------
    ifstream file(a_filename.c_str(), ios::binary | ios::ate);
    if (!file) return false;

    streamsize fileSize = file.tellg();
    if (fileSize < 132) return false;   // too small to be a valid DICOM file
    file.seekg(0, ios::beg);

    vector<uint8_t> buf(static_cast<size_t>(fileSize));
    if (!file.read(reinterpret_cast<char*>(buf.data()), fileSize)) return false;
    file.close();

    const uint8_t* raw  = buf.data();
    const size_t   total = buf.size();

    // ------------------------------------------------------------------
    // 2.  Validate the DICOM preamble ("DICM" at offset 128).
    // ------------------------------------------------------------------
    // Some older DICOM files lack the preamble.  We try without it below
    // if the standard header is missing.
    bool hasPreamble = (total >= 132 &&
                        raw[128]=='D' && raw[129]=='I' &&
                        raw[130]=='C' && raw[131]=='M');

    // ------------------------------------------------------------------
    // 3.  Parse the File Meta Information group (0002,xxxx) which is
    //     always Explicit VR Little Endian, even for implicit data sets.
    //     Extract the Transfer Syntax UID (0002,0010).
    // ------------------------------------------------------------------
    bool explicitVR = true;  // default assumption
    string transferSyntax;

    if (hasPreamble)
    {
        DicomState meta;
        meta.data       = raw;
        meta.size       = total;
        meta.pos        = 132;
        meta.explicitVR = true;   // meta group is always explicit

        // Read only group 0002 elements.
        while (meta.pos < total)
        {
            uint16_t g, e;
            if (!peekTag(meta, g, e)) break;
            if (g != 0x0002) break;   // end of meta group

            char      vr[2];
            uint32_t  len = 0;
            const uint8_t* vp = nullptr;
            if (!readElement(meta, g, e, vr, len, vp)) break;
            if (vp == nullptr) break;

            if (g == 0x0002 && e == 0x0010 && len > 0)
            {
                // Transfer Syntax UID – strip any trailing NUL / space
                transferSyntax.assign(reinterpret_cast<const char*>(vp), len);
                while (!transferSyntax.empty() &&
                       (transferSyntax.back() == '\0' ||
                        transferSyntax.back() == ' '))
                    transferSyntax.pop_back();
            }
        }

        // Determine VR mode from transfer syntax.
        // 1.2.840.10008.1.2   = Implicit VR Little Endian
        // 1.2.840.10008.1.2.1 = Explicit VR Little Endian  (default)
        // 1.2.840.10008.1.2.2 = Explicit VR Big Endian     (rare, not supported)
        if (transferSyntax == "1.2.840.10008.1.2")
            explicitVR = false;
    }

    // ------------------------------------------------------------------
    // 4.  Determine start offset for the actual data set.
    // ------------------------------------------------------------------
    size_t dataSetOffset = 132;
    if (!hasPreamble)
    {
        // No preamble – start scanning from byte 0
        dataSetOffset = 0;
        explicitVR    = true;  // make best-effort assumption
    }

    // ------------------------------------------------------------------
    // 5.  Scan the data set for image-related tags.
    // ------------------------------------------------------------------
    DicomState ds;
    ds.data       = raw;
    ds.size       = total;
    ds.pos        = dataSetOffset;
    ds.explicitVR = explicitVR;

    uint32_t rows           = 0;
    uint32_t cols           = 0;
    uint32_t bitsAllocated  = 16;
    uint32_t pixelRep       = 0;   // 0 = unsigned, 1 = signed
    const uint8_t* pixelPtr = nullptr;
    uint32_t pixelLen       = 0;

    while (ds.pos < total)
    {
        uint16_t g, e;
        if (!peekTag(ds, g, e)) break;

        // Stop before private pixel data padding or items.
        if (g == 0xFFFE) break;

        char      vr[2];
        uint32_t  len = 0;
        const uint8_t* vp = nullptr;
        if (!readElement(ds, g, e, vr, len, vp)) break;

        if (g == 0x0028)
        {
            if (e == 0x0010 && vp && len >= 2)  // Rows
                rows = readU16LE(vp);
            else if (e == 0x0011 && vp && len >= 2)  // Columns
                cols = readU16LE(vp);
            else if (e == 0x0100 && vp && len >= 2)  // Bits Allocated
                bitsAllocated = readU16LE(vp);
            else if (e == 0x0103 && vp && len >= 2)  // Pixel Representation
                pixelRep = readU16LE(vp);
        }
        else if (g == 0x7FE0 && e == 0x0010)  // Pixel Data
        {
            pixelPtr = vp;
            pixelLen = len;
            break;   // We have everything we need.
        }
    }

    // ------------------------------------------------------------------
    // 6.  Validate and build the output cImage.
    // ------------------------------------------------------------------
    if (rows == 0 || cols == 0 || pixelPtr == nullptr) return false;
    if (bitsAllocated != 8 && bitsAllocated != 16)     return false;

    // Allocate output as 8-bit GL_LUMINANCE.
    if (!a_image->allocate(cols, rows, GL_LUMINANCE)) return false;

    uint8_t* dst = a_image->getData();
    if (!dst) return false;

    size_t nPixels = static_cast<size_t>(rows) * static_cast<size_t>(cols);

    if (bitsAllocated == 8)
    {
        // Direct copy – one byte per pixel.
        if (pixelLen < nPixels) return false;
        memcpy(dst, pixelPtr, nPixels);
    }
    else
    {
        // 16-bit – normalise to 8-bit.
        if (pixelLen < nPixels * 2) return false;
        normalise16to8(reinterpret_cast<const uint16_t*>(pixelPtr),
                       dst,
                       nPixels,
                       pixelRep == 1);
    }

    return true;
}


// ---------------------------------------------------------------------------
// cGetDCMSlicePosition  –  public API
// ---------------------------------------------------------------------------

/*!
    Reads the Image Position (Patient) Z coordinate from a DICOM file.
    This is used to sort slices before loading them as a volume.

    \param  a_filename  Full path to the .dcm file.
    \param  a_position  Output: the Z position value.

    \return __true__ if the tag was found and parsed, __false__ otherwise.
*/
bool cGetDCMSlicePosition(const std::string& a_filename, double& a_position)
{
    ifstream file(a_filename.c_str(), ios::binary | ios::ate);
    if (!file) return false;

    streamsize fileSize = file.tellg();
    if (fileSize < 132) return false;
    file.seekg(0, ios::beg);

    vector<uint8_t> buf(static_cast<size_t>(fileSize));
    if (!file.read(reinterpret_cast<char*>(buf.data()), fileSize)) return false;
    file.close();

    const uint8_t* raw   = buf.data();
    const size_t   total = buf.size();

    bool hasPreamble = (total >= 132 &&
                        raw[128]=='D' && raw[129]=='I' &&
                        raw[130]=='C' && raw[131]=='M');

    // Skip File Meta Information to find transfer syntax.
    bool explicitVR = true;
    string transferSyntax;

    if (hasPreamble)
    {
        DicomState meta;
        meta.data       = raw;
        meta.size       = total;
        meta.pos        = 132;
        meta.explicitVR = true;

        while (meta.pos < total)
        {
            uint16_t g, e;
            if (!peekTag(meta, g, e)) break;
            if (g != 0x0002) break;

            char     vr[2];
            uint32_t len = 0;
            const uint8_t* vp = nullptr;
            if (!readElement(meta, g, e, vr, len, vp)) break;
            if (vp == nullptr) break;

            if (g == 0x0002 && e == 0x0010 && len > 0)
            {
                transferSyntax.assign(reinterpret_cast<const char*>(vp), len);
                while (!transferSyntax.empty() &&
                       (transferSyntax.back() == '\0' ||
                        transferSyntax.back() == ' '))
                    transferSyntax.pop_back();
            }
        }
        if (transferSyntax == "1.2.840.10008.1.2")
            explicitVR = false;
    }

    DicomState ds;
    ds.data       = raw;
    ds.size       = total;
    ds.pos        = hasPreamble ? 132 : 0;
    ds.explicitVR = explicitVR;

    // Tag (0020,0032) Image Position (Patient) – DS, three backslash-separated values.
    while (ds.pos < total)
    {
        uint16_t g, e;
        if (!peekTag(ds, g, e)) break;
        if (g == 0xFFFE) break;

        char     vr[2];
        uint32_t len = 0;
        const uint8_t* vp = nullptr;
        if (!readElement(ds, g, e, vr, len, vp)) break;

        if (g == 0x0020 && e == 0x0032 && vp && len > 0)
        {
            // DS value: "X\Y\Z" – we want Z (the third component).
            string s(reinterpret_cast<const char*>(vp), len);
            size_t first  = s.find('\\');
            if (first == string::npos) return false;
            size_t second = s.find('\\', first + 1);
            if (second == string::npos) return false;
            string zStr = s.substr(second + 1);
            // strip trailing padding
            size_t last = zStr.find_last_not_of(" \t\r\n\0");
            if (last != string::npos) zStr = zStr.substr(0, last + 1);
            istringstream iss(zStr);
            iss >> a_position;
            return !iss.fail();
        }

        // Stop at pixel data or after image plane tags.
        if (g == 0x7FE0) break;
    }

    // Fallback: try Instance Number (0020,0013) as a proxy for slice order.
    ds.pos = hasPreamble ? 132 : 0;
    while (ds.pos < total)
    {
        uint16_t g, e;
        if (!peekTag(ds, g, e)) break;
        if (g == 0xFFFE) break;

        char     vr[2];
        uint32_t len = 0;
        const uint8_t* vp = nullptr;
        if (!readElement(ds, g, e, vr, len, vp)) break;

        if (g == 0x0020 && e == 0x0013 && vp && len > 0)
        {
            double val;
            if (parseDSValue(vp, len, val))
            {
                a_position = val;
                return true;
            }
        }
        if (g == 0x7FE0) break;
    }

    return false;
}


//------------------------------------------------------------------------------
} // namespace chai3d
//------------------------------------------------------------------------------
