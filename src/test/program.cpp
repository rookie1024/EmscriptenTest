#include "program.hpp"

#include <array>

#include "glad.hpp"

#undef Success // I hate that Xlib does this.

#include <Eigen/Eigen>

#include <diag.hpp>
#include <matrixMath.hpp>
#include <path.hpp>
#include <transformStack.hpp>

#include <cegl/configInfo.hpp>

#include <models.hpp>

using namespace pth::lit;

Program::Program(int, char **) :
    m_mt(std::random_device{}()),
    m_particles(1024) {
  // Window and context setup

  const unsigned int START_W = 1280, START_H = 720;

  m_xDisp   = cx::Display(nullptr);
  auto root = cx::Window::root(m_xDisp);
  m_win     = cx::Window(root, 0, 0, START_W, START_H);

  m_win.title("a window");

  m_disp = cegl::Display(m_xDisp);

  info("using EGL version " + std::to_string(m_disp.majorVer()) + "." +
       std::to_string(m_disp.minorVer()));

  auto &&configs = m_disp.chooseConfig({
      // clang-format off
      EGL_RED_SIZE,     8,
      EGL_GREEN_SIZE,   8,
      EGL_BLUE_SIZE,    8,
      EGL_ALPHA_SIZE,   EGL_DONT_CARE,
      EGL_DEPTH_SIZE,   24,
      EGL_STENCIL_SIZE, EGL_DONT_CARE,
      EGL_SAMPLES,      4,
      // clang-format on
  });

  auto &&config = configs[0];

  info("using config " +
       std::to_string(cegl::ConfigInfo(m_disp, config), true));

  m_surf = cegl::Surface(m_disp, config, m_win, {});
  m_ctx  = cegl::Context(m_surf, {EGL_CONTEXT_CLIENT_VERSION, 2});

  m_makeCurrent = cegl::MakeCurrent(m_surf, m_surf, m_ctx);

#if !defined(_JS)
  if (!gladLoadGLES2Loader(reinterpret_cast<GLADloadproc>(eglGetProcAddress)))
    die("failed to load OpenGL");
#endif

  // Material setup

  m_blit = cgl::Material();

  {
    cgl::SetupMaterial setup(m_blit);

    setup.add(GL_VERTEX_SHADER, "assets/shd/blit.vert");
    setup.add(GL_FRAGMENT_SHADER, "assets/shd/blit.frag");

    setup.input(0, "in_POSITION");
    setup.input(1, "in_COLOR");
    setup.input(2, "in_UV0");
  }

  m_blitFback = cgl::Material();

  {
    cgl::SetupMaterial setup(m_blitFback);

    setup.add(GL_VERTEX_SHADER, "assets/shd/blit.vert");
    setup.add(GL_FRAGMENT_SHADER, "assets/shd/blitFback.frag");

    setup.input(0, "in_POSITION");
    setup.input(1, "in_COLOR");
    setup.input(2, "in_UV0");
  }

  m_particle = cgl::Material();

  {
    cgl::SetupMaterial setup(m_particle);

    setup.add(GL_VERTEX_SHADER, "assets/shd/particle.vert");
    setup.add(GL_FRAGMENT_SHADER, "assets/shd/particle.frag");

    setup.input(0, "in_POSITION");
  }

  // Model setup

  mdl::setupBlitQuad(m_blitQuad);
  mdl::blitQuad(m_blitQuad,
                0.0f,
                Eigen::Vector3f(1.0f, 1.0f, 1.0f),
                cgl::FreqStatic);

  mdl::setupBlitQuad(m_bkgdQuad);
  mdl::blitQuad(m_bkgdQuad,
                1.0f,
                Eigen::Vector3f(0.0f, 0.0f, 0.0f),
                cgl::FreqStatic);

  mdl::setupFanPath(m_circle);

  mdl::setupStrokePath(m_fieldLines);

  {
    pth::Path path;

    path.open(pth::Vec(0.0f, 1.0f))
        .arc(pth::Vec(0.0f, -1.0f), 1.0f)
        .arc(pth::Vec(0.0f, 1.0f), 1.0f)
        .close();

    mdl::fanPath(
        m_circle, path, 64, Eigen::Vector3f(1.0f, 1.0f, 1.0f), cgl::FreqStatic);
  }

  // Texture setup

  m_white = cgl::TextureUnits(1);
  m_white.addTex(0, 0, GL_TEXTURE_2D)
      .color(Eigen::Vector4f(1.0f, 1.0f, 1.0f, 1.0f));

  // Framebuffer setup

  m_mainFbufs = cgl::Framebuffers(MAIN_FBUF_COUNT);
  m_mainRbufs = cgl::Renderbuffers(MAIN_FBUF_COUNT);

  for (int i = 0; i < MAIN_FBUF_COUNT; ++i) {
    auto &&map = m_mainFbufMap[i];

    map = cgl::TextureUnits(1);
    map.addTex(0, 0, GL_TEXTURE_2D);
  }

  m_particleMap   = cgl::TextureUnits(1);
  m_particleFbufs = cgl::Framebuffers(1);
  m_particleRbufs = cgl::Renderbuffers(1);
  m_particleMap.addTex(0, 0, GL_TEXTURE_2D);

  // ===== NB: THIS MUST COME LAST =====

  resize(START_W, START_H);
}

