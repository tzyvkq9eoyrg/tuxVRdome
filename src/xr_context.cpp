#define XR_USE_GRAPHICS_API_VULKAN

#include "xr_context.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#define XR_TRY(call) \
  if (!check((call), #call)) return false

namespace {

constexpr XrViewConfigurationType kViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
constexpr float kNearZ = 0.05f;
constexpr float kFarZ = 100.0f;

Vec3 toVec3(const XrVector3f& v) { return {v.x, v.y, v.z}; }
Quat toQuat(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }

bool vkCheck(VkResult result, const char* what) {
  if (result == VK_SUCCESS) return true;
  std::fprintf(stderr, "Vulkan: %s failed (%d)\n", what, static_cast<int>(result));
  return false;
}

#define VK_TRY(call) \
  if (!vkCheck((call), #call)) return false

// GL_EXT_memory_object(_fd) and GL_EXT_semaphore(_fd), loaded at runtime.
struct GlInterop {
  PFNGLCREATEMEMORYOBJECTSEXTPROC createMemoryObjects = nullptr;
  PFNGLDELETEMEMORYOBJECTSEXTPROC deleteMemoryObjects = nullptr;
  PFNGLMEMORYOBJECTPARAMETERIVEXTPROC memoryObjectParameteriv = nullptr;
  PFNGLIMPORTMEMORYFDEXTPROC importMemoryFd = nullptr;
  PFNGLTEXTURESTORAGEMEM2DEXTPROC textureStorageMem2D = nullptr;
  PFNGLGENSEMAPHORESEXTPROC genSemaphores = nullptr;
  PFNGLDELETESEMAPHORESEXTPROC deleteSemaphores = nullptr;
  PFNGLIMPORTSEMAPHOREFDEXTPROC importSemaphoreFd = nullptr;
  PFNGLWAITSEMAPHOREEXTPROC waitSemaphore = nullptr;
  PFNGLSIGNALSEMAPHOREEXTPROC signalSemaphore = nullptr;
  PFNGLGETUNSIGNEDBYTEI_VEXTPROC getUnsignedBytei_v = nullptr;

  bool load() {
    auto get = [](const char* name) { return glfwGetProcAddress(name); };
    createMemoryObjects = reinterpret_cast<PFNGLCREATEMEMORYOBJECTSEXTPROC>(get("glCreateMemoryObjectsEXT"));
    deleteMemoryObjects = reinterpret_cast<PFNGLDELETEMEMORYOBJECTSEXTPROC>(get("glDeleteMemoryObjectsEXT"));
    memoryObjectParameteriv = reinterpret_cast<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>(get("glMemoryObjectParameterivEXT"));
    importMemoryFd = reinterpret_cast<PFNGLIMPORTMEMORYFDEXTPROC>(get("glImportMemoryFdEXT"));
    textureStorageMem2D = reinterpret_cast<PFNGLTEXTURESTORAGEMEM2DEXTPROC>(get("glTextureStorageMem2DEXT"));
    genSemaphores = reinterpret_cast<PFNGLGENSEMAPHORESEXTPROC>(get("glGenSemaphoresEXT"));
    deleteSemaphores = reinterpret_cast<PFNGLDELETESEMAPHORESEXTPROC>(get("glDeleteSemaphoresEXT"));
    importSemaphoreFd = reinterpret_cast<PFNGLIMPORTSEMAPHOREFDEXTPROC>(get("glImportSemaphoreFdEXT"));
    waitSemaphore = reinterpret_cast<PFNGLWAITSEMAPHOREEXTPROC>(get("glWaitSemaphoreEXT"));
    signalSemaphore = reinterpret_cast<PFNGLSIGNALSEMAPHOREEXTPROC>(get("glSignalSemaphoreEXT"));
    getUnsignedBytei_v = reinterpret_cast<PFNGLGETUNSIGNEDBYTEI_VEXTPROC>(get("glGetUnsignedBytei_vEXT"));
    return createMemoryObjects && deleteMemoryObjects && memoryObjectParameteriv && importMemoryFd &&
           textureStorageMem2D && genSemaphores && deleteSemaphores && importSemaphoreFd &&
           waitSemaphore && signalSemaphore && getUnsignedBytei_v;
  }
};

GlInterop gl;

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                  VkPipelineStageFlags dstStage) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

XrContext::~XrContext() {
  if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
  glFinish();

  for (auto& s : sharedImages_) {
    if (s.fbo) glDeleteFramebuffers(1, &s.fbo);
    if (s.texture) glDeleteTextures(1, &s.texture);
    if (s.memoryObject) gl.deleteMemoryObjects(1, &s.memoryObject);
    if (s.image != VK_NULL_HANDLE) vkDestroyImage(device_, s.image, nullptr);
    if (s.memory != VK_NULL_HANDLE) vkFreeMemory(device_, s.memory, nullptr);
  }
  if (glReadyGl_) gl.deleteSemaphores(1, &glReadyGl_);
  if (vkDoneGl_) gl.deleteSemaphores(1, &vkDoneGl_);
  if (device_ != VK_NULL_HANDLE) {
    if (glReadyVk_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, glReadyVk_, nullptr);
    if (vkDoneVk_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, vkDoneVk_, nullptr);
    if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
    if (commandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, commandPool_, nullptr);
  }

  for (auto& sc : swapchains_) xrDestroySwapchain(sc.handle);
  for (XrSpace space : aimSpaces_)
    if (space != XR_NULL_HANDLE) xrDestroySpace(space);
  if (actionSet_ != XR_NULL_HANDLE) xrDestroyActionSet(actionSet_);
  if (viewSpace_ != XR_NULL_HANDLE) xrDestroySpace(viewSpace_);
  if (localSpace_ != XR_NULL_HANDLE) xrDestroySpace(localSpace_);
  if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
  if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
  if (vkInstance_ != VK_NULL_HANDLE) vkDestroyInstance(vkInstance_, nullptr);
  if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
}

bool XrContext::check(XrResult result, const char* what) const {
  if (XR_SUCCEEDED(result)) return true;
  char name[XR_MAX_RESULT_STRING_SIZE] = "";
  if (instance_ != XR_NULL_HANDLE) xrResultToString(instance_, result, name);
  std::fprintf(stderr, "OpenXR: %s failed: %s (%d)\n", what, name, static_cast<int>(result));
  return false;
}

bool XrContext::init() {
  if (!gl.load()) {
    std::fprintf(stderr, "OpenGL driver lacks GL_EXT_memory_object_fd / GL_EXT_semaphore_fd.\n");
    return false;
  }

  uint32_t extCount = 0;
  if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr))) {
    std::fprintf(stderr,
                 "No OpenXR runtime found. Start SteamVR and set it as the OpenXR runtime "
                 "(SteamVR settings > OpenXR).\n");
    return false;
  }
  std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
  XR_TRY(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data()));
  const bool hasVulkan = std::any_of(exts.begin(), exts.end(), [](const XrExtensionProperties& e) {
    return std::strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0;
  });
  if (!hasVulkan) {
    std::fprintf(stderr, "The active OpenXR runtime does not support %s.\n",
                 XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    return false;
  }

  const char* enabledExts[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
  // SteamVR derives its app key from this name; keep it free of spaces.
  std::strncpy(instanceInfo.applicationInfo.applicationName, "tuxvrdome",
               XR_MAX_APPLICATION_NAME_SIZE - 1);
  std::strncpy(instanceInfo.applicationInfo.engineName, "tuxvrdome", XR_MAX_ENGINE_NAME_SIZE - 1);
  instanceInfo.applicationInfo.applicationVersion = 1;
  instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  instanceInfo.enabledExtensionCount = 1;
  instanceInfo.enabledExtensionNames = enabledExts;
  XR_TRY(xrCreateInstance(&instanceInfo, &instance_));

  XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
  if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &props)))
    std::printf("OpenXR runtime: %s\n", props.runtimeName);

  XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
  systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  const XrResult systemResult = xrGetSystem(instance_, &systemInfo, &systemId_);
  if (systemResult == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
    std::fprintf(stderr, "No headset found. Connect the Steam Frame and start SteamVR.\n");
    return false;
  }
  XR_TRY(systemResult);

  if (!createVulkanDevice()) return false;

  XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
  binding.instance = vkInstance_;
  binding.physicalDevice = physicalDevice_;
  binding.device = device_;
  binding.queueFamilyIndex = queueFamily_;
  binding.queueIndex = 0;

  XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
  sessionInfo.next = &binding;
  sessionInfo.systemId = systemId_;
  XR_TRY(xrCreateSession(instance_, &sessionInfo, &session_));

  XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
  spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  XR_TRY(xrCreateReferenceSpace(session_, &spaceInfo, &localSpace_));
  spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  XR_TRY(xrCreateReferenceSpace(session_, &spaceInfo, &viewSpace_));

  if (!createActions()) return false;
  return createSwapchains();
}

