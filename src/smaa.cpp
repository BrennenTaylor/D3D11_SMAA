#include <stdexcept>
#include <string>
#include <vector>

#include <d3dcompiler.h>

#include <agz/smaa/smaa.h>

namespace agz { namespace smaa {

namespace detail
{

// ========= compile shader =========

inline ComPtr<ID3D10Blob> CompileToByteCode(
    const char *source, const char *target,
    const D3D_SHADER_MACRO *macros = nullptr)
{
#ifdef _DEBUG
    constexpr UINT COMPILER_FLAGS = D3DCOMPILE_DEBUG |
                                    D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    constexpr UINT COMPILER_FLAGS = 0;
#endif

    const size_t sourceLen = std::strlen(source);

    ComPtr<ID3D10Blob> ret, err;
    const HRESULT hr = D3DCompile(
        source, sourceLen,
        nullptr, macros, nullptr,
        "main", target,
        COMPILER_FLAGS, 0,
        ret.GetAddressOf(), err.GetAddressOf());

    if(FAILED(hr))
    {
        auto rawErrMsg = reinterpret_cast<const char *>(
            err->GetBufferPointer());
        throw std::runtime_error(rawErrMsg);
    }

    return ret;
}

// ========= create shader =========

inline ComPtr<ID3D11VertexShader> CreateVertexShader(
    ID3D11Device *device, void *byteCode, size_t len)
{
    ComPtr<ID3D11VertexShader> shader;
    const HRESULT hr = device->CreateVertexShader(
        byteCode, len, nullptr, shader.GetAddressOf());
    return FAILED(hr) ? nullptr : shader;
}

inline ComPtr<ID3D11PixelShader> CreatePixelShader(
    ID3D11Device *device, void *byteCode, size_t len)
{
    ComPtr<ID3D11PixelShader> shader;
    const HRESULT hr = device->CreatePixelShader(
        byteCode, len, nullptr, shader.GetAddressOf());
    return FAILED(hr) ? nullptr : shader;
}

// ========= shader sources =========

static const char *COMMON_VERTEX_SHADER_SOURCE = R"___(
struct VSInput
{
    float2 position : POSITION;
    float2 texCoord : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD;
};

VSOutput main(VSInput input)
{
    VSOutput output = (VSOutput)0;
    output.position = float4(input.position, 0.5, 1);
    output.texCoord = input.texCoord;
    return output;
}
)___";

static const char *EDGE_DETECTION_SHADER_SOURCE = R"___(
// #define EDGE_THRESHOLD  XXX
// #define CONTRAST_FACTOR XXX

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD;
};

Texture2D<float3> ImageTexture : register(t0);
SamplerState      PointSampler : register(s0);

float4 main(PSInput input) : SV_TARGET
{
    const float3 LUM_FACTOR = float3(0.2126, 0.7152, 0.0722);

    // sample image pixels

    float3 c = ImageTexture.SampleLevel(
        PointSampler, input.texCoord, 0);
    float3 c_left = ImageTexture.SampleLevel(
        PointSampler, input.texCoord, 0, int2(-1, 0));
    float3 c_top = ImageTexture.SampleLevel(
        PointSampler, input.texCoord, 0, int2(0, -1));

    // eval left/top edge

    float2 delta = float2(
        dot(LUM_FACTOR, abs(c - c_left)),
        dot(LUM_FACTOR, abs(c - c_top)));
    
    float2 is_edge = step(EDGE_THRESHOLD, delta);

    if(dot(is_edge, 1) == 0)
        discard;

    // eval local constract

    float3 c_left2 = ImageTexture.SampleLevel(
        PointSampler, input.texCoord, 0, int2(-2, 0));
    float3 c_top2 = ImageTexture.SampleLevel(
        PointSampler, input.texCoord, 0, int2(0, -2));
    float3 c_right = ImageTexture.SampleLevel(
        PointSampler, input.texCoord, 0, int2(1, 0));
    float3 c_bottom = ImageTexture.SampleLevel(
        PointSampler, input.texCoord, 0, int2(0, 1));

    float4 delta_local = float4(
        dot(LUM_FACTOR, abs(c_left - c_left2)),
        dot(LUM_FACTOR, abs(c_top  - c_top2)),
        dot(LUM_FACTOR, abs(c      - c_right)),
        dot(LUM_FACTOR, abs(c      - c_bottom)));

    // local constract adaptation

    float max_delta_local = max(max(delta_local.r, delta_local.g),
                                max(delta_local.b, delta_local.a));

    is_edge *= step(CONTRAST_FACTOR * max_delta_local, delta);

    return float4(is_edge, 0, 0);
}
)___";

static const char *BLENDING_WEIGHT_SHADER_SOURCE = R"___(
// #define EDGE_DETECTION_MAX_LEN XXX
// #define PIXEL_SIZE_IN_TEXCOORD XXX
// #define CORNER_AREA_FACTOR     XXX

