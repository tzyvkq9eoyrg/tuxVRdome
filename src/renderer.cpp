#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979f;

const char* kSrgbFunction = R"(
vec3 linearToSrgb(vec3 c) {
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
)";

// Brightness, contrast and saturation for pictures mpv does not adjust (photos).
const char* kAdjustFunction = R"(
uniform vec3 uAdjust;  // brightness, contrast, saturation, each -1..1
vec3 adjustPicture(vec3 c) {
  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
  c = mix(vec3(l), c, 1.0 + uAdjust.z);
  c = (c - 0.18) * (1.0 + uAdjust.y) + 0.18;
  return max(c * exp2(uAdjust.x * 1.5), 0.0);
}
)";

// The background behind the flat screen: a calm night sky with a faint
// aurora, rendered once into an equirectangular texture (row 0 = the top).
const char* kSkyFragment = R"(
in vec2 vNdc;
out vec4 fragColor;
const float PI = 3.14159265359;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x),
             mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
float fbm(vec2 p) {
  float v = 0.0, a = 0.5;
  for (int i = 0; i < 5; ++i) {
    v += a * noise(p);
    p *= 2.03;
    a *= 0.5;
  }
  return v;
}
// The sky at a direction above the horizon: gradient, stars, aurora, and a
// dark mountain ridge along the horizon.
vec3 sky(float lon, float lat) {
  // Noise along the horizon on a circle, so there is no seam behind you.
  vec2 around = vec2(cos(lon), sin(lon));
  float ridge = 0.025 + 0.06 * fbm(around * 2.2 + 21.0) + 0.02 * fbm(around * 9.0 + 4.0);
  if (lat < ridge) {
    // Mountains: nearly black, a faint cold rim light along the crest.
    return mix(vec3(0.003, 0.004, 0.007), vec3(0.012, 0.022, 0.030),
               smoothstep(ridge - 0.02, ridge, lat));
  }
  vec3 color = mix(vec3(0.010, 0.018, 0.034), vec3(0.002, 0.004, 0.010), smoothstep(0.0, 1.1, lat));
  // Stars.
  vec2 grid = vec2(lon * cos(lat), lat) * 140.0;
  vec2 cell = floor(grid);
  float h = hash(cell);
  if (h > 0.986) {
    vec2 offset = vec2(hash(cell + 3.1), hash(cell + 7.7)) - 0.5;
    float d = length(fract(grid) - 0.5 - offset * 0.6);
    color += vec3(0.8, 0.85, 1.0) * smoothstep(0.14, 0.0, d) * (h - 0.986) / 0.014 * 0.5;
  }
  // Aurora: curtains along a wavy band, with fine vertical rays.
  float band = 0.40 + 0.08 * sin(lon * 2.0 + 0.7) + 0.08 * (fbm(around * 1.5 + 3.0) - 0.5);
  float above = lat - band;
  float curtain = exp(-max(-above, 0.0) * 16.0) * exp(-max(above, 0.0) * 3.0);
  float rays = 0.45 + 0.55 * pow(noise(around * 45.0), 2.0);
  float strength = smoothstep(0.38, 0.72, fbm(around * 1.2 + 11.0));
  float glow = curtain * rays * strength * (0.6 + 0.4 * fbm(around * 4.0 + 5.0));
  vec3 hue = mix(vec3(0.10, 0.85, 0.45), vec3(0.45, 0.20, 0.75), smoothstep(0.0, 0.35, above));
  color += hue * glow * 0.2 * smoothstep(0.0, 0.12, lat);
  return color;
}

void main() {
  // Texture row 0 (the framebuffer's bottom) is the zenith.
  float lon = vNdc.x * PI;
  float lat = -vNdc.y * 0.5 * PI;
  vec3 color;
  if (lat >= 0.0) {
    color = sky(lon, lat);
  } else {
    // A frozen lake mirroring the sky faintly (a little rippled), turning
    // into dark snow towards your feet.
    float ripple = (noise(vec2(lon * 40.0, lat * 300.0)) - 0.5) * 0.01;
    vec3 mirror = sky(lon + ripple, -lat) * 0.3;
    vec3 snow = vec3(0.004, 0.006, 0.010);
    color = mix(mirror, snow, smoothstep(-0.05, -0.7, lat));
  }
  fragColor = vec4(color, 1.0);
}
)";

constexpr int kScreenColumns = 64;

