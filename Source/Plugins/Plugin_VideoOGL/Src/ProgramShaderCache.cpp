// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "ProgramShaderCache.h"
#include "MathUtil.h"
#include "StreamBuffer.h"

namespace OGL
{

static const u32 UBO_LENGTH = 4*1024*1024;

GLuint ProgramShaderCache::CurrentProgram = 0;
ProgramShaderCache::PCache ProgramShaderCache::pshaders;
GLintptr ProgramShaderCache::s_vs_data_offset;
u8 *ProgramShaderCache::s_ubo_buffer;
u32 ProgramShaderCache::s_ubo_buffer_size;
bool ProgramShaderCache::s_ubo_dirty;

LinearDiskCache<u64, u8> g_program_disk_cache;
GLenum ProgramFormat;

GLuint ProgramShaderCache::PCacheEntry::prog_format = 0;

u64 ProgramShaderCache::CurrentShaderProgram;

static StreamBuffer *s_buffer;

u64 Create_Pair(u32 key1, u32 key2)
{
	return (((u64)key1) << 32) | key2;
}
void Get_Pair(u64 key, u32 *key1, u32 *key2)
{
	*key1 = (key & 0xFFFFFFFF00000000) >> 32;
	*key2 = key & 0xFFFFFFFF;
}

const char *UniformNames[NUM_UNIFORMS] =
{
	// PIXEL SHADER UNIFORMS
	I_COLORS,
	I_KCOLORS,
	I_ALPHA,
	I_TEXDIMS,
	I_ZBIAS ,
	I_INDTEXSCALE ,
	I_INDTEXMTX,
	I_FOG,
	I_PLIGHTS,
	I_PMATERIALS,
	// VERTEX SHADER UNIFORMS
	I_POSNORMALMATRIX,
	I_PROJECTION ,
	I_MATERIALS,
	I_LIGHTS,
	I_TEXMATRICES,
	I_TRANSFORMMATRICES ,
	I_NORMALMATRICES ,
	I_POSTTRANSFORMMATRICES,
	I_DEPTHPARAMS,
};

void ProgramShaderCache::SetProgramVariables(PCacheEntry &entry)
{
	// Bind UBO 
	if (g_ActiveConfig.backend_info.bSupportsGLSLUBO)
	{
		GLint PSBlock_id = glGetUniformBlockIndex(entry.prog_id, "PSBlock");
		GLint VSBlock_id = glGetUniformBlockIndex(entry.prog_id, "VSBlock");
		
		if(PSBlock_id != -1)
			glUniformBlockBinding(entry.prog_id, PSBlock_id, 1);
		if(VSBlock_id != -1)
			glUniformBlockBinding(entry.prog_id, VSBlock_id, 2);
	}
	
	// We cache our uniform locations for now
	// Once we move up to a newer version of GLSL, ~1.30
	// We can remove this

	// (Sonicadvance): For some reason this fails on my hardware
	//glGetUniformIndices(entry.prog_id, NUM_UNIFORMS, UniformNames, entry.UniformLocations);
	// Got to do it this crappy way.
	entry.UniformLocations[0] = glGetUniformLocation(entry.prog_id, UniformNames[0]);
	if (!g_ActiveConfig.backend_info.bSupportsGLSLUBO)
		for (int a = 1; a < NUM_UNIFORMS; ++a)
			entry.UniformLocations[a] = glGetUniformLocation(entry.prog_id, UniformNames[a]);

	// Bind Texture Sampler
	for (int a = 0; a <= 9; ++a)
	{
		char name[8];
		snprintf(name, 8, "samp%d", a);
		
		// Still need to get sampler locations since we aren't binding them statically in the shaders
		int loc = glGetUniformLocation(entry.prog_id, name);
		if (loc != -1)
			glUniform1i(loc, a);
	}

}

void ProgramShaderCache::SetProgramBindings ( ProgramShaderCache::PCacheEntry& entry )
{
	if (g_ActiveConfig.backend_info.bSupportsDualSourceBlend)
	{
		// So we do support extended blending
		// So we need to set a few more things here.
		// Bind our out locations
		glBindFragDataLocationIndexed(entry.prog_id, 0, 0, "ocol0");
		glBindFragDataLocationIndexed(entry.prog_id, 0, 1, "ocol1");
	}
	else
		glBindFragDataLocation(entry.prog_id, 0, "ocol0");

	// Need to set some attribute locations
	glBindAttribLocation(entry.prog_id, SHADER_POSITION_ATTRIB, "rawpos");
	
	glBindAttribLocation(entry.prog_id, SHADER_POSMTX_ATTRIB,   "fposmtx");
	
	glBindAttribLocation(entry.prog_id, SHADER_COLOR0_ATTRIB,   "color0");
	glBindAttribLocation(entry.prog_id, SHADER_COLOR1_ATTRIB,   "color1");
	
	glBindAttribLocation(entry.prog_id, SHADER_NORM0_ATTRIB,    "rawnorm0");
	glBindAttribLocation(entry.prog_id, SHADER_NORM1_ATTRIB,    "rawnorm1");
	glBindAttribLocation(entry.prog_id, SHADER_NORM2_ATTRIB,    "rawnorm2");
	
	for(int i=0; i<8; i++) {
		char attrib_name[8];
		snprintf(attrib_name, 8, "tex%d", i);
		glBindAttribLocation(entry.prog_id, SHADER_TEXTURE0_ATTRIB+i, attrib_name);
	}
}


void ProgramShaderCache::SetBothShaders(GLuint PS, GLuint VS)
{
	if(!PS || !VS) {
		ERROR_LOG(VIDEO, "tried to bind a zero shader");
		return;
	}
	
	u64 ShaderPair = Create_Pair(PS, VS);
	
	// program is already bound
	if(ShaderPair == CurrentShaderProgram) return;
	
	PCache::iterator iter = pshaders.find(ShaderPair);
	if (iter != pshaders.end())
	{
		PCacheEntry &entry = iter->second;
		glUseProgram(entry.prog_id);
		CurrentProgram = entry.prog_id;
		CurrentShaderProgram = ShaderPair;
		return;
	}

	PCacheEntry entry;
	entry.Create(PS, VS);

	// Right, the program is created now
	// Let's attach everything
	glAttachShader(entry.prog_id, entry.vsid);
	glAttachShader(entry.prog_id, entry.psid);

	if (g_ActiveConfig.backend_info.bSupportsGLSLCache)
		glProgramParameteri(entry.prog_id, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);

	SetProgramBindings(entry);
	
	glLinkProgram(entry.prog_id);
	
#if defined(_DEBUG) || defined(DEBUGFAST) || defined(DEBUG_GLSL)
	GLsizei length = 0;
	glGetProgramiv(entry.prog_id, GL_INFO_LOG_LENGTH, &length);
	if (length > 1)
	{
		GLsizei charsWritten;
		GLchar* infoLog = new GLchar[length];
		glGetProgramInfoLog(entry.prog_id, length, &charsWritten, infoLog);
		ERROR_LOG(VIDEO, "Program info log:\n%s", infoLog);
		char szTemp[MAX_PATH];
		sprintf(szTemp, "p_%d.txt", entry.prog_id);
		FILE *fp = fopen(szTemp, "wb");
		fwrite(infoLog, length, 1, fp);
		delete[] infoLog;
		
		glGetShaderiv(VS, GL_SHADER_SOURCE_LENGTH, &length);
		GLchar* src = new GLchar[length];
		glGetShaderSource(VS, length, &charsWritten, src);
		fwrite(src, strlen(src), 1, fp);
		delete [] src;
		
		glGetShaderiv(PS, GL_SHADER_SOURCE_LENGTH, &length);
		src = new GLchar[length];
		glGetShaderSource(PS, length, &charsWritten, src);
		fwrite(src, strlen(src), 1, fp);
		delete [] src;
		fclose(fp);
	}

	GLint linkStatus;
	glGetProgramiv(entry.prog_id, GL_LINK_STATUS, &linkStatus);
	if (linkStatus != GL_TRUE)
	{
		// Compile failed
		ERROR_LOG(VIDEO, "Program linking failed; see info log");

		// Don't try to use this shader
		glDeleteProgram(entry.prog_id);
		return;
	}
#endif

	glUseProgram(entry.prog_id);

	SetProgramVariables(entry);

	pshaders[ShaderPair] = entry;
	CurrentShaderProgram = ShaderPair;
	CurrentProgram = entry.prog_id;
}

void ProgramShaderCache::SetMultiPSConstant4fv(unsigned int offset, const float *f, unsigned int count)
{
	s_ubo_dirty = true;
	memcpy(s_ubo_buffer+(offset*4*sizeof(float)), f, count*4*sizeof(float));
}

void ProgramShaderCache::SetMultiVSConstant4fv(unsigned int offset, const float *f, unsigned int count)
{
	s_ubo_dirty = true;
	memcpy(s_ubo_buffer+(offset*4*sizeof(float))+s_vs_data_offset, f, count*4*sizeof(float));
}

void ProgramShaderCache::UploadConstants()
{
	if(s_ubo_dirty) {
		s_buffer->Alloc(s_ubo_buffer_size);
		size_t offset = s_buffer->Upload(s_ubo_buffer, s_ubo_buffer_size);
		glBindBufferRange(GL_UNIFORM_BUFFER, 1, s_buffer->getBuffer(), offset, s_vs_data_offset);
		glBindBufferRange(GL_UNIFORM_BUFFER, 2, s_buffer->getBuffer(), offset + s_vs_data_offset, s_ubo_buffer_size - s_vs_data_offset);
		s_ubo_dirty = false;
	}
}

GLuint ProgramShaderCache::GetCurrentProgram(void)
{
	return CurrentProgram;
}

ProgramShaderCache::PCacheEntry ProgramShaderCache::GetShaderProgram(void)
{
	return pshaders[CurrentShaderProgram];
}

void ProgramShaderCache::Init(void)
{
	// We have to get the UBO alignment here because
	// if we generate a buffer that isn't aligned
	// then the UBO will fail.
	if (g_ActiveConfig.backend_info.bSupportsGLSLUBO)
	{
		GLint Align;
		glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &Align);

		GLintptr const ps_data_size = ROUND_UP(C_PENVCONST_END * sizeof(float) * 4, Align);
		GLintptr const vs_data_size = ROUND_UP(C_VENVCONST_END * sizeof(float) * 4, Align);
		s_vs_data_offset = ps_data_size;
		s_ubo_buffer_size = ps_data_size + vs_data_size;

		// We multiply by *4*4 because we need to get down to basic machine units.
		// So multiply by four to get how many floats we have from vec4s
		// Then once more to get bytes
		s_buffer = new StreamBuffer(GL_UNIFORM_BUFFER, UBO_LENGTH);
		
		s_ubo_buffer = new u8[s_ubo_buffer_size];
		memset(s_ubo_buffer, 0, s_ubo_buffer_size);
		s_ubo_dirty = true;
	}

