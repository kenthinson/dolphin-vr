// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#ifdef _WIN32
#include "VideoBackends/OGL/GLInterface/WGL.h"
#else
#include "VideoBackends/OGL/GLInterface/GLX.h"
#endif

#include "Common/CommonFuncs.h"
#include "Core/HW/Memmap.h"

#include "VideoBackends/OGL/FramebufferManager.h"
#include "VideoBackends/OGL/Render.h"
#include "VideoBackends/OGL/TextureConverter.h"
#include "VideoBackends/OGL/VROGL.h"

#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VR.h"

namespace OGL
{

int FramebufferManager::m_targetWidth;
int FramebufferManager::m_targetHeight;
int FramebufferManager::m_msaaSamples;

GLenum FramebufferManager::m_textureType;
GLuint* FramebufferManager::m_efbFramebuffer;
GLuint FramebufferManager::m_xfbFramebuffer;
GLuint FramebufferManager::m_efbColor;
GLuint FramebufferManager::m_efbDepth;
GLuint FramebufferManager::m_efbColorSwap; // for hot swap when reinterpreting EFB pixel formats

// Only used in MSAA mode.
GLuint* FramebufferManager::m_resolvedFramebuffer;
GLuint FramebufferManager::m_resolvedColorTexture;
GLuint FramebufferManager::m_resolvedDepthTexture;

// reinterpret pixel format
SHADER FramebufferManager::m_pixel_format_shaders[2];

// EFB pokes
GLuint FramebufferManager::m_EfbColorPokes_VBO;
GLuint FramebufferManager::m_EfbColorPokes_VAO;
SHADER FramebufferManager::m_EfbColorPokes;

bool FramebufferManager::m_stereo3d = false;
int FramebufferManager::m_eye_count = 1;

FramebufferManager::FramebufferManager(int targetWidth, int targetHeight, int msaaSamples)
{
	m_xfbFramebuffer = 0;
	m_efbColor = 0;
	m_efbDepth = 0;
	m_efbColorSwap = 0;
	m_resolvedColorTexture = 0;
	m_resolvedDepthTexture = 0;

	if (g_has_hmd || g_ActiveConfig.iStereoMode > 0)
	{
		m_stereo3d = true;
		m_eye_count = 2;
	}
	else
	{
		m_stereo3d = false;
		m_eye_count = 1;
	}

	m_targetWidth = targetWidth;
	m_targetHeight = targetHeight;

	m_msaaSamples = msaaSamples;

	if (g_has_hmd)
		VR_ConfigureHMD();

	// The EFB can be set to different pixel formats by the game through the
	// BPMEM_ZCOMPARE register (which should probably have a different name).
	// They are:
	// - 24-bit RGB (8-bit components) with 24-bit Z
	// - 24-bit RGBA (6-bit components) with 24-bit Z
	// - Multisampled 16-bit RGB (5-6-5 format) with 16-bit Z
	// We only use one EFB format here: 32-bit ARGB with 24-bit Z.
	// Multisampling depends on user settings.
	// The distinction becomes important for certain operations, i.e. the
	// alpha channel should be ignored if the EFB does not have one.

	glActiveTexture(GL_TEXTURE0 + 9);

	GLuint glObj[3];
	glGenTextures(3, glObj);
	m_efbColor = glObj[0];
	m_efbDepth = glObj[1];
	m_efbColorSwap = glObj[2];

	m_EFBLayers = (g_has_hmd || g_ActiveConfig.iStereoMode > 0) ? 2 : 1;
	m_efbFramebuffer = new GLuint[m_EFBLayers]();
	m_resolvedFramebuffer = new GLuint[m_EFBLayers]();

	// OpenGL MSAA textures are a different kind of texture type and must be allocated
	// with a different function, so we create them separately.
	if (m_msaaSamples <= 1)
	{
		m_textureType = GL_TEXTURE_2D_ARRAY;

		glBindTexture(m_textureType, m_efbColor);
		glTexParameteri(m_textureType, GL_TEXTURE_MAX_LEVEL, 0);
		glTexParameteri(m_textureType, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(m_textureType, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage3D(m_textureType, 0, GL_RGBA, m_targetWidth, m_targetHeight, m_EFBLayers, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

		glBindTexture(m_textureType, m_efbDepth);
		glTexParameteri(m_textureType, GL_TEXTURE_MAX_LEVEL, 0);
		glTexParameteri(m_textureType, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(m_textureType, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage3D(m_textureType, 0, GL_DEPTH_COMPONENT24, m_targetWidth, m_targetHeight, m_EFBLayers, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);

		glBindTexture(m_textureType, m_efbColorSwap);
		glTexParameteri(m_textureType, GL_TEXTURE_MAX_LEVEL, 0);
		glTexParameteri(m_textureType, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(m_textureType, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage3D(m_textureType, 0, GL_RGBA, m_targetWidth, m_targetHeight, m_EFBLayers, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	}
	else
	{
		GLenum resolvedType = GL_TEXTURE_2D_ARRAY;

		// Only use a layered multisample texture if needed. Some drivers
		// slow down significantly with single-layered multisample textures.
		if (m_EFBLayers > 1)
		{
			m_textureType = GL_TEXTURE_2D_MULTISAMPLE_ARRAY;

			glBindTexture(m_textureType, m_efbColor);
			glTexImage3DMultisample(m_textureType, m_msaaSamples, GL_RGBA, m_targetWidth, m_targetHeight, m_EFBLayers, false);

			glBindTexture(m_textureType, m_efbDepth);
			glTexImage3DMultisample(m_textureType, m_msaaSamples, GL_DEPTH_COMPONENT24, m_targetWidth, m_targetHeight, m_EFBLayers, false);

			glBindTexture(m_textureType, m_efbColorSwap);
			glTexImage3DMultisample(m_textureType, m_msaaSamples, GL_RGBA, m_targetWidth, m_targetHeight, m_EFBLayers, false);
			glBindTexture(m_textureType, 0);
		}
		else
		{
			m_textureType = GL_TEXTURE_2D_MULTISAMPLE;

			glBindTexture(m_textureType, m_efbColor);
			glTexImage2DMultisample(m_textureType, m_msaaSamples, GL_RGBA, m_targetWidth, m_targetHeight, false);

			glBindTexture(m_textureType, m_efbDepth);
			glTexImage2DMultisample(m_textureType, m_msaaSamples, GL_DEPTH_COMPONENT24, m_targetWidth, m_targetHeight, false);

			glBindTexture(m_textureType, m_efbColorSwap);
			glTexImage2DMultisample(m_textureType, m_msaaSamples, GL_RGBA, m_targetWidth, m_targetHeight, false);
			glBindTexture(m_textureType, 0);
		}

		// Although we are able to access the multisampled texture directly, we don't do it everywhere.
		// The old way is to "resolve" this multisampled texture by copying it into a non-sampled texture.
		// This would lead to an unneeded copy of the EFB, so we are going to avoid it.
		// But as this job isn't done right now, we do need that texture for resolving:
		glGenTextures(2, glObj);
		m_resolvedColorTexture = glObj[0];
		m_resolvedDepthTexture = glObj[1];

		glBindTexture(resolvedType, m_resolvedColorTexture);
		glTexParameteri(resolvedType, GL_TEXTURE_MAX_LEVEL, 0);
		glTexParameteri(resolvedType, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(resolvedType, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage3D(resolvedType, 0, GL_RGBA, m_targetWidth, m_targetHeight, m_EFBLayers, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

		glBindTexture(resolvedType, m_resolvedDepthTexture);
		glTexParameteri(resolvedType, GL_TEXTURE_MAX_LEVEL, 0);
		glTexParameteri(resolvedType, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(resolvedType, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage3D(resolvedType, 0, GL_DEPTH_COMPONENT24, m_targetWidth, m_targetHeight, m_EFBLayers, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);

		// Bind resolved textures to resolved framebuffer.
		glGenFramebuffers(m_EFBLayers, m_resolvedFramebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, m_resolvedFramebuffer[0]);
		FramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, resolvedType, m_resolvedColorTexture, 0);
		FramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, resolvedType, m_resolvedDepthTexture, 0);

		// Bind all the other layers as separate FBOs for blitting.
		for (unsigned int i = 1; i < m_EFBLayers; i++)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, m_resolvedFramebuffer[i]);
			glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_resolvedColorTexture, 0, i);
			glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_resolvedDepthTexture, 0, i);
		}
	}

	// Create XFB framebuffer; targets will be created elsewhere.
	glGenFramebuffers(1, &m_xfbFramebuffer);

	// Bind target textures to EFB framebuffer.
	glGenFramebuffers(m_EFBLayers, m_efbFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_efbFramebuffer[0]);
	FramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_textureType, m_efbColor, 0);
	FramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_textureType, m_efbDepth, 0);

	VR_StartFramebuffer(m_targetWidth, m_targetHeight);

	// Bind all the other layers as separate FBOs for blitting.
	for (unsigned int i = 1; i < m_EFBLayers; i++)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_efbFramebuffer[i]);
		glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_efbColor, 0, i);
		glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_efbDepth, 0, i);
	}

	// EFB framebuffer is currently bound, make sure to clear its alpha value to 1.f
	glViewport(0, 0, m_targetWidth, m_targetHeight);
	glScissor(0, 0, m_targetWidth, m_targetHeight);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClearDepthf(1.0f);
	glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

	// reinterpret pixel format
	const char* vs = m_EFBLayers > 1 ?
		"void main(void) {\n"
		"	vec2 rawpos = vec2(gl_VertexID&1, gl_VertexID&2);\n"
		"	gl_Position = vec4(rawpos*2.0-1.0, 0.0, 1.0);\n"
		"}\n" :
		"flat out int layer;\n"
		"void main(void) {\n"
		"	layer = 0;\n"
		"	vec2 rawpos = vec2(gl_VertexID&1, gl_VertexID&2);\n"
		"	gl_Position = vec4(rawpos*2.0-1.0, 0.0, 1.0);\n"
		"}\n";

	// The way to sample the EFB is based on the on the current configuration.
	// As we use the same sampling way for both interpreting shaders, the sampling
	// shader are generated first:
	std::string sampler;
	if (m_msaaSamples <= 1)
	{
		// non-msaa, so just fetch the pixel
		sampler =
			"SAMPLER_BINDING(9) uniform sampler2DArray samp9;\n"
			"vec4 sampleEFB(ivec3 pos) {\n"
			"	return texelFetch(samp9, pos, 0);\n"
			"}\n";
	}
	else if (g_ogl_config.bSupportSampleShading)
	{
		// msaa + sample shading available, so just fetch the sample
		// This will lead to sample shading, but it's the only way to not loose
		// the values of each sample.
		if (m_EFBLayers > 1)
		{
			sampler =
				"SAMPLER_BINDING(9) uniform sampler2DMSArray samp9;\n"
				"vec4 sampleEFB(ivec3 pos) {\n"
				"	return texelFetch(samp9, pos, gl_SampleID);\n"
				"}\n";
		}
		else
		{
			sampler =
				"SAMPLER_BINDING(9) uniform sampler2DMS samp9;\n"
				"vec4 sampleEFB(ivec3 pos) {\n"
				"	return texelFetch(samp9, pos.xy, gl_SampleID);\n"
				"}\n";
		}
	}
	else
	{
		// msaa without sample shading: calculate the mean value of the pixel
		std::stringstream samples;
		samples << m_msaaSamples;
		if (m_EFBLayers > 1)
		{
			sampler =
				"SAMPLER_BINDING(9) uniform sampler2DMSArray samp9;\n"
				"vec4 sampleEFB(ivec3 pos) {\n"
				"	vec4 color = vec4(0.0, 0.0, 0.0, 0.0);\n"
				"	for(int i=0; i<" + samples.str() + "; i++)\n"
				"		color += texelFetch(samp9, pos, 0), i);\n"
				"	return color / " + samples.str() + ";\n"
				"}\n";
		}
		else
		{
			sampler =
				"SAMPLER_BINDING(9) uniform sampler2DMS samp9;\n"
				"vec4 sampleEFB(ivec3 pos) {\n"
				"	vec4 color = vec4(0.0, 0.0, 0.0, 0.0);\n"
				"	for(int i=0; i<" + samples.str() + "; i++)\n"
				"		color += texelFetch(samp9, pos.xy, i);\n"
				"	return color / " + samples.str() + ";\n"
				"}\n";
		}
	}

	std::string ps_rgba6_to_rgb8 = sampler +
		"flat in int layer;\n"
		"out vec4 ocol0;\n"
		"void main()\n"
		"{\n"
		"	ivec4 src6 = ivec4(round(sampleEFB(ivec3(gl_FragCoord.xy, layer)) * 63.f));\n"
		"	ivec4 dst8;\n"
		"	dst8.r = (src6.r << 2) | (src6.g >> 4);\n"
		"	dst8.g = ((src6.g & 0xF) << 4) | (src6.b >> 2);\n"
		"	dst8.b = ((src6.b & 0x3) << 6) | src6.a;\n"
		"	dst8.a = 255;\n"
		"	ocol0 = float4(dst8) / 255.f;\n"
		"}";

	std::string ps_rgb8_to_rgba6 = sampler +
		"flat in int layer;\n"
		"out vec4 ocol0;\n"
		"void main()\n"
		"{\n"
		"	ivec4 src8 = ivec4(round(sampleEFB(ivec3(gl_FragCoord.xy, layer)) * 255.f));\n"
		"	ivec4 dst6;\n"
		"	dst6.r = src8.r >> 2;\n"
		"	dst6.g = ((src8.r & 0x3) << 4) | (src8.g >> 4);\n"
		"	dst6.b = ((src8.g & 0xF) << 2) | (src8.b >> 6);\n"
		"	dst6.a = src8.b & 0x3F;\n"
		"	ocol0 = float4(dst6) / 63.f;\n"
		"}";

	std::stringstream vertices, layers;
	vertices << m_EFBLayers * 3;
	layers << m_EFBLayers;
	std::string gs =
		"layout(triangles) in;\n"
		"layout(triangle_strip, max_vertices = " + vertices.str() + ") out;\n"
		"flat out int layer;\n"
		"void main()\n"
		"{\n"
		"	for (int j = 0; j < " + layers.str() + "; ++j) {\n"
		"		for (int i = 0; i < 3; ++i) {\n"
		"			layer = j;\n"
		"			gl_Layer = j;\n"
		"			gl_Position = gl_in[i].gl_Position;\n"
		"			EmitVertex();\n"
		"		}\n"
		"		EndPrimitive();\n"
		"	}\n"
		"}\n";

	ProgramShaderCache::CompileShader(m_pixel_format_shaders[0], vs, ps_rgb8_to_rgba6.c_str(), (m_EFBLayers > 1) ? gs.c_str() : nullptr);
	ProgramShaderCache::CompileShader(m_pixel_format_shaders[1], vs, ps_rgba6_to_rgb8.c_str(), (m_EFBLayers > 1) ? gs.c_str() : nullptr);

	ProgramShaderCache::CompileShader(m_EfbColorPokes,
		StringFromFormat(
		"in vec2 rawpos;\n"
		"in vec4 color0;\n"
		"out vec4 v_c;\n"
		"void main(void) {\n"
		"	gl_Position = vec4(((rawpos + 0.5) / vec2(640.0, 528.0) * 2.0 - 1.0) * vec2(1.0, -1.0), 0.0, 1.0);\n"
		"	gl_PointSize = %d.0 / 640.0;\n"
		"	v_c = color0;\n"
		"}\n", m_targetWidth).c_str(),

		StringFromFormat(
		"in vec4 %s_c;\n"
		"out vec4 ocol0;\n"
		"void main(void) {\n"
		"	ocol0 = %s_c.bgra;\n"
		"}\n", m_EFBLayers > 1 ? "g" : "v", m_EFBLayers > 1 ? "g" : "v").c_str(),

		m_EFBLayers > 1 ? StringFromFormat(
		"layout(points) in;\n"
		"layout(points, max_vertices = %d) out;\n"
		"in vec4 v_c[1];\n"
		"out vec4 g_c;\n"
		"void main()\n"
		"{\n"
		"	for (int j = 0; j < %d; ++j) {\n"
		"		gl_Layer = j;\n"
		"		gl_Position = gl_in[0].gl_Position;\n"
		"		gl_PointSize = %d.0 / 640.0;\n"
		"		g_c = v_c[0];\n"
		"		EmitVertex();\n"
		"		EndPrimitive();\n"
		"	}\n"
		"}\n", m_EFBLayers, m_EFBLayers, m_targetWidth).c_str() : nullptr);
	glGenBuffers(1, &m_EfbColorPokes_VBO);
	glGenVertexArrays(1, &m_EfbColorPokes_VAO);
	glBindBuffer(GL_ARRAY_BUFFER, m_EfbColorPokes_VBO);
	glBindVertexArray(m_EfbColorPokes_VAO );
	glEnableVertexAttribArray(SHADER_POSITION_ATTRIB);
	glVertexAttribPointer(SHADER_POSITION_ATTRIB, 2, GL_UNSIGNED_SHORT, 0, sizeof(EfbPokeData), (void*)offsetof(EfbPokeData, x));
	glEnableVertexAttribArray(SHADER_COLOR0_ATTRIB);
	glVertexAttribPointer(SHADER_COLOR0_ATTRIB, 4, GL_UNSIGNED_BYTE, 1, sizeof(EfbPokeData), (void*)offsetof(EfbPokeData, data));

	glEnable(GL_PROGRAM_POINT_SIZE);
}

FramebufferManager::~FramebufferManager()
{
	VR_StopRendering();
	VR_StopFramebuffer();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	GLuint glObj[3];

	// Note: OpenGL deletion functions silently ignore parameters of "0".

	glDeleteFramebuffers(m_EFBLayers, m_efbFramebuffer);
	glDeleteFramebuffers(m_EFBLayers, m_resolvedFramebuffer);
	delete [] m_efbFramebuffer;
	delete [] m_resolvedFramebuffer;
	m_efbFramebuffer = nullptr;
	m_resolvedFramebuffer = nullptr;

	glDeleteFramebuffers(1, &m_xfbFramebuffer);
	m_xfbFramebuffer = 0;

	glObj[0] = m_resolvedColorTexture;
	glObj[1] = m_resolvedDepthTexture;
	glDeleteTextures(2, glObj);
	m_resolvedColorTexture = 0;
	m_resolvedDepthTexture = 0;

	glObj[0] = m_efbColor;
	glObj[1] = m_efbDepth;
	glObj[2] = m_efbColorSwap;
	glDeleteTextures(3, glObj);
	m_efbColor = 0;
	m_efbDepth = 0;
	m_efbColorSwap = 0;

	// reinterpret pixel format
	m_pixel_format_shaders[0].Destroy();
	m_pixel_format_shaders[1].Destroy();

	// EFB pokes
	glDeleteBuffers(1, &m_EfbColorPokes_VBO);
	glDeleteVertexArrays(1, &m_EfbColorPokes_VAO);
	m_EfbColorPokes_VBO = 0;
	m_EfbColorPokes_VAO = 0;
	m_EfbColorPokes.Destroy();
}

GLuint FramebufferManager::GetEFBColorTexture(const EFBRectangle& sourceRc)
{
	if (m_msaaSamples <= 1)
	{
		return m_efbColor;
	}
	else
	{
		// Transfer the EFB to a resolved texture. EXT_framebuffer_blit is
		// required.

		TargetRectangle targetRc = g_renderer->ConvertEFBRectangle(sourceRc);
		targetRc.ClampUL(0, 0, m_targetWidth, m_targetHeight);

		// Resolve.
		for (unsigned int i = 0; i < m_EFBLayers; i++)
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, m_efbFramebuffer[i]);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolvedFramebuffer[i]);
			glBlitFramebuffer(
				targetRc.left, targetRc.top, targetRc.right, targetRc.bottom,
				targetRc.left, targetRc.top, targetRc.right, targetRc.bottom,
				GL_COLOR_BUFFER_BIT, GL_NEAREST
				);
		}