const char* kScreenVertex = R"(#version 330 core
layout(location = 0) in vec2 aPos;  // -0.5 .. 0.5 across the screen
uniform mat4 uViewProj;
uniform mat4 uAnchor;  // recentered head position and heading
uniform vec3 uSize;    // width, height, distance
uniform bool uCurved;
uniform bool uFlip;
out vec2 vUV;
void main() {
  vUV = vec2(aPos.x + 0.5, 0.5 - aPos.y);  // mpv stores frames top row first
  if (uFlip) vUV.y = 1.0 - vUV.y;
  vec3 p;
  if (uCurved) {
    // Width is the arc length on a circle around the viewer.
    float angle = clamp(aPos.x * uSize.x / uSize.z, -2.6, 2.6);
    p = vec3(uSize.z * sin(angle), aPos.y * uSize.y, -uSize.z * cos(angle));
  } else {
    p = vec3(aPos.x * uSize.x, aPos.y * uSize.y, -uSize.z);
  }
  gl_Position = uViewProj * uAnchor * vec4(p, 1.0);
}
)";

const char* kScreenFragment = R"(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform bool uUseTexture;
uniform bool uEncodeSrgb;
uniform vec3 uColor;
uniform vec2 uEyeScale;   // part of the frame that belongs to this eye
uniform vec2 uEyeOffset;
void main() {
  vec3 c = uUseTexture ? adjustPicture(texture(uTex, uEyeOffset + vUV * uEyeScale).rgb) : uColor;
  c = clamp(c, 0.0, 1.0);
  fragColor = vec4(uEncodeSrgb ? linearToSrgb(c) : c, 1.0);
}
)";

// One triangle covering the whole viewport.
const char* kSurroundVertex = R"(#version 330 core
out vec2 vNdc;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
  vNdc = p;
  gl_Position = vec4(p, 0.0, 1.0);
}
)";

const char* kSurroundFragment = R"(
in vec2 vNdc;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec4 uTangents;   // tan of the fov angles: left, right, down, up
uniform mat3 uRotation;   // eye orientation relative to the recentered front
uniform int uProjection;  // 0 = 180, 1 = 360, 2 = fisheye, 3 = equi-angular cubemap,
                          // 4 = cubemap turned to a 2x3 grid
uniform float uFisheyeFov;
uniform bool uEncodeSrgb;
uniform bool uFlip;
uniform vec2 uEyeScale;
uniform vec2 uEyeOffset;
uniform float uZoom;      // > 1 magnifies
const float PI = 3.14159265359;

// YouTube's equi-angular cubemap, laid out like ffmpeg's v360 "eac" input:
// a 3x2 grid with left, front, right on top and down, back, up below, the
// bottom row turned by 270, 90 and 270 degrees.
vec2 eacUv(vec3 dir) {
  vec3 v = vec3(dir.x, -dir.y, -dir.z);  // v360 axes: x right, y down, z forward
  float phi = atan(v.x, v.z);
  float theta = asin(clamp(v.y, -1.0, 1.0));
  const float Q = PI / 4.0;
  int face;  // grid position: 0 left, 1 front, 2 right, 3 down, 4 back, 5 up
  float phiNorm;
  if (phi >= -Q && phi < Q) {
    face = 1;
    phiNorm = phi;
  } else if (phi >= -3.0 * Q && phi < -Q) {
    face = 0;
    phiNorm = phi + PI / 2.0;
  } else if (phi >= Q && phi < 3.0 * Q) {
    face = 2;
    phiNorm = phi - PI / 2.0;
  } else {
    face = 4;
    phiNorm = phi + (phi > 0.0 ? -PI : PI);
  }
  float threshold = atan(cos(phiNorm));
  if (theta > threshold) face = 3;
  else if (theta < -threshold) face = 5;

  vec2 f;
  if (face == 0)      f = vec2(-v.z / v.x, -v.y / v.x);
  else if (face == 1) f = vec2( v.x / v.z,  v.y / v.z);
  else if (face == 2) f = vec2(-v.z / v.x,  v.y / v.x);
  else if (face == 3) f = vec2( v.x / v.y, -v.z / v.y);
  else if (face == 4) f = vec2( v.x / v.z, -v.y / v.z);
  else                f = vec2(-v.x / v.y, -v.z / v.y);
  if (face == 4) f = vec2(-f.y, f.x);                    // turned 90 degrees
  else if (face == 3 || face == 5) f = vec2(f.y, -f.x);  // turned 270 degrees

  f = (2.0 / PI) * atan(f) + 0.5;  // equi-angular: equal angle per pixel
  float column = float(face - 3 * (face / 3));
  float row = float(face / 3);
  return vec2((column + f.x) / 3.0, (row + f.y) / 2.0);
}

