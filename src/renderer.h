#pragma once

#include "gl_util.h"
#include "video_format.h"
#include "xmath.h"
#include "xr_context.h"

// Where the flat virtual screen sits and how big it is.
struct ScreenPlacement {
  Vec3 anchorPosition;     // head position at the last recenter
  // Head heading at the last recenter (in lying mode the full head orientation).
  Mat4 anchorRotation;
  float turn = 0.0f;       // extra turn about the anchor's up axis (stick turning)
  float width = 4.0f;      // meters (arc length when curved)
  float distance = 3.0f;   // meters
  bool curved = false;     // bend around the anchor so every point is equally far
  float zoom = 1.0f;       // 180°/360° video: magnification of the view

  // Where "front" and "up" of the video are.
  Mat4 rotation() const { return anchorRotation * rotationY(turn); }
};

// The highlighted button on a panel, in texture pixels.
struct PanelHover {
  float rect[4] = {0, 0, 0, 0};  // x0, y0, x1, y1
  float radius = 18;             // corner radius; half the size for a round button
  bool danger = false;           // destructive action (quit, delete): highlighted red
};

// Draws the video for one eye: on a flat screen, or as an immersive
// 180/360/fisheye surround computed per pixel from the view direction.
class SceneRenderer {
 public:
  ~SceneRenderer();
  bool init();

  // Draws into the currently bound framebuffer. videoTexture 0 = no video yet.
  // frameAspect is the whole frame's width / height (both eyes, if stereo).
  void drawEye(const EyeMatrices& eye, int eyeIndex, bool srgbTarget, GLuint videoTexture,
               float frameAspect, const VideoFormat& format, bool flipVideo,
               const ScreenPlacement& screen);

  // Draws a textured panel (premultiplied alpha) over what is already there.
  // model maps the unit quad (-0.5..0.5) to the panel in local space. Only
  // the top visibleV (0..1) of the texture is shown. The cursor (panel
  // coords, v = 0 at the top) is drawn as a dot; hover, if given, is
  // highlighted. With a glassBackdrop (a mipmapped copy of the eye image so
  // far), the panel shows it blurred behind its translucent parts, like
  // frosted glass; glassAlpha is the panel's own lowest coverage, used to
  // find its edge.
  void drawPanel(const EyeMatrices& eye, bool srgbTarget, GLuint texture, const Mat4& model,
                 bool showCursor, float cursorU, float cursorV, float visibleV = 1.0f,
                 const PanelHover* hover = nullptr, GLuint glassBackdrop = 0,
                 float glassAlpha = 1.0f);
  // Draws a thin pointer ray from `from` to `to` (local space).
  void drawLaser(const EyeMatrices& eye, bool srgbTarget, Vec3 from, Vec3 to);

  // Brightness, contrast, saturation (-100..100) applied to the video texture
  // while drawing; for pictures mpv does not adjust (photos). 0 = unchanged.
  void setPictureAdjustment(int brightness, int contrast, int saturation);
  // A night sky with a faint aurora around the flat screen (else black).
  void setBackground(bool enabled) { background_ = enabled; }

  // Blurs a mipmapped copy of an eye image (width × height) with a Gaussian
  // at quarter resolution, for glass panels. Returns the blurred texture
  // (linear colour); changes the framebuffer binding and viewport.
  GLuint blurBackdrop(GLuint mipmappedSource, int width, int height);

 private:
  struct ScreenUniforms {
    GLint viewProj, anchor, size, curved, tex, useTexture, encodeSrgb, color, flip, eyeScale,
        eyeOffset, adjust;
  };
  struct SurroundUniforms {
    GLint tex, tangents, rotation, projection, fisheyeFov, encodeSrgb, flip, eyeScale, eyeOffset,
        zoom, adjust;
  };
  void createSky();
  GLuint skyTexture_ = 0;
  bool background_ = true;
  float adjust_[3] = {0, 0, 0};

  struct PanelUniforms {
    GLint mvp, tex, encodeSrgb, cursor, showCursor, visibleV, showHover, hover, hoverRadius,
        hoverDanger, glass, glassAlpha, backdrop, screenSize;
  };
  struct LaserUniforms {
    GLint viewProj, corners, color, encodeSrgb;
  };

  // Glass blur: two quarter-resolution targets, horizontal then vertical pass.
  struct BlurTarget {
    GLuint texture = 0, fbo = 0;
  };
  BlurTarget blur_[2];
  int blurWidth_ = 0, blurHeight_ = 0;
  GLuint blurProgram_ = 0;
  GLint blurSrc_ = -1, blurStep_ = -1, blurLod_ = -1;

  GLuint screenProgram_ = 0;
  GLuint surroundProgram_ = 0;
  GLuint panelProgram_ = 0;
  GLuint laserProgram_ = 0;
  PanelUniforms pu_{};
  LaserUniforms lu_{};
  GLuint vao_ = 0;  // unit quad, also used for vertex-less draws
  GLuint vbo_ = 0;
  GLuint screenVao_ = 0;  // strip of columns that can be bent into a curve
  GLuint screenVbo_ = 0;
  int screenVertexCount_ = 0;
  ScreenUniforms su_{};
  SurroundUniforms ru_{};
};
