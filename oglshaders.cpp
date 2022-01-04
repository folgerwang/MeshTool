#include "oglshaders.h"
#include "debugout.h"
#include "oglwidget.h"

static string basic_vert_str =
{
#include "shaders/basic.vert"
};

static string basic_frag_str =
{
#include "shaders/basic.frag"
};

static string basic_nolight_vert_str =
{
#include "shaders/basicnolight.vert"
};

static string basic_nolight_frag_str =
{
#include "shaders/basicnolight.frag"
};

static string basiclight_vert_str =
{
#include "shaders/basiclight.vert"
};

static string basiclight_frag_str =
{
#include "shaders/basiclight.frag"
};

static string screenquad_vert_str =
{
#include "shaders/screenquad.vert"
};

static string screenquad_frag_str =
{
#include "shaders/screenquad.frag"
};

bool OGLWidget::initShaders()
{
    vector<string> headers;
    GLuint shaders[kNumShaders];
    shaders[kBasicVert] = loadShader(basic_vert_str, headers, kGlVertexShader);
    shaders[kBasicNoLightVert] = loadShader(basic_nolight_vert_str, headers, kGlVertexShader);
    shaders[kBasicLightVert] = loadShader(basiclight_vert_str, headers, kGlVertexShader);
    shaders[kScreenQuadVert] = loadShader(screenquad_vert_str, headers, kGlVertexShader);
    shaders[kBasicPixel] = loadShader(basic_frag_str, headers, kGlFragmentShader);
    shaders[kBasicNoLightPixel] = loadShader(basic_nolight_frag_str, headers, kGlFragmentShader);
    shaders[kBasicLightPixel] = loadShader(basiclight_frag_str, headers, kGlFragmentShader);
    shaders[kScreenQuadPixel] = loadShader(screenquad_frag_str, headers, kGlFragmentShader);

    m_programs[kBasicProgram] = createProgram(shaders[kBasicVert], shaders[kBasicPixel]);
    m_programs[kBasicNoLightProgram] = createProgram(shaders[kBasicNoLightVert], shaders[kBasicNoLightPixel]);
    m_programs[kBasicLightProgram] = createProgram(shaders[kBasicLightVert], shaders[kBasicLightPixel]);
    m_programs[kScreenQuadProgram] = createProgram(shaders[kScreenQuadVert], shaders[kScreenQuadPixel]);

    for (int i = 0; i < kNumShaders; i++)
    {
        glDeleteShader(shaders[i]);
    }
    return true;
}

bool OGLWidget::checkCompileError(GLuint shader, string shader_body, bool is_strict/* = false*/)
{
    // check if shader compiled
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

    if (!compiled || is_strict)
    {
        if (!compiled) {
            core::output_debug_info("Error: compiling shader", shader_body);
        }
        GLint infoLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen) {
            char* buf = new char[uint32_t(infoLen)];
            if (buf) {
                glGetShaderInfoLog(shader, infoLen, nullptr, buf);
                cout << "Shader Log:" << shader_body;
                delete[] buf;
            }
        }
        if (!compiled) {
            glDeleteShader(shader);
            shader = 0;
            return false;
        }
    }
    return true;
}

GLuint OGLWidget::loadShader(const string& shader_body, const vector<string>& headers, ShaderType shader_type)
{
    GLuint target = shader_type == kGlVertexShader ? GL_VERTEX_SHADER : (shader_type == kGlFragmentShader ? GL_FRAGMENT_SHADER : GL_COMPUTE_SHADER);
    GLuint shader = glCreateShader(target);
    const char* sourceItems[2];
    int sourceCount = 0;
    for (uint32_t i = 0; i < headers.size(); i++)
    {
        sourceItems[sourceCount++] = headers[i].c_str();
    }
    sourceItems[sourceCount++] = shader_body.c_str();

    glShaderSource(shader, sourceCount, sourceItems, nullptr);

    glCompileShader(shader);
    if (!checkCompileError(shader, shader_body))
    {
         return GLuint(-1);
    }

    return shader;
}