void main() {
  vec2 t = vNdc * 0.5 + 0.5;
  vec3 dir = normalize(uRotation * vec3(mix(uTangents.x, uTangents.y, t.x) / uZoom,
                                        mix(uTangents.z, uTangents.w, t.y) / uZoom, -1.0));
  vec2 uv;
  bool inside = true;
  if (uProjection == 3) {
    uv = eacUv(dir);
  } else if (uProjection == 4) {
    // The same cubemap turned 90 degrees counterclockwise (2x3 grid), as
    // YouTube packs each eye of stereo 360 video.
    vec2 s = eacUv(dir);
    uv = vec2(s.y, 1.0 - s.x);
  } else if (uProjection == 2) {
    // Equidistant fisheye: distance from the center grows linearly with the
    // angle from the forward axis.
    float theta = acos(clamp(-dir.z, -1.0, 1.0));
    float r = theta / uFisheyeFov;  // 0.5 at the edge of the lens circle
    vec2 d = dir.xy;
    float len = length(d);
    d = len > 1e-6 ? d / len : vec2(0.0);
    uv = vec2(0.5 + r * d.x, 0.5 - r * d.y);
    inside = r <= 0.5;
  } else {
    float lon = atan(dir.x, -dir.z);  // 0 = front
    float lat = asin(clamp(dir.y, -1.0, 1.0));
    float span = uProjection == 0 ? PI : 2.0 * PI;
    uv = vec2(0.5 + lon / span, 0.5 - lat / PI);
    inside = abs(lon) <= 0.5 * span;
  }
  if (uFlip) uv.y = 1.0 - uv.y;

  // Mip selection from derivatives; for 360 pick the version without the
  // jump at the back seam so no line appears there.
  vec2 dx = dFdx(uv), dy = dFdy(uv);
  vec2 shifted = vec2(fract(uv.x + 0.5), uv.y);
  vec2 dxs = dFdx(shifted), dys = dFdy(shifted);
  if (abs(dxs.x) + abs(dys.x) < abs(dx.x) + abs(dy.x)) {
    dx = dxs;
    dy = dys;
  }
  // Cube face edges jump across the frame; the full-resolution level avoids
  // seams there and roughly matches the headset's resolution for 8K video.
  if (uProjection == 3 || uProjection == 4) {
    dx = vec2(0.0);
    dy = vec2(0.0);
  }

  vec2 eyeUv = uEyeOffset + clamp(uv, 0.0005, 0.9995) * uEyeScale;
  vec3 c = inside ? adjustPicture(textureGrad(uTex, eyeUv, dx * uEyeScale, dy * uEyeScale).rgb)
                  : vec3(0.0);
  c = clamp(c, 0.0, 1.0);
  fragColor = vec4(uEncodeSrgb ? linearToSrgb(c) : c, 1.0);
}
)";

// One pass of a separable Gaussian blur (sigma 6 texels of the target):
// each pixel averages 31 neighbours along uStep, read at mip level uLod.
const char* kBlurFragment = R"(
in vec2 vNdc;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform vec2 uStep;  // one target texel along the blur direction, in uv
uniform float uLod;
void main() {
  vec2 uv = vNdc * 0.5 + 0.5;
  vec3 sum = vec3(0.0);
  float total = 0.0;
  for (int i = -15; i <= 15; ++i) {
    float weight = exp(-float(i * i) / 72.0);
    sum += textureLod(uSrc, uv + uStep * float(i), uLod).rgb * weight;
    total += weight;
  }
  fragColor = vec4(sum / total, 1.0);
}
)";

const char* kPanelVertex = R"(#version 330 core
layout(location = 0) in vec2 aPos;
uniform mat4 uMVP;
uniform float uVisibleV;  // the panel shows only the top part of the texture
out vec2 vUV;
out vec2 vLocal;  // 0..1 across the visible panel, v = 0 at the top
void main() {
  vLocal = vec2(aPos.x + 0.5, 0.5 - aPos.y);  // texture rows are stored top first
  vUV = vLocal * vec2(1.0, uVisibleV);
  gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
)";