// Bindings are suggested for several common controller profiles; SteamVR maps
// whichever fits best onto the Steam Frame controllers.
bool XrContext::createActions() {
  XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strncpy(setInfo.actionSetName, "player", XR_MAX_ACTION_SET_NAME_SIZE - 1);
  std::strncpy(setInfo.localizedActionSetName, "Player", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
  XR_TRY(xrCreateActionSet(instance_, &setInfo, &actionSet_));

  XR_TRY(xrStringToPath(instance_, "/user/hand/left", &handPaths_[0]));
  XR_TRY(xrStringToPath(instance_, "/user/hand/right", &handPaths_[1]));

  auto createAction = [&](XrActionType type, const char* name, const char* localized,
                          XrAction& out) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.countSubactionPaths = 2;
    info.subactionPaths = handPaths_;
    return check(xrCreateAction(actionSet_, &info, &out), name);
  };
  if (!createAction(XR_ACTION_TYPE_POSE_INPUT, "aim", "Pointer", aimAction_) ||
      !createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "select", "Select", selectAction_) ||
      !createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "grab", "Grab screen / menu", gripAction_) ||
      !createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Toggle menu", menuAction_) ||
      !createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "play_pause", "Play / pause", playPauseAction_) ||
      !createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "recenter", "Recenter", recenterAction_) ||
      !createAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Seek / volume", stickAction_))
    return false;

  suggestBindings("/interaction_profiles/valve/index_controller",
                  {{aimAction_, "/user/hand/left/input/aim/pose"},
                   {aimAction_, "/user/hand/right/input/aim/pose"},
                   {selectAction_, "/user/hand/left/input/trigger/click"},
                   {selectAction_, "/user/hand/right/input/trigger/click"},
                   {gripAction_, "/user/hand/left/input/squeeze/value"},
                   {gripAction_, "/user/hand/right/input/squeeze/value"},
                   {menuAction_, "/user/hand/left/input/b/click"},
                   {playPauseAction_, "/user/hand/right/input/a/click"},
                   {recenterAction_, "/user/hand/right/input/b/click"},
                   {stickAction_, "/user/hand/left/input/thumbstick"},
                   {stickAction_, "/user/hand/right/input/thumbstick"}});
  suggestBindings("/interaction_profiles/oculus/touch_controller",
                  {{aimAction_, "/user/hand/left/input/aim/pose"},
                   {aimAction_, "/user/hand/right/input/aim/pose"},
                   {selectAction_, "/user/hand/left/input/trigger/value"},
                   {selectAction_, "/user/hand/right/input/trigger/value"},
                   {gripAction_, "/user/hand/left/input/squeeze/value"},
                   {gripAction_, "/user/hand/right/input/squeeze/value"},
                   {menuAction_, "/user/hand/left/input/menu/click"},
                   {playPauseAction_, "/user/hand/right/input/a/click"},
                   // The Steam Frame's upper right button arrives as B.
                   {recenterAction_, "/user/hand/right/input/b/click"},
                   {recenterAction_, "/user/hand/left/input/y/click"},
                   {stickAction_, "/user/hand/left/input/thumbstick"},
                   {stickAction_, "/user/hand/right/input/thumbstick"}});
  suggestBindings("/interaction_profiles/khr/simple_controller",
                  {{aimAction_, "/user/hand/left/input/aim/pose"},
                   {aimAction_, "/user/hand/right/input/aim/pose"},
                   {selectAction_, "/user/hand/left/input/select/click"},
                   {selectAction_, "/user/hand/right/input/select/click"},
                   {menuAction_, "/user/hand/left/input/menu/click"},
                   {menuAction_, "/user/hand/right/input/menu/click"}});

  for (int h = 0; h < 2; ++h) {
    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action = aimAction_;
    spaceInfo.subactionPath = handPaths_[h];
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;
    XR_TRY(xrCreateActionSpace(session_, &spaceInfo, &aimSpaces_[h]));
  }

  XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attachInfo.countActionSets = 1;
  attachInfo.actionSets = &actionSet_;
  XR_TRY(xrAttachSessionActionSets(session_, &attachInfo));
  return true;
}

