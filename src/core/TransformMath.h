#pragma once

// 2D affine transform math. Taken verbatim from resolve-multi-transform, kept
// in the mtx namespace so a future diff against the original is a no-op.
//
// Header-only and device-safe on purpose: if a CUDA path is ever turned on,
// having two implementations of this drift apart is the most likely source of
// subtle CPU/GPU mismatches, so there is exactly one.

#if defined(__CUDACC__)
    #define MTX_HD __host__ __device__
#else
    #define MTX_HD
    #include <cmath>
#endif

namespace mtx {

constexpr double kPi = 3.14159265358979323846;

MTX_HD inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

MTX_HD inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/** @brief A 2D affine transform stored as the top two rows of a 3x3 matrix.
 *
 *      | m0 m1 m2 |   | x |
 *      | m3 m4 m5 | * | y |
 *      |  0  0  1 |   | 1 |
 */
struct Mat3
{
    float m[6];

    MTX_HD static Mat3 Identity()
    {
        Mat3 r;
        r.m[0] = 1.0f; r.m[1] = 0.0f; r.m[2] = 0.0f;
        r.m[3] = 0.0f; r.m[4] = 1.0f; r.m[5] = 0.0f;
        return r;
    }

    MTX_HD static Mat3 Translate(float tx, float ty)
    {
        Mat3 r = Identity();
        r.m[2] = tx;
        r.m[5] = ty;
        return r;
    }

    MTX_HD static Mat3 Scale(float sx, float sy)
    {
        Mat3 r = Identity();
        r.m[0] = sx;
        r.m[4] = sy;
        return r;
    }

    /** @param degrees counter-clockwise rotation. */
    MTX_HD static Mat3 Rotate(float degrees)
    {
        const float rad = degrees * static_cast<float>(kPi) / 180.0f;
        const float c = cosf(rad);
        const float s = sinf(rad);
        Mat3 r;
        r.m[0] =  c; r.m[1] = -s; r.m[2] = 0.0f;
        r.m[3] =  s; r.m[4] =  c; r.m[5] = 0.0f;
        return r;
    }

    MTX_HD void Apply(float x, float y, float& outX, float& outY) const
    {
        outX = m[0] * x + m[1] * y + m[2];
        outY = m[3] * x + m[4] * y + m[5];
    }

    MTX_HD float Determinant() const { return m[0] * m[4] - m[1] * m[3]; }
};

/** @brief Matrix product: applying the result is equivalent to applying @p b then @p a. */
MTX_HD inline Mat3 operator*(const Mat3& a, const Mat3& b)
{
    Mat3 r;
    r.m[0] = a.m[0] * b.m[0] + a.m[1] * b.m[3];
    r.m[1] = a.m[0] * b.m[1] + a.m[1] * b.m[4];
    r.m[2] = a.m[0] * b.m[2] + a.m[1] * b.m[5] + a.m[2];
    r.m[3] = a.m[3] * b.m[0] + a.m[4] * b.m[3];
    r.m[4] = a.m[3] * b.m[1] + a.m[4] * b.m[4];
    r.m[5] = a.m[3] * b.m[2] + a.m[4] * b.m[5] + a.m[5];
    return r;
}

/** @brief Inverse of an affine transform.
 *
 * A degenerate matrix (zero scale) has no inverse; we return identity rather
 * than producing infinities, so a scale of 0 renders as an untransformed frame
 * instead of NaN garbage. Callers that care should test Determinant() first.
 */
MTX_HD inline Mat3 Invert(const Mat3& t)
{
    const float det = t.Determinant();
    if (det > -1e-12f && det < 1e-12f) return Mat3::Identity();

    const float inv = 1.0f / det;
    Mat3 r;
    r.m[0] =  t.m[4] * inv;
    r.m[1] = -t.m[1] * inv;
    r.m[3] = -t.m[3] * inv;
    r.m[4] =  t.m[0] * inv;
    r.m[2] = -(r.m[0] * t.m[2] + r.m[1] * t.m[5]);
    r.m[5] = -(r.m[3] * t.m[2] + r.m[4] * t.m[5]);
    return r;
}

/** @brief Build scale/rotate/translate about an arbitrary anchor, in pixels.
 *
 * Order is: move anchor to origin, scale, rotate, move back, then translate.
 * Scaling and rotation therefore both happen about the anchor, which is what
 * "anchor point" means to anyone coming from motion-graphics tools.
 */
MTX_HD inline Mat3 MakeTransform(float anchorX, float anchorY,
                                 float scaleX,  float scaleY,
                                 float rotationDegrees,
                                 float translateX, float translateY)
{
    return Mat3::Translate(translateX + anchorX, translateY + anchorY)
         * Mat3::Rotate(rotationDegrees)
         * Mat3::Scale(scaleX, scaleY)
         * Mat3::Translate(-anchorX, -anchorY);
}

} // namespace mtx