const char* kPanelFragment = R"(
in vec2 vUV;
in vec2 vLocal;
out vec4 fragColor;
uniform sampler2D uTex;  // sRGB texture: sampling returns linear color
uniform bool uEncodeSrgb;
uniform vec2 uCursor;    // in vLocal coordinates
uniform bool uShowCursor;
uniform float uVisibleV;
uniform bool uShowHover;
uniform vec4 uHover;     // hovered button in texture pixels: x0, y0, x1, y1
uniform float uHoverRadius;
uniform bool uHoverDanger;
uniform bool uGlass;
uniform float uGlassAlpha;   // the panel's lowest coverage: below it is outside
uniform sampler2D uBackdrop; // the eye image so far, blurred (at a lower resolution)
uniform vec2 uScreenSize;    // the eye image's size in pixels

// Signed distance to a rounded rectangle (negative inside).
float roundedRectDistance(vec2 p, vec4 rect, float radius) {
  vec2 center = 0.5 * (rect.xy + rect.zw);
  vec2 q = abs(p - center) - 0.5 * (rect.zw - rect.xy) + radius;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

void main() {
  vec4 c = texture(uTex, vUV);  // premultiplied alpha
  // The button's icon or text: opaque light strokes in the panel texture
  // (glass buttons themselves are translucent, Material ones dark).
  float lum = dot(c.rgb / max(c.a, 1e-4), vec3(0.2126, 0.7152, 0.0722));
  float ink = uGlass ? smoothstep(0.6, 0.9, c.a) * smoothstep(0.5, 0.8, lum)
                     : smoothstep(0.15, 0.35, lum);
  if (uGlass) {
    // Frosted glass: behind the panel's translucent parts, the scene blurred.
    float inside = clamp(c.a / uGlassAlpha, 0.0, 1.0);
    vec2 screenUV = gl_FragCoord.xy / uScreenSize;
    c = vec4(c.rgb + (1.0 - c.a) * texture(uBackdrop, screenUV).rgb * inside, max(c.a, inside));
  }
  if (uShowHover) {
    vec2 p = vUV * vec2(textureSize(uTex, 0));
    float d = roundedRectDistance(p, uHover, uHoverRadius);
    float inside = 1.0 - smoothstep(-1.0, 0.0, d);
    float rim = (1.0 - smoothstep(1.0, 2.5, abs(d + 2.0))) * inside;  // ring inside the edge
    if (uHoverDanger) {
      // Red glass (or a red Material button); the icon turns white and stays readable.
      vec3 red = uGlass ? vec3(0.42, 0.02, 0.02) : vec3(0.60, 0.05, 0.04);
      c.rgb = mix(c.rgb, red * c.a, inside * (1.0 - ink) * (uGlass ? 0.75 : 0.95));
      c.rgb = mix(c.rgb, vec3(1.0) * c.a, inside * ink);
      if (uGlass) c.rgb = mix(c.rgb, vec3(1.0, 0.62, 0.58) * c.a, rim * 0.5);
    } else if (uGlass) {
      // Lit glass: a little brighter, with a light rim.
      c.rgb += vec3(0.08) * c.a * inside * (1.0 - ink);
      c.rgb = mix(c.rgb, vec3(0.95) * c.a, rim * 0.45);
    } else {
      // Values are linear: +0.06 lifts a dark button about as much as the
      // old sRGB hover colour did.
      c.rgb += vec3(0.06) * c.a * inside;
      c.rgb = mix(c.rgb, vec3(0.35) * c.a, rim);
    }
  }
  if (uShowCursor) {
    // Measured in texture pixels, so the dot has the same size on every panel.
    vec2 cursorUV = uCursor * vec2(1.0, uVisibleV);
    float r = length((vUV - cursorUV) * vec2(textureSize(uTex, 0)));
    float ring = 1.0 - smoothstep(17.0, 21.0, r);
    float core = 1.0 - smoothstep(11.0, 14.0, r);
    c = mix(c, vec4(0.05, 0.05, 0.05, 1.0), ring);
    c = mix(c, vec4(1.0), core);
  }
  if (uEncodeSrgb && c.a > 0.0) c.rgb = linearToSrgb(c.rgb / c.a) * c.a;
  fragColor = c;
}
)";

// A quad given as four corners in local space, drawn as a triangle strip.
const char* kLaserVertex = R"(#version 330 core
uniform mat4 uViewProj;
uniform vec3 uCorners[4];
void main() {
  gl_Position = uViewProj * vec4(uCorners[gl_VertexID], 1.0);
}
)";