// IMPROVE: performance optimization

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD;
};

Texture2D<float4> EdgeTexture      : register(t0);
Texture2D<float4> InnerAreaTexture : register(t1);

SamplerState PointSampler  : register(s0);
SamplerState LinearSampler : register(s1);

bool is_edge_end(float e, float ce)
{
    return e < 0.87 || ce > 0.01;
}

float end_len_high(float e, float ce)
{
    // 4 weighted contributor of e:
    // 0.6525, 0.21875, 0.09375, 0.03125
    if(e < 0.64)
        return 0;
    float ans_e = e > 0.87 ? 2 : 1;

    // there is 16 value cases of ce
    // in which only 4 'segments' of end len
    // TODO: LUT optimization
    float ans_ce;
    if(ce < 0.09)
        ans_ce = 2;
    else if(ce < 0.2)
        ans_ce = 1;
    else if(ce < 0.3)
        ans_ce = 2;
    else
        ans_ce = 1;

    return min(ans_e, ans_ce);
}

float end_len_low(float e, float ce)
{
    if(e < 0.64)
        return 0;
    float ans_e = e > 0.87 ? 2 : 1;

    // TODO: LUT optimization
    float ans_ce;
    if(ce < 0.03)
        ans_ce = 2;
    else if(ce < 0.09)
        ans_ce = 1;
    else if(ce < 0.2)
        ans_ce = 0;
    else if(ce < 0.3)
        ans_ce = 1;
    else
        ans_ce = 0;

    return min(ans_e, ans_ce);
}

float find_left_end(float2 c)
{
    c += float2(-0.25, 0.125) * PIXEL_SIZE_IN_TEXCOORD;

    for(int p2 = 0; p2 < EDGE_DETECTION_MAX_LEN; ++p2)
    {
        float2 ce_e = EdgeTexture.SampleLevel(LinearSampler, c, 0).rg;

        if(is_edge_end(ce_e.g, ce_e.r))
        {
            float ans = -2 * p2 + 1 - end_len_high(ce_e.g, ce_e.r);
            return min(0, ans);
        }

        c += float2(-2, 0) * PIXEL_SIZE_IN_TEXCOORD;
    }

    return -2 * EDGE_DETECTION_MAX_LEN + 1;
}

float find_right_end(float2 c)
{
    c += float2(1.25, 0.125) * PIXEL_SIZE_IN_TEXCOORD;

    for(int p2 = 0; p2 < EDGE_DETECTION_MAX_LEN; ++p2)
    {
        float2 ce_e = EdgeTexture.SampleLevel(LinearSampler, c, 0).rg;

        if(is_edge_end(ce_e.g, ce_e.r))
            return 2 * p2 + end_len_low(ce_e.g, ce_e.r);

        c += float2(2, 0) * PIXEL_SIZE_IN_TEXCOORD;
    }

    return 2 * EDGE_DETECTION_MAX_LEN;
}

float find_top_end(float2 c)
{
    c += float2(-0.125, -0.25) * PIXEL_SIZE_IN_TEXCOORD;

    for(int p2 = 0; p2 < EDGE_DETECTION_MAX_LEN; ++p2)
    {
        float2 e_ce = EdgeTexture.SampleLevel(LinearSampler, c, 0).rg;

        if(is_edge_end(e_ce.r, e_ce.g))
        {
            float ans = -2 * p2 + 1 - end_len_high(e_ce.r, e_ce.g);
            return min(0, ans);
        }

        c += float2(0, -2) * PIXEL_SIZE_IN_TEXCOORD;
    }

    return -2 * EDGE_DETECTION_MAX_LEN + 1;
}

float find_bottom_end(float2 c)
{
    c += float2(-0.125, 1.25) * PIXEL_SIZE_IN_TEXCOORD;

    for(int p2 = 0; p2 < EDGE_DETECTION_MAX_LEN; ++p2)
    {
        float2 e_ce = EdgeTexture.SampleLevel(LinearSampler, c, 0).rg;

        if(is_edge_end(e_ce.r, e_ce.g))
            return 2 * p2 + end_len_low(e_ce.r, e_ce.g);

        c += float2(0, 2) * PIXEL_SIZE_IN_TEXCOORD;
    }

    return 2 * EDGE_DETECTION_MAX_LEN;
}

float2 inner_area(float dist1, float cross1, float dist2, float cross2)
{
    float base_u = (2 * EDGE_DETECTION_MAX_LEN + 1) * round(4 * cross1);
    float base_v = (2 * EDGE_DETECTION_MAX_LEN + 1) * round(4 * cross2);

    float pixel_u = base_u + dist1;
    float pixel_v = base_v + dist2;

    float u = (pixel_u + 0.5) / ((2 * EDGE_DETECTION_MAX_LEN + 1) * 5);
    float v = (pixel_v + 0.5) / ((2 * EDGE_DETECTION_MAX_LEN + 1) * 5);

    return InnerAreaTexture.SampleLevel(PointSampler, float2(u, v), 0).rg;
}