		// Return to EFB.
		glBindFramebuffer(GL_FRAMEBUFFER, m_efbFramebuffer[0]);

		return m_resolvedColorTexture;
	}
}

GLuint FramebufferManager::GetEFBDepthTexture(const EFBRectangle& sourceRc)
{
	if (m_msaaSamples <= 1)
	{
		return m_efbDepth;
	}
	else
	{
		// Transfer the EFB to a resolved texture.

		TargetRectangle targetRc = g_renderer->ConvertEFBRectangle(sourceRc);
		targetRc.ClampUL(0, 0, m_targetWidth, m_targetHeight);

		// Resolve.
		for (unsigned int i = 0; i < m_EFBLayers; i++)
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, m_efbFramebuffer[i]);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolvedFramebuffer[i]);
			glBlitFramebuffer(
				targetRc.left, targetRc.top, targetRc.right, targetRc.bottom,
				targetRc.left, targetRc.top, targetRc.right, targetRc.bottom,
				GL_DEPTH_BUFFER_BIT, GL_NEAREST
				);
		}

		// Return to EFB.
		glBindFramebuffer(GL_FRAMEBUFFER, m_efbFramebuffer[0]);

		return m_resolvedDepthTexture;
	}
}

void FramebufferManager::CopyToRealXFB(u32 xfbAddr, u32 fbWidth, u32 fbHeight, const EFBRectangle& sourceRc,float Gamma)
{
	u8* xfb_in_ram = Memory::GetPointer(xfbAddr);
	if (!xfb_in_ram)
	{
		WARN_LOG(VIDEO, "Tried to copy to invalid XFB address");
		return;
	}

	TargetRectangle targetRc = g_renderer->ConvertEFBRectangle(sourceRc);
	TextureConverter::EncodeToRamYUYV(ResolveAndGetRenderTarget(sourceRc), targetRc, xfb_in_ram, fbWidth, fbHeight);
}