const char* kLaserFragment = R"(
out vec4 fragColor;
uniform vec4 uColor;  // premultiplied, linear
uniform bool uEncodeSrgb;
void main() {
  vec4 c = uColor;
  if (uEncodeSrgb && c.a > 0.0) c.rgb = linearToSrgb(c.rgb / c.a) * c.a;
  fragColor = c;
}
)";

std::string fragmentSource(const char* body, const char* extra = "") {
  return std::string("#version 330 core\n") + kSrgbFunction + extra + body;
}

void eyeRect(StereoLayout stereo, int eyeIndex, float scale[2], float offset[2]) {
  scale[0] = scale[1] = 1.0f;
  offset[0] = offset[1] = 0.0f;
  if (stereo == StereoLayout::SideBySide) {
    scale[0] = 0.5f;
    offset[0] = 0.5f * eyeIndex;  // left eye = left half
  } else if (stereo == StereoLayout::TopBottom) {
    scale[1] = 0.5f;
    offset[1] = 0.5f * eyeIndex;  // left eye = top half
  }
}

}  // namespace

SceneRenderer::~SceneRenderer() {
  glDeleteBuffers(1, &vbo_);
  glDeleteVertexArrays(1, &vao_);
  glDeleteBuffers(1, &screenVbo_);
  glDeleteVertexArrays(1, &screenVao_);
  glDeleteProgram(screenProgram_);
  glDeleteProgram(surroundProgram_);
  glDeleteProgram(panelProgram_);
  glDeleteProgram(laserProgram_);
  glDeleteProgram(blurProgram_);
  glDeleteTextures(1, &skyTexture_);
  for (BlurTarget& t : blur_) {
    glDeleteFramebuffers(1, &t.fbo);
    glDeleteTextures(1, &t.texture);
  }
}

