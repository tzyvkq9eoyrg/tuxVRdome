#pragma once

#include "gl_util.h"
#include "xmath.h"

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>

#include <utility>
#include <vector>

struct EyeMatrices {
  Mat4 projection;
  Mat4 view;
  // The same view as raw orientation + field-of-view tangents, for shaders
  // that compute a view ray per pixel.
  Quat orientation;
  Vec3 position;
  float tanLeft = 0, tanRight = 0, tanUp = 0, tanDown = 0;
};

struct HandState {
  bool poseValid = false;
  Vec3 aimPosition;      // pointing ray origin, local space
  Quat aimOrientation;   // the ray points along -Z of this orientation
  bool select = false;          // trigger held
  bool selectPressed = false;   // trigger went down this frame
  bool grip = false;            // grip button held
  bool gripPressed = false;     // grip went down this frame
  float stickX = 0, stickY = 0;
};

struct ControllerState {
  HandState hands[2];  // 0 = left, 1 = right
  bool menuPressed = false;
  bool playPausePressed = false;
  bool recenterPressed = false;
};

// Owns the OpenXR instance, session, swapchains and reference spaces.
//
// SteamVR on Linux only composites Vulkan images reliably, so the session uses
// Vulkan. The scene itself is drawn with OpenGL (mpv renders with OpenGL) into
// per-eye images whose memory is shared between both APIs; Vulkan then blits
// them into the OpenXR swapchains. Semaphores shared with GL order the work.
class XrContext {
 public:
  ~XrContext();

  // Debug mode: fill the eyes with a pulsing color straight from Vulkan,
  // bypassing GL and the shared semaphores. Set before init().
  void setTestPattern(bool enabled) { testPattern_ = enabled; }

  // Requires the GL context to be current.
  bool init();
  // Handles session state changes. Sets quit when the runtime wants us to exit.
  void pollEvents(bool& quit);
  bool isRunning() const { return running_; }

  // Waits for and begins the next frame. Returns true if the eyes should be drawn.
  bool beginFrame();
  // Submits the frame (and the drawn eyes, if beginFrame() returned true).
  void endFrame();

  int eyeCount() const { return static_cast<int>(eyes_.size()); }
  const EyeMatrices& eye(int i) const { return eyes_[i]; }
  // Binds eye i's GL framebuffer and viewport.
  void bindEye(int i);
  int eyeWidth(int i) const { return swapchains_[i].width; }
  int eyeHeight(int i) const { return swapchains_[i].height; }
  GLuint eyeFramebuffer(int i) const { return sharedImages_[i].fbo; }
  // True if the eye images encode to sRGB themselves (shaders then output linear).
  bool srgbSwapchain() const { return srgb_; }

  // Head pose in the local reference space at the predicted display time.
  bool headPose(Vec3& position, Quat& orientation) const;

  // Controller input, updated by beginFrame().
  const ControllerState& controllers() const { return controllers_; }

  // Time the last endFrame() spent waiting for the previous frame's GPU copy.
  double lastFenceWaitMs() const { return lastFenceWaitMs_; }

 private:
  struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int width = 0;
    int height = 0;
    std::vector<VkImage> images;
    uint32_t acquired = 0;
  };

  // An eye render target visible to both Vulkan and OpenGL.
  struct SharedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    GLuint memoryObject = 0;
    GLuint texture = 0;
    GLuint fbo = 0;
  };

  bool createActions();
  void suggestBindings(const char* profile,
                       const std::vector<std::pair<XrAction, const char*>>& bindings);
  void updateControllers();
  bool createVulkanDevice();
  bool createSwapchains();
  bool createSharedImage(int width, int height, SharedImage& out);
  bool createSharedSemaphore(VkSemaphore& vk, GLuint& gl);
  bool initSharedImageLayouts();
  void copyEyesToSwapchains();
  bool check(XrResult result, const char* what) const;

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace localSpace_ = XR_NULL_HANDLE;
  XrSpace viewSpace_ = XR_NULL_HANDLE;
  XrActionSet actionSet_ = XR_NULL_HANDLE;
  XrAction aimAction_ = XR_NULL_HANDLE;
  XrAction selectAction_ = XR_NULL_HANDLE;
  XrAction gripAction_ = XR_NULL_HANDLE;
  XrAction menuAction_ = XR_NULL_HANDLE;
  XrAction playPauseAction_ = XR_NULL_HANDLE;
  XrAction recenterAction_ = XR_NULL_HANDLE;
  XrAction stickAction_ = XR_NULL_HANDLE;
  XrPath handPaths_[2] = {XR_NULL_PATH, XR_NULL_PATH};
  XrSpace aimSpaces_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  ControllerState controllers_;
  XrFrameState frameState_{XR_TYPE_FRAME_STATE};
  bool running_ = false;
  bool frameBegun_ = false;
  bool eyesAcquired_ = false;
  bool srgb_ = false;
  bool testPattern_ = false;
  uint64_t frameCount_ = 0;
  double lastFenceWaitMs_ = 0;

  VkInstance vkInstance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool commandPool_ = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  VkFormat sharedFormat_ = VK_FORMAT_R8G8B8A8_SRGB;

  // GL signals glReady after drawing; Vulkan signals vkDone after copying.
  VkSemaphore glReadyVk_ = VK_NULL_HANDLE;
  VkSemaphore vkDoneVk_ = VK_NULL_HANDLE;
  GLuint glReadyGl_ = 0;
  GLuint vkDoneGl_ = 0;

  std::vector<Swapchain> swapchains_;
  std::vector<SharedImage> sharedImages_;
  std::vector<GLuint> sharedTextures_;  // the eye images' GL textures, for semaphore calls
  std::vector<GLenum> sharedLayouts_;
  std::vector<XrView> views_;
  std::vector<XrCompositionLayerProjectionView> projViews_;
  std::vector<EyeMatrices> eyes_;
};
