// Host stubs: only to link the text-processing parts of the engine.
#include <GLES3/gl3.h>
#include <cstdio>
#include <cstdarg>
extern "C" {
int __android_log_print(int, const char* tag, const char* fmt, ...) { va_list a; va_start(a, fmt); std::printf("[%s] ", tag); std::vprintf(fmt, a); std::printf("\n"); va_end(a); return 0; }
int __android_log_write(int, const char* tag, const char* t) { std::printf("[%s] %s\n", tag, t); return 0; }
#define STUB(ret, name, args, val) ret GL_APIENTRY name args { return val; }
#define VSTUB(name, args) void GL_APIENTRY name args {}
STUB(const GLubyte*, glGetString, (GLenum), (const GLubyte*)"stub")
STUB(const GLubyte*, glGetStringi, (GLenum, GLuint), (const GLubyte*)"stub")
VSTUB(glGetIntegerv, (GLenum, GLint* v)) 
VSTUB(glGetShaderPrecisionFormat, (GLenum, GLenum, GLint*, GLint*))
STUB(GLenum, glGetError, (void), 0)
STUB(GLint, glGetUniformLocation, (GLuint, const GLchar*), -1)
VSTUB(glUseProgram, (GLuint)) VSTUB(glUniform1i, (GLint, GLint)) VSTUB(glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
VSTUB(glUniform4fv, (GLint, GLsizei, const GLfloat*)) VSTUB(glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))
STUB(GLuint, glCreateShader, (GLenum), 0) STUB(GLuint, glCreateProgram, (void), 0)
VSTUB(glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) VSTUB(glCompileShader, (GLuint))
VSTUB(glGetShaderiv, (GLuint, GLenum, GLint*)) VSTUB(glGetProgramiv, (GLuint, GLenum, GLint*))
VSTUB(glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) VSTUB(glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
VSTUB(glAttachShader, (GLuint, GLuint)) VSTUB(glDetachShader, (GLuint, GLuint)) VSTUB(glBindAttribLocation, (GLuint, GLuint, const GLchar*)) VSTUB(glLinkProgram, (GLuint)) VSTUB(glDeleteProgram, (GLuint)) VSTUB(glDeleteShader, (GLuint))
}
