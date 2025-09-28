/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2022-2024 LibRaw LLC (info@libraw.org)
    Copyright (C) 2024 Daniel Vogelbacher
    Copyright (C) 2025 Kolton Yager

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#pragma once

#include "common/RawImage.h"
#include "decompressors/AbstractDecompressor.h"
#include "tiff/TiffIFD.h"
#include <array>
#include <cstdint>
#include <vector>

namespace rawspeed {

/// Decompressor for Panasonic's RW2 version 8 format.
///
/// V8 is similar to lossless JPEG compression from the JPEG 92 spec and DNG
/// Each raw file is broken up into a number of separate strips, each of which
/// was separately encoded, and which can be decoded independently. For each
/// strip, an initial predicted value is provided. The strip's data buffer is
/// then decoded using the Huffman table provided in metadata. Each value
/// decoded from the strip is a difference between the predicted value and
/// actual value, allowing the actual value to be reconstructed.
class PanasonicV8Decompressor final : public AbstractDecompressor {
private:
  Buffer mInputFile;
  mutable RawImage mRawOutput;

  /// Four values, one for each component of the sensor's color filter array.
  using Bayer2x2 = std::array<uint16_t, 4>;

  /// Decompressor parameters populated from tags. They remain constant after
  /// construction.
  struct DecompressorParams {
    std::vector<uint32_t> stripByteOffsets, stripLineOffsets, stripBitLengths;
    std::vector<uint16_t> stripWidths, stripHeights;
    uint16_t horizontalStripCount, verticalStripCount;

    Bayer2x2 initialPrediction;

    /// Huffman decoding shift down value. Appears to be unused.
    std::vector<uint16_t> huffShiftDown;

    uint16_t gammaClipVal;
  } mParams;

  // Pre-cached Huffman decoded values for rapid lookup.
  struct HuffmanLUTEntry {
    uint8_t bitcount = 7, diffCat = 0;
  };
  using HuffmanLUT = std::vector<HuffmanLUTEntry>;
  HuffmanLUT mHuffmanLUT;

  // Lookup table for the raw's gamma curve. Appears to be unused.
  // All known samples utilize an identity function.
  std::vector<uint16_t> mGammaLUT;

  /// Huffman decoder helper class. Defined only in the cpp file.
  class InternalHuffDecoder;

  /// Thread safe function for decompressing a single data-stripstrip within a
  /// Rw2V8 raw image.
  void decompressStrip(const unsigned stripIdx, InternalHuffDecoder decoder,
                       Array2DRef<uint16_t> outBuffer) const;

  // Helpers called from the constructor
  void validateParams();
  void populateHuffmanLUT(const TiffIFD& ifd);
  void populateGammaLUT(const TiffIFD& ifd);

public:
  PanasonicV8Decompressor(Buffer inputFile, RawImage outputImg,
                          const TiffIFD& ifd);

  /// Run the decompressor on the provided raw image
  void decompress() const;
};

} // namespace rawspeed