void FramebufferManager::SetFramebuffer(GLuint fb)
{
	glBindFramebuffer(GL_FRAMEBUFFER, fb != 0 ? fb : GetEFBFramebuffer());
}

void FramebufferManager::FramebufferTexture(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
	if (textarget == GL_TEXTURE_2D_ARRAY || textarget == GL_TEXTURE_2D_MULTISAMPLE_ARRAY)
	{
		if (m_EFBLayers > 1)
			glFramebufferTexture(target, attachment, texture, level);
		else
			glFramebufferTextureLayer(target, attachment, texture, level, 0);
	}
	else
	{
		glFramebufferTexture2D(target, attachment, textarget, texture, level);
	}
}

void FramebufferManager::SwapAsyncFrontBuffers()
{
	if (m_msaaSamples <= 1)
	{
		// TODO!!!!!!!
	}
	else
	{
		// TODO!!!!!!!
	}
}


// Apply AA if enabled
GLuint FramebufferManager::ResolveAndGetRenderTarget(const EFBRectangle &source_rect)
{
	return GetEFBColorTexture(source_rect);
}

GLuint FramebufferManager::ResolveAndGetDepthTarget(const EFBRectangle &source_rect)
{
	return GetEFBDepthTexture(source_rect);
}

void FramebufferManager::ReinterpretPixelData(unsigned int convtype)
{
	g_renderer->ResetAPIState();

	OpenGL_BindAttributelessVAO();

	GLuint src_texture = 0;

	// We aren't allowed to render and sample the same texture in one draw call,
	// so we have to create a new texture and overwrite it completely.
	// To not allocate one big texture every time, we've allocated two on
	// initialization and just swap them here:
	src_texture = m_efbColor;
	m_efbColor = m_efbColorSwap;
	m_efbColorSwap = src_texture;
	FramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_textureType, m_efbColor, 0);

	glViewport(0,0, m_targetWidth, m_targetHeight);
	glActiveTexture(GL_TEXTURE0 + 9);
	glBindTexture(m_textureType, src_texture);

	m_pixel_format_shaders[convtype ? 1 : 0].Bind();
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindTexture(m_textureType, 0);

	g_renderer->RestoreAPIState();
}

