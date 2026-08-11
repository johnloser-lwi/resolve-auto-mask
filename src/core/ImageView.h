// Host-independent views over a premultiplied float RGBA buffer.
//
// Everything under src/core works in terms of these, which is what lets the
// identical segmentation and compositing code run inside Resolve and inside
// tools/masktest. Nothing here may include an OFX header.
#pragma once

#include <cstddef>
#include <cstdint>

namespace am {

struct RectI {
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // [x1,x2) [y1,y2)

  int width() const { return x2 - x1; }
  int height() const { return y2 - y1; }
  bool empty() const { return x2 <= x1 || y2 <= y1; }

  void grow(int px) {
    x1 -= px;
    y1 -= px;
    x2 += px;
    y2 += px;
  }

  void unionWith(const RectI& o) {
    if (o.empty()) return;
    if (empty()) {
      *this = o;
      return;
    }
    if (o.x1 < x1) x1 = o.x1;
    if (o.y1 < y1) y1 = o.y1;
    if (o.x2 > x2) x2 = o.x2;
    if (o.y2 > y2) y2 = o.y2;
  }
};

// A non-owning read view. `data` points at the pixel whose coordinate is (0,0).
//
// rowStride is in FLOATS, not bytes, and is deliberately signed: an OFX image
// is bottom-up, so a top-down view of it has data pointing at the last row and
// a negative stride. That is what lets the plugin hand core a top-down view of
// a host image with no copy and no flip pass. Anything that indexes a row must
// use ptrdiff_t arithmetic or it wraps.
struct ImageView {
  const float* data = nullptr;
  int width = 0;
  int height = 0;
  std::ptrdiff_t rowStride = 0;

  const float* row(int y) const { return data + rowStride * y; }
  const float* at(int x, int y) const { return row(y) + std::ptrdiff_t(x) * 4; }
  bool valid() const { return data != nullptr && width > 0 && height > 0; }
};

// The same, writable. Kept as a separate type rather than a const_cast at the
// call site so a function signature says whether it writes.
struct MutableImageView {
  float* data = nullptr;
  int width = 0;
  int height = 0;
  std::ptrdiff_t rowStride = 0;

  float* row(int y) const { return data + rowStride * y; }
  float* at(int x, int y) const { return row(y) + std::ptrdiff_t(x) * 4; }
  bool valid() const { return data != nullptr && width > 0 && height > 0; }

  ImageView readOnly() const { return ImageView{data, width, height, rowStride}; }
};

}  // namespace am
