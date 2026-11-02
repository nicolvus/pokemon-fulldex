// UPSCALER on the Switch: the widescreen world, drawn at WORLD DETAIL's lines, brought up to the
// screen by AMD FidelityFX Super Resolution 1 (EASU, then RCAS; AMD's ffx_fsr1.h, MIT licence) or
// NVIDIA Image Scaling (NVScaler; NVIDIA's NIS_Scaler.h and its filter tables, MIT licence,
// Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES), both ported to OpenGL ES 2 fragment
// shaders, instead of a bilinear stretch. Drawn with OpenGL ES in SDL's own renderer context (the Switch's SDL draws
// with OpenGL ES 2), between SDL's draws: the GL state SDL keeps is put back afterwards.
#pragma once
#ifdef __SWITCH__
#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <SDL.h>
#include <SDL_opengles2.h>

class Upscaler
{
public:
    enum { OFF, FSR, NIS };
    // The world's picture (0xAARRGGBB, w x h) scaled to the whole of the renderer's output by
    // `mode`, its colours times k / 255 (the game's fade). False if it could not be drawn (then
    // nothing was).
    bool Draw(SDL_Renderer* r, const uint32_t* px, int w, int h, int k, int mode)
    {
        if (mode == OFF) return false;
        if (failed || SDL_GetRenderTarget(r)) return false;
        if (mode == NIS && ready && !nis) return false;
        int outW = 0, outH = 0;
        if (SDL_GetRendererOutputSize(r, &outW, &outH) != 0 || outW <= 0 || outH <= 0) return false;
        if (!ready && !Init()) return false;
        SDL_RenderFlush(r); // (SDL's clear and anything before, then this)
        Saved s = Save();
        gl.Disable(GL_BLEND);
        gl.Disable(GL_SCISSOR_TEST);
        gl.ActiveTexture(GL_TEXTURE0);
        // The picture.
        gl.BindTexture(GL_TEXTURE_2D, inTex);
        if (w != inW || h != inH)
        {
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            inW = w;
            inH = h;
        }
        else gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (mode == NIS)
        {
            // One pass, onto the screen.
            gl.Viewport(0, 0, outW, outH);
            gl.UseProgram(nis);
            gl.Uniform1i(gl.GetUniformLocation(nis, "src"), 0);
            gl.Uniform2f(gl.GetUniformLocation(nis, "inSize"), (float)w, (float)h);
            gl.Uniform2f(gl.GetUniformLocation(nis, "outSize"), (float)outW, (float)outH);
            gl.Uniform1f(gl.GetUniformLocation(nis, "k"), k / 255.0f);
            Quad();
            Restore(s);
            return true;
        }
        // EASU into a picture the screen's size.
        gl.BindTexture(GL_TEXTURE_2D, midTex);
        if (outW != midW || outH != midH)
        {
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, outW, outH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            midW = outW;
            midH = outH;
        }
        gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, midTex, 0);
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            fprintf(stderr, "[upscale] the framebuffer is not complete: upscaling off\n");
            failed = true;
            Restore(s);
            return false;
        }
        gl.Viewport(0, 0, outW, outH);
        gl.UseProgram(easu);
        gl.BindTexture(GL_TEXTURE_2D, inTex);
        gl.Uniform1i(gl.GetUniformLocation(easu, "src"), 0);
        gl.Uniform2f(gl.GetUniformLocation(easu, "inSize"), (float)w, (float)h);
        gl.Uniform2f(gl.GetUniformLocation(easu, "outSize"), (float)outW, (float)outH);
        Quad();
        // RCAS onto the screen.
        gl.BindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        gl.Viewport(0, 0, outW, outH);
        gl.UseProgram(rcas);
        gl.BindTexture(GL_TEXTURE_2D, midTex);
        gl.Uniform1i(gl.GetUniformLocation(rcas, "src"), 0);
        gl.Uniform2f(gl.GetUniformLocation(rcas, "size"), (float)outW, (float)outH);
        gl.Uniform1f(gl.GetUniformLocation(rcas, "sharpness"), 0.87f); // (exp2(-0.2): AMD's usual)
        gl.Uniform1f(gl.GetUniformLocation(rcas, "k"), k / 255.0f);
        Quad();
        Restore(s);
        return true;
    }