XFBSource::~XFBSource()
{
	glDeleteTextures(1, &texture);
}

void XFBSource::DecodeToTexture(u32 xfbAddr, u32 fbWidth, u32 fbHeight)
{
	TextureConverter::DecodeToTexture(xfbAddr, fbWidth, fbHeight, texture);
}

void XFBSource::CopyEFB(float Gamma)
{
	g_renderer->ResetAPIState();

	// Copy EFB data to XFB and restore render target again
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, FramebufferManager::GetXFBFramebuffer());

	for (int i = 0; i < m_layers; i++)
	{
		// Bind EFB and texture layer
		glBindFramebuffer(GL_READ_FRAMEBUFFER, FramebufferManager::GetEFBFramebuffer(i));
		glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, i);

		glBlitFramebuffer(
			0, 0, texWidth, texHeight,
			0, 0, texWidth, texHeight,
			GL_COLOR_BUFFER_BIT, GL_NEAREST
		);
	}

	// Return to EFB.
	FramebufferManager::SetFramebuffer(0);

	g_renderer->RestoreAPIState();

}

XFBSourceBase* FramebufferManager::CreateXFBSource(unsigned int target_width, unsigned int target_height, unsigned int layers)
{
	GLuint texture;

	glGenTextures(1, &texture);

	glActiveTexture(GL_TEXTURE0 + 9);
	glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, target_width, target_height, layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

	return new XFBSource(texture, layers);
}

void FramebufferManager::GetTargetSize(unsigned int *width, unsigned int *height)
{
	*width = m_targetWidth;
	*height = m_targetHeight;
}

void FramebufferManager::PokeEFB(EFBAccessType type, const std::vector<EfbPokeData>& data)
{
	switch (type)
	{
	case POKE_COLOR:
	{
		g_renderer->ResetAPIState();

		glBindVertexArray(m_EfbColorPokes_VAO);
		glBindBuffer(GL_ARRAY_BUFFER, m_EfbColorPokes_VBO);
		glBufferData(GL_ARRAY_BUFFER, sizeof(EfbPokeData) * data.size(), data.data(), GL_STREAM_DRAW);
		m_EfbColorPokes.Bind();
		glViewport(0, 0, m_targetWidth, m_targetHeight);
		glDrawArrays(GL_POINTS, 0, (GLsizei)data.size());

		g_renderer->RestoreAPIState();

		// TODO: Could just update the EFB cache with the new value
		ClearEFBCache();
		break;
	}

	default:
		break;
	}
}

}  // namespace OGL
