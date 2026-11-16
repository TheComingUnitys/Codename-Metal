// Headless Metal 4 app-renderer self-test.
//
// Exercises exactly the parts of metal/native/Metal4App.mm that the engine path
// depends on, without launching the game:
//   * GL-location -> MSL-slot attribute mapping (position on slot 3, like the
//     5/8-attribute OpenFL display shaders)
//   * constant-step attributes bound at bufferCount+1 (alpha)
//   * indexed drawing
//   * per-stage uniform buffers (VS matrix + FS tint at buffer(0))
//   * Metal 4 resource lifetime anchors (locals must survive until commit)
//   * offscreen render + CPU readback
//
// Build/run: ./metal/scripts/run_metal4_selftest.sh

#include "../native/Metal4App.h"
#include <Metal/Metal.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

static const char* kVS =
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct VIn { float4 pos [[attribute(3)]]; float2 uv [[attribute(4)]]; float alpha [[attribute(5)]]; };\n"
	"struct VOut { float4 position [[position]]; float2 uv; float alpha; };\n"
	"vertex VOut xlatMtlMain(VIn in [[stage_in]], constant float4x4& m [[buffer(0)]]) {\n"
	"  VOut o; o.position = m * float4(in.pos.xy, 0.0, in.pos.w); o.uv = in.uv; o.alpha = in.alpha; return o;\n"
	"}\n";

static const char* kFS =
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct VOut { float4 position [[position]]; float2 uv; float alpha; };\n"
	"fragment float4 xlatMtlMain(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4& tint [[buffer(0)]]) {\n"
	"  return t.sample(s, in.uv) * tint * in.alpha;\n"
	"}\n";

static int fail(const char* what)
{
	std::fprintf(stderr, "[selftest] FAIL: %s\n", what);
	return 1;
}

int main()
{
	if (!cne_metal4_app_init_offscreen(128, 128))
		return fail("init_offscreen (Metal 4 unavailable?)");

	const int W = 128, H = 128;
	const int fbo = 1, whiteTex = 2, vbo = 10, ibo = 11, program = 20;

	// 1x1 opaque white texture.
	const unsigned char white[4] = {255, 255, 255, 255};
	cne_metal4_app_texture(whiteTex, 1, 1, 0, white);
	cne_metal4_app_texture(fbo, W, H, 1, nullptr);

	if (!cne_metal4_app_program(program, kVS, (int)std::strlen(kVS), 0, kFS, (int)std::strlen(kFS), 0, nullptr, 0))
		return fail("program compile");

	// 4 vertices: x,y,u,v (stride 16).  The quad occupies the middle half so the
	// corner readback also proves the clear/load behaviour.
	const float verts[4][4] = {
		{0.25f, 0.25f, 0.f, 0.f},
		{0.75f, 0.25f, 1.f, 0.f},
		{0.25f, 0.75f, 0.f, 1.f},
		{0.75f, 0.75f, 1.f, 1.f},
	};
	cne_metal4_app_buffer(vbo, verts, (int)sizeof(verts), 0);
	const int vbo3 = 12;
	// Engine-style vec3 position + vec2 uv layout (stride 20, uv at offset 12).
	const float verts3[4][5] = {
		{0.25f, 0.25f, 0.f, 0.f, 0.f},
		{0.75f, 0.25f, 0.f, 1.f, 0.f},
		{0.25f, 0.75f, 0.f, 0.f, 1.f},
		{0.75f, 0.75f, 0.f, 1.f, 1.f},
	};
	cne_metal4_app_buffer(vbo3, verts3, (int)sizeof(verts3), 0);
	const unsigned short indices[6] = {0, 1, 2, 2, 1, 3};
	cne_metal4_app_buffer(ibo, indices, (int)sizeof(indices), 1);

	// GL 0..1 -> NDC, column-major (same layout OpenFL uploads).
	const float matrix[16] = {2.f, 0, 0, 0, 0, -2.f, 0, 0, 0, 0, 1.f, 0, -1.f, 1.f, 0, 1.f};
	const float tint[4] = {1.f, 0.f, 1.f, 1.f};   // magenta
	const float alphaConst[4] = {0.5f, 0, 0, 1.f};

	int attribLoc[3] = {3, 4, 5};
	int attribBuf[3] = {0, 0, 0};
	int attribSize[3] = {2, 2, 1};
	int attribType[3] = {1, 1, 1};
	int attribNorm[3] = {1, 1, 1};
	int attribOff[3] = {0, 8, 0};
	int bufferIds[1] = {vbo};
	int strides[1] = {16};
	int constantLoc[1] = {5};
	int constantComp[1] = {1};
	float constantValues[4] = {alphaConst[0], 0.f, 0.f, 1.f};
	int texIds[1] = {whiteTex};

	// vec3-position variant (descriptor Float3 feeding the MSL float4 input).
	int attribLoc3[3] = {3, 4, 5};
	int attribBuf3[3] = {0, 0, 0};
	int attribSize3[3] = {3, 2, 1};
	int attribType3[3] = {1, 1, 1};
	int attribNorm3[3] = {1, 1, 1};
	int attribOff3[3] = {0, 12, 0};
	int bufferIds3[1] = {vbo3};
	int strides3[1] = {20};

	// Two frames: the second also proves the command buffer/event reuse path and
	// the vec3/Float3 vertex descriptor (w must default to 1 for the matrix).
	unsigned char pixels[W * H * 4];
	for (int frame = 0; frame < 2; ++frame)
	{
		const bool vec3 = (frame == 1);
		cne_metal4_app_target(fbo, W, H, 0, 0.f, 0.f, 0.f, 1.f, 1);
		cne_metal4_app_draw(program, 0, 0, 0, 0, 0,
			vec3 ? bufferIds3 : bufferIds, vec3 ? strides3 : strides, 1,
			vec3 ? attribLoc3 : attribLoc, vec3 ? attribBuf3 : attribBuf,
			vec3 ? attribSize3 : attribSize, vec3 ? attribType3 : attribType,
			vec3 ? attribNorm3 : attribNorm, vec3 ? attribOff3 : attribOff, 3,
			texIds, 1,
			constantLoc, constantComp, constantValues, 1,
			matrix, (int)sizeof(matrix), tint, (int)sizeof(tint),
			indices, (int)sizeof(indices), 0, 6, 0, 0, 0);
		cne_metal4_app_frame_end();

		if (!cne_metal4_app_read_texture(fbo, pixels, W, H))
			return fail("readback");

		const int cx = W / 2, cy = H / 2;
		const unsigned char* p = &pixels[(cy * W + cx) * 4];
		std::printf("[selftest] frame %d center pixel = %u %u %u %u\n", frame, p[0], p[1], p[2], p[3]);
		// tint(1,0,1) * alpha(0.5) => ~128,0,128,128.
		if (p[0] < 100 || p[0] > 160 || p[1] > 30 || p[2] < 100 || p[2] > 160 || p[3] < 100 || p[3] > 160)
			return fail("center pixel is not the expected half-intensity magenta (attribute/constant/uniform bridge broken)");
	}

	// A corner outside the 0..1 quad must stay at the black clear colour.
	const unsigned char* corner = &pixels[0];
	if (corner[0] > 10 || corner[1] > 10 || corner[2] > 10)
		return fail("corner pixel should be black (draw covered the wrong area)");

	cne_metal4_app_shutdown();
	std::printf("[selftest] PASS\n");
	return 0;
}