GLuint OGLWidget::createProgram(GLuint vertex_shader, GLuint pixel_shader)
{
    GLuint program = glCreateProgram();

    glAttachShader(program, vertex_shader);
    glAttachShader(program, pixel_shader);

    glLinkProgram(program);

    // check if program linked
    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if (!success)
    {
        GLint bufLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
        if (bufLength) {
            char* buf = new char[uint32_t(bufLength)];
            if (buf) {
                glGetProgramInfoLog(program, bufLength, nullptr, buf);
                core::output_debug_info("Error: could not link program", buf);
                delete [] buf;
            }
        }
        glDeleteProgram(program);
        program = 0;
    }

    glDetachShader(program, vertex_shader);
    glDetachShader(program, pixel_shader);

    return program;
}

GLuint OGLWidget::createProgram(GLuint compute_shader)
{
    GLuint program = glCreateProgram();

    glAttachShader(program, compute_shader);

    glLinkProgram(program);

    // check if program linked
    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if (!success)
    {
        GLint bufLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
        if (bufLength) {
            char* buf = new char[uint32_t(bufLength)];
            if (buf) {
                glGetProgramInfoLog(program, bufLength, nullptr, buf);
                core::output_debug_info("Error: could not link program", buf);
                delete[] buf;
            }
        }
        glDeleteProgram(program);
        program = 0;
    }

    glDetachShader(program, compute_shader);

    return program;
}

GLuint OGLWidget::getAttribLocation(GLuint program, string attrib)
{
    GLint attrib_idx = glGetAttribLocation(program, attrib.c_str());
    if (attrib_idx == -1)
    {
        //core::output_debug_info("Error: failed get attribute", attrib.c_str());
        return GLuint(-1);
    }

    return GLuint(attrib_idx);
}

GLuint OGLWidget::getUniformLocation(GLuint program, string uniform)
{
    GLint location = glGetUniformLocation(program, uniform.c_str());
    if (location == -1)
    {
        //core::output_debug_info("Error: failed get attribute", uniform.c_str());
        return GLuint(-1);
    }

    return GLuint(location);
}