private:
    struct
    {
        PFNGLCREATESHADERPROC CreateShader;
        PFNGLSHADERSOURCEPROC ShaderSource;
        PFNGLCOMPILESHADERPROC CompileShader;
        PFNGLGETSHADERIVPROC GetShaderiv;
        PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
        PFNGLCREATEPROGRAMPROC CreateProgram;
        PFNGLATTACHSHADERPROC AttachShader;
        PFNGLBINDATTRIBLOCATIONPROC BindAttribLocation;
        PFNGLLINKPROGRAMPROC LinkProgram;
        PFNGLGETPROGRAMIVPROC GetProgramiv;
        PFNGLUSEPROGRAMPROC UseProgram;
        PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
        PFNGLUNIFORM1IPROC Uniform1i;
        PFNGLUNIFORM1FPROC Uniform1f;
        PFNGLUNIFORM2FPROC Uniform2f;
        PFNGLUNIFORM4FVPROC Uniform4fv;
        PFNGLGENTEXTURESPROC GenTextures;
        PFNGLBINDTEXTUREPROC BindTexture;
        PFNGLTEXIMAGE2DPROC TexImage2D;
        PFNGLTEXSUBIMAGE2DPROC TexSubImage2D;
        PFNGLTEXPARAMETERIPROC TexParameteri;
        PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
        PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
        PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
        PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
        PFNGLVIEWPORTPROC Viewport;
        PFNGLENABLEPROC Enable;
        PFNGLDISABLEPROC Disable;
        PFNGLISENABLEDPROC IsEnabled;
        PFNGLGETINTEGERVPROC GetIntegerv;
        PFNGLACTIVETEXTUREPROC ActiveTexture;
        PFNGLBINDBUFFERPROC BindBuffer;
        PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
        PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
        PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray;
        PFNGLGETVERTEXATTRIBIVPROC GetVertexAttribiv;
        PFNGLDRAWARRAYSPROC DrawArrays;
    } gl {};

    // What this changes of SDL's state, put back as it was.
    struct Saved
    {
        GLint program, arrayBuffer, activeTexture, texture0, fbo, viewport[4];
        GLboolean blend, scissor;
        GLint attribOn[4];
    };
    Saved Save()
    {
        Saved s {};
        gl.GetIntegerv(GL_CURRENT_PROGRAM, &s.program);
        gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuffer);
        gl.GetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.texture0);
        gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
        gl.GetIntegerv(GL_VIEWPORT, s.viewport);
        s.blend = gl.IsEnabled(GL_BLEND);
        s.scissor = gl.IsEnabled(GL_SCISSOR_TEST);
        for (int i = 0; i < 4; i++) gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s.attribOn[i]);
        return s;
    }
    void Restore(const Saved& s)
    {
        for (int i = 0; i < 4; i++)
            if (s.attribOn[i]) gl.EnableVertexAttribArray(i);
            else gl.DisableVertexAttribArray(i);
        gl.BindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        gl.Viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
        if (s.blend) gl.Enable(GL_BLEND);
        if (s.scissor) gl.Enable(GL_SCISSOR_TEST);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, s.texture0);
        gl.ActiveTexture(s.activeTexture);
        gl.BindBuffer(GL_ARRAY_BUFFER, s.arrayBuffer);
        gl.UseProgram(s.program);
    }

    // A triangle over the whole viewport (attribute 0, from memory).
    void Quad()
    {
        static const GLfloat tri[] = { -1, -1, 3, -1, -1, 3 };
        gl.BindBuffer(GL_ARRAY_BUFFER, 0);
        for (int i = 1; i < 4; i++) gl.DisableVertexAttribArray(i);
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
        gl.DrawArrays(GL_TRIANGLES, 0, 3);
    }

    GLuint Compile(GLenum type, const char* src)
    {
        const GLuint sh = gl.CreateShader(type);
        gl.ShaderSource(sh, 1, &src, nullptr);
        gl.CompileShader(sh);
        GLint ok = 0;
        gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = {};
            gl.GetShaderInfoLog(sh, sizeof log, nullptr, log);
            fprintf(stderr, "[upscale] shader: %s\n", log);
            return 0;
        }
        return sh;
    }
    GLuint Program(const char* fragment)
    {
        static const char* vertex = "attribute vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }";
        const GLuint vs = Compile(GL_VERTEX_SHADER, vertex), fs = Compile(GL_FRAGMENT_SHADER, fragment);
        if (!vs || !fs) return 0;
        const GLuint p = gl.CreateProgram();
        gl.AttachShader(p, vs);
        gl.AttachShader(p, fs);
        gl.BindAttribLocation(p, 0, "pos");
        gl.LinkProgram(p);
        GLint ok = 0;
        gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
        return ok ? p : 0;
    }
    GLuint Texture()
    {
        GLuint t = 0;
        gl.GenTextures(1, &t);
        gl.BindTexture(GL_TEXTURE_2D, t);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return t;
    }

    bool Init()
    {
        failed = true; // (until it is all there)
        bool all = true;
        auto get = [&](auto& fn, const char* name) {
            fn = (std::remove_reference_t<decltype(fn)>)SDL_GL_GetProcAddress(name);
            if (!fn) all = false;
        };
        get(gl.CreateShader, "glCreateShader"); get(gl.ShaderSource, "glShaderSource");
        get(gl.CompileShader, "glCompileShader"); get(gl.GetShaderiv, "glGetShaderiv");
        get(gl.GetShaderInfoLog, "glGetShaderInfoLog"); get(gl.CreateProgram, "glCreateProgram");
        get(gl.AttachShader, "glAttachShader"); get(gl.BindAttribLocation, "glBindAttribLocation");
        get(gl.LinkProgram, "glLinkProgram"); get(gl.GetProgramiv, "glGetProgramiv");
        get(gl.UseProgram, "glUseProgram"); get(gl.GetUniformLocation, "glGetUniformLocation");
        get(gl.Uniform1i, "glUniform1i"); get(gl.Uniform1f, "glUniform1f"); get(gl.Uniform2f, "glUniform2f");
        get(gl.Uniform4fv, "glUniform4fv");
        get(gl.GenTextures, "glGenTextures"); get(gl.BindTexture, "glBindTexture");
        get(gl.TexImage2D, "glTexImage2D"); get(gl.TexSubImage2D, "glTexSubImage2D");
        get(gl.TexParameteri, "glTexParameteri"); get(gl.GenFramebuffers, "glGenFramebuffers");
        get(gl.BindFramebuffer, "glBindFramebuffer"); get(gl.FramebufferTexture2D, "glFramebufferTexture2D");
        get(gl.CheckFramebufferStatus, "glCheckFramebufferStatus"); get(gl.Viewport, "glViewport");
        get(gl.Enable, "glEnable"); get(gl.Disable, "glDisable"); get(gl.IsEnabled, "glIsEnabled");
        get(gl.GetIntegerv, "glGetIntegerv"); get(gl.ActiveTexture, "glActiveTexture");
        get(gl.BindBuffer, "glBindBuffer"); get(gl.VertexAttribPointer, "glVertexAttribPointer");
        get(gl.EnableVertexAttribArray, "glEnableVertexAttribArray");
        get(gl.DisableVertexAttribArray, "glDisableVertexAttribArray");
        get(gl.GetVertexAttribiv, "glGetVertexAttribiv"); get(gl.DrawArrays, "glDrawArrays");
        if (!all)
        {
            fprintf(stderr, "[upscale] OpenGL ES functions missing: upscaling off\n");
            return false;
        }
        Saved s = Save();
        easu = Program(EasuSource());
        rcas = Program(RcasSource());
        // (NIS's filter tables, 2 x 64 phases of 8, as 256 uniform vectors: if the graphics chip
        // cannot hold them, NIS is left out and FSR stays.)
        GLint vectors = 0;
        gl.GetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &vectors);
        nis = vectors >= 300 ? Program(NisSource()) : 0;
        if (nis)
        {
    static const float kNisScale[64 * 8] = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0029f, -0.0127f, 1.0f, 0.0132f, -0.0034f, 0.0f, 0.0f, 0.0f,
        0.0063f, -0.0249f, 0.9985f, 0.0269f, -0.0068f, 0.0f, 0.0f, 0.0f,
        0.0088f, -0.0361f, 0.9956f, 0.0415f, -0.0103f, 0.0005f, 0.0f, 0.0f,
        0.0117f, -0.0474f, 0.9932f, 0.0562f, -0.0142f, 0.0005f, 0.0f, 0.0f,
        0.0142f, -0.0576f, 0.9897f, 0.0713f, -0.0181f, 0.0005f, 0.0f, 0.0f,
        0.0166f, -0.0674f, 0.9844f, 0.0874f, -0.022f, 0.001f, 0.0f, 0.0f,
        0.0186f, -0.0762f, 0.9785f, 0.104f, -0.0264f, 0.0015f, 0.0f, 0.0f,
        0.0205f, -0.085f, 0.9727f, 0.1206f, -0.0308f, 0.002f, 0.0f, 0.0f,
        0.0225f, -0.0928f, 0.9648f, 0.1382f, -0.0352f, 0.0024f, 0.0f, 0.0f,
        0.0239f, -0.1006f, 0.9575f, 0.1558f, -0.0396f, 0.0029f, 0.0f, 0.0f,
        0.0254f, -0.1074f, 0.9487f, 0.1738f, -0.0439f, 0.0034f, 0.0f, 0.0f,
        0.0264f, -0.1138f, 0.939f, 0.1929f, -0.0488f, 0.0044f, 0.0f, 0.0f,
        0.0278f, -0.1191f, 0.9282f, 0.2119f, -0.0537f, 0.0049f, 0.0f, 0.0f,
        0.0288f, -0.1245f, 0.917f, 0.231f, -0.0581f, 0.0059f, 0.0f, 0.0f,
        0.0293f, -0.1294f, 0.9058f, 0.251f, -0.063f, 0.0063f, 0.0f, 0.0f,
        0.0303f, -0.1333f, 0.8926f, 0.271f, -0.0679f, 0.0073f, 0.0f, 0.0f,
        0.0308f, -0.1367f, 0.8789f, 0.2915f, -0.0728f, 0.0083f, 0.0f, 0.0f,
        0.0308f, -0.1401f, 0.8657f, 0.312f, -0.0776f, 0.0093f, 0.0f, 0.0f,
        0.0313f, -0.1426f, 0.8506f, 0.333f, -0.0825f, 0.0103f, 0.0f, 0.0f,
        0.0313f, -0.1445f, 0.8354f, 0.354f, -0.0874f, 0.0112f, 0.0f, 0.0f,
        0.0313f, -0.146f, 0.8193f, 0.3755f, -0.0923f, 0.0122f, 0.0f, 0.0f,
        0.0313f, -0.147f, 0.8022f, 0.3965f, -0.0967f, 0.0137f, 0.0f, 0.0f,
        0.0308f, -0.1479f, 0.7856f, 0.4185f, -0.1016f, 0.0146f, 0.0f, 0.0f,
        0.0303f, -0.1479f, 0.7681f, 0.4399f, -0.106f, 0.0156f, 0.0f, 0.0f,
        0.0298f, -0.1479f, 0.7505f, 0.4614f, -0.1104f, 0.0166f, 0.0f, 0.0f,
        0.0293f, -0.147f, 0.7314f, 0.4829f, -0.1147f, 0.0181f, 0.0f, 0.0f,
        0.0288f, -0.146f, 0.7119f, 0.5049f, -0.1187f, 0.019f, 0.0f, 0.0f,
        0.0278f, -0.1445f, 0.6929f, 0.5264f, -0.1226f, 0.02f, 0.0f, 0.0f,
        0.0273f, -0.1431f, 0.6724f, 0.5479f, -0.126f, 0.0215f, 0.0f, 0.0f,
        0.0264f, -0.1411f, 0.6528f, 0.5693f, -0.1299f, 0.0225f, 0.0f, 0.0f,
        0.0254f, -0.1387f, 0.6323f, 0.5903f, -0.1328f, 0.0234f, 0.0f, 0.0f,
        0.0244f, -0.1357f, 0.6113f, 0.6113f, -0.1357f, 0.0244f, 0.0f, 0.0f,
        0.0234f, -0.1328f, 0.5903f, 0.6323f, -0.1387f, 0.0254f, 0.0f, 0.0f,
        0.0225f, -0.1299f, 0.5693f, 0.6528f, -0.1411f, 0.0264f, 0.0f, 0.0f,
        0.0215f, -0.126f, 0.5479f, 0.6724f, -0.1431f, 0.0273f, 0.0f, 0.0f,
        0.02f, -0.1226f, 0.5264f, 0.6929f, -0.1445f, 0.0278f, 0.0f, 0.0f,
        0.019f, -0.1187f, 0.5049f, 0.7119f, -0.146f, 0.0288f, 0.0f, 0.0f,
        0.0181f, -0.1147f, 0.4829f, 0.7314f, -0.147f, 0.0293f, 0.0f, 0.0f,
        0.0166f, -0.1104f, 0.4614f, 0.7505f, -0.1479f, 0.0298f, 0.0f, 0.0f,
        0.0156f, -0.106f, 0.4399f, 0.7681f, -0.1479f, 0.0303f, 0.0f, 0.0f,
        0.0146f, -0.1016f, 0.4185f, 0.7856f, -0.1479f, 0.0308f, 0.0f, 0.0f,
        0.0137f, -0.0967f, 0.3965f, 0.8022f, -0.147f, 0.0313f, 0.0f, 0.0f,
        0.0122f, -0.0923f, 0.3755f, 0.8193f, -0.146f, 0.0313f, 0.0f, 0.0f,
        0.0112f, -0.0874f, 0.354f, 0.8354f, -0.1445f, 0.0313f, 0.0f, 0.0f,
        0.0103f, -0.0825f, 0.333f, 0.8506f, -0.1426f, 0.0313f, 0.0f, 0.0f,
        0.0093f, -0.0776f, 0.312f, 0.8657f, -0.1401f, 0.0308f, 0.0f, 0.0f,
        0.0083f, -0.0728f, 0.2915f, 0.8789f, -0.1367f, 0.0308f, 0.0f, 0.0f,
        0.0073f, -0.0679f, 0.271f, 0.8926f, -0.1333f, 0.0303f, 0.0f, 0.0f,
        0.0063f, -0.063f, 0.251f, 0.9058f, -0.1294f, 0.0293f, 0.0f, 0.0f,
        0.0059f, -0.0581f, 0.231f, 0.917f, -0.1245f, 0.0288f, 0.0f, 0.0f,
        0.0049f, -0.0537f, 0.2119f, 0.9282f, -0.1191f, 0.0278f, 0.0f, 0.0f,
        0.0044f, -0.0488f, 0.1929f, 0.939f, -0.1138f, 0.0264f, 0.0f, 0.0f,
        0.0034f, -0.0439f, 0.1738f, 0.9487f, -0.1074f, 0.0254f, 0.0f, 0.0f,
        0.0029f, -0.0396f, 0.1558f, 0.9575f, -0.1006f, 0.0239f, 0.0f, 0.0f,
        0.0024f, -0.0352f, 0.1382f, 0.9648f, -0.0928f, 0.0225f, 0.0f, 0.0f,
        0.002f, -0.0308f, 0.1206f, 0.9727f, -0.085f, 0.0205f, 0.0f, 0.0f,
        0.0015f, -0.0264f, 0.104f, 0.9785f, -0.0762f, 0.0186f, 0.0f, 0.0f,
        0.001f, -0.022f, 0.0874f, 0.9844f, -0.0674f, 0.0166f, 0.0f, 0.0f,
        0.0005f, -0.0181f, 0.0713f, 0.9897f, -0.0576f, 0.0142f, 0.0f, 0.0f,
        0.0005f, -0.0142f, 0.0562f, 0.9932f, -0.0474f, 0.0117f, 0.0f, 0.0f,
        0.0005f, -0.0103f, 0.0415f, 0.9956f, -0.0361f, 0.0088f, 0.0f, 0.0f,
        0.0f, -0.0068f, 0.0269f, 0.9985f, -0.0249f, 0.0063f, 0.0f, 0.0f,
        0.0f, -0.0034f, 0.0132f, 1.0f, -0.0127f, 0.0029f, 0.0f, 0.0f };
    static const float kNisUsm[64 * 8] = {
        0.0f, -0.6001f, 1.2002f, -0.6001f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0029f, -0.6084f, 1.1987f, -0.5903f, -0.0029f, 0.0f, 0.0f, 0.0f,
        0.0049f, -0.6147f, 1.1958f, -0.5791f, -0.0068f, 0.0005f, 0.0f, 0.0f,
        0.0073f, -0.6196f, 1.189f, -0.5659f, -0.0103f, 0.0f, 0.0f, 0.0f,
        0.0093f, -0.6235f, 1.1802f, -0.5513f, -0.0151f, 0.0f, 0.0f, 0.0f,
        0.0112f, -0.6265f, 1.1699f, -0.5352f, -0.0195f, 0.0005f, 0.0f, 0.0f,
        0.0122f, -0.627f, 1.1582f, -0.5181f, -0.0259f, 0.0005f, 0.0f, 0.0f,
        0.0142f, -0.6284f, 1.1455f, -0.5005f, -0.0317f, 0.0005f, 0.0f, 0.0f,
        0.0156f, -0.6265f, 1.1274f, -0.479f, -0.0386f, 0.0005f, 0.0f, 0.0f,
        0.0166f, -0.6235f, 1.1089f, -0.457f, -0.0454f, 0.001f, 0.0f, 0.0f,
        0.0176f, -0.6187f, 1.0879f, -0.4346f, -0.0532f, 0.001f, 0.0f, 0.0f,
        0.0181f, -0.6138f, 1.0659f, -0.4102f, -0.0615f, 0.0015f, 0.0f, 0.0f,
        0.019f, -0.6069f, 1.0405f, -0.3843f, -0.0698f, 0.0015f, 0.0f, 0.0f,
        0.0195f, -0.6006f, 1.0161f, -0.3574f, -0.0796f, 0.002f, 0.0f, 0.0f,
        0.02f, -0.5928f, 0.9893f, -0.3286f, -0.0898f, 0.0024f, 0.0f, 0.0f,
        0.02f, -0.582f, 0.958f, -0.2988f, -0.1001f, 0.0029f, 0.0f, 0.0f,
        0.02f, -0.5728f, 0.9292f, -0.269f, -0.1104f, 0.0034f, 0.0f, 0.0f,
        0.02f, -0.562f, 0.8975f, -0.2368f, -0.1226f, 0.0039f, 0.0f, 0.0f,
        0.0205f, -0.5498f, 0.8643f, -0.2046f, -0.1343f, 0.0044f, 0.0f, 0.0f,
        0.02f, -0.5371f, 0.8301f, -0.1709f, -0.1465f, 0.0049f, 0.0f, 0.0f,
        0.0195f, -0.5239f, 0.7944f, -0.1367f, -0.1587f, 0.0054f, 0.0f, 0.0f,
        0.0195f, -0.5107f, 0.7598f, -0.1021f, -0.1724f, 0.0059f, 0.0f, 0.0f,
        0.019f, -0.4966f, 0.7231f, -0.0649f, -0.1865f, 0.0063f, 0.0f, 0.0f,
        0.0186f, -0.4819f, 0.6846f, -0.0288f, -0.1997f, 0.0068f, 0.0f, 0.0f,
        0.0186f, -0.4668f, 0.646f, 0.0093f, -0.2144f, 0.0073f, 0.0f, 0.0f,
        0.0176f, -0.4507f, 0.6055f, 0.0479f, -0.229f, 0.0083f, 0.0f, 0.0f,
        0.0171f, -0.437f, 0.5693f, 0.0859f, -0.2446f, 0.0088f, 0.0f, 0.0f,
        0.0161f, -0.4199f, 0.5283f, 0.1255f, -0.2598f, 0.0098f, 0.0f, 0.0f,
        0.0161f, -0.4048f, 0.4883f, 0.1655f, -0.2754f, 0.0103f, 0.0f, 0.0f,
        0.0151f, -0.3887f, 0.4497f, 0.2041f, -0.291f, 0.0107f, 0.0f, 0.0f,
        0.0142f, -0.3711f, 0.4072f, 0.2446f, -0.3066f, 0.0117f, 0.0f, 0.0f,
        0.0137f, -0.3555f, 0.3672f, 0.2852f, -0.3228f, 0.0122f, 0.0f, 0.0f,
        0.0132f, -0.3394f, 0.3262f, 0.3262f, -0.3394f, 0.0132f, 0.0f, 0.0f,
        0.0122f, -0.3228f, 0.2852f, 0.3672f, -0.3555f, 0.0137f, 0.0f, 0.0f,
        0.0117f, -0.3066f, 0.2446f, 0.4072f, -0.3711f, 0.0142f, 0.0f, 0.0f,
        0.0107f, -0.291f, 0.2041f, 0.4497f, -0.3887f, 0.0151f, 0.0f, 0.0f,
        0.0103f, -0.2754f, 0.1655f, 0.4883f, -0.4048f, 0.0161f, 0.0f, 0.0f,
        0.0098f, -0.2598f, 0.1255f, 0.5283f, -0.4199f, 0.0161f, 0.0f, 0.0f,
        0.0088f, -0.2446f, 0.0859f, 0.5693f, -0.437f, 0.0171f, 0.0f, 0.0f,
        0.0083f, -0.229f, 0.0479f, 0.6055f, -0.4507f, 0.0176f, 0.0f, 0.0f,
        0.0073f, -0.2144f, 0.0093f, 0.646f, -0.4668f, 0.0186f, 0.0f, 0.0f,
        0.0068f, -0.1997f, -0.0288f, 0.6846f, -0.4819f, 0.0186f, 0.0f, 0.0f,
        0.0063f, -0.1865f, -0.0649f, 0.7231f, -0.4966f, 0.019f, 0.0f, 0.0f,
        0.0059f, -0.1724f, -0.1021f, 0.7598f, -0.5107f, 0.0195f, 0.0f, 0.0f,
        0.0054f, -0.1587f, -0.1367f, 0.7944f, -0.5239f, 0.0195f, 0.0f, 0.0f,
        0.0049f, -0.1465f, -0.1709f, 0.8301f, -0.5371f, 0.02f, 0.0f, 0.0f,
        0.0044f, -0.1343f, -0.2046f, 0.8643f, -0.5498f, 0.0205f, 0.0f, 0.0f,
        0.0039f, -0.1226f, -0.2368f, 0.8975f, -0.562f, 0.02f, 0.0f, 0.0f,
        0.0034f, -0.1104f, -0.269f, 0.9292f, -0.5728f, 0.02f, 0.0f, 0.0f,
        0.0029f, -0.1001f, -0.2988f, 0.958f, -0.582f, 0.02f, 0.0f, 0.0f,
        0.0024f, -0.0898f, -0.3286f, 0.9893f, -0.5928f, 0.02f, 0.0f, 0.0f,
        0.002f, -0.0796f, -0.3574f, 1.0161f, -0.6006f, 0.0195f, 0.0f, 0.0f,
        0.0015f, -0.0698f, -0.3843f, 1.0405f, -0.6069f, 0.019f, 0.0f, 0.0f,
        0.0015f, -0.0615f, -0.4102f, 1.0659f, -0.6138f, 0.0181f, 0.0f, 0.0f,
        0.001f, -0.0532f, -0.4346f, 1.0879f, -0.6187f, 0.0176f, 0.0f, 0.0f,
        0.001f, -0.0454f, -0.457f, 1.1089f, -0.6235f, 0.0166f, 0.0f, 0.0f,
        0.0005f, -0.0386f, -0.479f, 1.1274f, -0.6265f, 0.0156f, 0.0f, 0.0f,
        0.0005f, -0.0317f, -0.5005f, 1.1455f, -0.6284f, 0.0142f, 0.0f, 0.0f,
        0.0005f, -0.0259f, -0.5181f, 1.1582f, -0.627f, 0.0122f, 0.0f, 0.0f,
        0.0005f, -0.0195f, -0.5352f, 1.1699f, -0.6265f, 0.0112f, 0.0f, 0.0f,
        0.0f, -0.0151f, -0.5513f, 1.1802f, -0.6235f, 0.0093f, 0.0f, 0.0f,
        0.0f, -0.0103f, -0.5659f, 1.189f, -0.6196f, 0.0073f, 0.0f, 0.0f,
        0.0005f, -0.0068f, -0.5791f, 1.1958f, -0.6147f, 0.0049f, 0.0f, 0.0f,
        0.0f, -0.0029f, -0.5903f, 1.1987f, -0.6084f, 0.0029f, 0.0f, 0.0f };
            gl.UseProgram(nis);
            gl.Uniform4fv(gl.GetUniformLocation(nis, "coefS"), 128, kNisScale);
            gl.Uniform4fv(gl.GetUniformLocation(nis, "coefU"), 128, kNisUsm);
        }
        else fprintf(stderr, "[upscale] NIS could not be made (%d uniform vectors)\n", (int)vectors);
        inTex = Texture();
        midTex = Texture();
        gl.GenFramebuffers(1, &fbo);
        Restore(s);
        if (!easu || !rcas || !fbo)
        {
            fprintf(stderr, "[upscale] the shaders could not be made: upscaling off\n");
            return false;
        }
        fprintf(stderr, "[upscale] ready\n");
        failed = false;
        ready = true;
        return true;
    }

    // EASU (ffx_fsr1.h, FsrEasuF): 12 texels round the point, their luma's direction and
    // length shaping a Lanczos-like lobe, the result kept within the four nearest. The input's
    // rows are top first, as is the output picture's (read back that way by RCAS). The bytes are
    // SDL's ARGB8888 read as RGBA, so blue and red swap places (.bgr).
    static const char* EasuSource()
    {
        return R"(precision highp float;
uniform sampler2D src;
uniform vec2 inSize, outSize;
vec3 T(vec2 p) { return texture2D(src, (p + 0.5) / inSize).bgr; }
float L(vec3 c) { return c.g + 0.5 * (c.r + c.b); }
void SetF(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
    float lenX = max(abs(lD - lC), abs(lC - lB));
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) / max(lenX, 1.0 / 65536.0), 0.0, 1.0);
    len += lenX * lenX * w;
    float lenY = max(abs(lE - lC), abs(lC - lA));
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) / max(lenY, 1.0 / 65536.0), 0.0, 1.0);
    len += lenY * lenY * w;
}
void Tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c)
{
    vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len;
    float d2 = min(dot(v, v), clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w;
    aW += w;
}
void main()
{
    vec2 ip = vec2(gl_FragCoord.x, gl_FragCoord.y) - 0.5;
    vec2 pp = (ip + 0.5) * inSize / outSize - 0.5;
    vec2 fp = floor(pp);
    pp -= fp;
    vec3 bC = T(fp + vec2(0.0, -1.0)), cC = T(fp + vec2(1.0, -1.0));
    vec3 eC = T(fp + vec2(-1.0, 0.0)), fC = T(fp), gC = T(fp + vec2(1.0, 0.0)), hC = T(fp + vec2(2.0, 0.0));
    vec3 iC = T(fp + vec2(-1.0, 1.0)), jC = T(fp + vec2(0.0, 1.0)), kC = T(fp + vec2(1.0, 1.0)), lC = T(fp + vec2(2.0, 1.0));
    vec3 nC = T(fp + vec2(0.0, 2.0)), oC = T(fp + vec2(1.0, 2.0));
    float bL = L(bC), cL = L(cC), eL = L(eC), fL = L(fC), gL = L(gC), hL = L(hC);
    float iL = L(iC), jL = L(jC), kL = L(kC), lL = L(lC), nL = L(nC), oL = L(oC);
    vec2 dir = vec2(0.0);
    float len = 0.0;
    SetF(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    SetF(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    SetF(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    SetF(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    float dirR = dot(dir, dir);
    if (dirR < 1.0 / 32768.0) dir = vec2(1.0, 0.0);
    else dir *= inversesqrt(dirR);
    len = len * 0.5;
    len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    Tap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);
    Tap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);
    Tap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);
    Tap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);
    Tap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);
    Tap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);
    Tap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);
    Tap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);
    Tap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);
    Tap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);
    Tap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);
    Tap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);
    vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));
    gl_FragColor = vec4(clamp(aC / aW, mn, mx), 1.0);
})";
    }

    // RCAS (FsrRcasF): sharpening that limits itself where it would clip, onto the screen
    // (whose rows are bottom first: the picture is turned the right way up here).
    static const char* RcasSource()
    {
        return R"(precision highp float;
uniform sampler2D src;
uniform vec2 size;
uniform float sharpness, k;
vec3 T(vec2 p) { return texture2D(src, (clamp(p, vec2(0.0), size - 1.0) + 0.5) / size).rgb; }
void main()
{
    vec2 p = vec2(floor(gl_FragCoord.x), size.y - 1.0 - floor(gl_FragCoord.y));
    vec3 b = T(p + vec2(0.0, -1.0)), d = T(p + vec2(-1.0, 0.0)), e = T(p), f = T(p + vec2(1.0, 0.0)), h = T(p + vec2(0.0, 1.0));
    vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));
    vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * sharpness;
    vec3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    gl_FragColor = vec4(c * k, 1.0);
})";
    }

    // NVScaler (NIS_Scaler.h, NIS_SCALER 1, SDR, sharpness 0.5), per output pixel: the 6 x 6 lumas
    // round the point, the edge map of the 4 nearest (interpolated) choosing among a plain 6-tap
    // filter and four directional ones, each with unsharp masking limited by luma and ringing; the
    // colour is the bilinear one with its luma replaced. Picture rows top first; the screen's are
    // bottom first (turned here). The bytes are SDL's ARGB8888 read as RGBA (.bgr).
    static const char* NisSource()
    {
        return R"(precision highp float;
uniform sampler2D src;
uniform vec2 inSize, outSize;
uniform float k;
uniform vec4 coefS[128];
uniform vec4 coefU[128];
const float kDetectRatio = 2.0 * 1127.0 / 1024.0, kDetectThres = 64.0 / 1024.0;
const float kMinContrastRatio = 2.0, kRatioNorm = 1.0 / 8.0, kEps = 1.0 / 255.0;
const float kSharpStartY = 0.45, kSharpScaleY = 1.0 / 0.45;
const float kSharpStrengthMin = 0.4, kSharpStrengthScale = 1.2, kSharpLimitMin = 0.14, kSharpLimitScale = 0.36;
float Y(vec3 c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
vec3 T(vec2 t) { return texture2D(src, (t + 0.5) / inSize).bgr; }
vec4 Edge(float q00, float q01, float q02, float q10, float q11, float q12, float q20, float q21, float q22)
{
    float g0 = abs(q00 + q01 + q02 - q20 - q21 - q22);
    float g45 = abs(q10 + q00 + q01 - q21 - q22 - q12);
    float g90 = abs(q00 + q10 + q20 - q02 - q12 - q22);
    float g135 = abs(q10 + q20 + q21 - q01 - q02 - q12);
    float a = max(g0, g90), ai = min(g0, g90), b = max(g45, g135), bi = min(g45, g135);
    if (a + b == 0.0) return vec4(0.0);
    float e090 = min(a / (a + b), 1.0), e45 = 1.0 - e090;
    bool c090 = a > ai * kDetectRatio && a > kDetectThres && a > bi;
    bool c45 = b > bi * kDetectRatio && b > kDetectThres && b > ai;
    float f090 = c090 && c45 ? e090 : 1.0, f45 = c090 && c45 ? e45 : 1.0;
    return vec4(c090 && a == g0 ? f090 : 0.0, c090 && a != g0 ? f090 : 0.0, c45 && b == g45 ? f45 : 0.0, c45 && b != g45 ? f45 : 0.0);
}
float LTI(float p0, float p1, float p2, float p3, float p4, float p5, int phase)
{
    bool sel = phase <= 32;
    float s = sel ? p0 : p3;
    float aMin = min(min(p1, p2), s), aMax = max(max(p1, p2), s);
    s = sel ? p2 : p5;
    float bMin = min(min(p3, p4), s), bMax = max(max(p3, p4), s);
    float ac = aMax - aMin, bc = bMax - bMin;
    float ratio = max(ac, bc) / (min(ac, bc) + kEps);
    return 1.0 - clamp((ratio - kMinContrastRatio) * kRatioNorm, 0.0, 1.0);
}
float Poly6(float p0, float p1, float p2, float p3, float p4, float p5, int phase)
{
    vec4 a = coefS[phase * 2], b = coefS[phase * 2 + 1], ua = coefU[phase * 2], ub = coefU[phase * 2 + 1];
    vec4 q = vec4(p0, p1, p2, p3);
    vec2 r = vec2(p4, p5);
    float y = dot(a, q) + dot(b.xy, r);
    float usm = dot(ua, q) + dot(ub.xy, r);
    float ys = 1.0 - clamp((y - kSharpStartY) * kSharpScaleY, 0.0, 1.0);
    usm *= ys * kSharpStrengthScale + kSharpStrengthMin;
    float lim = (ys * kSharpLimitScale + kSharpLimitMin) * y;
    usm = min(lim, max(-lim, usm));
    usm *= LTI(p0, p1, p2, p3, p4, p5, phase);
    return y + usm;
}
int Phase(float f) { return int(clamp(floor(f * 64.0), 0.0, 63.0)); }
void main()
{
    vec2 dst = vec2(floor(gl_FragCoord.x), outSize.y - 1.0 - floor(gl_FragCoord.y));
    vec2 srcP = (dst + 0.5) * inSize / outSize - 0.5;
    vec2 base = floor(srcP);
    float fx = srcP.x - base.x, fy = srcP.y - base.y;
    int fxi = Phase(fx), fyi = Phase(fy);
    float p[36];
    vec3 cf, cg, cj, ck;
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++)
        {
            vec3 c = T(base + vec2(float(j - 2), float(i - 2)));
            p[i * 6 + j] = Y(c);
            if (i == 2 && j == 2) cf = c;
            if (i == 2 && j == 3) cg = c;
            if (i == 3 && j == 2) cj = c;
            if (i == 3 && j == 3) ck = c;
        }
    // The edge map of the four nearest texels (each from the 3 x 3 round it), interpolated.
    vec4 e00 = Edge(p[7], p[8], p[9], p[13], p[14], p[15], p[19], p[20], p[21]);
    vec4 e01 = Edge(p[8], p[9], p[10], p[14], p[15], p[16], p[20], p[21], p[22]);
    vec4 e10 = Edge(p[13], p[14], p[15], p[19], p[20], p[21], p[25], p[26], p[27]);
    vec4 e11 = Edge(p[14], p[15], p[16], p[20], p[21], p[22], p[26], p[27], p[28]);
    vec4 w = mix(mix(e00, e01, fx), mix(e10, e11, fx), fy);
    // The plain filter.
    vec4 ya = coefS[fyi * 2], yb = coefS[fyi * 2 + 1], xa = coefS[fxi * 2], xb = coefS[fxi * 2 + 1];
    float cy[6], cx[6];
    cy[0] = ya.x; cy[1] = ya.y; cy[2] = ya.z; cy[3] = ya.w; cy[4] = yb.x; cy[5] = yb.y;
    cx[0] = xa.x; cx[1] = xa.y; cx[2] = xa.z; cx[3] = xa.w; cx[4] = xb.x; cx[5] = xb.y;
    float hAcc = 0.0;
    for (int j = 0; j < 6; j++)
    {
        float vAcc = 0.0;
        for (int i = 0; i < 6; i++) vAcc += p[i * 6 + j] * cy[i];
        hAcc += vAcc * cx[j];
    }
    float opY = hAcc * (1.0 - w.x - w.y - w.z - w.w);
    // The directional ones (AddDirFilters).
    if (w.x > 0.0)
        opY += Poly6(mix(p[2], p[3], fx), mix(p[8], p[9], fx), mix(p[14], p[15], fx), mix(p[20], p[21], fx),
                     mix(p[26], p[27], fx), mix(p[32], p[33], fx), fyi) * w.x;
    if (w.y > 0.0)
        opY += Poly6(mix(p[12], p[18], fy), mix(p[13], p[19], fy), mix(p[14], p[20], fy), mix(p[15], p[21], fy),
                     mix(p[16], p[22], fy), mix(p[17], p[23], fy), fxi) * w.y;
    if (w.z > 0.0)
    {
        float b45 = 0.5 + 0.5 * (fx - fy);
        float t1 = mix(p[13], p[8], b45), t3 = mix(p[20], p[15], b45), t5 = mix(p[27], p[22], b45);
        b45 -= 0.5;
        bool pos = b45 >= 0.0;
        float ab = abs(b45);
        float t0 = mix(p[7], pos ? p[2] : p[12], ab), t2 = mix(p[14], pos ? p[9] : p[19], ab);
        float t4 = mix(p[21], pos ? p[16] : p[26], ab), t6 = mix(p[28], pos ? p[23] : p[33], ab);
        float pp = fx + fy;
        if (pp >= 1.0) opY += Poly6(t1, t2, t3, t4, t5, t6, Phase(pp - 1.0)) * w.z;
        else opY += Poly6(t0, t1, t2, t3, t4, t5, Phase(pp)) * w.z;
    }
    if (w.w > 0.0)
    {
        float b135 = 0.5 * (fx + fy);
        float t1 = mix(p[19], p[26], b135), t3 = mix(p[14], p[21], b135), t5 = mix(p[9], p[16], b135);
        b135 -= 0.5;
        bool pos = b135 >= 0.0;
        float ab = abs(b135);
        float t0 = mix(p[25], pos ? p[32] : p[18], ab), t2 = mix(p[20], pos ? p[27] : p[13], ab);
        float t4 = mix(p[15], pos ? p[22] : p[8], ab), t6 = mix(p[10], pos ? p[17] : p[3], ab);
        float pp = 1.0 + (fx - fy);
        if (pp >= 1.0) opY += Poly6(t1, t2, t3, t4, t5, t6, Phase(pp - 1.0)) * w.w;
        else opY += Poly6(t0, t1, t2, t3, t4, t5, Phase(pp)) * w.w;
    }
    vec3 op = mix(mix(cf, cg, fx), mix(cj, ck, fx), fy);
    op = clamp(op + (opY - Y(op)), 0.0, 1.0);
    gl_FragColor = vec4(op * k, 1.0);
})";
    }

    bool ready = false, failed = false;
    GLuint easu = 0, rcas = 0, nis = 0, inTex = 0, midTex = 0, fbo = 0;
    int inW = 0, inH = 0, midW = 0, midH = 0;
};
#endif
