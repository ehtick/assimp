/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2025, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the following
conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
---------------------------------------------------------------------------
*/

/** @file  ASEParser.cpp
 *  @brief Implementation of the ASE parser class
 */

#ifndef ASSIMP_BUILD_NO_ASE_IMPORTER
#ifndef ASSIMP_BUILD_NO_3DS_IMPORTER

// internal headers
#include "ASELoader.h"
#include "PostProcessing/TextureTransform.h"

#include <assimp/fast_atof.h>
#include <assimp/DefaultLogger.hpp>

namespace Assimp {
using namespace Assimp::ASE;

// ------------------------------------------------------------------------------------------------
// Begin an ASE parsing function

#define AI_ASE_PARSER_INIT() \
    int iDepth = 0;

// ------------------------------------------------------------------------------------------------
// Handle a "top-level" section in the file. EOF is no error in this case.

#define AI_ASE_HANDLE_TOP_LEVEL_SECTION()          \
    else if ('{' == *mFilePtr)                     \
        ++iDepth;                                  \
    else if ('}' == *mFilePtr) {                   \
        if (0 == --iDepth) {                       \
            ++mFilePtr;                            \
            SkipToNextToken();                     \
            return;                                \
        }                                          \
    }                                              \
    if ('\0' == *mFilePtr) {                       \
        return;                                    \
    }                                              \
    if (IsLineEnd(*mFilePtr) && !bLastWasEndLine) {\
        ++iLineNumber;                             \
        bLastWasEndLine = true;                    \
    } else                                         \
        bLastWasEndLine = false;                   \
    ++mFilePtr;

// ------------------------------------------------------------------------------------------------
// Handle a nested section in the file. EOF is an error in this case
// @param level "Depth" of the section
// @param msg Full name of the section (including the asterisk)

#define AI_ASE_HANDLE_SECTION(level, msg)                          \
    if ('{' == *mFilePtr)                                          \
        iDepth++;                                                  \
    else if ('}' == *mFilePtr) {                                   \
        if (0 == --iDepth) {                                       \
            ++mFilePtr;                                            \
            SkipToNextToken();                                     \
            return;                                                \
        }                                                          \
    } else if ('\0' == *mFilePtr) {                                \
        LogError("Encountered unexpected EOL while parsing a " msg \
                 " chunk (Level " level ")");                      \
    }                                                              \
    if (IsLineEnd(*mFilePtr) && !bLastWasEndLine) {                \
        ++iLineNumber;                                             \
        bLastWasEndLine = true;                                    \
    } else                                                         \
        bLastWasEndLine = false;                                   \
    ++mFilePtr;

// ------------------------------------------------------------------------------------------------
Parser::Parser(const char *file, size_t fileLen, unsigned int fileFormatDefault) :
        mFilePtr(nullptr), mEnd (nullptr) {
    ai_assert(file != nullptr);

    mFilePtr = file;
    mEnd = mFilePtr + fileLen;
    iFileFormat = fileFormatDefault;

    // make sure that the color values are invalid
    m_clrBackground.r = get_qnan();
    m_clrAmbient.r = get_qnan();

    // setup some default values
    iLineNumber = 0;
    iFirstFrame = 0;
    iLastFrame = 0;
    iFrameSpeed = 30; // use 30 as default value for this property
    iTicksPerFrame = 1; // use 1 as default value for this property
    bLastWasEndLine = false; // need to handle \r\n seqs due to binary file mapping
}

// ------------------------------------------------------------------------------------------------
void Parser::LogWarning(const char *szWarn) {
    ai_assert(nullptr != szWarn);

    char szTemp[2048];
#if _MSC_VER >= 1400
    sprintf_s(szTemp, "Line %u: %s", iLineNumber, szWarn);
#else
    ai_snprintf(szTemp, sizeof(szTemp), "Line %u: %s", iLineNumber, szWarn);
#endif

    // output the warning to the logger ...
    ASSIMP_LOG_WARN(szTemp);
}

// ------------------------------------------------------------------------------------------------
void Parser::LogInfo(const char *szWarn) {
    ai_assert(nullptr != szWarn);

    char szTemp[1024];
#if _MSC_VER >= 1400
    sprintf_s(szTemp, "Line %u: %s", iLineNumber, szWarn);
#else
    ai_snprintf(szTemp, 1024, "Line %u: %s", iLineNumber, szWarn);
#endif

    // output the information to the logger ...
    ASSIMP_LOG_INFO(szTemp);
}

// ------------------------------------------------------------------------------------------------
AI_WONT_RETURN void Parser::LogError(const char *szWarn) {
    ai_assert(nullptr != szWarn);

    char szTemp[1024];
#if _MSC_VER >= 1400
    sprintf_s(szTemp, "Line %u: %s", iLineNumber, szWarn);
#else
    ai_snprintf(szTemp, 1024, "Line %u: %s", iLineNumber, szWarn);
#endif

    // throw an exception
    throw DeadlyImportError(szTemp);
}

// ------------------------------------------------------------------------------------------------
bool Parser::SkipToNextToken() {
    while (true) {
        char me = *mFilePtr;

        if (mFilePtr == mEnd) {
            return false;
        }

        // increase the line number counter if necessary
        if (IsLineEnd(me) && !bLastWasEndLine) {
            ++iLineNumber;
            bLastWasEndLine = true;
        } else
            bLastWasEndLine = false;
        if ('*' == me || '}' == me || '{' == me) {
            return true;
        }
        if ('\0' == me) {
            return false;
        }

        ++mFilePtr;
    }
}

// ------------------------------------------------------------------------------------------------
bool Parser::SkipSection() {
    // must handle subsections ...
    int iCnt = 0;
    while (true) {
        if ('}' == *mFilePtr) {
            --iCnt;
            if (0 == iCnt) {
                // go to the next valid token ...
                ++mFilePtr;
                SkipToNextToken();
                return true;
            }
        } else if ('{' == *mFilePtr) {
            ++iCnt;
        } else if ('\0' == *mFilePtr) {
            LogWarning("Unable to parse block: Unexpected EOF, closing bracket \'}\' was expected [#1]");
            return false;
        } else if (IsLineEnd(*mFilePtr))
            ++iLineNumber;
        ++mFilePtr;
    }
}

// ------------------------------------------------------------------------------------------------
void Parser::Parse() {
    AI_ASE_PARSER_INIT();
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            // Version should be 200. Validate this ...
            if (TokenMatch(mFilePtr, "3DSMAX_ASCIIEXPORT", 18)) {
                unsigned int fmt;
                ParseLV4MeshLong(fmt);

                if (fmt > 200) {
                    LogWarning("Unknown file format version: *3DSMAX_ASCIIEXPORT should \
                               be <= 200");
                }
                // *************************************************************
                // - fmt will be 0 if we're unable to read the version number
                // there are some faulty files without a version number ...
                // in this case we'll guess the exact file format by looking
                // at the file extension (ASE, ASK, ASC)
                // *************************************************************

                if (fmt) {
                    iFileFormat = fmt;
                }
                continue;
            }
            // main scene information
            if (TokenMatch(mFilePtr, "SCENE", 5)) {
                ParseLV1SceneBlock();
                continue;
            }
            // "group" - no implementation yet, in facte
            // we're just ignoring them for the moment
            if (TokenMatch(mFilePtr, "GROUP", 5)) {
                Parse();
                continue;
            }
            // material list
            if (TokenMatch(mFilePtr, "MATERIAL_LIST", 13)) {
                ParseLV1MaterialListBlock();
                continue;
            }
            // geometric object (mesh)
            if (TokenMatch(mFilePtr, "GEOMOBJECT", 10))

            {
                m_vMeshes.emplace_back("UNNAMED");
                ParseLV1ObjectBlock(m_vMeshes.back());
                continue;
            }
            // helper object = dummy in the hierarchy
            if (TokenMatch(mFilePtr, "HELPEROBJECT", 12))

            {
                m_vDummies.emplace_back();
                ParseLV1ObjectBlock(m_vDummies.back());
                continue;
            }
            // light object
            if (TokenMatch(mFilePtr, "LIGHTOBJECT", 11))

            {
                m_vLights.emplace_back("UNNAMED");
                ParseLV1ObjectBlock(m_vLights.back());
                continue;
            }
            // camera object
            if (TokenMatch(mFilePtr, "CAMERAOBJECT", 12)) {
                m_vCameras.emplace_back("UNNAMED");
                ParseLV1ObjectBlock(m_vCameras.back());
                continue;
            }
            // comment - print it on the console
            if (TokenMatch(mFilePtr, "COMMENT", 7)) {
                std::string out = "<unknown>";
                ParseString(out, "*COMMENT");
                LogInfo(("Comment: " + out).c_str());
                continue;
            }
            // ASC bone weights
            if (AI_ASE_IS_OLD_FILE_FORMAT() && TokenMatch(mFilePtr, "MESH_SOFTSKINVERTS", 18)) {
                ParseLV1SoftSkinBlock();
            }
        }
        AI_ASE_HANDLE_TOP_LEVEL_SECTION();
    }
}