// A rejected profile only loses that controller type, so failures just warn.
void XrContext::suggestBindings(const char* profile,
                                const std::vector<std::pair<XrAction, const char*>>& bindings) {
  XrPath profilePath;
  if (!check(xrStringToPath(instance_, profile, &profilePath), profile)) return;
  std::vector<XrActionSuggestedBinding> suggested;
  for (const auto& [action, path] : bindings) {
    XrPath binding;
    if (!check(xrStringToPath(instance_, path, &binding), path)) return;
    suggested.push_back({action, binding});
  }
  XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  info.interactionProfile = profilePath;
  info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
  info.suggestedBindings = suggested.data();
  check(xrSuggestInteractionProfileBindings(instance_, &info), profile);
}

void XrContext::updateControllers() {
  auto getBool = [&](XrAction action, XrPath subaction) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    info.subactionPath = subaction;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    xrGetActionStateBoolean(session_, &info, &state);
    return state;
  };
  auto pressed = [](const XrActionStateBoolean& s) {
    return s.isActive && s.currentState && s.changedSinceLastSync;
  };

  for (int h = 0; h < 2; ++h) {
    HandState& hand = controllers_.hands[h];
    const XrActionStateBoolean select = getBool(selectAction_, handPaths_[h]);
    hand.select = select.isActive && select.currentState;
    hand.selectPressed = pressed(select);
    const XrActionStateBoolean grip = getBool(gripAction_, handPaths_[h]);
    hand.grip = grip.isActive && grip.currentState;
    hand.gripPressed = pressed(grip);

    XrActionStateGetInfo stickInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    stickInfo.action = stickAction_;
    stickInfo.subactionPath = handPaths_[h];
    XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
    xrGetActionStateVector2f(session_, &stickInfo, &stick);
    hand.stickX = stick.isActive ? stick.currentState.x : 0.0f;
    hand.stickY = stick.isActive ? stick.currentState.y : 0.0f;

    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags needed =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    hand.poseValid =
        XR_SUCCEEDED(xrLocateSpace(aimSpaces_[h], localSpace_, frameState_.predictedDisplayTime,
                                   &location)) &&
        (location.locationFlags & needed) == needed;
    if (hand.poseValid) {
      hand.aimPosition = toVec3(location.pose.position);
      hand.aimOrientation = toQuat(location.pose.orientation);
    }
  }
  controllers_.menuPressed = pressed(getBool(menuAction_, XR_NULL_PATH));
  controllers_.playPausePressed = pressed(getBool(playPauseAction_, XR_NULL_PATH));
  controllers_.recenterPressed = pressed(getBool(recenterAction_, XR_NULL_PATH));
}

