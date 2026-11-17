#pragma once
// C API for the native Metal 4 app renderer (experimental, CNE_METAL4_NATIVE=1).
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

int  cne_metal4_app_init (void* caMetalLayer, int width, int height);
/** Headless/offscreen variant used by metal/autotests: no CAMetalLayer/drawable. */
int  cne_metal4_app_init_offscreen (int width, int height);
int  cne_metal4_app_active (void);
void cne_metal4_app_frame_begin (void);
void cne_metal4_app_frame_end (void);
void cne_metal4_app_target (int textureId, int width, int height, int isScreen,
	float clearR, float clearG, float clearB, float clearA, int clearColor);
void cne_metal4_app_viewport (int x, int y, int w, int h);
void cne_metal4_app_scissor (int enabled, int x, int y, int w, int h);
void cne_metal4_app_texture (int id, int w, int h, int isRT, const void* pixels, int format);
void cne_metal4_app_buffer (int id, const void* bytes, int size, int isIndex);
int  cne_metal4_app_program (int id, const void* vsData, int vsSize, int vsIsMetallib,
	const void* fsData, int fsSize, int fsIsMetallib,
	const int* attribLocations, int attribCount);
void cne_metal4_app_draw (int program, int blendEnabled, int srcRGB, int dstRGB,
	int srcA, int dstA,
	const int* bufferIds, const int* strides, int bufferCount,
	const int* attribLoc, const int* attribBuf, const int* attribSize,
	const int* attribType, const int* attribNorm, const int* attribOff, int attribCount,
	const int* textureIds, int textureCount,
	const int* constantLocations, const int* constantComponents, const float* constantValues, int constantCount,
	const void* vsUniformBytes, int vsUniformSize,
	const void* fsUniformBytes, int fsUniformSize,
	const void* indexBytes, int indexSize, int indexType, int indexCount,
	int baseVertex, int vertexCount, int primitive);
/** Test hook: after a frame is submitted, copy an RGBA8 texture into `out` (w*h*4). */
int  cne_metal4_app_read_texture (int textureId, void* out, int width, int height);
void cne_metal4_app_shutdown (void);

#ifdef __cplusplus
}
#endif