bool SceneRenderer::init() {
  screenProgram_ =
      compileProgram(kScreenVertex, fragmentSource(kScreenFragment, kAdjustFunction).c_str());
  surroundProgram_ =
      compileProgram(kSurroundVertex, fragmentSource(kSurroundFragment, kAdjustFunction).c_str());
  panelProgram_ = compileProgram(kPanelVertex, fragmentSource(kPanelFragment).c_str());
  laserProgram_ = compileProgram(kLaserVertex, fragmentSource(kLaserFragment).c_str());
  if (!screenProgram_ || !surroundProgram_ || !panelProgram_ || !laserProgram_) return false;

  pu_.mvp = glGetUniformLocation(panelProgram_, "uMVP");
  pu_.tex = glGetUniformLocation(panelProgram_, "uTex");
  pu_.encodeSrgb = glGetUniformLocation(panelProgram_, "uEncodeSrgb");
  pu_.cursor = glGetUniformLocation(panelProgram_, "uCursor");
  pu_.showCursor = glGetUniformLocation(panelProgram_, "uShowCursor");
  pu_.visibleV = glGetUniformLocation(panelProgram_, "uVisibleV");
  pu_.showHover = glGetUniformLocation(panelProgram_, "uShowHover");
  pu_.hover = glGetUniformLocation(panelProgram_, "uHover");
  pu_.hoverRadius = glGetUniformLocation(panelProgram_, "uHoverRadius");
  pu_.hoverDanger = glGetUniformLocation(panelProgram_, "uHoverDanger");
  pu_.glass = glGetUniformLocation(panelProgram_, "uGlass");
  pu_.glassAlpha = glGetUniformLocation(panelProgram_, "uGlassAlpha");
  pu_.backdrop = glGetUniformLocation(panelProgram_, "uBackdrop");
  pu_.screenSize = glGetUniformLocation(panelProgram_, "uScreenSize");

  blurProgram_ = compileProgram(kSurroundVertex, fragmentSource(kBlurFragment).c_str());
  if (!blurProgram_) return false;
  blurSrc_ = glGetUniformLocation(blurProgram_, "uSrc");
  blurStep_ = glGetUniformLocation(blurProgram_, "uStep");
  blurLod_ = glGetUniformLocation(blurProgram_, "uLod");

  lu_.viewProj = glGetUniformLocation(laserProgram_, "uViewProj");
  lu_.corners = glGetUniformLocation(laserProgram_, "uCorners");
  lu_.color = glGetUniformLocation(laserProgram_, "uColor");
  lu_.encodeSrgb = glGetUniformLocation(laserProgram_, "uEncodeSrgb");

  su_.viewProj = glGetUniformLocation(screenProgram_, "uViewProj");
  su_.anchor = glGetUniformLocation(screenProgram_, "uAnchor");
  su_.size = glGetUniformLocation(screenProgram_, "uSize");
  su_.curved = glGetUniformLocation(screenProgram_, "uCurved");
  su_.tex = glGetUniformLocation(screenProgram_, "uTex");
  su_.useTexture = glGetUniformLocation(screenProgram_, "uUseTexture");
  su_.encodeSrgb = glGetUniformLocation(screenProgram_, "uEncodeSrgb");
  su_.color = glGetUniformLocation(screenProgram_, "uColor");
  su_.flip = glGetUniformLocation(screenProgram_, "uFlip");
  su_.eyeScale = glGetUniformLocation(screenProgram_, "uEyeScale");
  su_.eyeOffset = glGetUniformLocation(screenProgram_, "uEyeOffset");

  ru_.tex = glGetUniformLocation(surroundProgram_, "uTex");
  ru_.tangents = glGetUniformLocation(surroundProgram_, "uTangents");
  ru_.rotation = glGetUniformLocation(surroundProgram_, "uRotation");
  ru_.projection = glGetUniformLocation(surroundProgram_, "uProjection");
  ru_.fisheyeFov = glGetUniformLocation(surroundProgram_, "uFisheyeFov");
  ru_.encodeSrgb = glGetUniformLocation(surroundProgram_, "uEncodeSrgb");
  ru_.flip = glGetUniformLocation(surroundProgram_, "uFlip");
  ru_.eyeScale = glGetUniformLocation(surroundProgram_, "uEyeScale");
  ru_.eyeOffset = glGetUniformLocation(surroundProgram_, "uEyeOffset");
  ru_.zoom = glGetUniformLocation(surroundProgram_, "uZoom");
  ru_.adjust = glGetUniformLocation(surroundProgram_, "uAdjust");
  su_.adjust = glGetUniformLocation(screenProgram_, "uAdjust");

  const float quad[] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};
  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);
  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

  std::vector<float> strip;
  for (int i = 0; i <= kScreenColumns; ++i) {
    const float x = -0.5f + static_cast<float>(i) / kScreenColumns;
    strip.insert(strip.end(), {x, -0.5f, x, 0.5f});
  }
  screenVertexCount_ = static_cast<int>(strip.size() / 2);
  glGenVertexArrays(1, &screenVao_);
  glGenBuffers(1, &screenVbo_);
  glBindVertexArray(screenVao_);
  glBindBuffer(GL_ARRAY_BUFFER, screenVbo_);
  glBufferData(GL_ARRAY_BUFFER, strip.size() * sizeof(float), strip.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glBindVertexArray(0);
  createSky();  // needs vao_
  return true;
}

