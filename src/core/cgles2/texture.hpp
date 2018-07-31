#pragma once

#include <GLES2/gl2.h>

#include <Eigen/Eigen>

#include <getPtr.hpp>
#include <linear.hpp>

namespace cgl {
class Texture {
  core::Linear<GLuint, 0> m_tex;

  Texture(GLuint tex) : m_tex(tex) {}

public:
  Texture() {}

  friend class Textures;
  friend class BindTexture;
  friend class BindFramebuffer;
};

class BindTexture {
  core::Linear<GLenum, 0> m_target;
  const Texture *         m_tex;
  GLenum                  m_unit;

public:
  BindTexture() {}

  BindTexture(GLenum unit, GLenum target, const Texture &);

  ~BindTexture();

  void param(GLenum, GLint);
  void param(GLenum, const GLint *);
  void param(GLenum, GLfloat);
  void param(GLenum, const GLfloat *);

  template <typename T>
  void param(GLenum id, const std::vector<T> &vec) {
    data(id, _getVecPtr(vec));
  }

  template <typename T>
  void param(GLenum id, const T &obj) {
    data(id, _getPtr(obj));
  }

  void image(GLint   lvl,
             GLint   internalFormat,
             GLsizei width,
             GLsizei height,
             GLint   border,
             GLenum  format,
             GLenum  type,
             const void *);

  void color(const Eigen::Vector4f &clr) {
    image(0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, &clr);
  }

  BindTexture &operator=(BindTexture &&) = default;

  friend class BindFramebuffer;
};
} // namespace cgl