float at_corner_factor(float dl, float dr, float e1, float e2)
{
    if(dl < dr && e1 > 0.1)
        return CORNER_AREA_FACTOR;
    if(dl > dr && e2 > 0.1)
        return CORNER_AREA_FACTOR;
    return 1;
}

float ab_corner_factor(float dl, float dr, float e3, float e4)
{
    if(dl < dr && e3 > 0.1)
        return CORNER_AREA_FACTOR;
    if(dl > dr && e4 > 0.1)
        return CORNER_AREA_FACTOR;
    return 1;
}

float al_corner_factor(float db, float dt, float e1, float e2)
{
    if(db < dt && e1 > 0.1)
        return CORNER_AREA_FACTOR;
    if(db > dt && e2 > 0.1)
        return CORNER_AREA_FACTOR;
    return 1;
}

float ar_corner_factor(float db, float dt, float e3, float e4)
{
    if(db < dt && e3 > 0.1)
        return CORNER_AREA_FACTOR;
    if(db > dt && e4 > 0.1)
        return CORNER_AREA_FACTOR;
    return 1;
}

float4 main(PSInput input) : SV_TARGET
{
    float4 output = (float4)0;

    float2 e = EdgeTexture.SampleLevel(PointSampler, input.texCoord, 0).rg;

    // edge at left side
    if(e.r)
    {
        float top_end = find_top_end(input.texCoord);
        float bottom_end = find_bottom_end(input.texCoord);

        float2 coord_top = float2(
            input.texCoord.x - 0.25 * PIXEL_SIZE_IN_TEXCOORD.x,
            input.texCoord.y + top_end * PIXEL_SIZE_IN_TEXCOORD.y);
        float2 coord_bottom = float2(
            input.texCoord.x - 0.25 * PIXEL_SIZE_IN_TEXCOORD.x,
            input.texCoord.y + (bottom_end + 1) * PIXEL_SIZE_IN_TEXCOORD.y);

        float cross_top = EdgeTexture.SampleLevel(
            LinearSampler, coord_top, 0).g;
        float cross_bottom = EdgeTexture.SampleLevel(
            LinearSampler, coord_bottom, 0).g;

        output.ba = inner_area(
            -top_end, cross_top, bottom_end, cross_bottom);

        float2 e1_coord = input.texCoord + float2(-2, bottom_end + 1) * PIXEL_SIZE_IN_TEXCOORD;
        float2 e2_coord = input.texCoord + float2(-2, top_end)        * PIXEL_SIZE_IN_TEXCOORD;
        float2 e3_coord = input.texCoord + float2( 1, bottom_end + 1) * PIXEL_SIZE_IN_TEXCOORD;
        float2 e4_coord = input.texCoord + float2( 1, top_end)        * PIXEL_SIZE_IN_TEXCOORD;

        float e1 = EdgeTexture.SampleLevel(PointSampler, e1_coord, 0).g;
        float e2 = EdgeTexture.SampleLevel(PointSampler, e2_coord, 0).g;
        float e3 = EdgeTexture.SampleLevel(PointSampler, e3_coord, 0).g;
        float e4 = EdgeTexture.SampleLevel(PointSampler, e4_coord, 0).g;

        output.b *= ar_corner_factor(bottom_end, -top_end, e3, e4);
        output.a *= al_corner_factor(bottom_end, -top_end, e1, e2);
    }

    // edge at top side
    if(e.g)
    {
        // find left/right edge length

        float left_end = find_left_end(input.texCoord);
        float right_end = find_right_end(input.texCoord);

        // compute at & ab

        float2 coord_left = float2(
            input.texCoord.x + left_end * PIXEL_SIZE_IN_TEXCOORD.x,
            input.texCoord.y - 0.25 * PIXEL_SIZE_IN_TEXCOORD.y);
        float2 coord_right = float2(
            input.texCoord.x + (right_end + 1) * PIXEL_SIZE_IN_TEXCOORD.x,
            input.texCoord.y - 0.25 * PIXEL_SIZE_IN_TEXCOORD.y);

        float cross_left = EdgeTexture.SampleLevel(
            LinearSampler, coord_left, 0).r;
        float cross_right = EdgeTexture.SampleLevel(
            LinearSampler, coord_right, 0).r;

        output.rg = inner_area(
            -left_end, cross_left, right_end, cross_right);

        // corner area factor

        float2 e1_coord = input.texCoord + float2(left_end,      -2) * PIXEL_SIZE_IN_TEXCOORD;
        float2 e2_coord = input.texCoord + float2(right_end + 1, -2) * PIXEL_SIZE_IN_TEXCOORD;
        float2 e3_coord = input.texCoord + float2(left_end,       1) * PIXEL_SIZE_IN_TEXCOORD;
        float2 e4_coord = input.texCoord + float2(right_end + 1,  1) * PIXEL_SIZE_IN_TEXCOORD;

        float e1 = EdgeTexture.SampleLevel(PointSampler, e1_coord, 0).r;
        float e2 = EdgeTexture.SampleLevel(PointSampler, e2_coord, 0).r;
        float e3 = EdgeTexture.SampleLevel(PointSampler, e3_coord, 0).r;
        float e4 = EdgeTexture.SampleLevel(PointSampler, e4_coord, 0).r;

        output.r *= ab_corner_factor(-left_end, right_end, e3, e4);
        output.g *= at_corner_factor(-left_end, right_end, e1, e2);
    }

    return output;
}
)___";