void SceneRenderer::drawEye(const EyeMatrices& eye, int eyeIndex, bool srgbTarget,
                            GLuint videoTexture, float frameAspect, const VideoFormat& format,
                            bool flipVideo, const ScreenPlacement& screen) {
  // mpv may leave its own GL state behind, so set everything we rely on.
  if (srgbTarget) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.004f, 0.004f, 0.006f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  float eyeScale[2], eyeOffset[2];
  eyeRect(format.stereo, eyeIndex, eyeScale, eyeOffset);
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(vao_);

  // Draws `texture` around the viewer, computed per pixel from the view
  // direction, turned with the screen (head translation is ignored on
  // purpose: surround video has no parallax).
  // roomFixed: stays put in the room instead (the background sky).
  auto surround = [&](GLuint texture, int projection, float fisheyeFov, const float scale[2],
                      const float offset[2], bool flip, float zoom, const float adjust[3],
                      bool roomFixed = false) {
    const Mat4 r = roomFixed ? rotation(eye.orientation)
                             : transposedRotation(screen.rotation()) * rotation(eye.orientation);
    const float rot3[9] = {r.m[0], r.m[1], r.m[2], r.m[4], r.m[5], r.m[6], r.m[8], r.m[9], r.m[10]};
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(vao_);
    glUseProgram(surroundProgram_);
    glUniform1i(ru_.tex, 0);
    glUniform4f(ru_.tangents, eye.tanLeft, eye.tanRight, eye.tanDown, eye.tanUp);
    glUniformMatrix3fv(ru_.rotation, 1, GL_FALSE, rot3);
    glUniform1i(ru_.projection, projection);
    glUniform1f(ru_.fisheyeFov, fisheyeFov);
    glUniform1i(ru_.encodeSrgb, !srgbTarget);
    glUniform1i(ru_.flip, flip);
    glUniform2fv(ru_.eyeScale, 1, scale);
    glUniform2fv(ru_.eyeOffset, 1, offset);
    glUniform1f(ru_.zoom, zoom);
    glUniform3fv(ru_.adjust, 1, adjust);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  };
  const float noAdjust[3] = {0, 0, 0};

  if (format.projection == Projection::Flat) {
    if (background_ && skyTexture_) {  // the night sky behind the screen, fixed in the room
      const float whole[2] = {1, 1}, none[2] = {0, 0};
      surround(skyTexture_, 1, 0.0f, whole, none, false, 1.0f, noAdjust, true);
    }
    glBindTexture(GL_TEXTURE_2D, videoTexture);
    // One eye's picture keeps the frame's aspect only after undoing the packing.
    const float eyeAspect = frameAspect * eyeScale[0] / eyeScale[1];
    const Mat4 anchor = translation(screen.anchorPosition) * screen.rotation();
    const Mat4 viewProj = eye.projection * eye.view;
    glBindVertexArray(screenVao_);
    glUseProgram(screenProgram_);
    glUniformMatrix4fv(su_.viewProj, 1, GL_FALSE, viewProj.m);
    glUniformMatrix4fv(su_.anchor, 1, GL_FALSE, anchor.m);
    glUniform3f(su_.size, screen.width, screen.width / eyeAspect, screen.distance);
    glUniform1i(su_.curved, screen.curved);
    glUniform1i(su_.tex, 0);
    glUniform1i(su_.useTexture, videoTexture != 0);
    glUniform1i(su_.encodeSrgb, !srgbTarget);
    glUniform3f(su_.color, 0.02f, 0.02f, 0.025f);
    glUniform1i(su_.flip, flipVideo);
    glUniform2fv(su_.eyeScale, 1, eyeScale);
    glUniform2fv(su_.eyeOffset, 1, eyeOffset);
    glUniform3fv(su_.adjust, 1, adjust_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, screenVertexCount_);
  } else if (videoTexture) {
    int projection = 0;
    float fisheyeFov = 0.0f;
    switch (format.projection) {
      case Projection::Equirect360: projection = 1; break;
      case Projection::Fisheye190: projection = 2; fisheyeFov = 190.0f * kPi / 180.0f; break;
      case Projection::Fisheye200: projection = 2; fisheyeFov = 200.0f * kPi / 180.0f; break;
      case Projection::EquiAngularCubemap: {
        // A cubemap is wider than tall; a tall eye picture holds it turned.
        const float eyeAspect = frameAspect * eyeScale[0] / eyeScale[1];
        projection = eyeAspect < 1.0f ? 4 : 3;
        break;
      }
      default: break;
    }
    surround(videoTexture, projection, fisheyeFov, eyeScale, eyeOffset, flipVideo, screen.zoom,
             adjust_);
  }

  glBindVertexArray(0);
  glDisable(GL_FRAMEBUFFER_SRGB);
}

namespace {

// State for drawing translucent things over the scene.
void beginOverlay(bool srgbTarget) {
  if (srgbTarget) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void endOverlay() {
  glBindVertexArray(0);
  glDisable(GL_BLEND);
  glDisable(GL_FRAMEBUFFER_SRGB);
}

}  // namespace

void SceneRenderer::drawPanel(const EyeMatrices& eye, bool srgbTarget, GLuint texture,
                              const Mat4& model, bool showCursor, float cursorU,
                              float cursorV, float visibleV, const PanelHover* hover,
                              GLuint glassBackdrop, float glassAlpha) {
  beginOverlay(srgbTarget);
  const Mat4 mvp = eye.projection * eye.view * model;
  glUseProgram(panelProgram_);
  glUniformMatrix4fv(pu_.mvp, 1, GL_FALSE, mvp.m);
  glUniform1f(pu_.visibleV, visibleV);
  glUniform1i(pu_.showHover, hover != nullptr);
  if (hover) {
    glUniform4fv(pu_.hover, 1, hover->rect);
    glUniform1f(pu_.hoverRadius, hover->radius);
    glUniform1i(pu_.hoverDanger, hover->danger);
  }
  glUniform1i(pu_.tex, 0);
  glUniform1i(pu_.backdrop, 1);
  glUniform1i(pu_.glass, glassBackdrop != 0);
  glUniform1f(pu_.glassAlpha, glassAlpha);
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  glUniform2f(pu_.screenSize, static_cast<float>(viewport[2]), static_cast<float>(viewport[3]));
  glUniform1i(pu_.encodeSrgb, !srgbTarget);
  glUniform2f(pu_.cursor, cursorU, cursorV);
  glUniform1i(pu_.showCursor, showCursor);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, glassBackdrop);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  endOverlay();
}