Program::~Program() {}

void Program::renderBackground(float alpha) {
  {
    cgl::UseProgram         pgm(m_blit);
    cgl::SelectTextureUnits tex(m_white);

    pgm.uniform("u_MAT_TRANSFORM").set(Eigen::Projective3f::Identity(), false);
    pgm.uniform("u_S2D_TEXTURE").set(0);
    pgm.uniform("u_FLT_ALPHA").set(alpha);

    cgl::SelectModel(m_bkgdQuad).draw();
  }
}

inline pth::Vec ortho(const pth::Vec &v) {
  return v.reverse().cwiseProduct(pth::Vec(-1.0f, 1.0f));
}

inline pth::Vec particleField(const pth::Vec &pos, double time) {
  pth::Vec           a(-0.75f, 0.0f), b(0.75f, 0.0f);
  float              theta = float(time) * 1_grad / 40.0f;
  Eigen::Rotation2Df rot(theta);

  a = pos - rot * a;
  b = pos - rot * b;

  pth::Vec aField = 0.25f * ortho(a) + 0.35f * a;
  aField.normalize();
  aField *= 1.0f / std::sqrt(1.0f + a.squaredNorm());

  pth::Vec bField = -0.45f * ortho(b) +
                    -0.65f * b * std::tanh(std::sqrt(b.norm()));
  bField.normalize();
  bField *= 1.0f / std::sqrt(1.0f + b.squaredNorm());

  return (aField + bField) * 0.5f;
}

// TODO: something about this is slow
void Program::renderFieldLines(double time) {
  cgl::UseProgram         pgm(m_blit);
  cgl::SelectTextureUnits tex(m_white);

  pgm.uniform("u_S2D_TEXTURE").set(0);

  pth::Path field;

  for (int r = 0; r < 11; ++r) {
    for (int c = 0; c < 11; ++c) {
      pth::Vec pos(pth::lerp(-1.0_sc, 1.0_sc, c / 10_sc),
                   pth::lerp(-1.0_sc, 1.0_sc, r / 10_sc));

      field.open(pos);

      for (int i = 0; i < 10; ++i) {
        pos += particleField(pos, time) * 0.02f;
        field.line(pos);
      }
    }
  }

  mdl::strokePath(m_fieldLines,
                  field,
                  4,
                  0.005f,
                  Eigen::Vector3f(1.0f, 0.0f, 0.0f),
                  cgl::FreqDynamic);

  TransformStack ts;

  ts << m_proj;
  ts << m_view;

  ts << Eigen::Translation3f(Eigen::Vector3f(0.0f, 0.0f, -0.1f));

  pgm.uniform("u_MAT_TRANSFORM").set(*ts, false);

  cgl::SelectModel(m_fieldLines).draw();
}

