// Our own tiny OpenGL loader: only the functions the engine uses, loaded
// through GLFW at startup. No glad/GLEW dependency.
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_WIN32) && !defined(APIENTRY)
#define APIENTRY __stdcall
#elif !defined(APIENTRY)
#define APIENTRY
#endif

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;

constexpr GLenum GL_FALSE = 0;
constexpr GLenum GL_TRUE = 1;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_LESS = 0x0201;
constexpr GLenum GL_BACK = 0x0405;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_UNSIGNED_INT = 0x1405;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_MULTISAMPLE = 0x809D;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x0100;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x4000;

// X(return type, name, parameter list)
#define SG_GL_FUNCTIONS(X)                                                                   \
  X(void, glEnable, (GLenum cap))                                                            \
  X(void, glDisable, (GLenum cap))                                                           \
  X(void, glClear, (GLbitfield mask))                                                        \
  X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                        \
  X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                              \
  X(void, glCullFace, (GLenum mode))                                                         \
  X(void, glDepthFunc, (GLenum func))                                                        \
  X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices))    \
  X(GLuint, glCreateShader, (GLenum type))                                                   \
  X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar* const* src, const GLint* len)) \
  X(void, glCompileShader, (GLuint s))                                                       \
  X(void, glGetShaderiv, (GLuint s, GLenum p, GLint* v))                                     \
  X(void, glGetShaderInfoLog, (GLuint s, GLsizei max, GLsizei* len, GLchar* log))            \
  X(void, glDeleteShader, (GLuint s))                                                        \
  X(GLuint, glCreateProgram, (void))                                                         \
  X(void, glAttachShader, (GLuint p, GLuint s))                                              \
  X(void, glLinkProgram, (GLuint p))                                                         \
  X(void, glGetProgramiv, (GLuint p, GLenum pname, GLint* v))                                \
  X(void, glGetProgramInfoLog, (GLuint p, GLsizei max, GLsizei* len, GLchar* log))           \
  X(void, glDeleteProgram, (GLuint p))                                                       \
  X(void, glUseProgram, (GLuint p))                                                          \
  X(GLint, glGetUniformLocation, (GLuint p, const GLchar* name))                             \
  X(void, glUniformMatrix4fv, (GLint loc, GLsizei n, GLboolean transpose, const GLfloat* v)) \
  X(void, glUniform3f, (GLint loc, GLfloat x, GLfloat y, GLfloat z))                         \
  X(void, glUniform1f, (GLint loc, GLfloat v))                                               \
  X(void, glGenVertexArrays, (GLsizei n, GLuint * arrays))                                   \
  X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* arrays))                           \
  X(void, glBindVertexArray, (GLuint a))                                                     \
  X(void, glGenBuffers, (GLsizei n, GLuint * buffers))                                       \
  X(void, glDeleteBuffers, (GLsizei n, const GLuint* buffers))                               \
  X(void, glBindBuffer, (GLenum target, GLuint b))                                           \
  X(void, glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))    \
  X(void, glEnableVertexAttribArray, (GLuint index))                                         \
  X(void, glVertexAttribPointer,                                                             \
    (GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* ptr))

#define SG_GL_DECLARE(ret, name, args) \
  using PFN_##name = ret(APIENTRY*) args; \
  extern PFN_##name name;
SG_GL_FUNCTIONS(SG_GL_DECLARE)
#undef SG_GL_DECLARE

namespace sg {
using GLLoadFn = void* (*)(const char* name);
// Returns false and prints the first missing function if loading fails.
bool loadGL(GLLoadFn loader);
}  // namespace sg