// ------------------------------------------------------------------------------------------------
void Parser::ParseLV1SoftSkinBlock() {
    // TODO: fix line counting here

    // **************************************************************
    // The soft skin block is formatted differently. There are no
    // nested sections supported and the single elements aren't
    // marked by keywords starting with an asterisk.

    /**
    FORMAT BEGIN

    *MESH_SOFTSKINVERTS {
    <nodename>
    <number of vertices>

    [for <number of vertices> times:]
        <number of weights> [for <number of weights> times:] <bone name> <weight>
    }

    FORMAT END
    */
    // **************************************************************
    while (true) {
        if (*mFilePtr == '}') {
            ++mFilePtr;
            return;
        } else if (*mFilePtr == '\0')
            return;
        else if (*mFilePtr == '{')
            ++mFilePtr;

        else // if (!IsSpace(*filePtr) && !IsLineEnd(*filePtr))
        {
            ASE::Mesh *curMesh = nullptr;
            unsigned int numVerts = 0;

            const char *sz = mFilePtr;
            while (!IsSpaceOrNewLine(*mFilePtr)) {
                ++mFilePtr;
            }

            const unsigned int diff = (unsigned int)(mFilePtr - sz);
            if (diff) {
                std::string name = std::string(sz, diff);
                for (std::vector<ASE::Mesh>::iterator it = m_vMeshes.begin();
                        it != m_vMeshes.end(); ++it) {
                    if ((*it).mName == name) {
                        curMesh = &(*it);
                        break;
                    }
                }
                if (!curMesh) {
                    LogWarning("Encountered unknown mesh in *MESH_SOFTSKINVERTS section");

                    // Skip the mesh data - until we find a new mesh
                    // or the end of the *MESH_SOFTSKINVERTS section
                    while (true) {
                        SkipSpacesAndLineEnd(&mFilePtr, mEnd);
                        if (*mFilePtr == '}') {
                            ++mFilePtr;
                            return;
                        } else if (!IsNumeric(*mFilePtr))
                            break;

                        SkipLine(&mFilePtr, mEnd);
                    }
                } else {
                    SkipSpacesAndLineEnd(&mFilePtr, mEnd);
                    ParseLV4MeshLong(numVerts);

                    // Reserve enough storage
                    curMesh->mBoneVertices.reserve(numVerts);

                    for (unsigned int i = 0; i < numVerts; ++i) {
                        SkipSpacesAndLineEnd(&mFilePtr, mEnd);
                        unsigned int numWeights;
                        ParseLV4MeshLong(numWeights);

                        curMesh->mBoneVertices.emplace_back();
                        ASE::BoneVertex &vert = curMesh->mBoneVertices.back();

                        // Reserve enough storage
                        vert.mBoneWeights.reserve(numWeights);

                        std::string bone;
                        for (unsigned int w = 0; w < numWeights; ++w) {
                            bone.clear();
                            ParseString(bone, "*MESH_SOFTSKINVERTS.Bone");

                            // Find the bone in the mesh's list
                            std::pair<int, ai_real> me;
                            me.first = -1;

                            for (unsigned int n = 0; n < curMesh->mBones.size(); ++n) {
                                if (curMesh->mBones[n].mName == bone) {
                                    me.first = n;
                                    break;
                                }
                            }
                            if (-1 == me.first) {
                                // We don't have this bone yet, so add it to the list
                                me.first = static_cast<int>(curMesh->mBones.size());
                                curMesh->mBones.emplace_back(bone);
                            }
                            ParseLV4MeshReal(me.second);

                            // Add the new bone weight to list
                            vert.mBoneWeights.push_back(me);
                        }
                    }
                }
            }
        }
        if (*mFilePtr == '\0')
            return;
        ++mFilePtr;
        SkipSpacesAndLineEnd(&mFilePtr, mEnd);
    }
}

// ------------------------------------------------------------------------------------------------
void Parser::ParseLV1SceneBlock() {
    AI_ASE_PARSER_INIT();
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;
            if (TokenMatch(mFilePtr, "SCENE_BACKGROUND_STATIC", 23))

            {
                // parse a color triple and assume it is really the bg color
                ParseLV4MeshFloatTriple(&m_clrBackground.r);
                continue;
            }
            if (TokenMatch(mFilePtr, "SCENE_AMBIENT_STATIC", 20))

            {
                // parse a color triple and assume it is really the bg color
                ParseLV4MeshFloatTriple(&m_clrAmbient.r);
                continue;
            }
            if (TokenMatch(mFilePtr, "SCENE_FIRSTFRAME", 16)) {
                ParseLV4MeshLong(iFirstFrame);
                continue;
            }
            if (TokenMatch(mFilePtr, "SCENE_LASTFRAME", 15)) {
                ParseLV4MeshLong(iLastFrame);
                continue;
            }
            if (TokenMatch(mFilePtr, "SCENE_FRAMESPEED", 16)) {
                ParseLV4MeshLong(iFrameSpeed);
                continue;
            }
            if (TokenMatch(mFilePtr, "SCENE_TICKSPERFRAME", 19)) {
                ParseLV4MeshLong(iTicksPerFrame);
                continue;
            }
        }
        AI_ASE_HANDLE_TOP_LEVEL_SECTION();
    }
}

// ------------------------------------------------------------------------------------------------
void Parser::ParseLV1MaterialListBlock() {
    AI_ASE_PARSER_INIT();

    unsigned int iMaterialCount = 0;
    unsigned int iOldMaterialCount = (unsigned int)m_vMaterials.size();
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;
            if (TokenMatch(mFilePtr, "MATERIAL_COUNT", 14)) {
                ParseLV4MeshLong(iMaterialCount);

                if (UINT_MAX - iOldMaterialCount < iMaterialCount) {
                    LogWarning("Out of range: material index is too large");
                    return;
                }

                // now allocate enough storage to hold all materials
                m_vMaterials.resize(iOldMaterialCount + iMaterialCount, Material("INVALID"));
                continue;
            }
            if (TokenMatch(mFilePtr, "MATERIAL", 8)) {
                // ensure we have at least one material allocated
                if (iMaterialCount == 0) {
                    LogWarning("*MATERIAL_COUNT unspecified or 0");
                    iMaterialCount = 1;
                    m_vMaterials.resize(iOldMaterialCount + iMaterialCount, Material("INVALID"));
                }

                unsigned int iIndex = 0;
                ParseLV4MeshLong(iIndex);

                if (iIndex >= iMaterialCount) {
                    LogWarning("Out of range: material index is too large");
                    iIndex = iMaterialCount - 1;
                }

                // get a reference to the material
                Material &sMat = m_vMaterials[iIndex + iOldMaterialCount];
                // parse the material block
                ParseLV2MaterialBlock(sMat);
                continue;
            }
            if( iDepth == 1 ){
                // CRUDE HACK: support missing brace after "Ascii Scene Exporter v2.51"
                LogWarning("Missing closing brace in material list");
                --mFilePtr;
                return;
            }
        }
        AI_ASE_HANDLE_TOP_LEVEL_SECTION();
    }
}