void Program::renderParticles(double time, double dt) {
  const int MAX_SPAWNS = 16;
  int       spawns     = MAX_SPAWNS;

  while (time > m_spawnTime) {
    --spawns;

    if (m_particles[m_nextParticle].remain > 1e-3f)
      warn("killing particle " + std::to_string(m_nextParticle));

    Eigen::Vector2f pos;

    if constexpr ((false)) {
      const float JITTER = 0.5f;

      std::uniform_real_distribution<float> posJitter(-JITTER, JITTER);

      pos = Eigen::Vector2f(-0.77f, -0.05f) +
            Eigen::Vector2f(posJitter(m_mt), posJitter(m_mt));
    }
    else {
      pos = (m_proj.inverse() *
             Eigen::Vector3f(
                 std::uniform_real_distribution<float>(-1.0f, 1.0f)(m_mt),
                 std::uniform_real_distribution<float>(-1.0f, 1.0f)(m_mt),
                 0.0f)
                 .homogeneous())
                .hnormalized()
                .template head<2>();
    }

    Eigen::Vector3f color;

    {
      Eigen::Vector3f palette[]{
          // Teal, magenta, black
          // Eigen::Vector3f(0.0f, 0.85f, 0.5f),
          // Eigen::Vector3f(0.95f, 0.1f, 0.85f),
          // Eigen::Vector3f(0.1f, 0.1f, 0.1f),

          // Orange, green, blue, magenta
          // Eigen::Vector3f(0.85f, 0.5f, 0.03f),
          // Eigen::Vector3f(0.25f, 0.85f, 0.1f),
          // Eigen::Vector3f(0.04f, 0.45f, 0.95f),
          // Eigen::Vector3f(0.85f, 0.02f, 0.55f),

          // Black, white, caramel
          Eigen::Vector3f(0.1f, 0.1f, 0.1f),
          Eigen::Vector3f(0.95f, 0.95f, 0.87f),
          Eigen::Vector3f(0.75f, 0.45f, 0.12f),
      };

      color = palette[std::uniform_int_distribution<std::size_t>(
          0, sizeof(palette) / sizeof(*palette) - 1)(m_mt)];

      std::uniform_real_distribution<float> clrJitter(-0.01f, 0.01f);

      color += Eigen::Vector3f(clrJitter(m_mt),
                               clrJitter(m_mt),
                               clrJitter(m_mt));

      color *= pth::lerp(
          0.75f,
          1.0f,
          float(std::pow(std::uniform_real_distribution(0_sc, 1_sc)(m_mt), 3)));
    }

    float life = std::uniform_real_distribution<float>(0.45f, 2.0f)(m_mt);

    m_particles[m_nextParticle] = Particle{
        .pos    = pos,
        .clr    = color,
        .size   = std::uniform_real_distribution<float>(0.05f, 0.13f)(m_mt),
        .life   = life,
        .remain = life,
    };

    m_nextParticle = (m_nextParticle + 1) % m_particles.size();

    m_spawnTime = (spawns ? m_spawnTime : time) +
                  std::uniform_real_distribution(0.0f, 0.01f)(m_mt);
  }

  // if (int count = MAX_SPAWNS - spawns; count) info(std::to_string(count));

  {
    cgl::UseProgram  pgm(m_particle);
    cgl::SelectModel mdl(m_circle);

    pgm.uniform("u_MAT_PROJ").set(m_proj, false);
    pgm.uniform("u_MAT_VIEW").set(m_view, false);

    TransformStack ts;

    for (int i = 0; i < m_particles.size(); ++i) {
      auto &&particle = m_particles[i];

      if (particle.remain < 1e-3f) continue;

      particle.remain -= dt;

      ts.save();

      Eigen::Vector3f pos;
      Eigen::Vector4f clr;

      float fadeIn = std::tanh((particle.life - particle.remain) / 1.0f);

      pos.template head<2>() = particle.pos;
      pos.z()                = 0.0f;

      clr.template head<3>() = particle.clr;
      clr.w()                = fadeIn;

      ts << Eigen::Translation3f(pos);

      ts << Eigen::UniformScaling<float>(particle.size *
                                         (particle.remain / particle.life) *
                                         pth::lerp(0.35f, 1.0f, fadeIn));

      pgm.uniform("u_MAT_WORLD").set(*ts, false);
      pgm.uniform("u_VEC_COLOR").set(clr);

      mdl.draw();

      ts.restore();

      particle.pos += particleField(particle.pos, time) * dt;
    }

    ts.restore();
  }
}