static const char *BLENDING_SHADER_SOURCE = R"___(
//#define PIXEL_SIZE_IN_TEXCOORD XXX

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD;
};

Texture2D<float4> ImageTexture  : register(t0);
Texture2D<float4> WeightTexture : register(t1);

SamplerState      PointSampler  : register(s0);
SamplerState      LinearSampler : register(s1);

float4 main(PSInput input) : SV_TARGET
{
    float2 w_up_left = WeightTexture.SampleLevel(
        PointSampler, input.texCoord, 0).rb;
    float w_right = WeightTexture.SampleLevel(
        PointSampler, input.texCoord, 0, int2(1, 0)).a;
    float w_down = WeightTexture.SampleLevel(
        PointSampler, input.texCoord, 0, int2(0, 1)).g;

    float w_sum = dot(float4(w_up_left, w_right, w_down), 1);
    if(w_sum == 0)
        return ImageTexture.SampleLevel(PointSampler, input.texCoord, 0);

    float4 up = ImageTexture.SampleLevel(
        LinearSampler, input.texCoord + PIXEL_SIZE_IN_TEXCOORD * float2(0, -w_up_left.r), 0);
    float4 right = ImageTexture.SampleLevel(
        LinearSampler, input.texCoord + PIXEL_SIZE_IN_TEXCOORD * float2(w_right, 0), 0);
    float4 down = ImageTexture.SampleLevel(
        LinearSampler, input.texCoord + PIXEL_SIZE_IN_TEXCOORD * float2(0, w_down), 0);
    float4 left = ImageTexture.SampleLevel(
        LinearSampler, input.texCoord + PIXEL_SIZE_IN_TEXCOORD * float2(-w_up_left.g, 0), 0);

    return (up    * w_up_left.r +
            right * w_right     +
            down  * w_down      +
            left  * w_up_left.g) / w_sum;
}
)___";

// ========= inner area texture =========

struct Vec2
{
    float x = 0, y = 0;

    Vec2 operator+(const Vec2 &rhs) const noexcept
        { return { x + rhs.x, y + rhs.y }; }
    
    Vec2 operator-(const Vec2 &rhs) const noexcept
        { return { x - rhs.x, y - rhs.y }; }
};

inline Vec2 operator*(float lhs, const Vec2 &rhs) noexcept
    { return { lhs * rhs.x, lhs * rhs.y }; }

/*
  given line segment ab and a pixel where pixel.x \in (a.x, b.x),
  compute areas of divided-by-line parts of the pixel

  returns: (a1, a2)
    a1: area in the lower pixel that
        should be covered by color of the upper pixel
    a2: area in the upper pixel that
        should be covered by color of the lower pixel
*/
inline std::pair<float, float> ComputePixelInnerArea(
    const Vec2 &a, const Vec2 &b, int pixel)
{
    const float xL = static_cast<float>(pixel);
    const float xR = static_cast<float>(pixel + 1);

    const float x0 = a.x, y0 = a.y;
    const float x1 = b.x, y1 = b.y;

    const float yL = y0 + (xL - x0) * (y1 - y0) / (x1 - x0);
    const float yR = y0 + (xR - x0) * (y1 - y0) / (x1 - x0);

    if((xL < x0 || xL >= x1) && (xR <= x0 || xR > x1))
        return { 0.0f, 0.0f };

    // case 1. one trapezoid

    if(std::abs(yL) < 1e-4f ||
       std::abs(yR) < 1e-4f ||
       ((yL > 0) == (yR > 0)))
    {
        const float area = (yL + yR) / 2;
        if(area < 0)
            return { 0.0f, -area };
        return { area, 0.0f };
    }

    // case 2. two triangles

    const float xM = -y0 * (x1 - x0) / (y1 - y0) + x0;
    
    float areaLeft  = std::abs(0.5f * yL * (xM - xL));
    float areaRight = std::abs(0.5f * yR * (xR - xM));

    if(xM <= x0)
        areaLeft = 0;
    if(xM >= x1)
        areaRight = 0;

    // left is higher than right
    if(yL < yR)
        return { areaRight, areaLeft };
    return { areaLeft, areaRight };
}