// ------------------------------------------------------------------------------------------------
void Parser::ParseLV2MaterialBlock(ASE::Material &mat) {
    AI_ASE_PARSER_INIT();

    unsigned int iNumSubMaterials = 0;
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;
            if (TokenMatch(mFilePtr, "MATERIAL_NAME", 13)) {
                if (!ParseString(mat.mName, "*MATERIAL_NAME"))
                    SkipToNextToken();
                continue;
            }
            // ambient material color
            if (TokenMatch(mFilePtr, "MATERIAL_AMBIENT", 16)) {
                ParseLV4MeshFloatTriple(&mat.mAmbient.r);
                continue;
            }
            // diffuse material color
            if (TokenMatch(mFilePtr, "MATERIAL_DIFFUSE", 16)) {
                ParseLV4MeshFloatTriple(&mat.mDiffuse.r);
                continue;
            }
            // specular material color
            if (TokenMatch(mFilePtr, "MATERIAL_SPECULAR", 17)) {
                ParseLV4MeshFloatTriple(&mat.mSpecular.r);
                continue;
            }
            // material shading type
            if (TokenMatch(mFilePtr, "MATERIAL_SHADING", 16)) {
                if (TokenMatch(mFilePtr, "Blinn", 5)) {
                    mat.mShading = Discreet3DS::Blinn;
                } else if (TokenMatch(mFilePtr, "Phong", 5)) {
                    mat.mShading = Discreet3DS::Phong;
                } else if (TokenMatch(mFilePtr, "Flat", 4)) {
                    mat.mShading = Discreet3DS::Flat;
                } else if (TokenMatch(mFilePtr, "Wire", 4)) {
                    mat.mShading = Discreet3DS::Wire;
                } else {
                    // assume gouraud shading
                    mat.mShading = Discreet3DS::Gouraud;
                    SkipToNextToken();
                }
                continue;
            }
            // material transparency
            if (TokenMatch(mFilePtr, "MATERIAL_TRANSPARENCY", 21)) {
                ParseLV4MeshReal(mat.mTransparency);
                mat.mTransparency = ai_real(1.0) - mat.mTransparency;
                continue;
            }
            // material self illumination
            if (TokenMatch(mFilePtr, "MATERIAL_SELFILLUM", 18)) {
                ai_real f = 0.0;
                ParseLV4MeshReal(f);

                mat.mEmissive.r = f;
                mat.mEmissive.g = f;
                mat.mEmissive.b = f;
                continue;
            }
            // material shininess
            if (TokenMatch(mFilePtr, "MATERIAL_SHINE", 14)) {
                ParseLV4MeshReal(mat.mSpecularExponent);
                mat.mSpecularExponent *= 15;
                continue;
            }
            // two-sided material
            if (TokenMatch(mFilePtr, "MATERIAL_TWOSIDED", 17)) {
                mat.mTwoSided = true;
                continue;
            }
            // material shininess strength
            if (TokenMatch(mFilePtr, "MATERIAL_SHINESTRENGTH", 22)) {
                ParseLV4MeshReal(mat.mShininessStrength);
                continue;
            }
            // diffuse color map
            if (TokenMatch(mFilePtr, "MAP_DIFFUSE", 11)) {
                // parse the texture block
                ParseLV3MapBlock(mat.sTexDiffuse);
                continue;
            }
            // ambient color map
            if (TokenMatch(mFilePtr, "MAP_AMBIENT", 11)) {
                // parse the texture block
                ParseLV3MapBlock(mat.sTexAmbient);
                continue;
            }
            // specular color map
            if (TokenMatch(mFilePtr, "MAP_SPECULAR", 12)) {
                // parse the texture block
                ParseLV3MapBlock(mat.sTexSpecular);
                continue;
            }
            // opacity map
            if (TokenMatch(mFilePtr, "MAP_OPACITY", 11)) {
                // parse the texture block
                ParseLV3MapBlock(mat.sTexOpacity);
                continue;
            }
            // emissive map
            if (TokenMatch(mFilePtr, "MAP_SELFILLUM", 13)) {
                // parse the texture block
                ParseLV3MapBlock(mat.sTexEmissive);
                continue;
            }
            // bump map
            if (TokenMatch(mFilePtr, "MAP_BUMP", 8)) {
                // parse the texture block
                ParseLV3MapBlock(mat.sTexBump);
            }
            // specular/shininess map
            if (TokenMatch(mFilePtr, "MAP_SHINESTRENGTH", 17)) {
                // parse the texture block
                ParseLV3MapBlock(mat.sTexShininess);
                continue;
            }
            // number of submaterials
            if (TokenMatch(mFilePtr, "NUMSUBMTLS", 10)) {
                ParseLV4MeshLong(iNumSubMaterials);

                // allocate enough storage
                mat.avSubMaterials.resize(iNumSubMaterials, Material("INVALID SUBMATERIAL"));
            }
            // submaterial chunks
            if (TokenMatch(mFilePtr, "SUBMATERIAL", 11)) {
                // ensure we have at least one material allocated
                if (iNumSubMaterials == 0) {
                    LogWarning("*NUMSUBMTLS unspecified or 0");
                    iNumSubMaterials = 1;
                    mat.avSubMaterials.resize(iNumSubMaterials, Material("INVALID SUBMATERIAL"));
                }

                unsigned int iIndex = 0;
                ParseLV4MeshLong(iIndex);

                if (iIndex >= iNumSubMaterials) {
                    LogWarning("Out of range: submaterial index is too large");
                    iIndex = iNumSubMaterials - 1;
                }

                // get a reference to the material
                if (iIndex < mat.avSubMaterials.size()) {
                    Material &sMat = mat.avSubMaterials[iIndex];

                    // parse the material block
                    ParseLV2MaterialBlock(sMat);
                }

                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("2", "*MATERIAL");
    }
}

// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3MapBlock(Texture &map) {
    AI_ASE_PARSER_INIT();

    // ***********************************************************
    // *BITMAP should not be there if *MAP_CLASS is not BITMAP,
    // but we need to expect that case ... if the path is
    // empty the texture won't be used later.
    // ***********************************************************
    bool parsePath = true;
    std::string temp;
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;
            // type of map
            if (TokenMatch(mFilePtr, "MAP_CLASS", 9)) {
                temp.clear();
                if (!ParseString(temp, "*MAP_CLASS"))
                    SkipToNextToken();
                if (temp != "Bitmap" && temp != "Normal Bump") {
                    ASSIMP_LOG_WARN("ASE: Skipping unknown map type: ", temp);
                    parsePath = false;
                }
                continue;
            }
            // path to the texture
            if (parsePath && TokenMatch(mFilePtr, "BITMAP", 6)) {
                if (!ParseString(map.mMapName, "*BITMAP"))
                    SkipToNextToken();

                if (map.mMapName == "None") {
                    // Files with 'None' as map name are produced by
                    // an Maja to ASE exporter which name I forgot ..
                    ASSIMP_LOG_WARN("ASE: Skipping invalid map entry");
                    map.mMapName = std::string();
                }

                continue;
            }
            // offset on the u axis
            if (TokenMatch(mFilePtr, "UVW_U_OFFSET", 12)) {
                ParseLV4MeshReal(map.mOffsetU);
                continue;
            }
            // offset on the v axis
            if (TokenMatch(mFilePtr, "UVW_V_OFFSET", 12)) {
                ParseLV4MeshReal(map.mOffsetV);
                continue;
            }
            // tiling on the u axis
            if (TokenMatch(mFilePtr, "UVW_U_TILING", 12)) {
                ParseLV4MeshReal(map.mScaleU);
                continue;
            }
            // tiling on the v axis
            if (TokenMatch(mFilePtr, "UVW_V_TILING", 12)) {
                ParseLV4MeshReal(map.mScaleV);
                continue;
            }
            // rotation around the z-axis
            if (TokenMatch(mFilePtr, "UVW_ANGLE", 9)) {
                ParseLV4MeshReal(map.mRotation);
                continue;
            }
            // map blending factor
            if (TokenMatch(mFilePtr, "MAP_AMOUNT", 10)) {
                ParseLV4MeshReal(map.mTextureBlend);
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*MAP_XXXXXX");
    }
}