void Program::render(double time) {
  double dt = std::min(0.1, time - m_lastTime);

  std::size_t lastMainFbuf = (m_mainFbuf - 1) % MAIN_FBUF_COUNT;

  {
    cgl::BindFramebuffer fbuf(GL_FRAMEBUFFER, m_particleFbufs[0]);

    glViewport(0, 0, m_width, m_height);

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glCullFace(GL_BACK);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // renderFieldLines(time);

    renderParticles(time, dt);
  }

  {
    cgl::BindFramebuffer fbuf(GL_FRAMEBUFFER, m_mainFbufs[m_mainFbuf]);

    glViewport(0, 0, m_width, m_height);

    glClearColor(0.0f, 0.5f, 0.0f, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    {
      cgl::UseProgram         pgm(m_blitFback);
      cgl::SelectTextureUnits tex(m_mainFbufMap[lastMainFbuf]);

      pgm.uniform("u_MAT_TRANSFORM")
          .set(Eigen::Projective3f::Identity(), false);
      pgm.uniform("u_S2D_TEXTURE").set(0);
      // pgm.uniform("u_FLT_TIME").set(float(time));
      pgm.uniform("u_FLT_DT").set(float(dt));

      cgl::SelectModel(m_blitQuad).draw();
    }

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    {
      cgl::UseProgram         pgm(m_blit);
      cgl::SelectTextureUnits tex(m_particleMap);

      pgm.uniform("u_MAT_TRANSFORM")
          .set(Eigen::Projective3f::Identity(), false);
      pgm.uniform("u_S2D_TEXTURE").set(0);
      pgm.uniform("u_FLT_ALPHA").set(1.0f);

      cgl::SelectModel(m_blitQuad).draw();
    }
  }

  glViewport(0, 0, m_width, m_height);

  glClearColor(0.0f, 0.5f, 0.0f, 1.0f);
  glClearDepthf(1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);

  {
    cgl::UseProgram         pgm(m_blit);
    cgl::SelectTextureUnits tex(m_mainFbufMap[m_mainFbuf]);

    pgm.uniform("u_MAT_TRANSFORM").set(Eigen::Projective3f::Identity(), false);
    pgm.uniform("u_S2D_TEXTURE").set(0);
    pgm.uniform("u_FLT_ALPHA").set(1.0f);

    cgl::SelectModel(m_blitQuad).draw();
  }

  m_surf.swap();

  m_mainFbuf = (m_mainFbuf + 1) % MAIN_FBUF_COUNT;
  m_lastTime = time;
}

void Program::resize(int width, int height) {
  m_width  = width;
  m_height = height;

  {
    TransformStack ts;

    const float SCALE = 4.0f;

    Eigen::Vector2f aspect(m_width, m_height);
    aspect.normalize();

    ts << Eigen::Ortho3f(SCALE * aspect, 0.0f, 100.0f);

    m_proj = *ts;

    ts.clear();

    m_view = *ts;
  }

  for (int i = 0; i < MAIN_FBUF_COUNT; ++i) {
    cgl::BindFramebuffer fbuf(GL_FRAMEBUFFER, m_mainFbufs[i]);

    {
      cgl::BindTexture tex = m_mainFbufMap[i].bindTex(0);

      tex.image(0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

      tex.param(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      tex.param(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      tex.param(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      tex.param(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

      fbuf.texture2D(GL_COLOR_ATTACHMENT0, tex, 0);
    }

    {
      cgl::BindRenderbuffer rbuf(GL_RENDERBUFFER, m_mainRbufs[i]);

      rbuf.storage(GL_DEPTH_COMPONENT16, width, height);

      fbuf.renderBuf(GL_DEPTH_ATTACHMENT, rbuf);
    }

    fbuf.assertStatus();

    glViewport(0, 0, width, height);

    glClearColor(0.0f, 0.5f, 0.0f, 0.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    renderBackground(1.0f);
  }

  {
    cgl::BindFramebuffer fbuf(GL_FRAMEBUFFER, m_particleFbufs[0]);

    {
      cgl::BindTexture tex = m_particleMap.bindTex(0);

      tex.image(0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

      tex.param(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      tex.param(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      tex.param(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      tex.param(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

      fbuf.texture2D(GL_COLOR_ATTACHMENT0, tex, 0);
    }

    {
      cgl::BindRenderbuffer rbuf(GL_RENDERBUFFER, m_particleRbufs[0]);

      rbuf.storage(GL_DEPTH_COMPONENT16, width, height);

      fbuf.renderBuf(GL_DEPTH_ATTACHMENT, rbuf);
    }

    fbuf.assertStatus();
  }
}