bool XrContext::createVulkanDevice() {
  // The runtime requires this call before any Vulkan objects are created.
  PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
  PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
  PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
  PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
  XR_TRY(xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsRequirements2KHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)));
  XR_TRY(xrGetInstanceProcAddr(instance_, "xrCreateVulkanInstanceKHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(&createInstance)));
  XR_TRY(xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsDevice2KHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(&getDevice)));
  XR_TRY(xrGetInstanceProcAddr(instance_, "xrCreateVulkanDeviceKHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(&createDevice)));

  XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  XR_TRY(getRequirements(instance_, systemId_, &requirements));

  VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  appInfo.pApplicationName = "tuxVRdome";
  appInfo.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo vkInstanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  vkInstanceInfo.pApplicationInfo = &appInfo;

  XrVulkanInstanceCreateInfoKHR xrInstanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  xrInstanceInfo.systemId = systemId_;
  xrInstanceInfo.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
  xrInstanceInfo.vulkanCreateInfo = &vkInstanceInfo;
  VkResult vkResult = VK_SUCCESS;
  XR_TRY(createInstance(instance_, &xrInstanceInfo, &vkInstance_, &vkResult));
  VK_TRY(vkResult);

  XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  deviceGetInfo.systemId = systemId_;
  deviceGetInfo.vulkanInstance = vkInstance_;
  XR_TRY(getDevice(instance_, &deviceGetInfo, &physicalDevice_));

  // GL and Vulkan must run on the same GPU to share memory.
  VkPhysicalDeviceIDProperties idProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 deviceProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  deviceProps.pNext = &idProps;
  vkGetPhysicalDeviceProperties2(physicalDevice_, &deviceProps);
  GLubyte glUuid[GL_UUID_SIZE_EXT] = {};
  gl.getUnsignedBytei_v(GL_DEVICE_UUID_EXT, 0, glUuid);
  std::printf("Vulkan device: %s\n", deviceProps.properties.deviceName);
  if (std::memcmp(glUuid, idProps.deviceUUID, VK_UUID_SIZE) != 0) {
    std::fprintf(stderr, "OpenGL is not running on the GPU SteamVR uses (%s).\n",
                 deviceProps.properties.deviceName);
    return false;
  }

  uint32_t familyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, nullptr);
  std::vector<VkQueueFamilyProperties> families(familyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, families.data());
  bool found = false;
  for (uint32_t i = 0; i < familyCount; ++i) {
    if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      queueFamily_ = i;
      found = true;
      break;
    }
  }
  if (!found) {
    std::fprintf(stderr, "Vulkan: no graphics queue\n");
    return false;
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queueInfo.queueFamilyIndex = queueFamily_;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &priority;
  const char* deviceExts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                              VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
  // SteamVR syncs with its compositor through timeline semaphores on our queue;
  // the runtime adds the extension, but the feature has to be enabled by us.
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
  VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  supported.pNext = &timeline;
  vkGetPhysicalDeviceFeatures2(physicalDevice_, &supported);
  std::printf("Vulkan timeline semaphores: %s\n", timeline.timelineSemaphore ? "yes" : "no");
  timeline.pNext = nullptr;

  VkDeviceCreateInfo vkDeviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  if (timeline.timelineSemaphore) vkDeviceInfo.pNext = &timeline;
  vkDeviceInfo.queueCreateInfoCount = 1;
  vkDeviceInfo.pQueueCreateInfos = &queueInfo;
  vkDeviceInfo.enabledExtensionCount = 2;
  vkDeviceInfo.ppEnabledExtensionNames = deviceExts;

  XrVulkanDeviceCreateInfoKHR xrDeviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  xrDeviceInfo.systemId = systemId_;
  xrDeviceInfo.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
  xrDeviceInfo.vulkanPhysicalDevice = physicalDevice_;
  xrDeviceInfo.vulkanCreateInfo = &vkDeviceInfo;
  XR_TRY(createDevice(instance_, &xrDeviceInfo, &device_, &vkResult));
  VK_TRY(vkResult);
  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

  VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = queueFamily_;
  VK_TRY(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_));
  VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cmdInfo.commandPool = commandPool_;
  cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdInfo.commandBufferCount = 1;
  VK_TRY(vkAllocateCommandBuffers(device_, &cmdInfo, &commandBuffer_));
  VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_TRY(vkCreateFence(device_, &fenceInfo, nullptr, &fence_));
  return true;
}