// ------------------------------------------------------------------------------------------------
bool Parser::ParseString(std::string &out, const char *szName) {
    char szBuffer[1024];
    if (!SkipSpaces(&mFilePtr, mEnd)) {

        ai_snprintf(szBuffer, 1024, "Unable to parse %s block: Unexpected EOL", szName);
        LogWarning(szBuffer);
        return false;
    }
    // there must be '"'
    if ('\"' != *mFilePtr) {

        ai_snprintf(szBuffer, 1024, "Unable to parse %s block: Strings are expected "
                                    "to be enclosed in double quotation marks",
                szName);
        LogWarning(szBuffer);
        return false;
    }
    ++mFilePtr;
    const char *sz = mFilePtr;
    while (true) {
        if ('\"' == *sz)
            break;
        else if ('\0' == *sz) {
            ai_snprintf(szBuffer, 1024, "Unable to parse %s block: Strings are expected to "
                                        "be enclosed in double quotation marks but EOF was reached before "
                                        "a closing quotation mark was encountered",
                    szName);
            LogWarning(szBuffer);
            return false;
        }
        sz++;
    }
    out = std::string(mFilePtr, (uintptr_t)sz - (uintptr_t)mFilePtr);
    mFilePtr = sz + 1;
    return true;
}

// ------------------------------------------------------------------------------------------------
void Parser::ParseLV1ObjectBlock(ASE::BaseNode &node) {
    AI_ASE_PARSER_INIT();
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            // first process common tokens such as node name and transform
            // name of the mesh/node
            if (TokenMatch(mFilePtr, "NODE_NAME", 9)) {
                if (!ParseString(node.mName, "*NODE_NAME"))
                    SkipToNextToken();
                continue;
            }
            // name of the parent of the node
            if (TokenMatch(mFilePtr, "NODE_PARENT", 11)) {
                if (!ParseString(node.mParent, "*NODE_PARENT"))
                    SkipToNextToken();
                continue;
            }
            // transformation matrix of the node
            if (TokenMatch(mFilePtr, "NODE_TM", 7)) {
                ParseLV2NodeTransformBlock(node);
                continue;
            }
            // animation data of the node
            if (TokenMatch(mFilePtr, "TM_ANIMATION", 12)) {
                ParseLV2AnimationBlock(node);
                continue;
            }

            if (node.mType == BaseNode::Light) {
                // light settings
                if (TokenMatch(mFilePtr, "LIGHT_SETTINGS", 14)) {
                    ParseLV2LightSettingsBlock((ASE::Light &)node);
                    continue;
                }
                // type of the light source
                if (TokenMatch(mFilePtr, "LIGHT_TYPE", 10)) {
                    if (!ASSIMP_strincmp("omni", mFilePtr, 4)) {
                        ((ASE::Light &)node).mLightType = ASE::Light::OMNI;
                    } else if (!ASSIMP_strincmp("target", mFilePtr, 6)) {
                        ((ASE::Light &)node).mLightType = ASE::Light::TARGET;
                    } else if (!ASSIMP_strincmp("free", mFilePtr, 4)) {
                        ((ASE::Light &)node).mLightType = ASE::Light::FREE;
                    } else if (!ASSIMP_strincmp("directional", mFilePtr, 11)) {
                        ((ASE::Light &)node).mLightType = ASE::Light::DIRECTIONAL;
                    } else {
                        LogWarning("Unknown kind of light source");
                    }
                    continue;
                }
            } else if (node.mType == BaseNode::Camera) {
                // Camera settings
                if (TokenMatch(mFilePtr, "CAMERA_SETTINGS", 15)) {
                    ParseLV2CameraSettingsBlock((ASE::Camera &)node);
                    continue;
                } else if (TokenMatch(mFilePtr, "CAMERA_TYPE", 11)) {
                    if (!ASSIMP_strincmp("target", mFilePtr, 6)) {
                        ((ASE::Camera &)node).mCameraType = ASE::Camera::TARGET;
                    } else if (!ASSIMP_strincmp("free", mFilePtr, 4)) {
                        ((ASE::Camera &)node).mCameraType = ASE::Camera::FREE;
                    } else {
                        LogWarning("Unknown kind of camera");
                    }
                    continue;
                }
            } else if (node.mType == BaseNode::Mesh) {
                // mesh data
                // FIX: Older files use MESH_SOFTSKIN
                if (TokenMatch(mFilePtr, "MESH", 4) ||
                        TokenMatch(mFilePtr, "MESH_SOFTSKIN", 13)) {
                    ParseLV2MeshBlock((ASE::Mesh &)node);
                    continue;
                }
                // mesh material index
                if (TokenMatch(mFilePtr, "MATERIAL_REF", 12)) {
                    ParseLV4MeshLong(((ASE::Mesh &)node).iMaterialIndex);
                    continue;
                }
            }
        }
        AI_ASE_HANDLE_TOP_LEVEL_SECTION();
    }
}

// ------------------------------------------------------------------------------------------------
void Parser::ParseLV2CameraSettingsBlock(ASE::Camera &camera) {
    AI_ASE_PARSER_INIT();
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;
            if (TokenMatch(mFilePtr, "CAMERA_NEAR", 11)) {
                ParseLV4MeshReal(camera.mNear);
                continue;
            }
            if (TokenMatch(mFilePtr, "CAMERA_FAR", 10)) {
                ParseLV4MeshReal(camera.mFar);
                continue;
            }
            if (TokenMatch(mFilePtr, "CAMERA_FOV", 10)) {
                ParseLV4MeshReal(camera.mFOV);
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("2", "CAMERA_SETTINGS");
    }
}

// ------------------------------------------------------------------------------------------------
void Parser::ParseLV2LightSettingsBlock(ASE::Light &light) {
    AI_ASE_PARSER_INIT();
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;
            if (TokenMatch(mFilePtr, "LIGHT_COLOR", 11)) {
                ParseLV4MeshFloatTriple(&light.mColor.r);
                continue;
            }
            if (TokenMatch(mFilePtr, "LIGHT_INTENS", 12)) {
                ParseLV4MeshReal(light.mIntensity);
                continue;
            }
            if (TokenMatch(mFilePtr, "LIGHT_HOTSPOT", 13)) {
                ParseLV4MeshReal(light.mAngle);
                continue;
            }
            if (TokenMatch(mFilePtr, "LIGHT_FALLOFF", 13)) {
                ParseLV4MeshReal(light.mFalloff);
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("2", "LIGHT_SETTINGS");
    }
}

