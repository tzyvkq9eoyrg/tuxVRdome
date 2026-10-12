#include "gl_util.h"

#include <cstdio>

static GLuint compileShader(GLenum type, const char* src) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::fprintf(stderr, "Shader compile error:\n%s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

GLuint compileProgram(const char* vertexSrc, const char* fragmentSrc) {
  GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSrc);
  GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSrc);
  if (!vs || !fs) {
    glDeleteShader(vs);
    glDeleteShader(fs);
    return 0;
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    std::fprintf(stderr, "Shader link error:\n%s\n", log);
    glDeleteProgram(program);
    return 0;
  }
  return program;
}