bool XrContext::createSwapchains() {
  uint32_t viewCount = 0;
  XR_TRY(xrEnumerateViewConfigurationViews(instance_, systemId_, kViewConfig, 0, &viewCount, nullptr));
  std::vector<XrViewConfigurationView> configViews(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  XR_TRY(xrEnumerateViewConfigurationViews(instance_, systemId_, kViewConfig, viewCount, &viewCount,
                                           configViews.data()));

  uint32_t formatCount = 0;
  XR_TRY(xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr));
  std::vector<int64_t> formats(formatCount);
  XR_TRY(xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data()));
  auto has = [&](VkFormat f) { return std::find(formats.begin(), formats.end(), f) != formats.end(); };
  // The blit converts between channel orders, so BGRA swapchains work too.
  // The shared GL image is always RGBA, since GL has no BGRA texture format.
  int64_t format = formats.empty() ? int64_t{VK_FORMAT_R8G8B8A8_SRGB} : formats[0];
  for (VkFormat preferred : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                             VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
    if (has(preferred)) {
      format = preferred;
      break;
    }
  }
  srgb_ = format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB;
  sharedFormat_ = srgb_ ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

  for (const auto& cv : configViews) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    // SteamVR samples the images when copying them to its compositor, so
    // SAMPLED is required even though we only write to them.
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = cv.recommendedImageRectWidth;
    info.height = cv.recommendedImageRectHeight;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;

    Swapchain sc;
    sc.width = static_cast<int>(info.width);
    sc.height = static_cast<int>(info.height);
    XR_TRY(xrCreateSwapchain(session_, &info, &sc.handle));
    swapchains_.push_back(sc);

    uint32_t imageCount = 0;
    XR_TRY(xrEnumerateSwapchainImages(sc.handle, 0, &imageCount, nullptr));
    std::vector<XrSwapchainImageVulkan2KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    XR_TRY(xrEnumerateSwapchainImages(sc.handle, imageCount, &imageCount,
                                      reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
    for (const auto& img : images) swapchains_.back().images.push_back(img.image);

    sharedImages_.emplace_back();
    if (!createSharedImage(sc.width, sc.height, sharedImages_.back())) return false;
  }

  // Vulkan leaves the eye images in TRANSFER_SRC layout after copying; GL
  // hands them back in the same layout.
  for (const auto& s : sharedImages_) {
    sharedTextures_.push_back(s.texture);
    sharedLayouts_.push_back(GL_LAYOUT_TRANSFER_SRC_EXT);
  }
  if (!createSharedSemaphore(glReadyVk_, glReadyGl_)) return false;
  if (!createSharedSemaphore(vkDoneVk_, vkDoneGl_)) return false;
  if (!initSharedImageLayouts()) return false;

  views_.assign(viewCount, {XR_TYPE_VIEW});
  projViews_.assign(viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
  eyes_.resize(viewCount);

  std::printf("OpenXR: %u views, %dx%d per eye, %s swapchain\n", viewCount,
              swapchains_.empty() ? 0 : swapchains_[0].width,
              swapchains_.empty() ? 0 : swapchains_[0].height, srgb_ ? "sRGB" : "linear");
  return true;
}

bool XrContext::createSharedImage(int width, int height, SharedImage& out) {
  VkExternalMemoryImageCreateInfo externalInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imageInfo.pNext = &externalInfo;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = sharedFormat_;
  imageInfo.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VK_TRY(vkCreateImage(device_, &imageInfo, nullptr, &out.image));

  VkMemoryRequirements reqs;
  vkGetImageMemoryRequirements(device_, out.image, &reqs);
  VkPhysicalDeviceMemoryProperties memProps;
  vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
  uint32_t typeIndex = UINT32_MAX;
  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if ((reqs.memoryTypeBits & (1u << i)) &&
        (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      typeIndex = i;
      break;
    }
  }
  if (typeIndex == UINT32_MAX) {
    std::fprintf(stderr, "Vulkan: no device-local memory for the eye images\n");
    return false;
  }

  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.image = out.image;
  VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
  exportInfo.pNext = &dedicated;
  exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocInfo.pNext = &exportInfo;
  allocInfo.allocationSize = reqs.size;
  allocInfo.memoryTypeIndex = typeIndex;
  VK_TRY(vkAllocateMemory(device_, &allocInfo, nullptr, &out.memory));
  VK_TRY(vkBindImageMemory(device_, out.image, out.memory, 0));

  auto getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
  VkMemoryGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
  fdInfo.memory = out.memory;
  fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  int fd = -1;
  VK_TRY(getMemoryFd(device_, &fdInfo, &fd));

  // GL takes ownership of the fd on import.
  gl.createMemoryObjects(1, &out.memoryObject);
  const GLint isDedicated = GL_TRUE;
  gl.memoryObjectParameteriv(out.memoryObject, GL_DEDICATED_MEMORY_OBJECT_EXT, &isDedicated);
  gl.importMemoryFd(out.memoryObject, reqs.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
  glCreateTextures(GL_TEXTURE_2D, 1, &out.texture);
  gl.textureStorageMem2D(out.texture, 1, srgb_ ? GL_SRGB8_ALPHA8 : GL_RGBA8, width, height,
                         out.memoryObject, 0);

  glCreateFramebuffers(1, &out.fbo);
  glNamedFramebufferTexture(out.fbo, GL_COLOR_ATTACHMENT0, out.texture, 0);
  const GLenum status = glCheckNamedFramebufferStatus(out.fbo, GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    std::fprintf(stderr, "Shared eye framebuffer incomplete (0x%x)\n", status);
    return false;
  }
  return true;
}

bool XrContext::createSharedSemaphore(VkSemaphore& vk, GLuint& glSemaphore) {
  VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
  exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  info.pNext = &exportInfo;
  VK_TRY(vkCreateSemaphore(device_, &info, nullptr, &vk));

  auto getSemaphoreFd =
      reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device_, "vkGetSemaphoreFdKHR"));
  VkSemaphoreGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
  fdInfo.semaphore = vk;
  fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  int fd = -1;
  VK_TRY(getSemaphoreFd(device_, &fdInfo, &fd));

  gl.genSemaphores(1, &glSemaphore);
  gl.importSemaphoreFd(glSemaphore, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
  return true;
}

// Puts the eye images into the layout GL expects and signals vkDone once, so
// the first frame's GL wait has something to wait for.
bool XrContext::initSharedImageLayouts() {
  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_TRY(vkBeginCommandBuffer(commandBuffer_, &beginInfo));
  for (const auto& s : sharedImages_) {
    imageBarrier(commandBuffer_, s.image, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  }
  VK_TRY(vkEndCommandBuffer(commandBuffer_));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &commandBuffer_;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &vkDoneVk_;
  VK_TRY(vkQueueSubmit(queue_, 1, &submit, fence_));
  return true;
}

void XrContext::pollEvents(bool& quit) {
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
    switch (event.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        const auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
        switch (e->state) {
          case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
            beginInfo.primaryViewConfigurationType = kViewConfig;
            if (check(xrBeginSession(session_, &beginInfo), "xrBeginSession")) running_ = true;
            break;
          }
          case XR_SESSION_STATE_STOPPING:
            xrEndSession(session_);
            running_ = false;
            break;
          case XR_SESSION_STATE_EXITING:
          case XR_SESSION_STATE_LOSS_PENDING:
            quit = true;
            break;
          default:
            break;
        }
        break;
      }
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        quit = true;
        break;
      default:
        break;
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

bool XrContext::beginFrame() {
  frameBegun_ = false;
  XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
  frameState_ = {XR_TYPE_FRAME_STATE};
  if (!check(xrWaitFrame(session_, &waitInfo, &frameState_), "xrWaitFrame")) return false;
  XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
  if (!check(xrBeginFrame(session_, &beginInfo), "xrBeginFrame")) return false;
  frameBegun_ = true;

  // Returns XR_SESSION_NOT_FOCUSED (a success code) while unfocused.
  const XrActiveActionSet activeSet{actionSet_, XR_NULL_PATH};
  XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
  syncInfo.countActiveActionSets = 1;
  syncInfo.activeActionSets = &activeSet;
  check(xrSyncActions(session_, &syncInfo), "xrSyncActions");
  updateControllers();

  if (!frameState_.shouldRender) return false;

  XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
  locateInfo.viewConfigurationType = kViewConfig;
  locateInfo.displayTime = frameState_.predictedDisplayTime;
  locateInfo.space = localSpace_;
  XrViewState viewState{XR_TYPE_VIEW_STATE};
  uint32_t count = static_cast<uint32_t>(views_.size());
  if (XR_FAILED(xrLocateViews(session_, &locateInfo, &viewState, count, &count, views_.data())))
    return false;
  const XrViewStateFlags needed =
      XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  if ((viewState.viewStateFlags & needed) != needed) return false;

  for (size_t i = 0; i < views_.size(); ++i) {
    const XrView& v = views_[i];
    eyes_[i].projection = projectionFromFov(v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp,
                                            v.fov.angleDown, kNearZ, kFarZ);
    eyes_[i].view = viewFromPose(toQuat(v.pose.orientation), toVec3(v.pose.position));
    eyes_[i].orientation = toQuat(v.pose.orientation);
    eyes_[i].position = toVec3(v.pose.position);
    eyes_[i].tanLeft = std::tan(v.fov.angleLeft);
    eyes_[i].tanRight = std::tan(v.fov.angleRight);
    eyes_[i].tanUp = std::tan(v.fov.angleUp);
    eyes_[i].tanDown = std::tan(v.fov.angleDown);
  }

  for (auto& sc : swapchains_) {
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(sc.handle, &acquireInfo, &sc.acquired);
    XrSwapchainImageWaitInfo imageWaitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    imageWaitInfo.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc.handle, &imageWaitInfo);
  }

  // Don't draw into the eye images until Vulkan has finished copying them.
  if (!testPattern_) {
    gl.waitSemaphore(vkDoneGl_, 0, nullptr, static_cast<GLuint>(sharedTextures_.size()),
                     sharedTextures_.data(), sharedLayouts_.data());
  }
  eyesAcquired_ = true;
  return true;
}

void XrContext::bindEye(int i) {
  const Swapchain& sc = swapchains_[i];
  glBindFramebuffer(GL_FRAMEBUFFER, sharedImages_[i].fbo);
  glViewport(0, 0, sc.width, sc.height);

  XrCompositionLayerProjectionView& pv = projViews_[i];
  pv.pose = views_[i].pose;
  pv.fov = views_[i].fov;
  pv.subImage.swapchain = sc.handle;
  pv.subImage.imageRect.offset = {0, 0};
  pv.subImage.imageRect.extent = {sc.width, sc.height};
  pv.subImage.imageArrayIndex = 0;
}

void XrContext::copyEyesToSwapchains() {
  const auto waitStart = std::chrono::steady_clock::now();
  vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
  lastFenceWaitMs_ =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
  vkResetFences(device_, 1, &fence_);
  vkResetCommandBuffer(commandBuffer_, 0);

  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(commandBuffer_, &beginInfo);
  for (size_t i = 0; i < swapchains_.size(); ++i) {
    const Swapchain& sc = swapchains_[i];
    VkImage target = sc.images[sc.acquired];
    // OpenXR hands out and expects back swapchain images in COLOR_ATTACHMENT_OPTIMAL.
    imageBarrier(commandBuffer_, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (testPattern_) {
      const float pulse = 0.25f + 0.25f * static_cast<float>((frameCount_ / 72) % 2);
      VkClearColorValue color{};
      color.float32[0] = 0.0f;
      color.float32[1] = i == 0 ? pulse : 0.0f;
      color.float32[2] = i == 0 ? 0.0f : pulse;
      color.float32[3] = 1.0f;
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(commandBuffer_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                           &range);
    } else {
      // GL stores rows bottom-up, Vulkan top-down: swap the source rows to flip.
      VkImageBlit region{};
      region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.srcOffsets[0] = {0, sc.height, 0};
      region.srcOffsets[1] = {sc.width, 0, 1};
      region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.dstOffsets[1] = {sc.width, sc.height, 1};
      vkCmdBlitImage(commandBuffer_, sharedImages_[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    }
    imageBarrier(commandBuffer_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  }
  vkEndCommandBuffer(commandBuffer_);

  ++frameCount_;
  const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = testPattern_ ? 0 : 1;
  submit.pWaitSemaphores = &glReadyVk_;
  submit.pWaitDstStageMask = &waitStage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &commandBuffer_;
  submit.signalSemaphoreCount = testPattern_ ? 0 : 1;
  submit.pSignalSemaphores = &vkDoneVk_;
  vkCheck(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");
}

void XrContext::endFrame() {
  if (!frameBegun_) return;
  frameBegun_ = false;
  lastFenceWaitMs_ = 0;

  const bool rendered = eyesAcquired_;
  if (eyesAcquired_) {
    eyesAcquired_ = false;
    if (!testPattern_) {
      gl.signalSemaphore(glReadyGl_, 0, nullptr, static_cast<GLuint>(sharedTextures_.size()),
                         sharedTextures_.data(), sharedLayouts_.data());
      glFlush();  // the signal must be submitted before Vulkan waits on it
    }

    copyEyesToSwapchains();
    for (auto& sc : swapchains_) {
      XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      xrReleaseSwapchainImage(sc.handle, &releaseInfo);
    }
  }

  XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  layer.space = localSpace_;
  layer.viewCount = static_cast<uint32_t>(projViews_.size());
  layer.views = projViews_.data();
  const XrCompositionLayerBaseHeader* layers[] = {
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};

  XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
  endInfo.displayTime = frameState_.predictedDisplayTime;
  endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  endInfo.layerCount = rendered ? 1 : 0;
  endInfo.layers = rendered ? layers : nullptr;
  check(xrEndFrame(session_, &endInfo), "xrEndFrame");
}

bool XrContext::headPose(Vec3& position, Quat& orientation) const {
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(viewSpace_, localSpace_, frameState_.predictedDisplayTime, &location)))
    return false;
  const XrSpaceLocationFlags needed =
      XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  if ((location.locationFlags & needed) != needed) return false;
  position = toVec3(location.pose.position);
  orientation = toQuat(location.pose.orientation);
  return true;
}
