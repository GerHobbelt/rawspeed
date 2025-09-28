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

#include "rawspeedconfig.h"
#include "decompressors/PanasonicV8Decompressor.h"
#include "adt/Array2DRef.h"
#include "bitstreams/BitStreamer.h"
#include "bitstreams/BitStreamerMSB.h"
#include "common/RawspeedException.h"
#include "decoders/RawDecoderException.h"
#include "io/Buffer.h"
#include "io/ByteStream.h"
#include "io/Endianness.h"
#include "tiff/TiffTag.h"
#include <algorithm>
#include <cstdint>
#include <functional>

namespace rawspeed {

// The set of templates and classes below define a specialized bit streamer
// which works mostly the same as BitStreamerMSB, but which reverses the
// ordering of the bits within each byte.
template <typename Tag>
struct BitStreamerReversedSequentialReplenisher
    : public BitStreamerForwardSequentialReplenisher<Tag> {
  using Base = BitStreamerForwardSequentialReplenisher<Tag>;
  using Traits = BitStreamerTraits<Tag>;
  using StreamTraits = BitStreamTraits<Traits::Tag>;

  using Base::BitStreamerForwardSequentialReplenisher;

  // Almost an exact copy of
  // BitStreamerForwardSequentialReplenisher::getInput(), but here we flip the
  // order of the bits within each byte, as they get loaded.
  std::array<std::byte, BitStreamerTraits<Tag>::MaxProcessBytes> getInput() {
    Base::establishClassInvariants();

    std::array<std::byte, BitStreamerTraits<Tag>::MaxProcessBytes> tmpStorage;
    auto tmp = Array1DRef<std::byte>(tmpStorage.data(),
                                     implicit_cast<int>(tmpStorage.size()));

    if (Base::getPos() + BitStreamerTraits<Tag>::MaxProcessBytes <=
        Base::input.size()) [[likely]] {
      auto currInput =
          Base::input
              .getCrop(Base::getPos(), BitStreamerTraits<Tag>::MaxProcessBytes)
              .getAsArray1DRef();
      invariant(currInput.size() == tmp.size());
      std::copy_n(currInput.begin(), BitStreamerTraits<Tag>::MaxProcessBytes,
                  tmp.begin());

      // Reverse the order of bits within each byte using a bit-twiddle trick.
      // Three operation bit reversal from:
      // https://graphics.stanford.edu/~seander/bithacks.html#ReverseByteWith64BitsDiv
      for (std::byte& b : tmp) {
        b = std::byte{
            uint8_t((uint8_t(b) * 0x0202020202ULL & 0x010884422010ULL) % 1023)};
      }

      return tmpStorage;
    }

    if (Base::getPos() >
        Base::input.size() + 2 * BitStreamerTraits<Tag>::MaxProcessBytes)
        [[unlikely]]
      ThrowIOE("Buffer overflow read in BitStreamer");

    variableLengthLoadNaiveViaMemcpy(tmp, Base::input, Base::getPos());

    return tmpStorage;
  }
};

class BitStreamerRevMSB;

template <> struct BitStreamerTraits<BitStreamerRevMSB> final {
  static constexpr BitOrder Tag = BitOrder::MSB;

  static constexpr bool canUseWithPrefixCodeDecoder = true;

  static constexpr int MaxProcessBytes = 4;
  static_assert(MaxProcessBytes == sizeof(uint32_t));
};

/// Variation of standard MSB bit streamer. Bits are processed in reverse order
/// (least significant bits consume first).
class BitStreamerRevMSB final
    : public BitStreamer<
          BitStreamerRevMSB,
          BitStreamerReversedSequentialReplenisher<BitStreamerRevMSB>> {
  using Base =
      BitStreamer<BitStreamerRevMSB,
                  BitStreamerReversedSequentialReplenisher<BitStreamerRevMSB>>;

public:
  using Base::Base;
};

/// Retrieve list of values from Panasonic TiffTag
template <typename T>
void getPanasonicTiffVector(const TiffIFD& ifd, TiffTag tag,
                            std::vector<T>& output) {
  ByteStream bs = ifd.getEntry(tag)->getData();
  output.resize(bs.getU16());

  // Note: Relying on ByteStream and its parent classes to prevent out-of-bounds
  // reading.
  for (T& v : output)
    v = bs.get<T>();
}

/// Utility class for Panasonic V8 entropy decoding
class PanasonicV8Decompressor::InternalHuffDecoder {
private:
  const HuffmanLUT& mLUT; // Reference to PanasonicV8Decompressor::mHuffmanLUT
  const std::vector<uint16_t>&
      mShiftDownList; // Reference to PanasonicV8Decompressor
                      // mParams.huffShiftDown
  BitStreamerRevMSB mBitPump;

public:
  InternalHuffDecoder(const PanasonicV8Decompressor::HuffmanLUT& LUT,
                      const std::vector<uint16_t>& shiftDownList,
                      Array1DRef<const uint8_t> bitStream)
      : mLUT(LUT), mShiftDownList(shiftDownList), mBitPump(bitStream) {}