void SceneRenderer::setPictureAdjustment(int brightness, int contrast, int saturation) {
  adjust_[0] = brightness / 100.0f;
  adjust_[1] = contrast / 100.0f;
  adjust_[2] = saturation / 100.0f;
}

// Renders the night sky once into a mipmapped equirectangular texture.
void SceneRenderer::createSky() {
  constexpr int w = 4096, h = 2048;
  const GLuint program = compileProgram(kSurroundVertex, fragmentSource(kSkyFragment).c_str());
  if (!program) return;
  const int levels = 1 + static_cast<int>(std::floor(std::log2(w)));
  glGenTextures(1, &skyTexture_);
  glBindTexture(GL_TEXTURE_2D, skyTexture_);
  glTexStorage2D(GL_TEXTURE_2D, levels, GL_SRGB8_ALPHA8, w, h);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, skyTexture_, 0);
  glViewport(0, 0, w, h);
  glEnable(GL_FRAMEBUFFER_SRGB);  // the shader writes linear colour
  glDisable(GL_BLEND);
  glUseProgram(program);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &fbo);
  glDeleteProgram(program);
  glGenerateMipmap(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
}

GLuint SceneRenderer::blurBackdrop(GLuint mipmappedSource, int width, int height) {
  // Quarter resolution: the source's mip level 2 is already a 4x4 average.
  const int w = std::max(1, width / 4), h = std::max(1, height / 4);
  if (w != blurWidth_ || h != blurHeight_) {
    for (BlurTarget& t : blur_) {
      if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
      if (t.texture) glDeleteTextures(1, &t.texture);
      glGenTextures(1, &t.texture);
      glBindTexture(GL_TEXTURE_2D, t.texture);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, w, h);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glGenFramebuffers(1, &t.fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.texture, 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    blurWidth_ = w;
    blurHeight_ = h;
  }
  glDisable(GL_FRAMEBUFFER_SRGB);  // linear values into float targets
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glViewport(0, 0, w, h);
  glUseProgram(blurProgram_);
  glUniform1i(blurSrc_, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(vao_);
  // Horizontal, from the source's level 2; then vertical, from that result.
  glBindFramebuffer(GL_FRAMEBUFFER, blur_[0].fbo);
  glBindTexture(GL_TEXTURE_2D, mipmappedSource);
  glUniform2f(blurStep_, 1.0f / w, 0.0f);
  glUniform1f(blurLod_, 2.0f);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindFramebuffer(GL_FRAMEBUFFER, blur_[1].fbo);
  glBindTexture(GL_TEXTURE_2D, blur_[0].texture);
  glUniform2f(blurStep_, 0.0f, 1.0f / h);
  glUniform1f(blurLod_, 0.0f);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glBindTexture(GL_TEXTURE_2D, 0);
  return blur_[1].texture;
}

void SceneRenderer::drawLaser(const EyeMatrices& eye, bool srgbTarget, Vec3 from, Vec3 to) {
  // A flat strip that always faces the eye.
  const Vec3 dir = normalize(to - from);
  const Vec3 side = cross(dir, normalize(eye.position - from));
  if (length(side) < 1e-4f) return;
  const Vec3 offset = normalize(side) * 0.0015f;  // 3 mm wide
  const Vec3 corners[4] = {from - offset, from + offset, to - offset, to + offset};

  beginOverlay(srgbTarget);
  const Mat4 viewProj = eye.projection * eye.view;
  glUseProgram(laserProgram_);
  glUniformMatrix4fv(lu_.viewProj, 1, GL_FALSE, viewProj.m);
  glUniform3fv(lu_.corners, 4, &corners[0].x);
  glUniform4f(lu_.color, 0.25f * 0.7f, 0.55f * 0.7f, 1.0f * 0.7f, 0.7f);
  glUniform1i(lu_.encodeSrgb, !srgbTarget);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  endOverlay();
}