	// Read our shader cache, only if supported
	if (g_ActiveConfig.backend_info.bSupportsGLSLCache)
	{
		PCacheEntry::prog_format = PCacheEntry::SetProgramFormat();

		char cache_filename[MAX_PATH];
		sprintf(cache_filename, "%sogl-%s-shaders.cache", File::GetUserPath(D_SHADERCACHE_IDX).c_str(),
			SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID.c_str());

		ProgramShaderCacheInserter inserter;
		g_program_disk_cache.OpenAndRead(cache_filename, inserter);
	}
	
	CurrentProgram = 0;
	CurrentShaderProgram = 0;
}

void ProgramShaderCache::Shutdown(void)
{
	if (g_ActiveConfig.backend_info.bSupportsGLSLCache)
	{
		PCache::iterator iter = pshaders.begin();
		for (; iter != pshaders.end(); ++iter)
		{
			g_program_disk_cache.Append(iter->second.uid, iter->second.GetProgram(), iter->second.Size());
			iter->second.FreeProgram();
		}

		g_program_disk_cache.Sync();
		g_program_disk_cache.Close();
	}

	glUseProgram(0);
	
	PCache::iterator iter = pshaders.begin();
	for (; iter != pshaders.end(); ++iter)
		iter->second.Destroy();
	pshaders.clear();

	if (g_ActiveConfig.backend_info.bSupportsGLSLUBO)
	{
		delete s_buffer;
		s_buffer = 0;
		delete [] s_ubo_buffer;
		s_ubo_buffer = 0;
	}
}

} // namespace OGL
