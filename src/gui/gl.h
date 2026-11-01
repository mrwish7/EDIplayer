// OpenGL for the user interface: desktop OpenGL through GLFW's header, or
// OpenGL ES 3 on Android. The UI only calls the basic texture functions
// directly; ImGui's OpenGL backend does the rest.
#pragma once

#ifdef __ANDROID__
#include <GLES3/gl3.h>
#else
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // before GLFW, which would otherwise define APIENTRY itself
#endif
#include <GLFW/glfw3.h>
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