// ------------------------------------------------------------------------------------------------
void Parser::ParseLV2AnimationBlock(ASE::BaseNode &mesh) {
    AI_ASE_PARSER_INIT();

    ASE::Animation *anim = &mesh.mAnim;
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;
            if (TokenMatch(mFilePtr, "NODE_NAME", 9)) {
                std::string temp;
                if (!ParseString(temp, "*NODE_NAME"))
                    SkipToNextToken();

                // If the name of the node contains .target it
                // represents an animated camera or spot light
                // target.
                if (std::string::npos != temp.find(".Target")) {
                    if ((mesh.mType != BaseNode::Camera || ((ASE::Camera &)mesh).mCameraType != ASE::Camera::TARGET) &&
                            (mesh.mType != BaseNode::Light || ((ASE::Light &)mesh).mLightType != ASE::Light::TARGET)) {

                        ASSIMP_LOG_ERROR("ASE: Found target animation channel "
                                         "but the node is neither a camera nor a spot light");
                        anim = nullptr;
                    } else
                        anim = &mesh.mTargetAnim;
                }
                continue;
            }

            // position keyframes
            if (TokenMatch(mFilePtr, "CONTROL_POS_TRACK", 17) ||
                    TokenMatch(mFilePtr, "CONTROL_POS_BEZIER", 18) ||
                    TokenMatch(mFilePtr, "CONTROL_POS_TCB", 15)) {
                if (!anim)
                    SkipSection();
                else
                    ParseLV3PosAnimationBlock(*anim);
                continue;
            }
            // scaling keyframes
            if (TokenMatch(mFilePtr, "CONTROL_SCALE_TRACK", 19) ||
                    TokenMatch(mFilePtr, "CONTROL_SCALE_BEZIER", 20) ||
                    TokenMatch(mFilePtr, "CONTROL_SCALE_TCB", 17)) {
                if (!anim || anim == &mesh.mTargetAnim) {
                    // Target animation channels may have no rotation channels
                    ASSIMP_LOG_ERROR("ASE: Ignoring scaling channel in target animation");
                    SkipSection();
                } else
                    ParseLV3ScaleAnimationBlock(*anim);
                continue;
            }
            // rotation keyframes
            if (TokenMatch(mFilePtr, "CONTROL_ROT_TRACK", 17) ||
                    TokenMatch(mFilePtr, "CONTROL_ROT_BEZIER", 18) ||
                    TokenMatch(mFilePtr, "CONTROL_ROT_TCB", 15)) {
                if (!anim || anim == &mesh.mTargetAnim) {
                    // Target animation channels may have no rotation channels
                    ASSIMP_LOG_ERROR("ASE: Ignoring rotation channel in target animation");
                    SkipSection();
                } else
                    ParseLV3RotAnimationBlock(*anim);
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("2", "TM_ANIMATION");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3ScaleAnimationBlock(ASE::Animation &anim) {
    AI_ASE_PARSER_INIT();
    unsigned int iIndex;

    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            bool b = false;

            // For the moment we're just reading the three floats -
            // we ignore the additional information for bezier's and TCBs

            // simple scaling keyframe
            if (TokenMatch(mFilePtr, "CONTROL_SCALE_SAMPLE", 20)) {
                b = true;
                anim.mScalingType = ASE::Animation::TRACK;
            }

            // Bezier scaling keyframe
            if (TokenMatch(mFilePtr, "CONTROL_BEZIER_SCALE_KEY", 24)) {
                b = true;
                anim.mScalingType = ASE::Animation::BEZIER;
            }
            // TCB scaling keyframe
            if (TokenMatch(mFilePtr, "CONTROL_TCB_SCALE_KEY", 21)) {
                b = true;
                anim.mScalingType = ASE::Animation::TCB;
            }
            if (b) {
                anim.akeyScaling.emplace_back();
                aiVectorKey &key = anim.akeyScaling.back();
                ParseLV4MeshRealTriple(&key.mValue.x, iIndex);
                key.mTime = (double)iIndex;
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*CONTROL_POS_TRACK");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3PosAnimationBlock(ASE::Animation &anim) {
    AI_ASE_PARSER_INIT();
    unsigned int iIndex;
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            bool b = false;

            // For the moment we're just reading the three floats -
            // we ignore the additional information for bezier's and TCBs

            // simple scaling keyframe
            if (TokenMatch(mFilePtr, "CONTROL_POS_SAMPLE", 18)) {
                b = true;
                anim.mPositionType = ASE::Animation::TRACK;
            }

            // Bezier scaling keyframe
            if (TokenMatch(mFilePtr, "CONTROL_BEZIER_POS_KEY", 22)) {
                b = true;
                anim.mPositionType = ASE::Animation::BEZIER;
            }
            // TCB scaling keyframe
            if (TokenMatch(mFilePtr, "CONTROL_TCB_POS_KEY", 19)) {
                b = true;
                anim.mPositionType = ASE::Animation::TCB;
            }
            if (b) {
                anim.akeyPositions.emplace_back();
                aiVectorKey &key = anim.akeyPositions.back();
                ParseLV4MeshRealTriple(&key.mValue.x, iIndex);
                key.mTime = (double)iIndex;
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*CONTROL_POS_TRACK");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3RotAnimationBlock(ASE::Animation &anim) {
    AI_ASE_PARSER_INIT();
    unsigned int iIndex;
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            bool b = false;

            // For the moment we're just reading the  floats -
            // we ignore the additional information for bezier's and TCBs

            // simple scaling keyframe
            if (TokenMatch(mFilePtr, "CONTROL_ROT_SAMPLE", 18)) {
                b = true;
                anim.mRotationType = ASE::Animation::TRACK;
            }

            // Bezier scaling keyframe
            if (TokenMatch(mFilePtr, "CONTROL_BEZIER_ROT_KEY", 22)) {
                b = true;
                anim.mRotationType = ASE::Animation::BEZIER;
            }
            // TCB scaling keyframe
            if (TokenMatch(mFilePtr, "CONTROL_TCB_ROT_KEY", 19)) {
                b = true;
                anim.mRotationType = ASE::Animation::TCB;
            }
            if (b) {
                anim.akeyRotations.emplace_back();
                aiQuatKey &key = anim.akeyRotations.back();
                aiVector3D v;
                ai_real f;
                ParseLV4MeshRealTriple(&v.x, iIndex);
                ParseLV4MeshReal(f);
                key.mTime = (double)iIndex;
                key.mValue = aiQuaternion(v, f);
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*CONTROL_ROT_TRACK");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV2NodeTransformBlock(ASE::BaseNode &mesh) {
    AI_ASE_PARSER_INIT();
    int mode = 0;
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;
            // name of the node
            if (TokenMatch(mFilePtr, "NODE_NAME", 9)) {
                std::string temp;
                if (!ParseString(temp, "*NODE_NAME"))
                    SkipToNextToken();

                std::string::size_type s;
                if (temp == mesh.mName) {
                    mode = 1;
                } else if (std::string::npos != (s = temp.find(".Target")) &&
                           mesh.mName == temp.substr(0, s)) {
                    // This should be either a target light or a target camera
                    if ((mesh.mType == BaseNode::Light && ((ASE::Light &)mesh).mLightType == ASE::Light::TARGET) ||
                            (mesh.mType == BaseNode::Camera && ((ASE::Camera &)mesh).mCameraType == ASE::Camera::TARGET)) {
                        mode = 2;
                    } else {
                        ASSIMP_LOG_ERROR("ASE: Ignoring target transform, "
                                         "this is no spot light or target camera");
                    }
                } else {
                    ASSIMP_LOG_ERROR("ASE: Unknown node transformation: ", temp);
                    // mode = 0
                }
                continue;
            }
            if (mode) {
                // fourth row of the transformation matrix - and also the
                // only information here that is interesting for targets
                if (TokenMatch(mFilePtr, "TM_ROW3", 7)) {
                    ParseLV4MeshRealTriple((mode == 1 ? mesh.mTransform[3] : &mesh.mTargetPosition.x));
                    continue;
                }
                if (mode == 1) {
                    // first row of the transformation matrix
                    if (TokenMatch(mFilePtr, "TM_ROW0", 7)) {
                        ParseLV4MeshRealTriple(mesh.mTransform[0]);
                        continue;
                    }
                    // second row of the transformation matrix
                    if (TokenMatch(mFilePtr, "TM_ROW1", 7)) {
                        ParseLV4MeshRealTriple(mesh.mTransform[1]);
                        continue;
                    }
                    // third row of the transformation matrix
                    if (TokenMatch(mFilePtr, "TM_ROW2", 7)) {
                        ParseLV4MeshRealTriple(mesh.mTransform[2]);
                        continue;
                    }
                    // inherited position axes
                    if (TokenMatch(mFilePtr, "INHERIT_POS", 11)) {
                        unsigned int aiVal[3];
                        ParseLV4MeshLongTriple(aiVal);

                        for (unsigned int i = 0; i < 3; ++i)
                            mesh.inherit.abInheritPosition[i] = aiVal[i] != 0;
                        continue;
                    }
                    // inherited rotation axes
                    if (TokenMatch(mFilePtr, "INHERIT_ROT", 11)) {
                        unsigned int aiVal[3];
                        ParseLV4MeshLongTriple(aiVal);

                        for (unsigned int i = 0; i < 3; ++i)
                            mesh.inherit.abInheritRotation[i] = aiVal[i] != 0;
                        continue;
                    }
                    // inherited scaling axes
                    if (TokenMatch(mFilePtr, "INHERIT_SCL", 11)) {
                        unsigned int aiVal[3];
                        ParseLV4MeshLongTriple(aiVal);

                        for (unsigned int i = 0; i < 3; ++i)
                            mesh.inherit.abInheritScaling[i] = aiVal[i] != 0;
                        continue;
                    }
                }
            }
        }
        AI_ASE_HANDLE_SECTION("2", "*NODE_TM");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV2MeshBlock(ASE::Mesh &mesh) {
    AI_ASE_PARSER_INIT();

    unsigned int iNumVertices = 0;
    unsigned int iNumFaces = 0;
    unsigned int iNumTVertices = 0;
    unsigned int iNumTFaces = 0;
    unsigned int iNumCVertices = 0;
    unsigned int iNumCFaces = 0;
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;
            // Number of vertices in the mesh
            if (TokenMatch(mFilePtr, "MESH_NUMVERTEX", 14)) {
                ParseLV4MeshLong(iNumVertices);
                continue;
            }
            // Number of texture coordinates in the mesh
            if (TokenMatch(mFilePtr, "MESH_NUMTVERTEX", 15)) {
                ParseLV4MeshLong(iNumTVertices);
                continue;
            }
            // Number of vertex colors in the mesh
            if (TokenMatch(mFilePtr, "MESH_NUMCVERTEX", 15)) {
                ParseLV4MeshLong(iNumCVertices);
                continue;
            }
            // Number of regular faces in the mesh
            if (TokenMatch(mFilePtr, "MESH_NUMFACES", 13)) {
                ParseLV4MeshLong(iNumFaces);
                continue;
            }
            // Number of UVWed faces in the mesh
            if (TokenMatch(mFilePtr, "MESH_NUMTVFACES", 15)) {
                ParseLV4MeshLong(iNumTFaces);
                continue;
            }
            // Number of colored faces in the mesh
            if (TokenMatch(mFilePtr, "MESH_NUMCVFACES", 15)) {
                ParseLV4MeshLong(iNumCFaces);
                continue;
            }
            // mesh vertex list block
            if (TokenMatch(mFilePtr, "MESH_VERTEX_LIST", 16)) {
                ParseLV3MeshVertexListBlock(iNumVertices, mesh);
                continue;
            }
            // mesh face list block
            if (TokenMatch(mFilePtr, "MESH_FACE_LIST", 14)) {
                ParseLV3MeshFaceListBlock(iNumFaces, mesh);
                continue;
            }
            // mesh texture vertex list block
            if (TokenMatch(mFilePtr, "MESH_TVERTLIST", 14)) {
                ParseLV3MeshTListBlock(iNumTVertices, mesh);
                continue;
            }
            // mesh texture face block
            if (TokenMatch(mFilePtr, "MESH_TFACELIST", 14)) {
                ParseLV3MeshTFaceListBlock(iNumTFaces, mesh);
                continue;
            }
            // mesh color vertex list block
            if (TokenMatch(mFilePtr, "MESH_CVERTLIST", 14)) {
                ParseLV3MeshCListBlock(iNumCVertices, mesh);
                continue;
            }
            // mesh color face block
            if (TokenMatch(mFilePtr, "MESH_CFACELIST", 14)) {
                ParseLV3MeshCFaceListBlock(iNumCFaces, mesh);
                continue;
            }
            // mesh normals
            if (TokenMatch(mFilePtr, "MESH_NORMALS", 12)) {
                ParseLV3MeshNormalListBlock(mesh);
                continue;
            }
            // another mesh UV channel ...
            if (TokenMatch(mFilePtr, "MESH_MAPPINGCHANNEL", 19)) {
                unsigned int iIndex(0);
                ParseLV4MeshLong(iIndex);
                if (0 == iIndex) {
                    LogWarning("Mapping channel has an invalid index. Skipping UV channel");
                    // skip it ...
                    SkipSection();
                } else {
                    if (iIndex < 2) {
                        LogWarning("Mapping channel has an invalid index. Skipping UV channel");
                        // skip it ...
                        SkipSection();
                    }
                    if (iIndex > AI_MAX_NUMBER_OF_TEXTURECOORDS) {
                        LogWarning("Too many UV channels specified. Skipping channel ..");
                        // skip it ...
                        SkipSection();
                    } else {
                        // parse the mapping channel
                        ParseLV3MappingChannel(iIndex - 1, mesh);
                    }
                    continue;
                }
            }
            // mesh animation keyframe. Not supported
            if (TokenMatch(mFilePtr, "MESH_ANIMATION", 14)) {

                LogWarning("Found *MESH_ANIMATION element in ASE/ASK file. "
                           "Keyframe animation is not supported by Assimp, this element "
                           "will be ignored");
                //SkipSection();
                continue;
            }
            if (TokenMatch(mFilePtr, "MESH_WEIGHTS", 12)) {
                ParseLV3MeshWeightsBlock(mesh);
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("2", "*MESH");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3MeshWeightsBlock(ASE::Mesh &mesh) {
    AI_ASE_PARSER_INIT();

    unsigned int iNumVertices = 0, iNumBones = 0;
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            // Number of bone vertices ...
            if (TokenMatch(mFilePtr, "MESH_NUMVERTEX", 14)) {
                ParseLV4MeshLong(iNumVertices);
                continue;
            }
            // Number of bones
            if (TokenMatch(mFilePtr, "MESH_NUMBONE", 12)) {
                ParseLV4MeshLong(iNumBones);
                continue;
            }
            // parse the list of bones
            if (TokenMatch(mFilePtr, "MESH_BONE_LIST", 14)) {
                ParseLV4MeshBones(iNumBones, mesh);
                continue;
            }
            // parse the list of bones vertices
            if (TokenMatch(mFilePtr, "MESH_BONE_VERTEX_LIST", 21)) {
                ParseLV4MeshBonesVertices(iNumVertices, mesh);
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*MESH_WEIGHTS");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV4MeshBones(unsigned int iNumBones, ASE::Mesh &mesh) {
    AI_ASE_PARSER_INIT();
    mesh.mBones.resize(iNumBones, Bone("UNNAMED"));
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            // Mesh bone with name ...
            if (TokenMatch(mFilePtr, "MESH_BONE_NAME", 14)) {
                // parse an index ...
                if (SkipSpaces(&mFilePtr, mEnd)) {
                    unsigned int iIndex = strtoul10(mFilePtr, &mFilePtr);
                    if (iIndex >= iNumBones) {
                        LogWarning("Bone index is out of bounds");
                        continue;
                    }
                    if (!ParseString(mesh.mBones[iIndex].mName, "*MESH_BONE_NAME"))
                        SkipToNextToken();
                    continue;
                }
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*MESH_BONE_LIST");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV4MeshBonesVertices(unsigned int iNumVertices, ASE::Mesh &mesh) {
    AI_ASE_PARSER_INIT();
    mesh.mBoneVertices.resize(iNumVertices);
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            // Mesh bone vertex
            if (TokenMatch(mFilePtr, "MESH_BONE_VERTEX", 16)) {
                // read the vertex index
                unsigned int iIndex = strtoul10(mFilePtr, &mFilePtr);
                if (mesh.mBoneVertices.empty()) {
                    SkipSection();
                }
                if (iIndex >= mesh.mBoneVertices.size() ) {
                    LogWarning("Bone vertex index is out of bounds. Using the largest valid "
                               "bone vertex index instead");
                    iIndex = (unsigned int)mesh.mBoneVertices.size() - 1;
                }

                // --- ignored
                ai_real afVert[3];
                ParseLV4MeshRealTriple(afVert);

                std::pair<int, float> pairOut;
                while (true) {
                    // first parse the bone index ...
                    if (!SkipSpaces(&mFilePtr, mEnd)) break;
                    pairOut.first = strtoul10(mFilePtr, &mFilePtr);

                    // then parse the vertex weight
                    if (!SkipSpaces(&mFilePtr, mEnd)) break;
                    mFilePtr = fast_atoreal_move(mFilePtr, pairOut.second);

                    // -1 marks unused entries
                    if (-1 != pairOut.first) {
                        mesh.mBoneVertices[iIndex].mBoneWeights.push_back(pairOut);
                    }
                }
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("4", "*MESH_BONE_VERTEX");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3MeshVertexListBlock(
        unsigned int iNumVertices, ASE::Mesh &mesh) {
    AI_ASE_PARSER_INIT();

    // allocate enough storage in the array
    mesh.mPositions.resize(iNumVertices);
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            // Vertex entry
            if (TokenMatch(mFilePtr, "MESH_VERTEX", 11)) {

                aiVector3D vTemp;
                unsigned int iIndex;
                ParseLV4MeshRealTriple(&vTemp.x, iIndex);

                if (iIndex >= iNumVertices) {
                    LogWarning("Invalid vertex index. It will be ignored");
                } else
                    mesh.mPositions[iIndex] = vTemp;
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*MESH_VERTEX_LIST");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3MeshFaceListBlock(unsigned int iNumFaces, ASE::Mesh &mesh) {
    AI_ASE_PARSER_INIT();

    // allocate enough storage in the face array
    mesh.mFaces.resize(iNumFaces);
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            // Face entry
            if (TokenMatch(mFilePtr, "MESH_FACE", 9)) {

                ASE::Face mFace;
                ParseLV4MeshFace(mFace);

                if (mFace.iFace >= iNumFaces) {
                    LogWarning("Face has an invalid index. It will be ignored");
                } else
                    mesh.mFaces[mFace.iFace] = mFace;
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*MESH_FACE_LIST");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3MeshTListBlock(unsigned int iNumVertices,
        ASE::Mesh &mesh, unsigned int iChannel) {
    AI_ASE_PARSER_INIT();

    // allocate enough storage in the array
    mesh.amTexCoords[iChannel].resize(iNumVertices);
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            // Vertex entry
            if (TokenMatch(mFilePtr, "MESH_TVERT", 10)) {
                aiVector3D vTemp;
                unsigned int iIndex;
                ParseLV4MeshRealTriple(&vTemp.x, iIndex);

                if (iIndex >= iNumVertices) {
                    LogWarning("Tvertex has an invalid index. It will be ignored");
                } else
                    mesh.amTexCoords[iChannel][iIndex] = vTemp;

                if (0.0f != vTemp.z) {
                    // we need 3 coordinate channels
                    mesh.mNumUVComponents[iChannel] = 3;
                }
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*MESH_TVERT_LIST");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3MeshTFaceListBlock(unsigned int iNumFaces,
        ASE::Mesh &mesh, unsigned int iChannel) {
    AI_ASE_PARSER_INIT();
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            // Face entry
            if (TokenMatch(mFilePtr, "MESH_TFACE", 10)) {
                unsigned int aiValues[3];
                unsigned int iIndex = 0;

                ParseLV4MeshLongTriple(aiValues, iIndex);
                if (iIndex >= iNumFaces || iIndex >= mesh.mFaces.size()) {
                    LogWarning("UV-Face has an invalid index. It will be ignored");
                } else {
                    // copy UV indices
                    mesh.mFaces[iIndex].amUVIndices[iChannel][0] = aiValues[0];
                    mesh.mFaces[iIndex].amUVIndices[iChannel][1] = aiValues[1];
                    mesh.mFaces[iIndex].amUVIndices[iChannel][2] = aiValues[2];
                }
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*MESH_TFACE_LIST");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3MappingChannel(unsigned int iChannel, ASE::Mesh &mesh) {
    AI_ASE_PARSER_INIT();

    unsigned int iNumTVertices = 0;
    unsigned int iNumTFaces = 0;
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            // Number of texture coordinates in the mesh
            if (TokenMatch(mFilePtr, "MESH_NUMTVERTEX", 15)) {
                ParseLV4MeshLong(iNumTVertices);
                continue;
            }
            // Number of UVWed faces in the mesh
            if (TokenMatch(mFilePtr, "MESH_NUMTVFACES", 15)) {
                ParseLV4MeshLong(iNumTFaces);
                continue;
            }
            // mesh texture vertex list block
            if (TokenMatch(mFilePtr, "MESH_TVERTLIST", 14)) {
                ParseLV3MeshTListBlock(iNumTVertices, mesh, iChannel);
                continue;
            }
            // mesh texture face block
            if (TokenMatch(mFilePtr, "MESH_TFACELIST", 14)) {
                ParseLV3MeshTFaceListBlock(iNumTFaces, mesh, iChannel);
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*MESH_MAPPING_CHANNEL");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3MeshCListBlock(unsigned int iNumVertices, ASE::Mesh &mesh) {
    AI_ASE_PARSER_INIT();

    // allocate enough storage in the array
    mesh.mVertexColors.resize(iNumVertices);
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            // Vertex entry
            if (TokenMatch(mFilePtr, "MESH_VERTCOL", 12)) {
                aiColor4D vTemp;
                vTemp.a = 1.0f;
                unsigned int iIndex;
                ParseLV4MeshFloatTriple(&vTemp.r, iIndex);

                if (iIndex >= iNumVertices) {
                    LogWarning("Vertex color has an invalid index. It will be ignored");
                } else
                    mesh.mVertexColors[iIndex] = vTemp;
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*MESH_CVERTEX_LIST");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3MeshCFaceListBlock(unsigned int iNumFaces, ASE::Mesh &mesh) {
    AI_ASE_PARSER_INIT();
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;

            // Face entry
            if (TokenMatch(mFilePtr, "MESH_CFACE", 10)) {
                unsigned int aiValues[3];
                unsigned int iIndex = 0;

                ParseLV4MeshLongTriple(aiValues, iIndex);
                if (iIndex >= iNumFaces || iIndex >= mesh.mFaces.size()) {
                    LogWarning("UV-Face has an invalid index. It will be ignored");
                } else {
                    // copy color indices
                    mesh.mFaces[iIndex].mColorIndices[0] = aiValues[0];
                    mesh.mFaces[iIndex].mColorIndices[1] = aiValues[1];
                    mesh.mFaces[iIndex].mColorIndices[2] = aiValues[2];
                }
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*MESH_CFACE_LIST");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV3MeshNormalListBlock(ASE::Mesh &sMesh) {
    AI_ASE_PARSER_INIT();

    // Allocate enough storage for the normals
    sMesh.mNormals.resize(sMesh.mFaces.size() * 3, aiVector3D(0.f, 0.f, 0.f));
    unsigned int index, faceIdx = UINT_MAX;

    // FIXME: rewrite this and find out how to interpret the normals
    // correctly. This is crap.

    // Smooth the vertex and face normals together. The result
    // will be edgy then, but otherwise everything would be soft ...
    while (true) {
        if ('*' == *mFilePtr) {
            ++mFilePtr;
            if (faceIdx != UINT_MAX && TokenMatch(mFilePtr, "MESH_VERTEXNORMAL", 17)) {
                aiVector3D vNormal;
                ParseLV4MeshRealTriple(&vNormal.x, index);
                if (faceIdx >= sMesh.mFaces.size())
                    continue;

                // Make sure we assign it to the correct face
                const ASE::Face &face = sMesh.mFaces[faceIdx];
                if (index == face.mIndices[0])
                    index = 0;
                else if (index == face.mIndices[1])
                    index = 1;
                else if (index == face.mIndices[2])
                    index = 2;
                else {
                    ASSIMP_LOG_ERROR("ASE: Invalid vertex index in MESH_VERTEXNORMAL section");
                    continue;
                }
                // We'll renormalize later
                sMesh.mNormals[faceIdx * 3 + index] += vNormal;
                continue;
            }
            if (TokenMatch(mFilePtr, "MESH_FACENORMAL", 15)) {
                aiVector3D vNormal;
                ParseLV4MeshRealTriple(&vNormal.x, faceIdx);

                if (faceIdx >= sMesh.mFaces.size()) {
                    ASSIMP_LOG_ERROR("ASE: Invalid vertex index in MESH_FACENORMAL section");
                    continue;
                }

                // We'll renormalize later
                sMesh.mNormals[faceIdx * 3] += vNormal;
                sMesh.mNormals[faceIdx * 3 + 1] += vNormal;
                sMesh.mNormals[faceIdx * 3 + 2] += vNormal;
                continue;
            }
        }
        AI_ASE_HANDLE_SECTION("3", "*MESH_NORMALS");
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV4MeshFace(ASE::Face &out) {
    // skip spaces and tabs
    if (!SkipSpaces(&mFilePtr, mEnd)) {
        LogWarning("Unable to parse *MESH_FACE Element: Unexpected EOL [#1]");
        SkipToNextToken();
        return;
    }

    // parse the face index
    out.iFace = strtoul10(mFilePtr, &mFilePtr);

    // next character should be ':'
    if (!SkipSpaces(&mFilePtr, mEnd)) {
        // FIX: there are some ASE files which haven't got : here ....
        LogWarning("Unable to parse *MESH_FACE Element: Unexpected EOL. \':\' expected [#2]");
        SkipToNextToken();
        return;
    }
    // FIX: There are some ASE files which haven't got ':' here
    if (':' == *mFilePtr) ++mFilePtr;

    // Parse all mesh indices
    for (unsigned int i = 0; i < 3; ++i) {
        unsigned int iIndex = 0;
        if (!SkipSpaces(&mFilePtr, mEnd)) {
            LogWarning("Unable to parse *MESH_FACE Element: Unexpected EOL");
            SkipToNextToken();
            return;
        }
        switch (*mFilePtr) {
        case 'A':
        case 'a':
            break;
        case 'B':
        case 'b':
            iIndex = 1;
            break;
        case 'C':
        case 'c':
            iIndex = 2;
            break;
        default:
            LogWarning("Unable to parse *MESH_FACE Element: Unexpected EOL. "
                       "A,B or C expected [#3]");
            SkipToNextToken();
            return;
        };
        ++mFilePtr;

        // next character should be ':'
        if (!SkipSpaces(&mFilePtr, mEnd) || ':' != *mFilePtr) {
            LogWarning("Unable to parse *MESH_FACE Element: "
                       "Unexpected EOL. \':\' expected [#2]");
            SkipToNextToken();
            return;
        }

        ++mFilePtr;
        if (!SkipSpaces(&mFilePtr, mEnd)) {
            LogWarning("Unable to parse *MESH_FACE Element: Unexpected EOL. "
                       "Vertex index expected [#4]");
            SkipToNextToken();
            return;
        }
        out.mIndices[iIndex] = strtoul10(mFilePtr, &mFilePtr);
    }

    // now we need to skip the AB, BC, CA blocks.
    while (true) {
        if ('*' == *mFilePtr) break;
        if (IsLineEnd(*mFilePtr)) {
            //iLineNumber++;
            return;
        }
        mFilePtr++;
    }

    // parse the smoothing group of the face
    if (TokenMatch(mFilePtr, "*MESH_SMOOTHING", 15)) {
        if (!SkipSpaces(&mFilePtr, mEnd)) {
            LogWarning("Unable to parse *MESH_SMOOTHING Element: "
                       "Unexpected EOL. Smoothing group(s) expected [#5]");
            SkipToNextToken();
            return;
        }

        // Parse smoothing groups until we don't anymore see commas
        // FIX: There needn't always be a value, sad but true
        while (true) {
            if (*mFilePtr < '9' && *mFilePtr >= '0') {
                uint32_t value = strtoul10(mFilePtr, &mFilePtr);
                if (value < 32) {
                    out.iSmoothGroup |= (1 << strtoul10(mFilePtr, &mFilePtr));
                } else {
                    const std::string message = std::string("Unable to set smooth group, value with ") + ai_to_string(value) + std::string(" out of range");
                    LogWarning(message.c_str());
                }
            }
            SkipSpaces(&mFilePtr, mEnd);
            if (',' != *mFilePtr) {
                break;
            }
            ++mFilePtr;
            SkipSpaces(&mFilePtr, mEnd);
        }
    }

    // *MESH_MTLID  is optional, too
    while (true) {
        if ('*' == *mFilePtr) {
            break;
        }
        if (IsLineEnd(*mFilePtr)) {
            return;
        }
        mFilePtr++;
    }

    if (TokenMatch(mFilePtr, "*MESH_MTLID", 11)) {
        if (!SkipSpaces(&mFilePtr, mEnd)) {
            LogWarning("Unable to parse *MESH_MTLID Element: Unexpected EOL. "
                       "Material index expected [#6]");
            SkipToNextToken();
            return;
        }
        out.iMaterial = strtoul10(mFilePtr, &mFilePtr);
    }
    return;
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV4MeshLongTriple(unsigned int *apOut) {
    ai_assert(nullptr != apOut);

    for (unsigned int i = 0; i < 3; ++i)
        ParseLV4MeshLong(apOut[i]);
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV4MeshLongTriple(unsigned int *apOut, unsigned int &rIndexOut) {
    ai_assert(nullptr != apOut);

    // parse the index
    ParseLV4MeshLong(rIndexOut);

    // parse the three others
    ParseLV4MeshLongTriple(apOut);
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV4MeshRealTriple(ai_real *apOut, unsigned int &rIndexOut) {
    ai_assert(nullptr != apOut);

    // parse the index
    ParseLV4MeshLong(rIndexOut);

    // parse the three others
    ParseLV4MeshRealTriple(apOut);
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV4MeshFloatTriple(float* apOut, unsigned int& rIndexOut) {
    ai_assert(nullptr != apOut);

    // parse the index
    ParseLV4MeshLong(rIndexOut);

    // parse the three others
    ParseLV4MeshFloatTriple(apOut);
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV4MeshRealTriple(ai_real *apOut) {
    ai_assert(nullptr != apOut);

    for (unsigned int i = 0; i < 3; ++i) {
        ParseLV4MeshReal(apOut[i]);
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV4MeshFloatTriple(float* apOut) {
    ai_assert(nullptr != apOut);

    for (unsigned int i = 0; i < 3; ++i) {
        ParseLV4MeshFloat(apOut[i]);
    }
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV4MeshReal(ai_real &fOut) {
    // skip spaces and tabs
    if (!SkipSpaces(&mFilePtr, mEnd)) {
        // LOG
        LogWarning("Unable to parse float: unexpected EOL [#1]");
        fOut = 0.0;
        ++iLineNumber;
        return;
    }
    // parse the first float
    mFilePtr = fast_atoreal_move(mFilePtr, fOut);
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV4MeshFloat(float &fOut) {
    // skip spaces and tabs
    if (!SkipSpaces(&mFilePtr, mEnd)) {
        // LOG
        LogWarning("Unable to parse float: unexpected EOL [#1]");
        fOut = 0.0;
        ++iLineNumber;
        return;
    }
    // parse the first float
    mFilePtr = fast_atoreal_move(mFilePtr, fOut);
}
// ------------------------------------------------------------------------------------------------
void Parser::ParseLV4MeshLong(unsigned int &iOut) {
    // Skip spaces and tabs
    if (!SkipSpaces(&mFilePtr, mEnd)) {
        // LOG
        LogWarning("Unable to parse long: unexpected EOL [#1]");
        iOut = 0;
        ++iLineNumber;
        return;
    }
    // parse the value
    iOut = strtoul10(mFilePtr, &mFilePtr);
}

}

#endif // ASSIMP_BUILD_NO_3DS_IMPORTER

#endif // !! ASSIMP_BUILD_NO_BASE_IMPORTER