inline std::pair<float, float> ComputePixelInnerArea(
    int dist1, int cross1, int dist2, int cross2)
{
    const float dist = static_cast<float>(dist1) +
                       static_cast<float>(dist2) + 1;

    if(cross1 == 0 && cross2 == 0)
    {
        //
        // ----------
        //
        return { 0.0f, 0.0f };
    }

    if(cross1 == 0 && cross2 == 1)
    {
        //          |
        // ----------
        //
        return ComputePixelInnerArea(
            { dist / 2, 0 }, { dist, -0.5f }, dist1);
    }

    if(cross1 == 0 && cross2 == 3)
    {
        //
        // ----------
        //          |
        return ComputePixelInnerArea(
            { dist / 2, 0 }, { dist, 0.5f }, dist1);
    }

    if(cross1 == 0 && cross2 == 4)
    {
        //          |
        // ----------
        //          |
        return { 0.0f, 0.0f };
    }

    if(cross1 == 1 && cross2 == 0)
    {
        // |
        // ----------
        //
        return ComputePixelInnerArea(
            { 0, -0.5f }, { dist / 2, 0 }, dist1);
    }

    if(cross1 == 1 && cross2 == 1)
    {
        // |        |
        // ----------
        //
        const auto aL = ComputePixelInnerArea(
            { 0, -0.5f }, { dist / 2, 0.0f }, dist1);
        const auto aR = ComputePixelInnerArea(
            { dist / 2, 0.0f }, { dist, -0.5f }, dist1);
        return { aL.first + aR.first, aL.second + aR.second };
    }

    if(cross1 == 1 && cross2 == 3)
    {
        // |
        // ----------
        //          |
        return ComputePixelInnerArea(
            { 0, -0.5f }, { dist, 0.5f }, dist1);
    }

    if(cross1 == 1 && cross2 == 4)
    {
        // |        |
        // ----------
        //          |
        return ComputePixelInnerArea(
            { 0, -0.5f }, { dist, 0.5f }, dist1);
    }

    if(cross1 == 3 && cross2 == 0)
    {
        //
        // ----------
        // |
        return ComputePixelInnerArea(
            { 0, 0.5f }, { dist / 2, 0 }, dist1);
    }

    if(cross1 == 3 && cross2 == 1)
    {
        //          |
        // ----------
        // |
        return ComputePixelInnerArea(
            { 0, 0.5f }, { dist, -0.5f }, dist1);
    }

    if(cross1 == 3 && cross2 == 3)
    {
        //
        // ----------
        // |        |
        const auto aL = ComputePixelInnerArea(
            { 0, 0.5f }, { dist / 2, 0.0f }, dist1);
        const auto aR = ComputePixelInnerArea(
            { dist / 2, 0.0f }, { dist, 0.5f }, dist1);
        return { aL.first + aR.first, aL.second + aR.second };
    }

    if(cross1 == 3 && cross2 == 4)
    {
        //          |
        // ----------
        // |        |
        return ComputePixelInnerArea(
            { 0, 0.5f }, { dist, -0.5f }, dist1);
    }

    if(cross1 == 4 && cross2 == 0)
    {
        // |
        // ----------
        // |
        return { 0.0f, 0.0f };
    }

    if(cross1 == 4 && cross2 == 1)
    {
        // |        |
        // ----------
        // |
        return ComputePixelInnerArea(
            { 0, 0.5f }, { dist, -0.5f }, dist1);
    }

    if(cross1 == 4 && cross2 == 3)
    {
        // |
        // ----------
        // |        |
        return ComputePixelInnerArea(
            { 0, -0.5f }, { dist, 0.5f }, dist1);
    }

    if(cross1 == 4 && cross2 == 4)
    {
        // |        |
        // ----------
        // |        |
        return { 0.0f, 0.0f };
    }

    return { 0.0f, 0.0f };
}

// this method will be called for only once during construction
// so it's not well optimized
inline std::vector<float> GenerateInnerAreaTexture(
    int maxEdgeDetectionLen,
    int *width, int *height)
{
    // the inner area texture contains 25 grids
    // grid [i, j] <=> [cross1, cross2] = [i / 4, j / 4]
    // each grid contains 2*maxEdgeDetectionLen+1 texels
    // texel [m, n] <=> [dist1, dist2] = [m, n]

    const int gridSidelen = 2 * maxEdgeDetectionLen + 1;
    const int sidelen     = 5 * gridSidelen;

    *width  = sidelen;
    *height = sidelen;

    // texel data

    std::vector<float> ret(sidelen * sidelen * 4, 0.0f);

    auto texel = [&](int dist1, int cross1, int dist2, int cross2)
        -> float*
    {
        const int base_u = gridSidelen * cross1;
        const int base_v = gridSidelen * cross2;
        
        const int u = base_u + dist1;
        const int v = base_v + dist2;

        const int idx = v * sidelen + u;
        return &ret[4 * idx];
    };

    // fill texels

    for(int cross1 = 0; cross1 < 5; ++cross1)
    {
        for(int cross2 = 0; cross2 < 5; ++cross2)
        {
            for(int dist1 = 0; dist1 < gridSidelen; ++dist1)
            {
                for(int dist2 = 0; dist2 < gridSidelen; ++dist2)
                {
                    const auto rg = ComputePixelInnerArea(
                        dist1, cross1, dist2, cross2);

                    auto pTexel = texel(dist1, cross1, dist2, cross2);
                    pTexel[0] = rg.first;
                    pTexel[1] = rg.second;
                    pTexel[3] = 1;
                }
            }
        }
    }

    return ret;
}

} // namespace detail

