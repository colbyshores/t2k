#pragma once

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../game/math_lut.h"   // fastSin/fastCos — see rotateMat below

namespace MathUtils {

inline glm::mat4 identity() {
    return glm::mat4(1.0f);
}

inline glm::mat4 perspective(float fovYDeg, float aspect, float zNear, float zFar) {
    return glm::perspective(glm::radians(fovYDeg), aspect, zNear, zFar);
}

inline glm::mat4 ortho(float left, float right, float bottom, float top,
                        float zNear = -1.0f, float zFar = 1.0f) {
    return glm::ortho(left, right, bottom, top, zNear, zFar);
}

inline glm::mat4 translateMat(const glm::mat4& mat, float x, float y, float z) {
    return glm::translate(mat, glm::vec3(x, y, z));
}

// ROTATION ON THE TRIG TABLE, NOT libm.
//
// This is the hottest trig site in the renderer and it was the last one still
// calling libm: `glm::rotate` computes sin+cos per call, and ONE pickup capsule
// asks for 122 rotations a frame (5 arms + 50 cubes, twice over, plus the
// nucleus) = 244 transcendental calls, every frame, for seconds at a time.
// entity_geometry.cpp's enemy and player meshes add more. On the OG 3DS's
// 268 MHz ARM11 -- no hardware transcendental, no L2 -- that is real money.
//
// SAFE UNDER THE GATE because rotateMat has exactly two callers,
// line_geometry.cpp and entity_geometry.cpp, and both are PRESENTATION
// builders. Nothing under src/game/ uses it, so this cannot move ported
// gameplay math (DOCTRINE.md: gameplay trig stays in double and stays off the
// table -- a ULP there can flip a round() and change how many enemies spawn).
//
// THE ANGLE IS WRAPPED FIRST, and that is load-bearing rather than tidy.
// math_lut.h's error grows with |rad| because `rad * TRIG_SCALE` quantises
// more coarsely as it grows (8.8e-5 by 1000 rad), and this function is handed
// unbounded time-derived phases -- emitSwirl's `sw` carries a `time * 0.11f`
// term that never wraps. Folding into [0,360) first pins every lookup inside
// one period, where the table's own <4.7e-6 bound holds for as long as the
// console stays on.
//
// The body is glm::rotate's own formula, unrolled, so only the source of c/s
// differs; the axis normalize is KEPT because callers legitimately pass
// non-unit axes (buildExplosions uses (1.0, 0.5, 0.3)).
inline glm::mat4 rotateMat(const glm::mat4& mat, float angleDeg, float ax, float ay, float az) {
    float d = angleDeg - 360.0f * std::floor(angleDeg * (1.0f / 360.0f));
    const float a = d * (3.14159265358979323846f / 180.0f);
    const float c = ts::fastCos(a);
    const float s = ts::fastSin(a);

    const glm::vec3 axis = glm::normalize(glm::vec3(ax, ay, az));
    const glm::vec3 temp = (1.0f - c) * axis;

    glm::mat4 R(1.0f);
    R[0][0] = c + temp[0] * axis[0];
    R[0][1] = temp[0] * axis[1] + s * axis[2];
    R[0][2] = temp[0] * axis[2] - s * axis[1];

    R[1][0] = temp[1] * axis[0] - s * axis[2];
    R[1][1] = c + temp[1] * axis[1];
    R[1][2] = temp[1] * axis[2] + s * axis[0];

    R[2][0] = temp[2] * axis[0] + s * axis[1];
    R[2][1] = temp[2] * axis[1] - s * axis[0];
    R[2][2] = c + temp[2] * axis[2];

    glm::mat4 out;
    out[0] = mat[0] * R[0][0] + mat[1] * R[0][1] + mat[2] * R[0][2];
    out[1] = mat[0] * R[1][0] + mat[1] * R[1][1] + mat[2] * R[1][2];
    out[2] = mat[0] * R[2][0] + mat[1] * R[2][1] + mat[2] * R[2][2];
    out[3] = mat[3];
    return out;
}

inline glm::mat4 scaleMat(const glm::mat4& mat, float sx, float sy, float sz) {
    return glm::scale(mat, glm::vec3(sx, sy, sz));
}

// Transform an array of 3D points in-place by a 4x4 matrix.
// points is an array of vec3 (or float[3]) with 'count' elements.
inline void transformPoints(const glm::mat4& mat, glm::vec3* points, int count) {
    for (int i = 0; i < count; i++) {
        glm::vec4 p = mat * glm::vec4(points[i], 1.0f);
        points[i] = glm::vec3(p);
    }
}

} // namespace MathUtils