  int32_t decodeNextDiffValue();
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Constructor populates decompressor parameters with values from ifd
PanasonicV8Decompressor::PanasonicV8Decompressor(Buffer inputFile,
                                                 RawImage outputImg,
                                                 const TiffIFD& ifd)
    : mInputFile(inputFile), mRawOutput(std::move(outputImg)) {
  if (mRawOutput->getCpp() != 1 ||
      mRawOutput->getDataType() != RawImageType::UINT16 ||
      mRawOutput->getBpp() != sizeof(uint16_t)) {
    ThrowRDE("Unexpected component count / data type");
  }

  mParams.horizontalStripCount =
      ifd.getEntry(TiffTag::PANASONIC_V8_NUMBER_OF_STRIPS_H)->getU16();
  mParams.verticalStripCount =
      ifd.getEntry(TiffTag::PANASONIC_V8_NUMBER_OF_STRIPS_V)->getU16();

  getPanasonicTiffVector(ifd, TiffTag::PANASONIC_V8_STRIP_BYTE_OFFSETS,
                         mParams.stripByteOffsets);
  getPanasonicTiffVector(ifd, TiffTag::PANASONIC_V8_STRIP_LINE_OFFSETS,
                         mParams.stripLineOffsets);
  getPanasonicTiffVector(ifd, TiffTag::PANASONIC_V8_STRIP_DATA_SIZE,
                         mParams.stripBitLengths);
  getPanasonicTiffVector(ifd, TiffTag::PANASONIC_V8_STRIP_WIDTHS,
                         mParams.stripWidths);
  getPanasonicTiffVector(ifd, TiffTag::PANASONIC_V8_STRIP_HEIGHTS,
                         mParams.stripHeights);

  // Get decoder's initial prediction value:
  // Note, the positions of the green samples are swapped. This is intentional,
  // the original implementation did this each swap redundantly during decoding
  // of each tile.
  mParams.initialPrediction[0] =
      ifd.getEntry(TiffTag::PANASONIC_V8_INIT_PRED_RED)->getU16();
  mParams.initialPrediction[2] =
      ifd.getEntry(TiffTag::PANASONIC_V8_INIT_PRED_GREEN1)->getU16();
  mParams.initialPrediction[1] =
      ifd.getEntry(TiffTag::PANASONIC_V8_INIT_PRED_GREEN2)->getU16();
  mParams.initialPrediction[3] =
      ifd.getEntry(TiffTag::PANASONIC_V8_INIT_PRED_BLUE)->getU16();

  getPanasonicTiffVector(ifd, TiffTag::PANASONIC_V8_HUF_SHIFT_DOWN,
                         mParams.huffShiftDown);

  mParams.gammaClipVal = ifd.getEntry(TiffTag::PANASONIC_V8_CLIP_VAL)->getU16();

  validateParams();
  populateHuffmanLUT(ifd);
  populateGammaLUT(ifd);
}

void PanasonicV8Decompressor::decompress() const {
  const unsigned totalStrips =
      mParams.horizontalStripCount * mParams.verticalStripCount;
#ifdef HAVE_OPENMP
  unsigned threadCount =
      std::min(int(totalStrips), rawspeed_get_number_of_processor_cores());
#pragma omp parallel for num_threads(threadCount)                              \
    schedule(static) default(none) shared(totalStrips)
#endif
  for (unsigned stripIdx = 0; stripIdx < totalStrips; ++stripIdx) {
    const uint32_t stripSize = (mParams.stripBitLengths[stripIdx] + 7) / 8;
    const uint32_t stripOffset = mParams.stripByteOffsets[stripIdx];

    // Note: Relying on Buffer to catch OOB access attempts
    DataBuffer stripBuffer(mInputFile.getSubView(stripOffset, stripSize),
                           Endianness::big);
    InternalHuffDecoder decoder(mHuffmanLUT, mParams.huffShiftDown,
                                stripBuffer.getAsArray1DRef());

    decompressStrip(stripIdx, decoder,
                    mRawOutput->getU16DataAsUncroppedArray2DRef());
  }
}

void PanasonicV8Decompressor::decompressStrip(
    const unsigned stripIdx, InternalHuffDecoder decoder,
    Array2DRef<uint16_t> outBuffer) const {
  const uint32_t stripWidth = mParams.stripWidths[stripIdx];
  const uint32_t stripHeight = mParams.stripHeights[stripIdx];
  const uint32_t stripOutputX = mParams.stripLineOffsets[stripIdx] & 0xFFFF;
  const uint32_t stripOutputY = mParams.stripLineOffsets[stripIdx] >> 16;

  std::vector<uint16_t> lineBuffer(stripWidth * 2);
  Bayer2x2 predicted = mParams.initialPrediction;

  for (unsigned row = 0; row < stripHeight; row += 2) {
    // Each decoded 'row' is actually two rows of pixels in the raw image
    // because the image is encoded in rows of 2x2 CFA tiles. Likewise the
    // effective width here is 2x the strip width.
    for (unsigned column = 0; column < stripWidth * 2; ++column) {
      const unsigned ccIdx =
          column % 4; // CFA color component index: r, g1, g2, b
      const int32_t diff = decoder.decodeNextDiffValue();
      const int32_t decodedValue = predicted[ccIdx] + diff;
      assert(decodedValue > 0);
      lineBuffer[column] =
          uint16_t(std::clamp(decodedValue, 0, int32_t(mParams.gammaClipVal)));

      if (ccIdx == 3) {
        // Completed decoding a 2x2 CFA tile. Update the predicted value to
        // equal the decoded value.
        std::copy_n(&lineBuffer[column - 3], 4, predicted.data());
      }
    }
    // At the end of the line, reset predicted value to the first tile of the
    // prior line.
    std::copy_n(&lineBuffer[0], 4, predicted.data());

    assert(mGammaLUT.empty());

    // Copy lineBuffer into output buffer.
    for (unsigned linePos = 0; linePos < stripWidth * 2; linePos += 4) {
      const uint32_t dstStartCol = stripOutputX + linePos / 2;

      if (mGammaLUT.empty()) [[likely]] {
        outBuffer[stripOutputY + row + 0](dstStartCol + 0) =
            lineBuffer[linePos + 0]; // Top Red
        outBuffer[stripOutputY + row + 0](dstStartCol + 1) =
            lineBuffer[linePos + 2]; // Top Green
        outBuffer[stripOutputY + row + 1](dstStartCol + 0) =
            lineBuffer[linePos + 1]; // Bottom Green
        outBuffer[stripOutputY + row + 1](dstStartCol + 1) =
            lineBuffer[linePos + 3]; // Bottom Blue
      } else [[unlikely]] {
        outBuffer[stripOutputY + row + 0](dstStartCol + 0) =
            mGammaLUT[lineBuffer[linePos + 0]]; // Top Red
        outBuffer[stripOutputY + row + 0](dstStartCol + 1) =
            mGammaLUT[lineBuffer[linePos + 2]]; // Top Green
        outBuffer[stripOutputY + row + 1](dstStartCol + 0) =
            mGammaLUT[lineBuffer[linePos + 1]]; // Bottom Green
        outBuffer[stripOutputY + row + 1](dstStartCol + 1) =
            mGammaLUT[lineBuffer[linePos + 3]]; // Bottom Blue
      }
    }
    // TODO: Investigate if it makes sense performance wise to structure
    // lineBuffer such that it can be memcpy'd into the output Buffer.
  }
}

int32_t inline PanasonicV8Decompressor::InternalHuffDecoder::
    decodeNextDiffValue() {
  // Retrieve the difference category, which indicates magnitude of the
  // difference between the predicted and actual value.
  const uint16_t next16 = uint16_t(mBitPump.peekBits(16));
  const auto& [bits, diffCat] = mLUT[next16];
  if (diffCat == 0 && bits == 7)
    ThrowRDE("Huffman decoding encountered an invalid value!");
  mBitPump.skipBits(bits); // Skip the bits that encoded the difference category

  // Zero in all known cases. In theory, it exists to allow some number of
  // bits to be truncated from the difference values in a given category.
  // Unclear if/when this would be used.
  const uint8_t shiftDown = mShiftDownList[diffCat] & 0x1F;
  assert(shiftDown == 0);

  const uint8_t diffBitCount = diffCat >= shiftDown ? diffCat - shiftDown : 0u;
  if (diffBitCount > 0) {
    // Decode difference value. The scheme here encodes signed integers in a
    // manner similar to offset binary encoding. Here, the encoding is biased by
    // the difference category such that abs(diff) is in the range
    // [2^{diffCat-1}, 2^{diffCat}).
    const uint32_t rawDiffBits = mBitPump.getBits(diffBitCount);
    const uint32_t sign = rawDiffBits >> (diffBitCount - 1);
    const uint32_t val = rawDiffBits << shiftDown;

    // In comments below, n = diffCat, d = shiftDown
    if (sign == 1)
      // Positive value in range [2^{n-1}, 2^{n})
      return val;
    else if (shiftDown == 0) [[likely]]
      // Negative value in interval (-2^{n}, -2^{n-1}]
      return val + (-1 << diffCat) + 1;
    else [[unlikely]] {
      // Unreachable in all known samples but should be correct
      // Same as negative value above, but accounting for down shift
      // values in range [-2^n - 2^{d-1}, -(2^{n-1} + d)]
      return val + (-1 << diffCat) + (1 << (shiftDown - 1));
    }
  }
  // diffBitCount of zero indicates no difference (next pixel is same as
  // predicted)
  return 0;
}

void PanasonicV8Decompressor::validateParams() {
  const unsigned totalStrips =
      mParams.horizontalStripCount * mParams.verticalStripCount;

  // Check that we won't be going OOB on any of these strip lists
  if (totalStrips > mParams.stripByteOffsets.size())
    ThrowRDE("Strip byte offset list does not have enough entries for the "
             "number of strips!");
  if (totalStrips > mParams.stripWidths.size())
    ThrowRDE("Strip widths list does not have enough entries for the number of "
             "strips!");
  if (totalStrips > mParams.stripHeights.size())
    ThrowRDE("Strip heights list does not have enough entries for the number "
             "of strips!");
  if (totalStrips > mParams.stripLineOffsets.size())
    ThrowRDE("Strip line offset list does not have enough entries for the "
             "number of strips!");
  if (totalStrips > mParams.stripBitLengths.size())
    ThrowRDE("Strip bit length list does not have enough entries for the "
             "number of strips!");

  if (std::any_of(mParams.huffShiftDown.begin(), mParams.huffShiftDown.end(),
                  [](uint16_t x) { return x != 0; })) {
    ThrowRDE("Non-zero shift down value encountered! Shift down decoding has "
             "never been tested!");
  }
}

void PanasonicV8Decompressor::populateHuffmanLUT(const TiffIFD& ifd) {
  ByteStream stream = ifd.getEntry(TiffTag::PANASONIC_V8_HUF_TABLE)->getData();

  struct HuffEntry {
    uint16_t bitcount, symbol, mask;
  };
  std::vector<HuffEntry> huffTable(stream.getU16());

  for (HuffEntry& entry : huffTable) {
    entry.bitcount = stream.getU16(); // Number of bits in symbol
    entry.symbol = uint16_t(stream.getU16() << (16u - entry.bitcount));
    entry.mask = uint16_t(
        0xffffu << (16u -
                    entry.bitcount)); // mask of the bits overlapping symbol
  }

  // Cache of Huffman table results for all possible 16-bit values.
  mHuffmanLUT.resize(1 + UINT16_MAX);

  // Populates LUT by checking for a bitwise match between each value and the
  // prefix codes recorded in the table.
  for (unsigned li = 0; li < mHuffmanLUT.size(); ++li) {
    PanasonicV8Decompressor::HuffmanLUTEntry& lutVal = mHuffmanLUT[li];
    for (unsigned ti = 0; ti < huffTable.size(); ++ti) {
      if ((uint16_t(li) & huffTable[ti].mask) == huffTable[ti].symbol) {
        lutVal.bitcount = uint8_t(huffTable[ti].bitcount);
        lutVal.diffCat = uint8_t(ti);
        break;
      }
    }
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunreachable-code"
/// Maybe the most complicated part of the entire file format, and seemingly,
/// completely unused.
void PanasonicV8Decompressor::populateGammaLUT(const TiffIFD& ifd) {
  // Retrieve encoded gamma curve from tags.
  std::vector<uint32_t> encodedGammaPoints, encodedGammaSlopes;
  getPanasonicTiffVector(ifd, TiffTag::PANASONIC_V8_GAMMA_POINTS,
                         encodedGammaPoints);
  getPanasonicTiffVector(ifd, TiffTag::PANASONIC_V8_GAMMA_SLOPES,
                         encodedGammaSlopes);

  // Determine if the points and slopes are all set to zero and 65536
  // respectively. If so, no gamma function needs to be applied. This is
  // currently true of all tested RW2 files.
  const bool gamamPointsAreIdentity =
      std::all_of(encodedGammaPoints.cbegin(), encodedGammaPoints.cend(),
                  [](const uint32_t p) { return p == 0u; });
  const bool gammaSlopesAreIdentity =
      std::all_of(encodedGammaSlopes.cbegin(), encodedGammaSlopes.cend(),
                  [](const uint32_t s) { return s == 65536u; });

  if (!gamamPointsAreIdentity || !gammaSlopesAreIdentity) {
    // Generate gamma LUT based on retrieved curve.
    ThrowRDE("Non-identity gamma curve encountered. Never encountered in any "
             "testing samples!");

    if (encodedGammaPoints.size() != 6 || encodedGammaSlopes.size() != 6) {
      ThrowRDE("Gamma curve point and/or slope list is not the expected length "
               "of 6");
    }

    // Decode point and slope lists. Defines a piece-wise function for the gamma
    // curve. Each point is an x,y intercept of a line segment with slope
    // encoded with a power of two exponent and sign bit. The x position of the
    // points must be ordered such that together they create six non-overlapping
    // intervals covering [0, UINT16_MAX].
    struct GammaPoint {
      uint16_t x, y;
    };
    struct GammaSlope {
      uint8_t sign, exp;
    };
    std::array<GammaPoint, 6> gammaPoints;
    std::array<GammaSlope, 6> gammaSlopes;
    for (unsigned i = 0; i < 6; ++i) {
      gammaPoints[i] = {uint16_t(encodedGammaPoints[i] & 0xFFFF),
                        uint16_t(encodedGammaPoints[i] >> 16)};
      gammaSlopes[i].sign = encodedGammaSlopes[i] & 0x10 ? 1 : 0;
      gammaSlopes[i].exp = uint8_t(encodedGammaSlopes[i] & 0x0F);
    }

    // Validate that the points are non-strictly ordered
    const bool pointsAreOrdered = std::is_sorted(
        gammaPoints.cbegin(), gammaPoints.cend(),
        [](const GammaPoint& a, const GammaPoint& b) { return a.x <= b.x; });
    if (!pointsAreOrdered) {
      ThrowRDE("Points in the gamma curve are out of order!");
    }

    // Evaluates the gamma curve for value x in the piece-wise function segment
    // 'i'
    const auto fnGamma = [&](const uint32_t x, const unsigned i) -> uint16_t {
      assert(i < gammaPoints.size());
      const GammaPoint& pt = gammaPoints[i];
      const GammaSlope& slope = gammaSlopes[i];

      uint32_t mx = 0;
      if (slope.exp == 15 && slope.sign == 1) {
        // An exponent of 15 and sign of 1 signals a special case where mx
        // becomes the y-intercept of the next curve segment or UINT16_MAX if
        // there is no next segment.
        mx = i <= 5 ? gammaPoints[i + 1].y : UINT16_MAX;
      } else if (slope.exp == 0) {
        // Exponent is zero, meaning the slope is 1 (no multiplication).
        mx = x - pt.x;
      } else if (slope.sign == 1) {
        mx = (x - pt.x) << slope.exp; // Positive slope
      } else {
        // Negative slope
        uint32_t h =
            1 << (slope.exp - 1); // Add slope/2 so that integer arithmetic for
                                  // `mx` will round correctly.
        mx = (x - pt.x + h) >> slope.exp;
      }

      // Add y-intercept and clamp to clipping value.
      return uint16_t(std::min(uint32_t(mParams.gammaClipVal), mx + pt.y));
    };

    mGammaLUT.resize(1 + UINT16_MAX);

    unsigned interval = 0;
    // Evaluate the gamma curve for all uint16_t values.
    for (uint32_t x = 0; x <= UINT16_MAX; ++x) {
      // Advance interval as needed, skipping over possible redundant intervals.
      while (interval + 1 < gammaPoints.size() &&
             x >= gammaPoints[interval + 1].x)
        ++interval;
      assert(gammaPoints[interval].x <= x);
      assert(interval + 1 == gammaPoints.size() ||
             gammaPoints[interval + 1].x > x);
      mGammaLUT[x] = fnGamma(x, interval);
    }
  }
}

#pragma GCC diagnostic pop

} // namespace rawspeed
