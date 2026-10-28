// ---------------------------------------------------------------------------
// native_replay: replay a CNE_METAL_CAPTURE_FRAME recording through a *direct*
// Metal renderer (no bgfx, no lime_gl_*), so the result can be compared
// pixel-by-pixel with the same frame rendered by the current bgfx path.
//
// Prototype for metal/audit/openfl-native.md §5.  Scope: one colour render
// target, empty attachments, up to N vertex streams, one index buffer, one
// texture per draw (the 2D subset that the game actually uses).
//
//   native_replay <capture-frame-dir> <out-prefix>
//
// Writes <out-prefix>.raw (rows top-to-bottom, BGRA8) + <out-prefix>.txt.
// ---------------------------------------------------------------------------

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

namespace {

std::string ReadText(const std::string& path)
{
    std::ifstream f(path);
    if (!f) return std::string();
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

std::vector<uint8_t> ReadBin(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> Split(const std::string& line)
{
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

// ------------------------------------------------------------------ MSL struct

struct Field { std::string name; uint32_t offset = 0; uint32_t size = 16; };

std::vector<Field> ParseUniformStruct(const std::string& msl)
{
    std::vector<Field> fields;
    size_t p = msl.find("struct XlatMtlUniforms");
    if (p == std::string::npos) return fields;
    size_t brace = msl.find('{', p);
    size_t end = msl.find('}', brace);
    if (brace == std::string::npos || end == std::string::npos) return fields;

    uint32_t offset = 0;
    for (auto& raw : Lines(msl.substr(brace + 1, end - brace - 1))) {
        auto t = Split(raw);
        if (t.size() < 2) continue;
        std::string type = t[0];
        std::string name = t[1];
        if (!name.empty() && (name.back() == ';')) name.pop_back();

        uint32_t size = 16;
        uint32_t count = 1;
        size_t bracket = name.find('[');
        if (bracket != std::string::npos) {
            count = (uint32_t)std::strtoul(name.c_str() + bracket + 1, nullptr, 10);
            name = name.substr(0, bracket);
        }
        if (type == "float4x4") size = 64 * count;
        else if (type == "float4x3" || type == "float4x2") size = 48 * count;
        else if (type == "float3x3") size = 48 * count;
        else if (type == "float3x2") size = 32 * count;
        else if (type == "float2x2") size = 16 * count;
        else if (type == "float4") size = 16 * count;
        else if (type == "float2") size = 8 * count;
        else if (type == "float") size = 4 * count;
        else if (type == "int4") size = 16 * count;
        else if (type == "int2") size = 8 * count;
        else if (type == "int") size = 4 * count;
        else size = 16 * count;

        Field f; f.name = name; f.offset = offset; f.size = size;
        fields.push_back(f);
        offset += size;
    }
    return fields;
}

// ------------------------------------------------------------------ GL mapping

int MapBlend(MTLBlendFactor* rgb, MTLBlendFactor* alpha, int glFactor)
{
    // Mirrors bgfx's s_blendFactor table (renderer_mtl.cpp), which is what the
    // current bgfx path actually uses.
    switch (glFactor) {
        case 0x0000: *rgb = MTLBlendFactorZero; *alpha = MTLBlendFactorZero; break;                    // ZERO
        case 0x0001: *rgb = MTLBlendFactorOne; *alpha = MTLBlendFactorOne; break;                      // ONE
        case 0x0300: *rgb = MTLBlendFactorSourceColor; *alpha = MTLBlendFactorSourceAlpha; break;      // SRC_COLOR
        case 0x0301: *rgb = MTLBlendFactorOneMinusSourceColor; *alpha = MTLBlendFactorOneMinusSourceAlpha; break;
        case 0x0302: *rgb = MTLBlendFactorSourceAlpha; *alpha = MTLBlendFactorSourceAlpha; break;      // SRC_ALPHA
        case 0x0303: *rgb = MTLBlendFactorOneMinusSourceAlpha; *alpha = MTLBlendFactorOneMinusSourceAlpha; break;
        case 0x0304: *rgb = MTLBlendFactorDestinationAlpha; *alpha = MTLBlendFactorDestinationAlpha; break;
        case 0x0305: *rgb = MTLBlendFactorOneMinusDestinationAlpha; *alpha = MTLBlendFactorOneMinusDestinationAlpha; break;
        case 0x0306: *rgb = MTLBlendFactorDestinationColor; *alpha = MTLBlendFactorDestinationAlpha; break;
        case 0x0307: *rgb = MTLBlendFactorOneMinusDestinationColor; *alpha = MTLBlendFactorOneMinusDestinationAlpha; break;
        case 0x0308: *rgb = MTLBlendFactorSourceAlphaSaturated; *alpha = MTLBlendFactorOne; break;
        case 0x8001: *rgb = MTLBlendFactorBlendColor; *alpha = MTLBlendFactorBlendColor; break;
        case 0x8002: *rgb = MTLBlendFactorOneMinusBlendColor; *alpha = MTLBlendFactorOneMinusBlendColor; break;
        default: *rgb = MTLBlendFactorOne; *alpha = MTLBlendFactorOne; break;
    }
    return 0;
}

MTLBlendOperation MapBlendOp(int glEq)
{
    switch (glEq) {
        case 0x8006: return MTLBlendOperationAdd;              // FUNC_ADD
        case 0x800A: return MTLBlendOperationSubtract;         // FUNC_SUBTRACT
        case 0x800B: return MTLBlendOperationReverseSubtract;  // FUNC_REVERSE_SUBTRACT
        case 0x8007: return MTLBlendOperationMin;
        case 0x8008: return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
    }
}

MTLVertexFormat MapVertexFormat(int bgfxType, int num, bool normalized)
{
    // bgfx AttribType: 0 Int8, 1 Uint8, 2 Uint10, 3 Int16, 4 Uint16, 5 Half, 6 Float, 7 Int32, 8 Uint32
    static MTLVertexFormat int8[5][2] = {
        {MTLVertexFormatChar2, MTLVertexFormatChar2Normalized},
        {MTLVertexFormatChar2, MTLVertexFormatChar2Normalized},
        {MTLVertexFormatChar3, MTLVertexFormatChar3Normalized},
        {MTLVertexFormatChar4, MTLVertexFormatChar4Normalized},
        {MTLVertexFormatChar4, MTLVertexFormatChar4Normalized},
    };
    static MTLVertexFormat uint8[5][2] = {
        {MTLVertexFormatUChar2, MTLVertexFormatUChar2Normalized},
        {MTLVertexFormatUChar2, MTLVertexFormatUChar2Normalized},
        {MTLVertexFormatUChar3, MTLVertexFormatUChar3Normalized},
        {MTLVertexFormatUChar4, MTLVertexFormatUChar4Normalized},
        {MTLVertexFormatUChar4, MTLVertexFormatUChar4Normalized},
    };
    static MTLVertexFormat int16[5][2] = {
        {MTLVertexFormatShort2, MTLVertexFormatShort2Normalized},
        {MTLVertexFormatShort2, MTLVertexFormatShort2Normalized},
        {MTLVertexFormatShort3, MTLVertexFormatShort3Normalized},
        {MTLVertexFormatShort4, MTLVertexFormatShort4Normalized},
        {MTLVertexFormatShort4, MTLVertexFormatShort4Normalized},
    };
    static MTLVertexFormat uint16[5][2] = {
        {MTLVertexFormatUShort2, MTLVertexFormatUShort2Normalized},
        {MTLVertexFormatUShort2, MTLVertexFormatUShort2Normalized},
        {MTLVertexFormatUShort3, MTLVertexFormatUShort3Normalized},
        {MTLVertexFormatUShort4, MTLVertexFormatUShort4Normalized},
        {MTLVertexFormatUShort4, MTLVertexFormatUShort4Normalized},
    };
    static MTLVertexFormat half[5] = {
        MTLVertexFormatHalf2, MTLVertexFormatHalf2, MTLVertexFormatHalf3, MTLVertexFormatHalf4, MTLVertexFormatHalf4
    };
    static MTLVertexFormat flt[5] = {
        MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4, MTLVertexFormatFloat4
    };
    int n = num < 1 ? 1 : (num > 4 ? 4 : num);
    bool norm = normalized;
    switch (bgfxType) {
        case 0: return int8[n][norm ? 1 : 0];
        case 1: return uint8[n][norm ? 1 : 0];
        case 3: return int16[n][norm ? 1 : 0];
        case 4: return uint16[n][norm ? 1 : 0];
        case 5: return half[n];
        case 7: return MTLVertexFormatInt;
        case 8: return MTLVertexFormatUInt;
        default: return flt[n];
    }
}

// ------------------------------------------------------------------ parse data

struct Attr { int attrib = 0, num = 4, type = 6, normalized = 0, offset = 0; };
struct Stream { int index = 0, stride = 0; uint32_t startVertex = 0; std::vector<Attr> attrs; std::string file; uint32_t bytes = 0; };
struct Uniform { std::string name; int sampler = 0, unit = 0; uint32_t memberCount = 1; int components = 4; uint32_t valueSize = 0; std::string file; };
struct TextureRef { int unit = 0, id = 0; };

struct Draw {
    int program = 0;
    int indexed = 0;
    uint32_t count = 0, firstIndex = 0;
    uint32_t indexType = 0;
    int backbufferW = 0, backbufferH = 0;
    int blend = 0;
    int blendFunc[4] = {1, 0, 1, 0};
    int blendEq[2] = {0x8006, 0x8006};
    int colorWrite[4] = {1, 1, 1, 1};
    int depthTest = 0, depthWrite = 0, cull = 0;
    int viewport[4] = {0, 0, 0, 0};
    int scissor = 0, scissorRect[4] = {0, 0, 0, 0};
    std::vector<Stream> streams;
    std::vector<Uniform> uniforms;
    std::vector<TextureRef> textures;
    std::string vsFile, fsFile, indexFile;
    uint32_t indexBytes = 0;
};

Draw ParseDraw(const std::string& dir, const std::string& name)
{
    Draw d;
    std::string text = ReadText(dir + "/" + name + ".txt");
    auto lines = Lines(text);
    int streamIndexWithAttrs = -1;
    for (auto& line : lines) {
        auto t = Split(line);
        if (t.empty()) continue;
        if (t[0] == "program" && t.size() >= 2) d.program = std::atoi(t[1].c_str());
        else if (t[0] == "indexed" && t.size() >= 2) d.indexed = std::atoi(t[1].c_str());
        else if (t[0] == "count" && t.size() >= 2) d.count = (uint32_t)std::strtoul(t[1].c_str(), nullptr, 10);
        else if (t[0] == "firstIndex" && t.size() >= 2) d.firstIndex = (uint32_t)std::strtoul(t[1].c_str(), nullptr, 10);
        else if (t[0] == "indexType" && t.size() >= 2) d.indexType = (uint32_t)std::strtoul(t[1].c_str(), nullptr, 10);
        else if (t[0] == "backbuffer" && t.size() >= 3) { d.backbufferW = std::atoi(t[1].c_str()); d.backbufferH = std::atoi(t[2].c_str()); }
        else if (t[0] == "blend" && t.size() >= 2) d.blend = std::atoi(t[1].c_str());
        else if (t[0] == "blendFunc" && t.size() >= 5) for (int i = 0; i < 4; ++i) d.blendFunc[i] = std::atoi(t[1 + i].c_str());
        else if (t[0] == "blendEq" && t.size() >= 3) for (int i = 0; i < 2; ++i) d.blendEq[i] = std::atoi(t[1 + i].c_str());
        else if (t[0] == "colorWrite" && t.size() >= 2) {
            std::string v = t[1];
            for (int i = 0; i < 4 && i < (int)v.size(); ++i) d.colorWrite[i] = (v[i] == '1') ? 1 : 0;
        }
        else if (t[0] == "depthTest" && t.size() >= 2) d.depthTest = std::atoi(t[1].c_str());
        else if (t[0] == "depthWrite" && t.size() >= 2) d.depthWrite = std::atoi(t[1].c_str());
        else if (t[0] == "cull" && t.size() >= 2) d.cull = std::atoi(t[1].c_str());
        else if (t[0] == "viewport" && t.size() >= 5) for (int i = 0; i < 4; ++i) d.viewport[i] = std::atoi(t[1 + i].c_str());
        else if (t[0] == "scissor" && t.size() >= 2) d.scissor = std::atoi(t[1].c_str());
        else if (t[0] == "scissorRect" && t.size() >= 5) for (int i = 0; i < 4; ++i) d.scissorRect[i] = std::atoi(t[1 + i].c_str());
        else if (t[0] == "vs" && t.size() >= 2) d.vsFile = t[1];
        else if (t[0] == "fs" && t.size() >= 2) d.fsFile = t[1];
        else if (t[0] == "indexFile" && t.size() >= 3) { d.indexFile = t[1]; d.indexBytes = (uint32_t)std::strtoul(t[2].c_str(), nullptr, 10); }
        else if (t[0] == "stream" && t.size() >= 7) {
            Stream s;
            s.index = std::atoi(t[1].c_str());
            s.stride = std::atoi(t[2].c_str());
            s.startVertex = (uint32_t)std::strtoul(t[3].c_str(), nullptr, 10);
            s.file = t[6];
            s.bytes = (uint32_t)std::strtoul(t[5].c_str(), nullptr, 10);
            d.streams.push_back(s);
            streamIndexWithAttrs = (int)d.streams.size() - 1;
        }
        else if (t[0] == "attr" && t.size() >= 6 && streamIndexWithAttrs >= 0) {
            Attr a;
            a.attrib = std::atoi(t[1].c_str());
            a.num = std::atoi(t[2].c_str());
            a.type = std::atoi(t[3].c_str());
            a.normalized = std::atoi(t[4].c_str());
            a.offset = std::atoi(t[5].c_str());
            d.streams[streamIndexWithAttrs].attrs.push_back(a);
        }
        else if (t[0] == "uniform" && t.size() >= 7) {
            Uniform u;
            u.name = t[1];
            u.sampler = std::atoi(t[2].c_str());
            u.unit = std::atoi(t[3].c_str());
            u.memberCount = (uint32_t)std::strtoul(t[4].c_str(), nullptr, 10);
            u.components = std::atoi(t[5].c_str());
            u.valueSize = (uint32_t)std::strtoul(t[6].c_str(), nullptr, 10);
            u.file = "prog" + std::to_string(d.program) + "_u_" + u.name + ".bin";
            d.uniforms.push_back(u);
        }
        else if (t[0] == "texture" && t.size() >= 3) {
            TextureRef tr; tr.unit = std::atoi(t[1].c_str()); tr.id = std::atoi(t[2].c_str());
            d.textures.push_back(tr);
        }
    }
    return d;
}

// ------------------------------------------------------------------ rendering

struct TextureData {
    id<MTLTexture> texture = nil;
    id<MTLSamplerState> sampler = nil;
};

struct Renderer {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLTexture> target = nil;
    int width = 0, height = 0;
    std::map<int, TextureData> textures;
    std::map<std::string, id<MTLRenderPipelineState>> pipelines;
    std::string dir;
    int draws = 0;

    id<MTLTexture> GetTarget(int w, int h)
    {
        if (target != nil && width == w && height == h) return target;
        width = w; height = h;
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                        width:(NSUInteger)w
                                                                                       height:(NSUInteger)h
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        target = [device newTextureWithDescriptor:desc];
        return target;
    }

    id<MTLSamplerState> GetSampler(const std::string& id)
    {
        std::string text = ReadText(dir + "/tex" + id + ".txt");
        int minFilter = 0x2601, magFilter = 0x2601, wrapS = 0x812F, wrapT = 0x812F;
        for (auto& line : Lines(text)) {
            auto t = Split(line);
            if (t.empty()) continue;
            if (t[0] == "filter" && t.size() >= 3) { minFilter = std::atoi(t[1].c_str()); magFilter = std::atoi(t[2].c_str()); }
            if (t[0] == "wrap" && t.size() >= 3) { wrapS = std::atoi(t[1].c_str()); wrapT = std::atoi(t[2].c_str()); }
        }
        MTLSamplerDescriptor* desc = [MTLSamplerDescriptor new];
        desc.minFilter = (minFilter == 0x2600) ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        desc.magFilter = (magFilter == 0x2600) ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        auto addr = [](int gl) {
            if (gl == 0x2901) return MTLSamplerAddressModeRepeat;
            if (gl == 0x8370) return MTLSamplerAddressModeMirrorRepeat;
            if (gl == 0x812D) return MTLSamplerAddressModeClampToBorderColor; // not used
            return MTLSamplerAddressModeClampToEdge;
        };
        desc.sAddressMode = addr(wrapS);
        desc.tAddressMode = addr(wrapT);
        return [device newSamplerStateWithDescriptor:desc];
    }

    TextureData* GetTexture(const std::string& id)
    {
        int key = std::atoi(id.c_str());
        auto it = textures.find(key);
        if (it != textures.end()) return &it->second;

        std::string text = ReadText(dir + "/tex" + id + ".txt");
        int w = 0, h = 0, internalFormat = 0x8058, format = 0x80E1, type = 0x1401;
        for (auto& line : Lines(text)) {
            auto t = Split(line);
            if (t.empty()) continue;
            if (t[0] == "size" && t.size() >= 3) { w = std::atoi(t[1].c_str()); h = std::atoi(t[2].c_str()); }
            if (t[0] == "internalFormat" && t.size() >= 2) internalFormat = std::atoi(t[1].c_str());
            if (t[0] == "format" && t.size() >= 2) format = std::atoi(t[1].c_str());
            if (t[0] == "type" && t.size() >= 2) type = std::atoi(t[1].c_str());
        }
        auto bytes = ReadBin(dir + "/tex" + id + ".bin");

        MTLPixelFormat pixelFormat = MTLPixelFormatBGRA8Unorm;
        int bpp = 4;
        if (internalFormat == 0x8058 || format == 0x80E1) { pixelFormat = MTLPixelFormatBGRA8Unorm; bpp = 4; }
        else if (type == 0x1406) { pixelFormat = MTLPixelFormatRGBA32Float; bpp = 16; }
        else { pixelFormat = MTLPixelFormatRGBA8Unorm; bpp = 4; }

        TextureData td;
        if (getenv("NR_DEBUG")) fprintf(stderr, "texture id=%s %dx%d fmt=0x%x bytes=%zu\n", id.c_str(), w, h, format, bytes.size());
        if (w > 0 && h > 0 && !bytes.empty()) {
            MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat
                                                                                            width:(NSUInteger)w
                                                                                           height:(NSUInteger)h
                                                                                        mipmapped:NO];
            desc.usage = MTLTextureUsageShaderRead;
            desc.storageMode = MTLStorageModeShared;
            td.texture = [device newTextureWithDescriptor:desc];
            [td.texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
                          mipmapLevel:0
                            withBytes:bytes.data()
                          bytesPerRow:(NSUInteger)(w * bpp)];
        }
        td.sampler = GetSampler(id);
        textures[key] = td;
        return &textures[key];
    }

    id<MTLRenderPipelineState> GetPipeline(const Draw& d)
    {
        std::string key = std::to_string(d.program) + "/" + std::to_string(d.blend);
        for (int i = 0; i < 4; ++i) key += "/" + std::to_string(d.blendFunc[i]);
        for (int i = 0; i < 2; ++i) key += "/" + std::to_string(d.blendEq[i]);
        for (int i = 0; i < 4; ++i) key += std::to_string(d.colorWrite[i]);
        for (auto& s : d.streams) {
            key += "|" + std::to_string(s.stride);
            for (auto& a : s.attrs)
                key += ":" + std::to_string(a.attrib) + "," + std::to_string(a.num) + "," + std::to_string(a.type) + "," + std::to_string(a.normalized) + "," + std::to_string(a.offset);
        }

        auto it = pipelines.find(key);
        if (it != pipelines.end()) return it->second;

        std::string vsSrc = ReadText(dir + "/" + d.vsFile);
        std::string fsSrc = ReadText(dir + "/" + d.fsFile);
        NSError* err = nil;
        id<MTLLibrary> vsLib = [device newLibraryWithSource:[NSString stringWithUTF8String:vsSrc.c_str()] options:nil error:&err];
        if (vsLib == nil) { fprintf(stderr, "vertex MSL compile failed: %s\n", err ? [[err description] UTF8String] : "?"); return nil; }
        id<MTLLibrary> fsLib = [device newLibraryWithSource:[NSString stringWithUTF8String:fsSrc.c_str()] options:nil error:&err];
        if (fsLib == nil) { fprintf(stderr, "fragment MSL compile failed: %s\n", err ? [[err description] UTF8String] : "?"); return nil; }
        id<MTLFunction> vsFn = [vsLib newFunctionWithName:@"xlatMtlMain"];
        id<MTLFunction> fsFn = [fsLib newFunctionWithName:@"xlatMtlMain"];
        if (vsFn == nil || fsFn == nil) { fprintf(stderr, "xlatMtlMain not found\n"); return nil; }

        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = vsFn;
        pd.fragmentFunction = fsFn;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

        if (d.blend) {
            MTLBlendFactor srcRGB, dstRGB, srcA, dstA;
            MapBlend(&srcRGB, &dstRGB, d.blendFunc[0]);
            MapBlend(&srcA, &dstA, d.blendFunc[2]);
            (void)srcA; (void)dstA;
            MapBlend(&srcA, &dstA, d.blendFunc[2]);
            MTLBlendFactor srcRGB2, dstRGB2, srcA2, dstA2;
            MapBlend(&srcRGB2, &dstRGB2, d.blendFunc[1]);
            MapBlend(&srcA2, &dstA2, d.blendFunc[3]);
            pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].sourceRGBBlendFactor = srcRGB;
            pd.colorAttachments[0].destinationRGBBlendFactor = dstRGB2;
            pd.colorAttachments[0].sourceAlphaBlendFactor = srcA;
            pd.colorAttachments[0].destinationAlphaBlendFactor = dstA2;
            pd.colorAttachments[0].rgbBlendOperation = MapBlendOp(d.blendEq[0]);
            pd.colorAttachments[0].alphaBlendOperation = MapBlendOp(d.blendEq[1]);
        }

        MTLColorWriteMask mask = 0;
        if (d.colorWrite[0]) mask |= MTLColorWriteMaskRed;
        if (d.colorWrite[1]) mask |= MTLColorWriteMaskGreen;
        if (d.colorWrite[2]) mask |= MTLColorWriteMaskBlue;
        if (d.colorWrite[3]) mask |= MTLColorWriteMaskAlpha;
        pd.colorAttachments[0].writeMask = mask;

        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        for (auto& s : d.streams) {
            NSUInteger bufferIndex = (NSUInteger)s.index + 1;
            vd.layouts[bufferIndex].stride = (NSUInteger)s.stride;
            vd.layouts[bufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;
            for (auto& a : s.attrs) {
                vd.attributes[(NSUInteger)a.attrib].format = MapVertexFormat(a.type, a.num, a.normalized != 0);
                vd.attributes[(NSUInteger)a.attrib].offset = (NSUInteger)a.offset;
                vd.attributes[(NSUInteger)a.attrib].bufferIndex = bufferIndex;
            }
        }
        pd.vertexDescriptor = vd;

        id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (pso == nil) { fprintf(stderr, "pipeline creation failed: %s\n", err ? [[err description] UTF8String] : "?"); return nil; }
        pipelines[key] = pso;
        return pso;
    }

    void Clear(MTLClearColor color)
    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = color;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }

    bool Play(const std::string& ops)
    {
        auto lines = Lines(ops);
        // find the backbuffer size first (it is in every draw header)
        for (auto& line : lines) {
            auto t = Split(line);
            if (t.size() >= 2 && t[0] == "DRAW") {
                Draw probe = ParseDraw(dir, t[1]);
                if (probe.backbufferW > 0) { GetTarget(probe.backbufferW, probe.backbufferH); break; }
            }
        }
        if (target == nil) { fprintf(stderr, "no backbuffer size in capture\n"); return false; }

        for (auto& line : lines) {
            auto t = Split(line);
            if (t.empty()) continue;
            if (t[0] == "CLEAR") {
                uint32_t rgba = 0; int mask = 0;
                for (size_t i = 1; i < t.size(); ++i) {
                    auto eq = t[i].find('=');
                    if (eq == std::string::npos) continue;
                    std::string k = t[i].substr(0, eq), v = t[i].substr(eq + 1);
                    if (k == "rgba") rgba = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
                    if (k == "mask") mask = std::atoi(v.c_str());
                }
                if (mask & 0x4000) { // GL_COLOR_BUFFER_BIT
                    MTLClearColor c;
                    c.red = ((rgba >> 24) & 0xff) / 255.0;
                    c.green = ((rgba >> 16) & 0xff) / 255.0;
                    c.blue = ((rgba >> 8) & 0xff) / 255.0;
                    c.alpha = (rgba & 0xff) / 255.0;
                    Clear(c);
                }
            } else if (t[0] == "DRAW") {
                Draw d = ParseDraw(dir, t[1]);
                if (target == nil) GetTarget(d.backbufferW, d.backbufferH);
                DrawOne(d);
                ++draws;
            }
        }
        return true;
    }

    void DrawOne(const Draw& d)
    {
        id<MTLRenderPipelineState> pso = GetPipeline(d);
        if (pso == nil) { fprintf(stderr, "draw program=%d: pipeline FAILED\n", d.program); return; }
        if (getenv("NR_DEBUG")) fprintf(stderr, "draw program=%d streams=%zu tex=%zu count=%u\n",
            d.program, d.streams.size(), d.textures.size(), d.count);

        std::string vsSrc = ReadText(dir + "/" + d.vsFile);
        std::string fsSrc = ReadText(dir + "/" + d.fsFile);
        std::vector<Field> vsFields = ParseUniformStruct(vsSrc);
        std::vector<Field> fsFields = ParseUniformStruct(fsSrc);

        auto buildUniforms = [&](const std::vector<Field>& fields) {
            std::vector<uint8_t> out;
            if (fields.empty()) return out;
            out.assign(fields.back().offset + fields.back().size, 0);
            for (auto& f : fields) {
                for (auto& u : d.uniforms) {
                    if (u.sampler) continue;
                    if (u.name != f.name) continue;
                    auto bytes = ReadBin(dir + "/" + u.file);
                    size_t n = bytes.size () < f.size ? bytes.size () : f.size;
                    if (f.offset + n <= out.size ()) memcpy(out.data () + f.offset, bytes.data (), n);
                    break;
                }
            }
            return out;
        };
        auto vsUniforms = buildUniforms(vsFields);
        auto fsUniforms = buildUniforms(fsFields);

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];

        MTLViewport vp;
        vp.originX = (double)d.viewport[0];
        vp.originY = (double)(d.backbufferH - (d.viewport[1] + d.viewport[3]));
        vp.width = (double)d.viewport[2];
        vp.height = (double)d.viewport[3];
        vp.znear = 0.0;
        vp.zfar = 1.0;
        [enc setViewport:vp];

        if (d.scissor) {
            MTLScissorRect sr;
            sr.x = (NSUInteger)d.scissorRect[0];
            sr.y = (NSUInteger)(d.backbufferH - (d.scissorRect[1] + d.scissorRect[3]));
            sr.width = (NSUInteger)d.scissorRect[2];
            sr.height = (NSUInteger)d.scissorRect[3];
            [enc setScissorRect:sr];
        }

        [enc setRenderPipelineState:pso];

        // vertex streams
        for (auto& s : d.streams) {
            auto bytes = ReadBin(dir + "/" + s.file);
            if (bytes.empty()) continue;
            NSUInteger offset = (NSUInteger)s.startVertex * (NSUInteger)s.stride;
            if (offset >= bytes.size()) continue;
            id<MTLBuffer> buf = [device newBufferWithBytes:bytes.data()
                                                  length:bytes.size()
                                                 options:MTLResourceStorageModeShared];
            [enc setVertexBuffer:buf offset:offset atIndex:(NSUInteger)s.index + 1];
        }

        // uniforms
        if (!vsUniforms.empty()) [enc setVertexBytes:vsUniforms.data() length:vsUniforms.size() atIndex:0];
        if (!fsUniforms.empty()) [enc setFragmentBytes:fsUniforms.data() length:fsUniforms.size() atIndex:0];

        // textures
        for (auto& tr : d.textures) {
            std::string id = std::to_string(tr.id);
            TextureData* td = GetTexture(id);
            if (td && td->texture) [enc setFragmentTexture:td->texture atIndex:(NSUInteger)tr.unit];
            if (td && td->sampler) [enc setFragmentSamplerState:td->sampler atIndex:(NSUInteger)tr.unit];
        }

        if (d.indexed) {
            auto idx = ReadBin(dir + "/" + d.indexFile);
            if (!idx.empty()) {
                id<MTLBuffer> ib = [device newBufferWithBytes:idx.data()
                                                     length:idx.size()
                                                    options:MTLResourceStorageModeShared];
                MTLIndexType it = (d.indexType == 0x1403) ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
                NSUInteger indexSize = (it == MTLIndexTypeUInt16) ? 2 : 4;
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                               indexCount:(NSUInteger)d.count
                                indexType:it
                              indexBuffer:ib
                        indexBufferOffset:(NSUInteger)d.firstIndex * indexSize];
            }
        } else {
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)d.count];
        }

        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }

    void Readback(const std::string& outPrefix)
    {
        NSUInteger rowBytes = (NSUInteger)width * 4;
        id<MTLBuffer> buf = [device newBufferWithLength:rowBytes * (NSUInteger)height
                                                options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromTexture:target
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
                   toBuffer:buf
          destinationOffset:0
     destinationBytesPerRow:rowBytes
   destinationBytesPerImage:rowBytes * (NSUInteger)height];
        [be endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        std::ofstream f(outPrefix + ".raw", std::ios::binary | std::ios::trunc);
        f.write((const char*)buf.contents, (std::streamsize)(rowBytes * (NSUInteger)height));
        std::ofstream m(outPrefix + ".txt", std::ios::trunc);
        m << "size " << width << " " << height << "\npitch " << rowBytes << "\nformat BGRA8\nbytes "
          << (rowBytes * (NSUInteger)height) << "\ndraws " << draws << "\n";
    }
};

} // namespace

int main(int argc, const char* argv[])
{
    @autoreleasepool {
        if (argc < 3) {
            fprintf(stderr, "usage: native_replay <capture-frame-dir> <out-prefix>\n");
            return 2;
        }
        std::string dir = argv[1];
        std::string outPrefix = argv[2];
        std::string ops = ReadText(dir + "/ops.txt");
        if (ops.empty()) { fprintf(stderr, "missing %s/ops.txt\n", dir.c_str()); return 2; }

        Renderer r;
        r.dir = dir;
        r.device = MTLCreateSystemDefaultDevice();
        if (r.device == nil) { fprintf(stderr, "no Metal device\n"); return 2; }
        r.queue = [r.device newCommandQueue];

        if (!r.Play(ops)) return 3;
        r.Readback(outPrefix);
        fprintf(stderr, "native replay: %d draws -> %s.raw\n", r.draws, outPrefix.c_str());
    }
    return 0;
}
