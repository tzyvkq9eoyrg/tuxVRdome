#pragma once

// GL_GLEXT_PROTOTYPES is set by CMake, so all GL 4.x entry points are
// declared here and resolved by libOpenGL (glvnd).
#include <GL/gl.h>
#include <GL/glext.h>

// Compiles and links a vertex + fragment shader. Returns 0 on failure.
GLuint compileProgram(const char* vertexSrc, const char* fragmentSrc);
