#pragma once
// HD textures from a PCSX2 texture pack of the PS2 version of BLACK (black.ini texture_pack). The pack names its files
// by hashes of PS2 video memory, which say nothing about the Xbox textures, so each game texture is matched to a pack
// texture by its picture: a 16x16 signature of colour, alpha and structure against an index of the pack (built once,
// cached in cache\texpack.idx). Each decision is kept in cache\texpack.map, one line per game texture; a line can be
// edited to "-" to keep the game's texture.
#include "d3d/device.h"

#include <wincodec.h>

#include <cstdint>
#include <string>
#include <vector>

namespace xd3d {

// Helpers of texture.cpp shared with the pack.
std::vector<uint32_t> decode_dxt(const uint8_t *src, UINT pitch, UINT w, UINT h, D3DFORMAT fmt);
std::vector<uint32_t> half_size(const std::vector<uint32_t> &src, UINT w, UINT h, UINT nw, UINT nh);
IWICImagingFactory *wic_factory();

namespace texpack {

bool enabled();
// The pack's texture for a game texture (id = texture_id; px = its level 0 as BGRA), or null. Matches once per id;
// the texture returned carries a reference for the caller.
IDirect3DTexture9 *lookup(const std::wstring &id, const std::vector<uint32_t> &px, UINT w, UINT h);
bool owns(IDirect3DBaseTexture9 *t); // a pack texture (its own mip chain: draws sample it mipmapped)
// Before a device Reset: the pack's textures live in D3DPOOL_DEFAULT. Drops the pack's own references; the
// texture cache drops its references too (texture_release_pack) and loads them again on demand.
void release_all();

} // namespace texpack
} // namespace xd3d
