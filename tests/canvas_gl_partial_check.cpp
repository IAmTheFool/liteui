#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>
#include <sstream>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <deque>
#include <list>
#include <queue>
#include <stack>
#include <thread>
#include <chrono>
#include <filesystem>
#include <random>
#include <numeric>
#include <iomanip>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cassert>
#include <condition_variable>
#include <future>
#include <format>
#include <charconv>
#include <string_view>
#include <type_traits>
#include <utility>
#include <tuple>
#include <bitset>
#include <locale>
#include <codecvt>
#include <ctime>
#include <cctype>
#include <cwchar>
#include <csignal>
#include <cerrno>
#include <exception>
#include <stdexcept>
#include <typeinfo>
#include <new>
#include <ratio>
#include <regex>
#include <system_error>
#include <shared_mutex>
#include <compare>
#include <concepts>
#include <ranges>
#include <iterator>
#define LITEUI_IMPLEMENTATION
#define private public
#include "liteui.hpp"
#undef private
#include <EGL/egl.h>
#include <EGL/eglext.h>

static int fails = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("FAIL line %d: %s\n",__LINE__,#c); ++fails; } }while(0)

struct Px { int r,g,b,a; };
static std::vector<uint8_t> readAll(const View::Computed &c, int w, int h) {
  std::vector<uint8_t> v(w*h*4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, c.canvasFbo);
  glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,v.data());
  return v;
}
static Px at(const std::vector<uint8_t>&v,int w,int x,int y){ const uint8_t*p=&v[(y*w+x)*4]; return {p[0],p[1],p[2],p[3]}; }
static bool is(Px p,int r,int g,int b,int a){ return std::abs(p.r-r)<=2&&std::abs(p.g-g)<=2&&std::abs(p.b-b)<=2&&std::abs(p.a-a)<=2; }