void OGLWidget::bindTexture2D(GLuint program, string uniform, int32_t unit, GLuint tex)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (tex != GLuint(-1) && loc >= 0)
    {
        glUniform1i(loc, unit);
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    else
    {
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void OGLWidget::bindTexture2D(GLint index, int32_t unit, GLuint tex)
{
    if (tex != GLuint(-1))
    {
        glUniform1i(index, unit);
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    else
    {
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void OGLWidget::bindTexture2DMultisample(GLuint program, string uniform, int32_t unit, GLuint tex)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (tex != GLuint(-1) && loc >= 0)
    {
        glUniform1i(loc, unit);
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(0x9100/*GL_TEXTURE_2D_MULTISAMPLE*/, tex);
    }
    else
    {
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void OGLWidget::bindTexture2DMultisample(GLint index, int32_t unit, GLuint tex)
{
    if (tex != GLuint(-1))
    {
        glUniform1i(index, unit);
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(0x9100/*GL_TEXTURE_2D_MULTISAMPLE*/, tex);
    }
    else
    {
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void OGLWidget::bindTextureRect(GLuint program, string uniform, int32_t unit, GLuint tex)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (tex != GLuint(-1) && loc >= 0)
    {
        glUniform1i(loc, unit);
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(0x84F5/*GL_TEXTURE_RECT*/, tex);
    }
    else
    {
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void OGLWidget::bindTextureRect(GLint index, int32_t unit, GLuint tex)
{
    if (tex != GLuint(-1))
    {
        glUniform1i(index, unit);
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(0x84F5/*GL_TEXTURE_RECT*/, tex);
    }
    else
    {
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void OGLWidget::bindTextureArray(GLuint program, string uniform, int32_t unit, GLuint tex)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (tex != GLuint(-1) && loc >= 0)
    {
        glUniform1i(loc, unit);
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(0x8c1a, tex); // GL_TEXTURE_2D_ARRAY
    }
    else
    {
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void OGLWidget::bindTextureArray(GLint index, int32_t unit, GLuint tex)
{
    if (tex != GLuint(-1))
    {
        glUniform1i(index, unit);
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(0x8c1a, tex); // GL_TEXTURE_2D_ARRAY
    }
    else
    {
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void OGLWidget::setUniform1i(GLuint program, string uniform, int32_t value)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (loc >= 0) {
        glUniform1i(loc, value);
    }
}

void OGLWidget::setUniform1i(GLint index, int32_t value)
{
    if (index >= 0) {
        glUniform1i(index, value);
    }
}

void OGLWidget::setUniform2i(GLuint program, string uniform, int32_t x, int32_t y)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (loc >= 0) {
        glUniform2i(loc, x, y);
    }
}

void OGLWidget::setUniform2i(GLint index, int32_t x, int32_t y)
{
    if (index >= 0) {
        glUniform2i(index, x, y);
    }
}

void OGLWidget::setUniform3i(GLuint program, string uniform, int32_t x, int32_t y, int32_t z)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (loc >= 0) {
        glUniform3i(loc, x, y, z);
    }
}

void OGLWidget::setUniform3i(GLint index, int32_t x, int32_t y, int32_t z)
{
    if (index >= 0) {
        glUniform3i(index, x, y, z);
    }
}

void OGLWidget::setUniform1f(GLuint program, string uniform, float value)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (loc >= 0) {
        glUniform1f(loc, value);
    }
}

void OGLWidget::setUniform1f(GLint index, float value)
{
    if (index >= 0) {
        glUniform1f(index, value);
    }
}

void OGLWidget::setUniform2f(GLuint program, string uniform, float x, float y)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (loc >= 0) {
        glUniform2f(loc, x, y);
    }
}

void OGLWidget::setUniform2f(GLint index, float x, float y)
{
    if (index >= 0) {
        glUniform2f(index, x, y);
    }
}

void OGLWidget::setUniform3f(GLuint program, string uniform, float x, float y, float z)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (loc >= 0) {
        glUniform3f(loc, x, y, z);
    }
}

void OGLWidget::setUniform3f(GLint index, float x, float y, float z)
{
    if (index >= 0) {
        glUniform3f(index, x, y, z);
    }
}

void OGLWidget::setUniform4f(GLuint program, string uniform, float x, float y, float z, float w)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (loc >= 0) {
        glUniform4f(loc, x, y, z, w);
    }
}

void OGLWidget::setUniform4f(GLint index, float x, float y, float z, float w)
{
    if (index >= 0) {
        glUniform4f(index, x, y, z, w);
    }
}

void OGLWidget::setUniform3fv(GLuint program, string uniform, const float *value, int32_t count)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (loc >= 0) {
        glUniform3fv(loc, count, value);
    }

}

void OGLWidget::setUniform3fv(GLint index, const float *value, int32_t count)
{
    if (index >= 0) {
        glUniform3fv(index, count, value);
    }

}

void OGLWidget::setUniform4fv(GLuint program, string uniform, const float *value, int32_t count)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (loc >= 0) {
        glUniform4fv(loc, count, value);
    }
}

void OGLWidget::setUniform4fv(GLint index, const float *value, int32_t count)
{
    if (index >= 0) {
        glUniform4fv(index, count, value);
    }
}

void OGLWidget::setUniformMatrix4fv(GLuint program, string uniform, const float *m, int32_t count, bool transpose)
{
    GLint loc = GLint(getUniformLocation(program, uniform));
    if (loc >= 0) {
        glUniformMatrix4fv(loc, count, transpose, m);
    }
}

void OGLWidget::setUniformMatrix4fv(GLint index, const float *m, int32_t count, bool transpose)
{
    if (index >= 0) {
        glUniformMatrix4fv(index, count, transpose, m);
    }
}