Common::Common(
    ID3D11Device *device, ID3D11DeviceContext *deviceContext)
{
    D  = device;
    DC = deviceContext;

    // vertex shader

    ComPtr<ID3D10Blob> vertexShaderByteCode = detail::CompileToByteCode(
        detail::COMMON_VERTEX_SHADER_SOURCE, "vs_5_0", nullptr);

    vertexShader = detail::CreateVertexShader(
        device,
        vertexShaderByteCode->GetBufferPointer(),
        vertexShaderByteCode->GetBufferSize());

    // vertex buffer

    struct Vertex { float x, y, u, v; };

    Vertex vertexData[] = {
        { -1, -1, +0, +1 },
        { -1, +3, +0, -1 },
        { +3, -1, +2, +1 }
    };

    D3D11_BUFFER_DESC bufferDesc;
    bufferDesc.Usage               = D3D11_USAGE_IMMUTABLE;
    bufferDesc.BindFlags           = D3D11_BIND_VERTEX_BUFFER;
    bufferDesc.ByteWidth           = static_cast<UINT>(sizeof(Vertex)) * 3;
    bufferDesc.CPUAccessFlags      = 0;
    bufferDesc.MiscFlags           = 0;
    bufferDesc.StructureByteStride = 0;

    D3D11_SUBRESOURCE_DATA subrscData;
    subrscData.pSysMem          = vertexData;
    subrscData.SysMemPitch      = 0;
    subrscData.SysMemSlicePitch = 0;

    HRESULT hr = device->CreateBuffer(
        &bufferDesc, &subrscData, vertexBuffer.GetAddressOf());
    if(FAILED(hr))
        throw std::runtime_error("MLAA: failed to initialize vertex buffer");

    // input layout

    D3D11_INPUT_ELEMENT_DESC inputDesc[] = {
        {
            "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,
            0, offsetof(Vertex, x),
            D3D11_INPUT_PER_VERTEX_DATA, 0
        },
        {
            "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,
            0, offsetof(Vertex, u),
            D3D11_INPUT_PER_VERTEX_DATA, 0
        }
    };

    hr = device->CreateInputLayout(
        inputDesc, 2,
        vertexShaderByteCode->GetBufferPointer(),
        vertexShaderByteCode->GetBufferSize(),
        inputLayout.GetAddressOf());
    if(FAILED(hr))
        throw std::runtime_error("MLAA: failed to create input layout");

    // samplers
    
    const float BORDER_COLOR[4] = { 0, 0, 0, 0 };

    D3D11_SAMPLER_DESC samplerDesc;
    samplerDesc.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MipLODBias     = 0;
    samplerDesc.MaxAnisotropy  = 0;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD         = -FLT_MAX;
    samplerDesc.MaxLOD         = FLT_MAX;
    memcpy(samplerDesc.BorderColor, BORDER_COLOR, sizeof(BORDER_COLOR));

    hr = device->CreateSamplerState(
        &samplerDesc, pointSampler.GetAddressOf());
    if(FAILED(hr))
        throw std::runtime_error("MLAA: failed to initialize point sampler");

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    hr = device->CreateSamplerState(
        &samplerDesc, linearSampler.GetAddressOf());
    if(FAILED(hr))
        throw std::runtime_error("MLAA: failed to initialize point sampler");
}