int main() {
  auto getPD = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay dpy = getPD(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
  EGLint maj, min; if(!eglInitialize(dpy,&maj,&min)){ std::printf("eglInitialize failed\n"); return 2; }
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint cfgAttr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
  EGLConfig cfg; EGLint n; eglChooseConfig(dpy,cfgAttr,&cfg,1,&n);
  EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy,cfg,EGL_NO_CONTEXT,ctxAttr);
  if(!eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx)){ std::printf("makeCurrent failed 0x%x\n",eglGetError()); return 2; }
  std::printf("GL: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

  liteui_canvas_gl::Renderer r;
  if(!r.init()){ std::printf("Renderer init failed\n"); return 2; }
  const int W=64,H=64;
  View::Computed c;
  CHECK(liteui_canvas_gl::Renderer::buildTarget(c,W,H));
  std::printf("MSAA target: %s\n", c.canvasMsaaFbo ? "yes" : "no");

  using Impl = CanvasContextImpl;
  auto red=[&](CanvasContext&x){ x.setFillColor(Color{255,0,0,255}); x.fillRect(0,0,64,64); };

  // ---- full pass: whole canvas red
  r.begin(c,W,H);
  { Impl cx(r,(float)W,(float)H,&c,1.0f); CHECK(cx.isFullRepaint()); CHECK(cx.damage().w==64); red(cx); }
  r.end(c);
  auto img = readAll(c,W,H);
  CHECK(is(at(img,W,5,5),255,0,0,255)); CHECK(is(at(img,W,60,60),255,0,0,255));

  // caller-visible GL state must be restored: give it a recognisable scissor
  glEnable(GL_SCISSOR_TEST); glScissor(3,4,5,6);

  // ---- partial pass: damage box [16,32)x[16,32); app paints ONE small green rect and a huge blue one clipped by damage
  liteui_canvas_damage::DeviceRect dev{16,16,32,32};
  r.begin(c,W,H,&dev);
  { Impl cx(r,(float)W,(float)H,&c,1.0f);
    static_cast<CanvasContext&>(cx).setDamageState(DirtyRect{16,16,16,16}, false);
    CHECK(!cx.isFullRepaint()); CHECK(cx.damage().x==16 && cx.damage().w==16);
    cx.setFillColor(Color{0,255,0,255}); cx.fillRect(20,20,4,4);          // inside damage
    cx.setFillColor(Color{0,0,255,255}); cx.fillRect(40,40,10,10);        // OUTSIDE damage: must be clipped away
    cx.setFillColor(Color{0,0,255,255}); cx.fillRect(30,30,10,10);        // straddles the edge: only [30,32) shows
    { CanvasImage pi(4,4); for(int i=0;i<16;++i){ pi.pixels[i*4]=0; pi.pixels[i*4+1]=0; pi.pixels[i*4+2]=255; pi.pixels[i*4+3]=255; }
      cx.putImageData(pi, 50, 50);                                         // OUTSIDE damage: must be clipped away too
      cx.putImageData(pi, 30, 17); }                                       // straddles the right edge: only x in [30,32) lands
  }
  r.end(c);
  GLint sb[4]; glGetIntegerv(GL_SCISSOR_BOX,sb); CHECK(sb[0]==3&&sb[1]==4&&sb[2]==5&&sb[3]==6); CHECK(glIsEnabled(GL_SCISSOR_TEST));
  glDisable(GL_SCISSOR_TEST);
  img = readAll(c,W,H);
  CHECK(is(at(img,W,5,5),255,0,0,255));      // outside damage: red kept
  CHECK(is(at(img,W,60,60),255,0,0,255));
  CHECK(is(at(img,W,15,20),255,0,0,255));    // just left of box: kept
  CHECK(is(at(img,W,32,20),255,0,0,255));    // just right of box (x1 exclusive): kept
  CHECK(is(at(img,W,20,15),255,0,0,255));    // just above
  CHECK(is(at(img,W,20,32),255,0,0,255));    // just below
  CHECK(is(at(img,W,16,16),0,0,0,0));        // inside box corner: cleared (transparent)
  CHECK(is(at(img,W,31,31),0,0,255,255));    // blue straddler visible inside box
  CHECK(is(at(img,W,22,22),0,255,0,255));    // green painted
  CHECK(is(at(img,W,18,28),0,0,0,0));        // inside box, unpainted: cleared
  CHECK(is(at(img,W,45,45),255,0,0,255));    // blue rect outside damage clipped away, red intact
  CHECK(is(at(img,W,33,33),255,0,0,255));    // straddler's part beyond box clipped
  CHECK(is(at(img,W,51,51),255,0,0,255));    // putImageData outside the box: clipped, red intact
  CHECK(is(at(img,W,30,18),0,0,255,255));    // putImageData inside the box landed
  CHECK(is(at(img,W,33,18),255,0,0,255));    // ...but not past the box edge

  // ---- stencil clip inside a partial pass still works (ctx.clip + scissor coexist)
  liteui_canvas_damage::DeviceRect dev2{0,0,32,32};
  r.begin(c,W,H,&dev2);
  { Impl cx(r,(float)W,(float)H,&c,1.0f);
    cx.save(); cx.beginPath(); cx.rect(4,4,8,8); cx.clip();
    cx.setFillColor(Color{255,255,0,255}); cx.fillRect(0,0,32,32); cx.restore();
  }
  r.end(c);
  img = readAll(c,W,H);
  CHECK(is(at(img,W,6,6),255,255,0,255)); CHECK(is(at(img,W,2,2),0,0,0,0)); CHECK(is(at(img,W,40,40),255,0,0,255));

  // ---- next full pass wipes everything again
  r.begin(c,W,H);
  { Impl cx(r,(float)W,(float)H,&c,1.0f); cx.setFillColor(Color{10,20,30,255}); cx.fillRect(0,0,8,8); }
  r.end(c);
  img = readAll(c,W,H);
  CHECK(is(at(img,W,2,2),10,20,30,255)); CHECK(is(at(img,W,40,40),0,0,0,0));


  // ================= Phase B: app-owned Textures on the real GL path =========
  {
    auto t = std::make_shared<CanvasTexture>(8, 8, PixelFormat::RGBA8_UNORM, TextureAlpha::Premultiplied);
    auto fillRect=[&](int x,int y,int w,int h,uint8_t R,uint8_t G,uint8_t B){
      std::vector<uint8_t> d(w*h*4); for(int i=0;i<w*h;++i){ d[i*4]=R; d[i*4+1]=G; d[i*4+2]=B; d[i*4+3]=255; }
      return t->update(x,y,w,h,d.data(),w*4); };
    CHECK(fillRect(0,0,4,8,255,0,0)); CHECK(fillRect(4,0,4,8,0,255,0)); // left red, right green
    auto pass=[&](auto&& body){ r.begin(c,W,H); { Impl cx(r,(float)W,(float)H,&c,1.0f); cx.setImageSampling(ImageSampling::Nearest); body(cx);} r.end(c); return readAll(c,W,H); };

    auto im = pass([&](Impl&cx){ cx.drawTexture(t,0,0,16,16); });                 // 2x magnified
    CHECK(is(at(im,W,2,2),255,0,0,255)); CHECK(is(at(im,W,10,2),0,255,0,255)); CHECK(is(at(im,W,2,12),255,0,0,255));
    CHECK(r.textures_.size()==1);

    CHECK(fillRect(0,0,2,2,0,0,255));                                              // update one 2x2 tile in place
    im = pass([&](Impl&cx){ cx.drawTexture(t,0,0,16,16); });
    CHECK(is(at(im,W,2,2),0,0,255,255));      // patched via glTexSubImage2D
    CHECK(is(at(im,W,5,5),255,0,0,255));      // texel (2,2) untouched: still red
    CHECK(is(at(im,W,10,2),0,255,0,255));
    CHECK(r.textures_.size()==1);

    // straight-alpha texture, half-transparent red, composited over a clear canvas
    auto h = std::make_shared<CanvasTexture>(1,1); const uint8_t hp[4]={255,0,0,128}; CHECK(h->update(0,0,1,1,hp,4));
    im = pass([&](Impl&cx){ cx.drawTexture(h,20,20,4,4); });
    CHECK(is(at(im,W,21,21),128,0,0,128));

    // minified smooth draw: builds mips; must not raise a GL error
    im = pass([&](Impl&cx){ cx.setImageSampling(ImageSampling::Smooth); cx.drawTexture(t,40,40,4,4); });
    CHECK(glGetError()==GL_NO_ERROR);
    CHECK(at(im,W,40,42).a==255);
    CHECK(fillRect(7,7,1,1,255,255,255));                                          // stale chain, redrawn next minified draw
    im = pass([&](Impl&cx){ cx.setImageSampling(ImageSampling::Smooth); cx.drawTexture(t,40,40,4,4); });
    CHECK(glGetError()==GL_NO_ERROR);

    // dropping the CanvasTexture releases its GPU copy at the next pass
    t.reset(); h.reset();
    im = pass([&](Impl&cx){ cx.setFillColor(Color{1,2,3,255}); cx.fillRect(0,0,1,1); });
    CHECK(r.textures_.size()==0);
    CHECK(glGetError()==GL_NO_ERROR);

    // context loss: abandon() forgets entries without touching GL; the next draw rebuilds from the CPU copy
    auto u = std::make_shared<CanvasTexture>(2,2,PixelFormat::RGBA8_UNORM,TextureAlpha::Premultiplied);
    { std::vector<uint8_t> d(16,255); CHECK(u->update(0,0,2,2,d.data(),8)); }
    im = pass([&](Impl&cx){ cx.drawTexture(u,0,0,8,8); });
    CHECK(r.textures_.size()==1);
  }

  // ---- wrapped texture on the real GL path: shares the image, uploads it premultiplied
  {
    auto img = std::make_shared<CanvasImage>(8, 8);
    for (int y=0;y<8;++y) for (int x=0;x<8;++x){ uint8_t*p=&img->pixels[(y*8+x)*4]; p[0]=x<4?255:0; p[1]=x<4?0:255; p[2]=0; p[3]=(y<4)?255:128; }
    std::weak_ptr<const CanvasImage> weak = img;
    auto t = CanvasTexture::wrap(img); img.reset();
    CHECK(!weak.expired());
    r.begin(c,W,H); { Impl cx(r,(float)W,(float)H,&c,1.0f); cx.setImageSampling(ImageSampling::Nearest); cx.drawTexture(t,0,0,16,16); } r.end(c);
    auto im = readAll(c,W,H);
    CHECK(is(at(im,W,2,2),255,0,0,255));      // opaque red
    CHECK(is(at(im,W,10,2),0,255,0,255));     // opaque green
    CHECK(is(at(im,W,2,12),128,0,0,128));     // half-transparent red, premultiplied by the upload
    CHECK(glGetError()==GL_NO_ERROR);
    t.reset();
    r.begin(c,W,H); r.end(c);                 // the next pass releases the GPU copy
    CHECK(weak.expired());                    // ...and the image went with the texture
    CHECK(r.textures_.size()==0);
  }
  r.destroy();
  CHECK(r.textures_.size()==0);
  std::printf(fails?"GL TEST FAILED (%d)\n":"GL TEST: all pass\n",fails);
  return fails;
}