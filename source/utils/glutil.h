/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glutil.h
 * @brief OpenGL API initializer, related functions.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#ifdef USE_PVR_PSP2
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#else
#include <vitaGL.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void gl_init();

void gl_preload();

void gl_swap();

const GLubyte *glGetString_soloader(GLenum name);

void glActiveTexture_soloader(GLenum texture);
void glBindTexture_soloader(GLenum target, GLuint texture);
void glCompileShader_soloader(GLuint shader);
void glLinkProgram_soloader(GLuint program);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

void glBindVertexArrayOES_soloader(GLuint array);
void glDeleteVertexArraysOES_soloader(GLsizei n, const GLuint *arrays);
void glGenVertexArraysOES_soloader(GLsizei n, GLuint *arrays);
void glDrawElementsInstancedEXT_soloader(GLenum mode, GLsizei count, GLenum type,
                                         const void *indices, GLsizei instancecount);
void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count);
void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const void *indices);
void glEnable_soloader(GLenum cap);
void glDisable_soloader(GLenum cap);
void glFinish_soloader(void);
void glFlush_soloader(void);
void glPixelStorei_soloader(GLenum pname, GLint param);
void glBindFramebuffer_soloader(GLenum target, GLuint framebuffer);
void glFramebufferTexture2D_soloader(GLenum target, GLenum attachment,
                                     GLenum textarget, GLuint texture, GLint level);
void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height);
void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height);
void glUseProgram_soloader(GLuint program);
void glVertexAttribPointer_soloader(GLuint index, GLint size, GLenum type,
                                    GLboolean normalized, GLsizei stride,
                                    const void *pointer);
void glVertexAttribDivisorEXT_soloader(GLuint index, GLuint divisor);
void glCompressedTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset,
                                        GLint yoffset, GLsizei width, GLsizei height,
                                        GLenum format, GLsizei imageSize,
                                        const void *data);
void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat,
                                     GLsizei width, GLsizei height, GLint border,
                                     GLsizei imageSize, const void *data);
void glTexParameteri_soloader(GLenum target, GLenum pname, GLint param);
void glTexParameterf_soloader(GLenum target, GLenum pname, GLfloat param);
void glTexParameteriv_soloader(GLenum target, GLenum pname, const GLint *params);
void glTexParameterfv_soloader(GLenum target, GLenum pname, const GLfloat *params);
void glTexParameterx_soloader(GLenum target, GLenum pname, GLfixed param);
void glTexParameterxv_soloader(GLenum target, GLenum pname, const GLfixed *params);
void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const void *pixels);
void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset,
                              GLint yoffset, GLsizei width, GLsizei height,
                              GLenum format, GLenum type, const void *pixels);
GLint glGetUniformLocation_soloader(GLuint program, const GLchar *name);
void glUniform1f_soloader(GLint location, GLfloat v0);
void glUniform1fv_soloader(GLint location, GLsizei count, const GLfloat *value);
void glUniform1i_soloader(GLint location, GLint v0);
void glUniform1iv_soloader(GLint location, GLsizei count, const GLint *value);
void glUniform2f_soloader(GLint location, GLfloat v0, GLfloat v1);
void glUniform2fv_soloader(GLint location, GLsizei count, const GLfloat *value);
void glUniform2i_soloader(GLint location, GLint v0, GLint v1);
void glUniform2iv_soloader(GLint location, GLsizei count, const GLint *value);
void glUniform3f_soloader(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
void glUniform3fv_soloader(GLint location, GLsizei count, const GLfloat *value);
void glUniform3i_soloader(GLint location, GLint v0, GLint v1, GLint v2);
void glUniform3iv_soloader(GLint location, GLsizei count, const GLint *value);
void glUniform4f_soloader(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
void glUniform4fv_soloader(GLint location, GLsizei count, const GLfloat *value);
void glUniform4i_soloader(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
void glUniform4iv_soloader(GLint location, GLsizei count, const GLint *value);
void glUniformMatrix2fv_soloader(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void glUniformMatrix3fv_soloader(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void glUniformMatrix4fv_soloader(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void glTexStorage2D_soloader(GLenum target, GLsizei levels, GLenum internalformat,
                             GLsizei width, GLsizei height);
void glInvalidateFramebuffer_soloader(GLenum target, GLsizei numAttachments,
                                      const GLenum *attachments);
GLsync glFenceSync_soloader(GLenum condition, GLbitfield flags);
void glDeleteSync_soloader(GLsync sync);
GLenum glClientWaitSync_soloader(GLsync sync, GLbitfield flags, GLuint64 timeout);
void glWaitSync_soloader(GLsync sync, GLbitfield flags, GLuint64 timeout);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