void Common::bindVertex() const
{
    const UINT stride = sizeof(float) * 4, offset = 0;
    DC->IASetVertexBuffers(
        0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);

    DC->IASetInputLayout(inputLayout.Get());
    DC->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Common::unbindVertex() const
{
    const UINT stride = 0, offset = 0;

    ID3D11Buffer *NULL_VERTEX_BUFFER = nullptr;
    DC->IASetVertexBuffers(
        0, 1, &NULL_VERTEX_BUFFER, &stride, &offset);

    DC->IASetInputLayout(nullptr);
}

EdgeDetection::EdgeDetection(
    ID3D11Device *device,
    float         edgeThreshold,
    float         localContrastFactor)
{
    const std::string edgeThresholdStr =
        std::to_string(edgeThreshold);
    const std::string localContrastFactorStr =
        std::to_string(localContrastFactor);

    const D3D_SHADER_MACRO MACROS[] = {
        { "EDGE_THRESHOLD" , edgeThresholdStr.c_str()       },
        { "CONTRAST_FACTOR", localContrastFactorStr.c_str() },
        { nullptr          , nullptr                        }
    };

    ComPtr<ID3D10Blob> shaderByteCode = detail::CompileToByteCode(
        detail::EDGE_DETECTION_SHADER_SOURCE, "ps_5_0", MACROS);

    pixelShader = detail::CreatePixelShader(
        device,
        shaderByteCode->GetBufferPointer(),
        shaderByteCode->GetBufferSize());
}

void EdgeDetection::detectEdge(
    const Common             &common,
    ID3D11ShaderResourceView *img) const
{
    // bind shader/texture/sampler

    common.DC->VSSetShader(common.vertexShader.Get(), nullptr, 0);
    common.DC->PSSetShader(pixelShader.Get(), nullptr, 0);
    common.DC->PSSetSamplers(0, 1, common.pointSampler.GetAddressOf());
    common.DC->PSSetShaderResources(0, 1, &img);

    // bind vertex buffer/input layout

    common.bindVertex();

    // emit drawcall

    common.DC->Draw(3, 0);

    // clear vertex buffer/input layout

    common.unbindVertex();

    // clear shader/texture/sampler

    ID3D11SamplerState *NULL_SAMPLER = nullptr;
    common.DC->PSSetSamplers(0, 1, &NULL_SAMPLER);

    ID3D11ShaderResourceView *NULL_IMG = nullptr;
    common.DC->PSSetShaderResources(0, 1, &NULL_IMG);

    common.DC->PSSetShader(nullptr, nullptr, 0);
    common.DC->VSSetShader(nullptr, nullptr, 0);
}

BlendingWeight::BlendingWeight(
    ID3D11Device *device,
    int           maxSearchDistanceLen,
    float         cornerAreaFactor,
    int           width,
    int           height)
{
    // pixel shader

    const std::string maxEdgeDetectionLenStr =
        std::to_string(maxSearchDistanceLen);
    const std::string cornerAreaFactorStr =
        std::to_string(cornerAreaFactor);

    const std::string pixelSizeInTexCoordStr = 
        "float2(" + std::to_string(1.0f / width) + ", "
                  + std::to_string(1.0f / height) + ")";

    const D3D_SHADER_MACRO WEIGHT_MACROS[] = {
        { "EDGE_DETECTION_MAX_LEN" , maxEdgeDetectionLenStr.c_str() },
        { "PIXEL_SIZE_IN_TEXCOORD" , pixelSizeInTexCoordStr.c_str() },
        { "CORNER_AREA_FACTOR"     , cornerAreaFactorStr.c_str()    },
        { nullptr                  , nullptr                        }
    };

    ComPtr<ID3D10Blob> weightShaderByteCode = detail::CompileToByteCode(
        detail::BLENDING_WEIGHT_SHADER_SOURCE, "ps_5_0", WEIGHT_MACROS);

    pixelShader = detail::CreatePixelShader(
        device,
        weightShaderByteCode->GetBufferPointer(),
        weightShaderByteCode->GetBufferSize());

    // inner area texture

    int innerWidth, innerHeight;
    const auto texData = detail::GenerateInnerAreaTexture(
        maxSearchDistanceLen, &innerWidth, &innerHeight);

    // create d3d11 texture

    D3D11_TEXTURE2D_DESC texDesc;
    texDesc.Width              = innerWidth;
    texDesc.Height             = innerHeight;
    texDesc.MipLevels          = 1;
    texDesc.ArraySize          = 1;
    texDesc.Format             = DXGI_FORMAT_R32G32B32A32_FLOAT;
    texDesc.SampleDesc.Count   = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage              = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags     = 0;
    texDesc.MiscFlags          = 0;

    D3D11_SUBRESOURCE_DATA initData;
    initData.pSysMem          = texData.data();
    initData.SysMemPitch      = innerWidth * sizeof(float) * 4;
    initData.SysMemSlicePitch = 0;

    HRESULT hr = device->CreateTexture2D(
        &texDesc, &initData, innerAreaTexture.GetAddressOf());
    if(FAILED(hr))
        throw std::runtime_error("failed to create inner area texture");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    srvDesc.Format                    = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels       = 1;

    hr = device->CreateShaderResourceView(
        innerAreaTexture.Get(), &srvDesc, innerAreaTextureSRV.GetAddressOf());
    if(FAILED(hr))
    {
        throw std::runtime_error(
            "failed to create shader resource view for inner area texture");
    }
}

void BlendingWeight::computeBlendingWeight(
    const Common             &common,
    ID3D11ShaderResourceView *edgeTexture)
{
    // bind shader/texture/sampler/constant buffer

    common.DC->VSSetShader(common.vertexShader.Get(), nullptr, 0);
    common.DC->PSSetShader(pixelShader.Get(), nullptr, 0);
    common.DC->PSSetSamplers(0, 1, common.pointSampler.GetAddressOf());
    common.DC->PSSetSamplers(1, 1, common.linearSampler.GetAddressOf());
    common.DC->PSSetShaderResources(0, 1, &edgeTexture);
    common.DC->PSSetShaderResources(1, 1, innerAreaTextureSRV.GetAddressOf());

    // bind vertex buffer/input layout

    common.bindVertex();

    // emit drawcall

    common.DC->Draw(3, 0);

    // clear vertex buffer/input layout

    common.unbindVertex();

    // clear shader/texture/sampler

    ID3D11SamplerState *NULL_SAMPLER = nullptr;
    common.DC->PSSetSamplers(0, 1, &NULL_SAMPLER);
    common.DC->PSSetSamplers(1, 1, &NULL_SAMPLER);

    ID3D11ShaderResourceView *NULL_IMG = nullptr;
    common.DC->PSSetShaderResources(0, 1, &NULL_IMG);
    common.DC->PSSetShaderResources(1, 1, &NULL_IMG);

    common.DC->PSSetShader(nullptr, nullptr, 0);
    common.DC->VSSetShader(nullptr, nullptr, 0);
}

inline Blending::Blending(
    ID3D11Device *device,
    int           width,
    int           height)
{
    const std::string pixelSizeInTexCoordStr =
        "float2(" + std::to_string(1.0f / width) + ", "
        + std::to_string(1.0f / height) + ")";

    const D3D_SHADER_MACRO MACROS[] = {
        { "PIXEL_SIZE_IN_TEXCOORD", pixelSizeInTexCoordStr.c_str()  },
        { nullptr                  , nullptr                        }
    };

    ComPtr<ID3D10Blob> blendingShaderByteCode = detail::CompileToByteCode(
        detail::BLENDING_SHADER_SOURCE, "ps_5_0", MACROS);

    pixelShader = detail::CreatePixelShader(
        device,
        blendingShaderByteCode->GetBufferPointer(),
        blendingShaderByteCode->GetBufferSize());
}

inline void Blending::blend(
    const Common         &common,
    ID3D11ShaderResourceView *weightTexture,
    ID3D11ShaderResourceView *img) const
{
    // bind shader/texture/sampler/constant buffer

    common.DC->VSSetShader(common.vertexShader.Get(), nullptr, 0);
    common.DC->PSSetShader(pixelShader.Get(), nullptr, 0);
    common.DC->PSSetSamplers(0, 1, common.pointSampler.GetAddressOf());
    common.DC->PSSetSamplers(1, 1, common.linearSampler.GetAddressOf());
    common.DC->PSSetShaderResources(0, 1, &img);
    common.DC->PSSetShaderResources(1, 1, &weightTexture);
    
    // bind vertex buffer/input layout

    common.bindVertex();

    // emit drawcall

    common.DC->Draw(3, 0);

    // clear vertex buffer/input layout

    common.unbindVertex();

    // clear shader/texture/sampler

    ID3D11SamplerState *NULL_SAMPLER = nullptr;
    common.DC->PSSetSamplers(0, 1, &NULL_SAMPLER);
    common.DC->PSSetSamplers(1, 1, &NULL_SAMPLER);

    ID3D11ShaderResourceView *NULL_IMG = nullptr;
    common.DC->PSSetShaderResources(0, 1, &NULL_IMG);
    common.DC->PSSetShaderResources(1, 1, &NULL_IMG);

    common.DC->PSSetShader(nullptr, nullptr, 0);
    common.DC->VSSetShader(nullptr, nullptr, 0);
}

SMAA::SMAA(
    ID3D11Device        *device,
    ID3D11DeviceContext *deviceContext,
    float                edgeThreshold,
    float                localContractFactor,
    int                  maxSearchDistanceLen,
    float                cornerAreaFactor,
    int                  width,
    int                  height)
    : common_(device, deviceContext),
      edgeDetection_(device, edgeThreshold, localContractFactor),
      blendingWeight_(
          device, maxSearchDistanceLen, cornerAreaFactor, width, height),
      blending_(device, width, height)
{
    
}

void SMAA::detectEdge(ID3D11ShaderResourceView *img) const
{
    edgeDetection_.detectEdge(common_, img);
}

void SMAA::computeBlendingWeight(
    ID3D11ShaderResourceView *edgeTexture)
{
    blendingWeight_.computeBlendingWeight(common_, edgeTexture);
}

void SMAA::blend(
    ID3D11ShaderResourceView *weightTexture,
    ID3D11ShaderResourceView *img)
{
    blending_.blend(common_, weightTexture, img);
}

} } // namespace agz::smaa
