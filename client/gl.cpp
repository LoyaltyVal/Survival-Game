#include "gl.h"

#include <cstdio>

#define SG_GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
SG_GL_FUNCTIONS(SG_GL_DEFINE)
#undef SG_GL_DEFINE

namespace sg {

bool loadGL(GLLoadFn loader) {
#define SG_GL_LOAD(ret, name, args)                                    \
  name = reinterpret_cast<PFN_##name>(loader(#name));                  \
  if (!name) {                                                         \
    std::fprintf(stderr, "[gl] missing OpenGL function %s\n", #name);  \
    return false;                                                      \
  }
  SG_GL_FUNCTIONS(SG_GL_LOAD)
#undef SG_GL_LOAD
  return true;
}

}  // namespace sg
